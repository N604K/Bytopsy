#include "protect.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>

#include "tree.h"
#include "util.h"

namespace {

using P = Protect;

// ---------------------------------------------------------------------------------------------
// Rules

struct Rule {
    P self = P::None;      // the protected item itself
    P inside = P::None;    // anything inside it (None: only the item itself is protected)
    P contains = P::None;  // a folder that contains it
    bool allow = false;    // carve-out: overrides less specific path rules (caches inside profiles)
    bool nameRule = false; // matches by item name anywhere instead of by path
    bool resolve = true;   // look for it on disk so deleting a parent folder warns too
    bool fileOnly = false; // name rules: files only (a folder named ".env" is usually a Python venv)
    std::wstring group, name, reason;
    std::vector<std::wstring> segs;  // path rules: lowercase pattern segments ('*' and '?' allowed)
    std::vector<std::wstring> disp;  // the same segments with their original case (for display)
    std::wstring pat;                // name rules: lowercase pattern
    int wild = 0;                    // number of wildcard segments (fewer = more specific)
};

struct Resolved {
    std::wstring lower;  // lowercase path
    std::wstring path;   // as found on disk
    size_t rule = 0;
};

struct State {
    std::mutex m;
    bool built = false;
    unsigned gen = 0;  // bumped on every rebuild, so caches of check results know they are stale
    std::vector<Rule> rules;
    std::vector<Resolved> resolved;
};

State& S() {
    static State* s = new State();  // intentionally leaked: used from worker threads at exit
    return *s;
}

std::wstring Env(const std::wstring& s) {
    std::vector<wchar_t> buf(32768);
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n > buf.size()) return L"";
    std::wstring out(buf.data());
    if (out.find(L'%') != std::wstring::npos) return L"";  // undefined variable: rule doesn't apply
    return out;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

// '\' separated, no "\\?\" prefix, no trailing separator, split into segments (case kept).
std::vector<std::wstring> SplitRaw(const std::wstring& path) {
    std::wstring p = path;
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0)
        p = L"\\\\" + p.substr(8);
    else if (p.rfind(L"\\\\?\\", 0) == 0)
        p = p.substr(4);
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find(L'\\', i);
        if (j == std::wstring::npos) j = p.size();
        if (j > i) out.push_back(p.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

std::vector<std::wstring> LowerSegs(std::vector<std::wstring> v) {
    for (auto& s : v) s = util::ToLower(s);
    return v;
}

std::wstring Join(const std::vector<std::wstring>& segs, size_t n) {
    std::wstring out;
    for (size_t i = 0; i < n && i < segs.size(); ++i) {
        if (i) out += L'\\';
        out += segs[i];
    }
    if (n == 1 && !out.empty() && out.back() == L':') out += L'\\';
    return out;
}

bool SegMatch(const std::wstring& pattern, const std::wstring& seg) {
    if (!util::HasWildcards(pattern)) return pattern == seg;
    return util::GlobMatchI(pattern, seg);
}

// Fast case-insensitive name tests used while walking large scan trees.
bool NameIs(const std::wstring& name, const std::wstring& lowerPat) {
    if (name.size() != lowerPat.size()) return false;
    for (size_t i = 0; i < name.size(); ++i)
        if (util::LowerChar(name[i]) != lowerPat[i]) return false;
    return true;
}

bool NameMatch(const std::wstring& name, const std::wstring& lowerPat) {
    if (!util::HasWildcards(lowerPat)) return NameIs(name, lowerPat);
    // "*.ext" fast path
    if (lowerPat.size() > 1 && lowerPat[0] == L'*' && !util::HasWildcards(lowerPat.substr(1))) {
        size_t n = lowerPat.size() - 1;
        if (name.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (util::LowerChar(name[name.size() - n + i]) != lowerPat[1 + i]) return false;
        return true;
    }
    return util::GlobMatchI(lowerPat, name);
}

// Placeholders expand to the current user's folder and to every profile under C:\Users.
std::vector<std::wstring> ExpandPlaceholders(const std::wstring& pattern) {
    static const std::pair<const wchar_t*, std::pair<const wchar_t*, const wchar_t*>> kMap[] = {
        {L"{HOME}", {L"%USERPROFILE%", L"%SystemDrive%\\Users\\*"}},
        {L"{ROAMING}", {L"%APPDATA%", L"%SystemDrive%\\Users\\*\\AppData\\Roaming"}},
        {L"{LOCAL}", {L"%LOCALAPPDATA%", L"%SystemDrive%\\Users\\*\\AppData\\Local"}},
        {L"{LOCALLOW}", {L"%USERPROFILE%\\AppData\\LocalLow", L"%SystemDrive%\\Users\\*\\AppData\\LocalLow"}},
    };
    for (auto& kv : kMap) {
        size_t at = pattern.find(kv.first);
        if (at == std::wstring::npos) continue;
        std::wstring rest = pattern.substr(at + wcslen(kv.first));
        std::wstring head = pattern.substr(0, at);
        return {head + kv.second.first + rest, head + kv.second.second + rest};
    }
    return {pattern};
}

class Builder {
public:
    explicit Builder(std::vector<Rule>& out) : out_(out) {}

    // A path rule. `pattern` may use %ENV%, {HOME}, {ROAMING}, {LOCAL}, {LOCALLOW} and '*' segments.
    void Path(const std::wstring& pattern, P self, P inside, const wchar_t* group, const wchar_t* name,
              const wchar_t* reason, P contains = P(255), bool resolve = true) {
        for (auto& p : ExpandPlaceholders(pattern)) {
            std::wstring e = Env(p);
            if (e.empty()) continue;
            Rule r;
            r.self = self;
            r.inside = inside;
            r.contains = contains == P(255) ? std::max(self, inside) : contains;
            r.group = group;
            r.name = name;
            r.reason = reason;
            r.resolve = resolve;
            r.disp = SplitRaw(e);
            r.segs = LowerSegs(r.disp);
            if (r.segs.empty()) continue;
            for (auto& s : r.segs)
                if (util::HasWildcards(s)) ++r.wild;
            out_.push_back(std::move(r));
        }
    }
    void Tree(const std::wstring& pattern, P level, const wchar_t* group, const wchar_t* name, const wchar_t* reason) {
        Path(pattern, level, level, group, name, reason);
    }
    void Self(const std::wstring& pattern, P level, const wchar_t* group, const wchar_t* name, const wchar_t* reason) {
        Path(pattern, level, P::None, group, name, reason);
    }
    // Carve-out inside a protected tree (caches that are safe to delete). `level` may be Caution.
    void Allow(const std::wstring& pattern, P level = P::None, const wchar_t* reason = L"") {
        size_t before = out_.size();
        Path(pattern, level, level, L"", L"", reason, P::None, false);
        for (size_t i = before; i < out_.size(); ++i) out_[i].allow = true;
    }
    // A name rule: matches any file or folder with this name, wherever it is.
    void Name(const std::wstring& pattern, P self, P inside, P contains, const wchar_t* group, const wchar_t* name,
              const wchar_t* reason, bool fileOnly = false) {
        Rule r;
        r.nameRule = true;
        r.fileOnly = fileOnly;
        r.self = self;
        r.inside = inside;
        r.contains = contains;
        r.group = group;
        r.name = name;
        r.reason = reason;
        r.pat = util::ToLower(pattern);
        out_.push_back(std::move(r));
    }

    // VS Code and its forks keep chat history and settings next to caches that are safe to delete.
    void EditorFamily(const std::wstring& roamingDir, const wchar_t* group, const wchar_t* name, const wchar_t* reason) {
        Tree(L"{ROAMING}\\" + roamingDir, P::Important, group, name, reason);
        for (const wchar_t* c : {L"Cache", L"CachedData", L"Code Cache", L"GPUCache", L"CachedExtensionVSIXs",
                                 L"CachedProfilesData", L"logs", L"Crashpad", L"DawnCache", L"DawnGraphiteCache",
                                 L"DawnWebGPUCache", L"Service Worker\\CacheStorage", L"Service Worker\\ScriptCache"})
            Allow(L"{ROAMING}\\" + roamingDir + L"\\" + c);
    }

    // Chromium browsers: the profile has passwords, cookies, history and extension data (wallets!).
    void Chromium(const std::wstring& userData, const wchar_t* name) {
        Tree(userData, P::Important, L"Browser", name,
             L"Browser profile: saved passwords, cookies, bookmarks, history and extension data (including crypto "
             L"wallet extensions). Use Cleanup \u2192 Browser caches to free space safely.");
        for (const wchar_t* c : {L"*\\Cache", L"*\\Code Cache", L"*\\GPUCache", L"*\\Service Worker\\CacheStorage",
                                 L"*\\Service Worker\\ScriptCache", L"*\\DawnCache", L"*\\DawnGraphiteCache",
                                 L"*\\DawnWebGPUCache", L"*\\GrShaderCache", L"*\\ShaderCache", L"GrShaderCache",
                                 L"ShaderCache", L"Crashpad", L"component_crx_cache", L"optimization_guide_model_store",
                                 L"Snapshots"})
            Allow(userData + L"\\" + c);
    }

private:
    std::vector<Rule>& out_;
};

// =============================================================================================
// Built-in rules. Keep this list honest: Blocked only for things whose deletion breaks Windows
// or that Windows has a proper tool for; Important for data that can't be downloaded again.

void BuiltinRules(std::vector<Rule>& v) {
    Builder b(v);

    // ---- Windows and boot ------------------------------------------------------------------
    const wchar_t* kSys = L"System";
    b.Tree(L"%SystemRoot%", P::Blocked, kSys, L"Windows folder",
           L"Deleting files here can stop Windows from starting. Use Cleanup or Windows Disk Cleanup instead.");
    b.Tree(L"%SystemRoot%\\WinSxS", P::Blocked, kSys, L"Windows component store",
           L"Shared system components (mostly hard links). Shrink it with: DISM /Online /Cleanup-Image /StartComponentCleanup");
    b.Tree(L"%SystemRoot%\\Installer", P::Blocked, kSys, L"Windows Installer cache",
           L"Needed to update, repair and uninstall programs. Deleting it breaks uninstallers.");
    b.Allow(L"%SystemRoot%\\Temp", P::Caution, L"Windows temp folder. Cleanup \u2192 Windows temporary files skips files in use.");
    b.Allow(L"%SystemRoot%\\SoftwareDistribution\\Download", P::Caution, L"Windows Update downloads; Windows fetches them again if needed.");
    b.Allow(L"%SystemRoot%\\Minidump");
    b.Allow(L"%SystemRoot%\\LiveKernelReports");
    b.Allow(L"%SystemRoot%\\MEMORY.DMP");
    b.Allow(L"%SystemRoot%\\Logs\\CBS\\*.cab");
    b.Allow(L"%SystemRoot%\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization\\Cache");
    b.Tree(L"*:\\System Volume Information", P::Blocked, kSys, L"System Restore and shadow copies",
           L"Manage restore points in System Properties \u2192 System Protection.");
    b.Tree(L"*:\\$Recycle.Bin", P::Blocked, kSys, L"Recycle Bin storage", L"Empty the bin with Cleanup \u2192 Recycle Bin instead.");
    b.Tree(L"*:\\Recovery", P::Blocked, kSys, L"Windows recovery environment", L"Needed to repair or reset Windows.");
    b.Tree(L"*:\\$WinREAgent", P::Blocked, kSys, L"Windows recovery update", L"Used by Windows Update for the recovery environment.");
    b.Tree(L"*:\\Boot", P::Blocked, kSys, L"Boot files", L"Needed to start the PC.");
    b.Tree(L"*:\\EFI", P::Blocked, kSys, L"Boot files", L"Needed to start the PC.");
    b.Tree(L"*:\\Config.Msi", P::Blocked, kSys, L"Installer rollback data", L"Used while programs are being installed.");
    for (const wchar_t* f : {L"bootmgr", L"BOOTNXT", L"BOOTSECT.BAK"})
        b.Self(std::wstring(L"*:\\") + f, P::Blocked, kSys, L"Boot file", L"Needed to start the PC.");
    b.Self(L"*:\\pagefile.sys", P::Blocked, kSys, L"Page file",
           L"Windows virtual memory. Resize it in System Properties \u2192 Advanced \u2192 Performance.");
    b.Self(L"*:\\swapfile.sys", P::Blocked, kSys, L"Swap file", L"Windows virtual memory for Store apps.");
    b.Self(L"*:\\hiberfil.sys", P::Blocked, kSys, L"Hibernation file",
           L"Turn hibernation off to remove it: powercfg /hibernate off (as administrator).");
    b.Path(L"*:\\Windows.old", P::Important, P::Important, kSys, L"Previous Windows installation",
           L"Needed to roll back an upgrade. Remove it with Cleanup \u2192 Previous Windows installation.");
    b.Tree(L"*:\\$Windows.~BT", P::Important, kSys, L"Windows upgrade files", L"Used by an upgrade in progress or for rollback.");
    b.Tree(L"*:\\$Windows.~WS", P::Important, kSys, L"Windows upgrade files", L"Used by an upgrade in progress or for rollback.");

    // ---- Program folders -------------------------------------------------------------------
    const wchar_t* kProg = L"Programs";
    const wchar_t* kUninstall = L"Installed program. Uninstall it in Settings \u2192 Apps; deleting the folder leaves a "
                                L"broken entry, services and drivers behind.";
    for (const wchar_t* pf : {L"%ProgramFiles%", L"%ProgramFiles(x86)%", L"%ProgramW6432%"}) {
        b.Path(pf, P::Blocked, P::Important, kProg, L"Program Files", kUninstall, P::Blocked, true);
        b.Tree(std::wstring(pf) + L"\\WindowsApps", P::Blocked, kProg, L"Microsoft Store apps",
               L"Managed by Windows. Uninstall Store apps in Settings \u2192 Apps.");
        b.Tree(std::wstring(pf) + L"\\Common Files", P::Blocked, kProg, L"Shared program components",
               L"Shared by many programs.");
    }
    b.Path(L"%ProgramData%", P::Blocked, P::Caution, kProg, L"ProgramData",
           L"Settings and data shared by all users. Programs may stop working without it.", P::Blocked, true);
    b.Tree(L"%ProgramData%\\Microsoft\\Windows Defender", P::Blocked, kSys, L"Microsoft Defender", L"Antivirus definitions and quarantine.");
    b.Tree(L"%ProgramData%\\Microsoft\\Crypto", P::Important, L"Keys", L"Machine keys and certificates", L"Removing it breaks certificates and encryption.");
    b.Tree(L"%ProgramData%\\Package Cache", P::Important, kProg, L"Installer package cache",
           L"Needed to repair, update or uninstall Visual Studio, .NET, VC++ runtimes and others.");
    b.Allow(L"%ProgramData%\\Microsoft\\Windows\\WER");

    // ---- User profiles -----------------------------------------------------------------------
    const wchar_t* kUser = L"User";
    b.Self(L"%SystemDrive%\\Users", P::Blocked, kUser, L"All user profiles", L"Contains every account's files and settings.");
    b.Self(L"%SystemDrive%\\Users\\*", P::Blocked, kUser, L"A user profile",
           L"A whole account. Remove accounts in Settings \u2192 Accounts.");
    b.Self(L"%USERPROFILE%", P::Blocked, kUser, L"Your user profile", L"Your whole account: files, settings and app data.");
    b.Tree(L"%SystemDrive%\\Users\\Default", P::Blocked, kUser, L"Default profile", L"Template for new accounts.");
    b.Self(L"{HOME}\\AppData", P::Blocked, kUser, L"AppData", L"Settings and data of every app you use.");
    b.Self(L"{ROAMING}", P::Blocked, kUser, L"AppData\\Roaming", L"Settings and data of every app you use.");
    b.Self(L"{LOCAL}", P::Blocked, kUser, L"AppData\\Local", L"Settings and data of every app you use.");
    b.Self(L"{LOCALLOW}", P::Important, kUser, L"AppData\\LocalLow", L"Often holds game saves and app settings.");
    b.Self(L"{HOME}\\NTUSER.DAT*", P::Blocked, kSys, L"Your registry", L"Your personal registry hive; Windows can't sign you in without it.");
    b.Self(L"{LOCAL}\\Microsoft\\Windows\\UsrClass.dat*", P::Blocked, kSys, L"Your registry", L"Part of your personal registry.");
    b.Self(L"{LOCAL}\\Microsoft", P::Important, kUser, L"Microsoft app data", L"Settings of Windows and Microsoft apps.");
    b.Self(L"{LOCAL}\\Microsoft\\Windows", P::Important, kUser, L"Windows user data", L"Settings of Windows and Explorer.");
    b.Self(L"{LOCAL}\\Packages", P::Blocked, kUser, L"Store app data", L"Data of every Microsoft Store app.");
    b.Self(L"{LOCAL}\\Programs", P::Important, kProg, L"Per-user programs", L"Apps installed just for you. Uninstall them in Settings \u2192 Apps.");
    b.Tree(L"{ROAMING}\\Microsoft\\Protect", P::Blocked, L"Keys", L"Windows data protection keys",
           L"Without these keys Windows can't decrypt your saved passwords, Wi-Fi keys and certificates.");
    b.Tree(L"{ROAMING}\\Microsoft\\Credentials", P::Important, L"Keys", L"Windows Credential Manager", L"Saved sign-ins for apps and network shares.");
    b.Tree(L"{LOCAL}\\Microsoft\\Credentials", P::Important, L"Keys", L"Windows Credential Manager", L"Saved sign-ins for apps and network shares.");
    b.Tree(L"{ROAMING}\\Microsoft\\SystemCertificates", P::Important, L"Keys", L"Your certificates", L"Personal certificates and their keys.");
    b.Tree(L"{ROAMING}\\Microsoft\\Crypto", P::Important, L"Keys", L"Your encryption keys", L"Private keys of your certificates.");
    b.Tree(L"{LOCAL}\\Microsoft\\Vault", P::Important, L"Keys", L"Windows Vault", L"Saved web and app credentials.");

    // ---- Personal folders (the folder itself; files inside are yours to manage) ---------------
    const wchar_t* kPers = L"Personal";
    const wchar_t* kPersR = L"One of your personal library folders. Delete files inside it, not the folder itself.";
    struct KF {
        const KNOWNFOLDERID* id;
        const wchar_t* name;
        P level;
    };
    const KF kfs[] = {{&FOLDERID_Desktop, L"Desktop", P::Important},     {&FOLDERID_Documents, L"Documents", P::Important},
                      {&FOLDERID_Pictures, L"Pictures", P::Important},   {&FOLDERID_Videos, L"Videos", P::Important},
                      {&FOLDERID_Music, L"Music", P::Important},         {&FOLDERID_Downloads, L"Downloads", P::Caution},
                      {&FOLDERID_Favorites, L"Favorites", P::Caution},   {&FOLDERID_Contacts, L"Contacts", P::Important}};
    for (auto& kf : kfs) {
        std::wstring p = KnownFolder(*kf.id);
        if (!p.empty()) b.Self(p, kf.level, kPers, kf.name, kPersR);
    }
    std::wstring saved = KnownFolder(FOLDERID_SavedGames);
    if (!saved.empty())
        b.Tree(saved, P::Important, L"Games", L"Saved Games", L"Game progress. Many games don't sync saves to the cloud.");
    b.Tree(L"{HOME}\\Documents\\My Games", P::Important, L"Games", L"Game saves and settings", L"Game progress and configuration.");

    // ---- Cloud sync roots: deleting here deletes on every device -------------------------------
    const wchar_t* kCloud = L"Cloud";
    const wchar_t* kCloudR = L"Synced folder: deleting here also deletes it from the cloud and from your other devices.";
    for (const wchar_t* c : {L"%OneDrive%", L"%OneDriveConsumer%", L"%OneDriveCommercial%", L"{HOME}\\OneDrive*"})
        b.Path(c, P::Important, P::Caution, kCloud, L"OneDrive", kCloudR, P::Important, true);
    b.Path(L"{HOME}\\Dropbox*", P::Important, P::Caution, kCloud, L"Dropbox", kCloudR);
    b.Path(L"{HOME}\\Google Drive*", P::Important, P::Caution, kCloud, L"Google Drive", kCloudR);
    b.Path(L"{HOME}\\iCloudDrive", P::Important, P::Caution, kCloud, L"iCloud Drive", kCloudR);
    b.Path(L"{HOME}\\iCloud Photos", P::Important, P::Caution, kCloud, L"iCloud Photos", kCloudR);
    b.Path(L"{HOME}\\Box", P::Important, P::Caution, kCloud, L"Box", kCloudR);
    b.Path(L"{HOME}\\MEGA", P::Important, P::Caution, kCloud, L"MEGA", kCloudR);
    b.Path(L"{HOME}\\pCloud Drive", P::Important, P::Caution, kCloud, L"pCloud", kCloudR);
    b.Path(L"{HOME}\\Nextcloud", P::Important, P::Caution, kCloud, L"Nextcloud", kCloudR);
    b.Path(L"{HOME}\\Sync", P::Important, P::Caution, kCloud, L"Sync folder", kCloudR);

    // ---- AI assistants: sessions, memory, settings and tokens ---------------------------------
    const wchar_t* kAI = L"AI";
    const wchar_t* kAIR = L"AI assistant data: conversation history, sessions, project memory, settings and sign-in. "
                          L"It can't be downloaded again.";
    b.Tree(L"{HOME}\\.claude", P::Important, kAI, L"Claude Code (sessions, memory, settings)",
           L"Claude Code conversation history (projects\\*.jsonl), CLAUDE.md memory, agents, commands, plugins, settings "
           L"and sign-in.");
    b.Self(L"{HOME}\\.claude.json", P::Important, kAI, L"Claude Code configuration", L"Claude Code account, MCP servers and per-project state.");
    b.Tree(L"{ROAMING}\\Claude", P::Important, kAI, L"Claude desktop app", L"Claude desktop settings, MCP server config and local data.");
    for (const wchar_t* c : {L"Cache", L"Code Cache", L"GPUCache", L"DawnCache", L"DawnGraphiteCache", L"DawnWebGPUCache",
                             L"Crashpad", L"logs", L"Service Worker\\CacheStorage"})
        b.Allow(std::wstring(L"{ROAMING}\\Claude\\") + c);
    b.Tree(L"{LOCAL}\\AnthropicClaude", P::Caution, kAI, L"Claude desktop app (program)", L"The installed app. Uninstall it in Settings \u2192 Apps.");
    b.Tree(L"{LOCAL}\\Packages\\Claude_*", P::Important, kAI, L"Claude (Store app)", kAIR);
    b.Tree(L"{LOCAL}\\Packages\\AnthropicPBC*", P::Important, kAI, L"Claude (Store app)", kAIR);
    b.Tree(L"{HOME}\\.codex", P::Important, kAI, L"OpenAI Codex CLI (sessions, settings)",
           L"Codex sessions and history, AGENTS.md, config.toml and sign-in (auth.json).");
    b.Tree(L"{LOCAL}\\Packages\\OpenAI.ChatGPT*", P::Important, kAI, L"ChatGPT desktop app", kAIR);
    b.Tree(L"{ROAMING}\\ChatGPT", P::Important, kAI, L"ChatGPT desktop app", kAIR);
    b.Tree(L"{LOCAL}\\OpenAI", P::Important, kAI, L"OpenAI apps", kAIR);
    b.EditorFamily(L"Cursor", kAI, L"Cursor (chat & composer history, settings)",
                   L"Cursor chat and composer history (state.vscdb), workspace history and settings.");
    b.Tree(L"{HOME}\\.cursor", P::Important, kAI, L"Cursor (rules, MCP, extensions)", L"Global Cursor rules, mcp.json and extensions.");
    b.Tree(L"{LOCAL}\\Programs\\cursor", P::Caution, kAI, L"Cursor (program)", L"The installed app. Uninstall it in Settings \u2192 Apps.");
    b.EditorFamily(L"Windsurf", kAI, L"Windsurf (Cascade history, settings)", kAIR);
    b.EditorFamily(L"Windsurf - Next", kAI, L"Windsurf Next", kAIR);
    b.Tree(L"{HOME}\\.codeium", P::Important, kAI, L"Codeium / Windsurf (memories, rules)", L"Cascade memories, global rules and sign-in.");
    b.Tree(L"{HOME}\\.windsurf", P::Important, kAI, L"Windsurf", kAIR);
    b.EditorFamily(L"Code", L"AI", L"VS Code (Copilot Chat, Cline, Roo, settings)",
                   L"VS Code settings plus AI extension data: Copilot Chat sessions, Cline/Roo task history, Continue.");
    b.EditorFamily(L"Code - Insiders", L"AI", L"VS Code Insiders (AI chats, settings)", kAIR);
    b.EditorFamily(L"VSCodium", L"AI", L"VSCodium (AI chats, settings)", kAIR);
    b.EditorFamily(L"Kiro", kAI, L"Kiro (specs, chat history)", kAIR);
    b.EditorFamily(L"Trae", kAI, L"Trae (chat history, settings)", kAIR);
    b.EditorFamily(L"Void", kAI, L"Void editor (chats, settings)", kAIR);
    b.Tree(L"{HOME}\\.kiro", P::Important, kAI, L"Kiro (steering, settings)", kAIR);
    b.Tree(L"{HOME}\\.trae", P::Important, kAI, L"Trae", kAIR);
    b.Tree(L"{LOCAL}\\github-copilot", P::Important, kAI, L"GitHub Copilot (sign-in)", L"Copilot sign-in tokens for all editors.");
    b.Tree(L"{HOME}\\.copilot", P::Important, kAI, L"GitHub Copilot CLI (sessions)", kAIR);
    b.Tree(L"{HOME}\\.gemini", P::Important, kAI, L"Gemini CLI (chats, GEMINI.md, sign-in)", kAIR);
    b.Tree(L"{HOME}\\.continue", P::Important, kAI, L"Continue (sessions, config)", kAIR);
    b.Tree(L"{HOME}\\Documents\\Cline", P::Important, kAI, L"Cline (rules, workflows, MCP)", kAIR);
    b.Tree(L"{HOME}\\.aider", P::Caution, kAI, L"Aider", L"Aider caches and settings.");
    b.Tree(L"{HOME}\\.augment", P::Important, kAI, L"Augment", kAIR);
    b.Tree(L"{HOME}\\.qwen", P::Important, kAI, L"Qwen Code", kAIR);
    b.Tree(L"{HOME}\\.local\\share\\opencode", P::Important, kAI, L"opencode (sessions)", kAIR);
    b.Tree(L"{HOME}\\.config\\opencode", P::Important, kAI, L"opencode (config)", kAIR);
    b.Tree(L"{HOME}\\.local\\share\\amp", P::Important, kAI, L"Amp (threads)", kAIR);
    b.Tree(L"{HOME}\\.config\\amp", P::Important, kAI, L"Amp (config)", kAIR);
    b.Tree(L"{LOCAL}\\crush", P::Important, kAI, L"Crush (sessions)", kAIR);
    b.Tree(L"{ROAMING}\\Block\\goose", P::Important, kAI, L"Goose (sessions, config)", kAIR);
    b.Tree(L"{HOME}\\.config\\goose", P::Important, kAI, L"Goose (config)", kAIR);
    b.Tree(L"{ROAMING}\\Zed", P::Important, kAI, L"Zed (settings)", kAIR);
    b.Tree(L"{LOCAL}\\Zed", P::Important, kAI, L"Zed (AI threads, history)", kAIR);
    b.Tree(L"{ROAMING}\\Jan", P::Important, kAI, L"Jan (chats, models)", kAIR);
    b.Tree(L"{ROAMING}\\Msty", P::Important, kAI, L"Msty (chats)", kAIR);
    b.Tree(L"{ROAMING}\\LM Studio", P::Important, kAI, L"LM Studio (chats, settings)", kAIR);
    b.Tree(L"{ROAMING}\\Perplexity", P::Important, kAI, L"Perplexity desktop app", kAIR);
    b.Tree(L"{HOME}\\.specstory", P::Important, kAI, L"SpecStory (saved AI chats)", kAIR);
    // Model stores: re-downloadable, but often tens of gigabytes.
    const wchar_t* kModels = L"AI models";
    const wchar_t* kModelsR = L"Local AI models. They can be downloaded again, but that can take a long time.";
    b.Tree(L"{HOME}\\.ollama", P::Caution, kModels, L"Ollama (models, identity key)",
           L"Ollama models (re-downloadable) and its identity key (id_ed25519). Use `ollama rm <model>` to remove one model.");
    b.Tree(L"{HOME}\\.lmstudio", P::Caution, kModels, L"LM Studio (models, chats)",
           L"LM Studio models and conversations. Delete single models from inside LM Studio.");
    b.Tree(L"{HOME}\\.cache\\lm-studio", P::Caution, kModels, L"LM Studio (models)", kModelsR);
    b.Tree(L"{HOME}\\.cache\\huggingface", P::Caution, kModels, L"Hugging Face cache", kModelsR);
    b.Tree(L"{LOCAL}\\nomic.ai", P::Caution, kModels, L"GPT4All", kModelsR);

    // ---- Keys and credentials ----------------------------------------------------------------
    const wchar_t* kKeys = L"Keys";
    b.Tree(L"{HOME}\\.ssh", P::Important, kKeys, L"SSH keys", L"Private keys and known hosts. Lost keys lock you out of servers and Git hosts.");
    b.Tree(L"{HOME}\\.gnupg", P::Important, kKeys, L"GnuPG keys", L"Private PGP keys used for signing and decryption.");
    b.Tree(L"{ROAMING}\\gnupg", P::Important, kKeys, L"GnuPG keys", L"Private PGP keys used for signing and decryption.");
    b.Tree(L"{HOME}\\.aws", P::Important, kKeys, L"AWS credentials", L"AWS access keys, profiles and SSO config.");
    b.Tree(L"{HOME}\\.azure", P::Important, kKeys, L"Azure CLI sign-in", L"Azure CLI profiles and tokens.");
    b.Tree(L"{HOME}\\.kube", P::Important, kKeys, L"Kubernetes config", L"kubeconfig with cluster endpoints and credentials.");
    b.Tree(L"{ROAMING}\\gcloud", P::Important, kKeys, L"Google Cloud SDK", L"gcloud credentials and configurations.");
    b.Tree(L"{HOME}\\.config\\gcloud", P::Important, kKeys, L"Google Cloud SDK", L"gcloud credentials and configurations.");
    b.Self(L"{HOME}\\.git-credentials", P::Important, kKeys, L"Git credentials", L"Stored Git passwords and tokens.");
    b.Tree(L"{ROAMING}\\GitHub CLI", P::Important, kKeys, L"GitHub CLI", L"gh sign-in and settings.");
    b.Self(L"{HOME}\\.gitconfig", P::Caution, L"Dev", L"Git configuration", L"Your Git name, email, aliases and signing setup.");
    b.Self(L"{HOME}\\.npmrc", P::Caution, kKeys, L"npm configuration", L"May contain registry tokens.");
    b.Self(L"{HOME}\\.pypirc", P::Caution, kKeys, L"PyPI configuration", L"May contain upload tokens.");
    b.Tree(L"{HOME}\\.docker", P::Caution, L"Dev", L"Docker CLI config", L"Registry sign-ins and contexts.");
    b.Tree(L"{HOME}\\.terraform.d", P::Caution, kKeys, L"Terraform", L"Terraform Cloud tokens and plugins.");
    b.Tree(L"{ROAMING}\\KeePass", P::Caution, kKeys, L"KeePass settings", L"KeePass configuration (your .kdbx database may be elsewhere).");
    b.Tree(L"{ROAMING}\\Bitwarden", P::Caution, kKeys, L"Bitwarden", L"Bitwarden local vault cache and settings.");
    b.Tree(L"{LOCAL}\\1Password", P::Caution, kKeys, L"1Password", L"1Password local data.");

    // ---- Crypto wallets -----------------------------------------------------------------------
    const wchar_t* kWal = L"Wallet";
    const wchar_t* kWalR = L"Cryptocurrency wallet. Without its keys or seed phrase the funds are lost for good.";
    for (const wchar_t* w : {L"Bitcoin", L"Ethereum", L"Electrum", L"Exodus", L"atomic", L"Litecoin", L"Dogecoin",
                             L"Zcash", L"Ledger Live", L"Guarda", L"Wasabi"})
        b.Tree(std::wstring(L"{ROAMING}\\") + w, P::Important, kWal, w, kWalR);
    b.Tree(L"{HOME}\\Documents\\Monero", P::Important, kWal, L"Monero", kWalR);

    // ---- Browsers, mail and messengers -----------------------------------------------------
    b.Chromium(L"{LOCAL}\\Google\\Chrome\\User Data", L"Google Chrome profile");
    b.Chromium(L"{LOCAL}\\Google\\Chrome Beta\\User Data", L"Chrome Beta profile");
    b.Chromium(L"{LOCAL}\\Microsoft\\Edge\\User Data", L"Microsoft Edge profile");
    b.Chromium(L"{LOCAL}\\BraveSoftware\\Brave-Browser\\User Data", L"Brave profile");
    b.Chromium(L"{LOCAL}\\Vivaldi\\User Data", L"Vivaldi profile");
    b.Chromium(L"{LOCAL}\\Chromium\\User Data", L"Chromium profile");
    b.Tree(L"{ROAMING}\\Opera Software", P::Important, L"Browser", L"Opera profile", L"Saved passwords, cookies, bookmarks and history.");
    b.Tree(L"{ROAMING}\\Mozilla\\Firefox", P::Important, L"Browser", L"Firefox profile", L"Saved passwords, cookies, bookmarks and history.");
    b.Tree(L"{ROAMING}\\Thunderbird", P::Important, L"Email", L"Thunderbird mail", L"Your local mail, accounts and address book.");
    b.Tree(L"{LOCAL}\\Microsoft\\Outlook", P::Important, L"Email", L"Outlook data files", L"Outlook mailbox data (.pst/.ost).");
    b.Tree(L"{HOME}\\Documents\\Outlook Files", P::Important, L"Email", L"Outlook data files", L"Outlook archives (.pst) may exist only here.");
    b.Tree(L"{ROAMING}\\Signal", P::Important, L"Messages", L"Signal Desktop", L"Message history that can't be restored from the server.");
    b.Tree(L"{ROAMING}\\Telegram Desktop", P::Caution, L"Messages", L"Telegram Desktop", L"Your Telegram session; you'll need to sign in again.");
    b.Tree(L"{LOCAL}\\Packages\\5319275A.WhatsAppDesktop_*", P::Important, L"Messages", L"WhatsApp Desktop", L"Local chat history and media.");

    // ---- Virtual machines, WSL, containers -------------------------------------------------
    const wchar_t* kVM = L"VM";
    b.Tree(L"{LOCAL}\\Packages\\CanonicalGroupLimited.*", P::Important, kVM, L"WSL Linux distribution",
           L"A whole Linux system (ext4.vhdx). Remove it with: wsl --unregister <name>");
    b.Tree(L"{LOCAL}\\Packages\\TheDebianProject.*", P::Important, kVM, L"WSL Linux distribution",
           L"A whole Linux system (ext4.vhdx). Remove it with: wsl --unregister <name>");
    b.Tree(L"{LOCAL}\\wsl", P::Important, kVM, L"WSL Linux distributions", L"Linux systems installed with WSL. Remove with: wsl --unregister <name>");
    b.Tree(L"{LOCAL}\\Docker\\wsl", P::Important, kVM, L"Docker Desktop data",
           L"Images, containers and volumes. Free space with: docker system prune");
    b.Tree(L"{HOME}\\VirtualBox VMs", P::Important, kVM, L"VirtualBox VMs", L"Virtual machines and their disks.");
    b.Tree(L"%PUBLIC%\\Documents\\Hyper-V", P::Important, kVM, L"Hyper-V disks", L"Virtual machine disks. Remove VMs in Hyper-V Manager.");
    b.Tree(L"%ProgramData%\\Microsoft\\Windows\\Hyper-V", P::Important, kVM, L"Hyper-V", L"Virtual machine configuration.");

    // ---- Anywhere: by name --------------------------------------------------------------------
    const wchar_t* kVcs = L"Version control database: history, branches, stashes and unpushed commits.";
    b.Name(L".git", P::Important, P::Important, P::Caution, L"Git", L"Git repository", kVcs);
    b.Name(L".svn", P::Important, P::Important, P::Caution, L"Git", L"Subversion working copy", kVcs);
    b.Name(L".hg", P::Important, P::Important, P::Caution, L"Git", L"Mercurial repository", kVcs);
    b.Name(L"*.kdbx", P::Important, P::None, P::Important, kKeys, L"KeePass database", L"Password database.", true);
    b.Name(L"*.kdb", P::Important, P::None, P::Important, kKeys, L"KeePass database", L"Password database.", true);
    b.Name(L"*.1pux", P::Important, P::None, P::Important, kKeys, L"1Password export", L"Password export.", true);
    b.Name(L"wallet.dat", P::Important, P::None, P::Important, kWal, L"Crypto wallet file", kWalR, true);
    b.Name(L"*.wallet", P::Important, P::None, P::Important, kWal, L"Crypto wallet file", kWalR, true);
    b.Name(L"*.pst", P::Important, P::None, P::Important, L"Email", L"Outlook archive (.pst)", L"Mail archive that often exists nowhere else.", true);
    for (const wchar_t* k : {L"*.pfx", L"*.p12", L"*.ppk", L"id_rsa", L"id_ed25519", L"id_ecdsa", L"id_dsa"})
        b.Name(k, P::Important, P::None, P::Important, kKeys, L"Private key", L"A private key or certificate; it can't be recreated.", true);
    b.Name(L"*.pem", P::Caution, P::None, P::Caution, kKeys, L"Key or certificate (.pem)", L"May be a private key.", true);
    b.Name(L".env", P::Caution, P::None, P::None, kKeys, L"Environment secrets (.env)", L"Usually holds API keys and passwords.", true);
    for (const wchar_t* d : {L"*.vhdx", L"*.vhd", L"*.vmdk", L"*.vdi", L"*.qcow2"})
        b.Name(d, P::Caution, P::None, P::Caution, kVM, L"Virtual disk", L"A whole virtual machine or WSL disk.", true);
    b.Name(L".claude", P::Caution, P::Caution, P::None, kAI, L"Claude Code project settings", L"Project agents, commands, hooks and settings.");
    b.Name(L".cursor", P::Caution, P::Caution, P::None, kAI, L"Cursor project rules", L"Project rules and MCP config.");
    b.Name(L".specstory", P::Important, P::Important, P::Caution, kAI, L"SpecStory chat history", L"Saved AI conversations for this project.");
    b.Name(L".kiro", P::Caution, P::Caution, P::None, kAI, L"Kiro specs", L"Project specs, steering and hooks.");
    b.Name(L"*.aider.chat.history.md", P::Caution, P::None, P::None, kAI, L"Aider chat history", L"Aider conversation log.", true);
}

// ---------------------------------------------------------------------------------------------
// User rules: %LOCALAPPDATA%\WinNCDU\protect.txt

const char kTemplate[] =
    "# WinNCDU protected locations\r\n"
    "#\r\n"
    "# One rule per line:   <level>  <path or name>  [; reason]\r\n"
    "#   level:  block      never delete\r\n"
    "#           important  ask twice, Recycle Bin preselected\r\n"
    "#           caution    show a warning\r\n"
    "#           allow      remove the protection of a built-in rule (more specific path wins)\r\n"
    "#   path:   a folder or file; the rule covers everything inside it. %ENV% variables,\r\n"
    "#           {HOME}, {ROAMING}, {LOCAL} and '*' segments work, e.g. {HOME}\\Projects\\*\\data\r\n"
    "#   name:   no backslash = any file or folder with that name, e.g. *.psd or secrets\r\n"
    "#\r\n"
    "# Examples:\r\n"
    "# block      D:\\Backups              ; nightly backups\r\n"
    "# important  {HOME}\\Work             ; client projects\r\n"
    "# caution    *.blend\r\n"
    "# allow      {HOME}\\.cache\\huggingface\r\n";

void UserRules(std::vector<Rule>& v) {
    std::wstring file = ProtectRulesFile();
    if (file.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") != 0 || !f) {
        // First run: write a commented template so people can find and extend it.
        size_t slash = file.rfind(L'\\');
        if (slash != std::wstring::npos) CreateDirectoryW(file.substr(0, slash).c_str(), nullptr);
        if (_wfopen_s(&f, file.c_str(), L"wb") == 0 && f) {
            fwrite(kTemplate, 1, sizeof(kTemplate) - 1, f);
            fclose(f);
        }
        return;
    }
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF) data.erase(0, 3);  // UTF-8 BOM
    std::wstring text = util::Utf8ToWide(data);
    Builder b(v);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        size_t b0 = line.find_first_not_of(L" \t");
        if (b0 == std::wstring::npos || line[b0] == L'#') continue;
        line = line.substr(b0);
        size_t sp = line.find_first_of(L" \t");
        if (sp == std::wstring::npos) continue;
        std::wstring level = util::ToLower(line.substr(0, sp));
        std::wstring rest = line.substr(line.find_first_not_of(L" \t", sp));
        std::wstring reason = L"Protected by your rule in protect.txt.";
        size_t semi = rest.find(L';');
        if (semi != std::wstring::npos) {
            std::wstring r = rest.substr(semi + 1);
            size_t rb = r.find_first_not_of(L" \t");
            if (rb != std::wstring::npos) reason = r.substr(rb);
            rest = rest.substr(0, semi);
        }
        while (!rest.empty() && (rest.back() == L' ' || rest.back() == L'\t')) rest.pop_back();
        if (rest.size() >= 2 && rest.front() == L'"' && rest.back() == L'"') rest = rest.substr(1, rest.size() - 2);
        if (rest.empty()) continue;
        P p;
        if (level == L"block" || level == L"blocked")
            p = P::Blocked;
        else if (level == L"important")
            p = P::Important;
        else if (level == L"caution" || level == L"warn")
            p = P::Caution;
        else if (level == L"allow")
            p = P::None;
        else
            continue;
        // Rule strings must outlive the builder call; keep them in the rule itself.
        bool isPath = rest.find(L'\\') != std::wstring::npos || rest.find(L':') != std::wstring::npos ||
                      rest.find(L'{') != std::wstring::npos || rest.find(L'%') != std::wstring::npos;
        size_t before = v.size();
        if (level == L"allow") {
            if (isPath) b.Allow(rest);
        } else if (isPath) {
            b.Tree(rest, p, L"Yours", L"", L"");
        } else {
            b.Name(rest, p, p, p, L"Yours", L"", L"");
        }
        for (size_t i = before; i < v.size(); ++i) {
            v[i].name = rest;
            if (!v[i].allow) v[i].reason = reason;
            v[i].wild = std::max(0, v[i].wild - 1);  // user rules win ties against built-ins
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Resolving rules against the disk (so deleting a parent folder warns about what's inside)

bool IsDriveSeg(const std::wstring& s) { return s.size() == 2 && s[1] == L':'; }

void ResolveRule(const Rule& r, size_t ruleIndex, std::vector<Resolved>& out) {
    // Breadth-first over segments; wildcard segments enumerate the directory.
    std::vector<std::wstring> cur;
    const std::wstring& first = r.segs[0];
    if (first == L"*:" || first == L"?:") {
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i))) continue;
            std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
            UINT t = GetDriveTypeW(root.c_str());
            if (t == DRIVE_FIXED || t == DRIVE_REMOVABLE) cur.push_back(root.substr(0, 2));
        }
    } else if (IsDriveSeg(first) || first.rfind(L"\\\\", 0) == 0) {
        cur.push_back(r.disp[0]);
    } else {
        return;
    }
    for (size_t i = 1; i < r.segs.size() && !cur.empty(); ++i) {
        std::vector<std::wstring> next;
        const std::wstring& seg = r.segs[i];
        for (auto& base : cur) {
            if (!util::HasWildcards(seg)) {
                next.push_back(base + L"\\" + r.disp[i]);
                continue;
            }
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW((ToLongPath(base + L"\\") + seg).c_str(), FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, nullptr, 0);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // "All Users", "Default User"
                bool last = i + 1 == r.segs.size();
                if (!last && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                next.push_back(base + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        cur.swap(next);
    }
    for (auto& p : cur) {
        std::wstring full = (p.size() == 2 && p[1] == L':') ? p + L"\\" : p;
        DWORD a = GetFileAttributesW(ToLongPath(full).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        Resolved rv;
        rv.path = full;
        rv.lower = util::ToLower(full);
        rv.rule = ruleIndex;
        out.push_back(std::move(rv));
    }
}

void BuildLocked(State& s) {
    s.rules.clear();
    s.resolved.clear();
    BuiltinRules(s.rules);
    UserRules(s.rules);
    for (size_t i = 0; i < s.rules.size(); ++i) {
        const Rule& r = s.rules[i];
        if (r.nameRule || r.allow || !r.resolve || r.contains == P::None) continue;
        ResolveRule(r, i, s.resolved);
    }
    // One entry per path (the same folder can come from the {HOME} and C:\Users\* variants).
    std::sort(s.resolved.begin(), s.resolved.end(), [](const Resolved& a, const Resolved& b) {
        return a.lower != b.lower ? a.lower < b.lower : a.rule < b.rule;
    });
    s.resolved.erase(std::unique(s.resolved.begin(), s.resolved.end(),
                                 [](const Resolved& a, const Resolved& b) { return a.lower == b.lower; }),
                     s.resolved.end());
    s.built = true;
    ++s.gen;
}

// ---------------------------------------------------------------------------------------------
// Checking

struct Collector {
    ProtectReport rep;
    std::map<std::wstring, size_t> index;  // group|name|rel -> hit

    void Add(const Rule& r, P level, ProtectRel rel, const std::wstring& path) {
        if (level == P::None) return;
        std::wstring key = r.group + L"|" + r.name + L"|" + std::to_wstring(static_cast<int>(rel));
        auto it = index.find(key);
        if (it != index.end()) {
            ProtectHit& h = rep.hits[it->second];
            ++h.count;
            if (level > h.level) h.level = level;
            return;
        }
        ProtectHit h;
        h.level = level;
        h.rel = rel;
        h.group = r.group;
        h.name = r.name;
        h.reason = r.reason;
        h.path = path;
        index[key] = rep.hits.size();
        rep.hits.push_back(std::move(h));
        if (level > rep.level) rep.level = level;
    }

    void Finish() {
        std::stable_sort(rep.hits.begin(), rep.hits.end(), [](const ProtectHit& a, const ProtectHit& b) {
            if (a.level != b.level) return a.level > b.level;
            return static_cast<int>(a.rel) < static_cast<int>(b.rel);
        });
    }
};

void WalkNode(const State& s, const Node* root, Collector& c) {
    std::vector<size_t> nameRules;
    for (size_t i = 0; i < s.rules.size(); ++i)
        if (s.rules[i].nameRule && s.rules[i].contains != P::None) nameRules.push_back(i);
    if (nameRules.empty()) return;
    const size_t kMaxVisit = 3000000;
    size_t visited = 0;
    std::vector<const Node*> stack;
    for (auto& ch : root->children) stack.push_back(ch.get());
    while (!stack.empty()) {
        if (++visited > kMaxVisit) {
            c.rep.truncated = true;
            return;
        }
        const Node* n = stack.back();
        stack.pop_back();
        bool matched = false;
        for (size_t ri : nameRules) {
            const Rule& r = s.rules[ri];
            if (r.fileOnly && n->IsDir()) continue;
            if (NameMatch(n->name, r.pat)) {
                c.Add(r, r.contains, ProtectRel::Contains, NodePath(n));
                matched = true;
            }
        }
        if (matched || !n->IsDir() || n->Has(NF_LINK)) continue;  // don't look inside a .git folder
        for (auto& ch : n->children) stack.push_back(ch.get());
    }
}

ProtectReport CheckLocked(const State& s, const std::wstring& path, const Node* node, bool walk = true) {
    Collector c;
    std::vector<std::wstring> raw = SplitRaw(path);
    std::vector<std::wstring> t = LowerSegs(raw);
    if (t.empty()) return c.rep;
    int isDir = node ? (node->IsDir() ? 1 : 0) : -1;  // -1: ask the file system when needed
    auto targetIsDir = [&] {
        if (isDir < 0) {
            DWORD a = GetFileAttributesW(ToLongPath(path).c_str());
            isDir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        }
        return isDir == 1;
    };

    // Whole drives (and network share roots) are never deleted.
    if (t.size() == 1 || (path.rfind(L"\\\\", 0) == 0 && t.size() <= 2)) {
        Rule r;
        r.group = L"System";
        r.name = L"Drive root";
        r.reason = L"A whole drive. Delete the folders you don't need instead.";
        c.Add(r, P::Blocked, ProtectRel::Self, path);
    }

    // Path rules: the most specific matching rule decides for the item itself.
    const Rule* best = nullptr;
    ProtectRel bestRel = ProtectRel::Self;
    for (const Rule& r : s.rules) {
        if (r.nameRule || r.segs.size() > t.size()) continue;
        bool ok = true;
        for (size_t i = 0; i < r.segs.size() && ok; ++i) ok = SegMatch(r.segs[i], t[i]);
        if (!ok) continue;
        ProtectRel rel = r.segs.size() == t.size() ? ProtectRel::Self : ProtectRel::Inside;
        P lvl = rel == ProtectRel::Self ? r.self : r.inside;
        if (!r.allow && lvl == P::None) continue;  // e.g. a "Self" rule doesn't cover what's inside
        bool better = !best;
        if (best) {
            if (r.segs.size() != best->segs.size())
                better = r.segs.size() > best->segs.size();
            else if (r.wild != best->wild)
                better = r.wild < best->wild;
            else if (r.allow != best->allow)
                better = r.allow;
            else
                better = lvl > (bestRel == ProtectRel::Self ? best->self : best->inside);
        }
        if (better) {
            best = &r;
            bestRel = rel;
        }
    }
    P pathLevel = P::None;
    if (best && !best->allow) {
        P lvl = bestRel == ProtectRel::Self ? best->self : best->inside;
        pathLevel = lvl;
        std::wstring at = bestRel == ProtectRel::Self ? path : Join(raw, best->segs.size());
        c.Add(*best, lvl, bestRel, at);
    } else if (best && best->allow && best->self != P::None) {
        Rule r = *best;
        r.group = L"System";
        r.name = Join(raw, best->segs.size());
        c.Add(r, best->self, ProtectRel::Self, path);
    }

    // Name rules on the item and its parents (e.g. something inside a .git folder).
    for (const Rule& r : s.rules) {
        if (!r.nameRule) continue;
        for (size_t i = 0; i < t.size(); ++i) {
            if (!NameMatch(raw[i], r.pat)) continue;
            bool self = i + 1 == t.size();
            if (r.fileOnly && (!self || targetIsDir())) continue;
            P lvl = self ? r.self : r.inside;
            if (lvl != P::None && lvl > pathLevel) c.Add(r, lvl, self ? ProtectRel::Self : ProtectRel::Inside, Join(raw, i + 1));
            break;
        }
    }

    // Protected locations below the item (deleting C:\Users\me deletes .claude and .ssh too).
    std::wstring prefix = Join(t, t.size());
    if (!prefix.empty() && prefix.back() != L'\\') prefix += L'\\';
    auto lo = std::lower_bound(s.resolved.begin(), s.resolved.end(), prefix,
                               [](const Resolved& a, const std::wstring& p) { return a.lower < p; });
    for (auto it = lo; it != s.resolved.end() && it->lower.compare(0, prefix.size(), prefix) == 0; ++it) {
        const Rule& r = s.rules[it->rule];
        c.Add(r, r.contains, ProtectRel::Contains, it->path);
    }

    if (walk && node && node->IsDir() && !node->Has(NF_LINK)) WalkNode(s, node, c);
    c.Finish();
    return c.rep;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Shield badges

void ProtectBadges::Clear() {
    hits_.clear();
    below_.clear();
}

// The most severe name rule that makes a folder containing `n` protected, or -1.
int ProtectBadges::MatchName(const Node* n) const {
    const State& s = S();
    int best = -1;
    for (size_t ri : nameRules_) {
        const Rule& r = s.rules[ri];
        if (r.fileOnly && n->IsDir()) continue;
        if (NameMatch(n->name, r.pat) && (best < 0 || r.contains > s.rules[best].contains)) best = static_cast<int>(ri);
    }
    return best;
}

// Called with the state lock held. Same matches as WalkNode, but memoized per folder and
// without recursion (scan trees can be thousands of levels deep).
const ProtectBadges::Below& ProtectBadges::Summarize(const Node* dir) {
    auto found = below_.find(dir);
    if (found != below_.end()) return found->second;
    const State& s = S();
    auto descend = [&](const Node* c) { return c->IsDir() && !c->Has(NF_LINK) && MatchName(c) < 0 && !below_.count(c); };
    std::vector<std::pair<const Node*, size_t>> stack{{dir, 0}};
    while (!stack.empty()) {
        const Node* d = stack.back().first;
        size_t& next = stack.back().second;
        if (next < d->children.size()) {
            const Node* c = d->children[next++].get();
            if (descend(c)) stack.push_back({c, 0});  // invalidates `next`; not used again this round
            continue;
        }
        Below b;
        auto add = [&b](Protect level, size_t rule, bool mixed, uint64_t count, const Node* first) {
            if (level == Protect::None || level < b.level) return;
            if (level > b.level) {
                b = {level, rule, mixed, count, first};
                return;
            }
            b.count += count;
            b.mixed = b.mixed || mixed || rule != b.rule;
        };
        for (auto& ch : d->children) {
            const Node* c = ch.get();
            int m = MatchName(c);
            if (m >= 0) {
                add(s.rules[m].contains, static_cast<size_t>(m), false, 1, c);  // a match counts itself, not its contents
            } else if (c->IsDir() && !c->Has(NF_LINK)) {
                const Below& cb = below_[c];
                add(cb.level, cb.rule, cb.mixed, cb.count, cb.first);
            }
        }
        below_[d] = b;
        stack.pop_back();
    }
    return below_[dir];
}

const ProtectHit& ProtectBadges::For(const Node* n) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    if (!s.built) BuildLocked(s);
    if (gen_ != s.gen) {
        Clear();
        nameRules_.clear();
        for (size_t i = 0; i < s.rules.size(); ++i)
            if (s.rules[i].nameRule && s.rules[i].contains != P::None) nameRules_.push_back(i);
        gen_ = s.gen;
    }
    auto found = hits_.find(n);
    if (found != hits_.end()) return found->second;

    ProtectReport rep = CheckLocked(s, NodePath(n), n, false);
    ProtectHit hit;
    if (rep.Any()) hit = rep.hits.front();
    if (n->IsDir() && !n->Has(NF_LINK)) {
        const Below& b = Summarize(n);
        if (b.level > hit.level) {
            const Rule& r = s.rules[b.rule];
            hit = ProtectHit{};
            hit.level = b.level;
            hit.rel = ProtectRel::Contains;
            hit.group = r.group;
            hit.name = b.mixed ? r.name + L" and other protected items" : r.name;
            hit.reason = r.reason;
            hit.path = NodePath(b.first);
            hit.count = b.count;
        }
    }
    return hits_.emplace(n, std::move(hit)).first->second;
}

namespace {
bool SameSource(const ProtectHit& a, const ProtectHit& b) { return a.level == b.level && a.path == b.path && a.name == b.name; }
}  // namespace

const ProtectHit* ProtectBadges::InList(const Node* n) {
    const ProtectHit& h = For(n);
    if (h.level == P::None) return nullptr;
    if (h.rel == ProtectRel::Inside && n->parent) {
        const ProtectHit& p = For(n->parent);
        if (p.rel != ProtectRel::Contains && SameSource(p, h)) return nullptr;
    }
    return &h;
}

const ProtectHit* ProtectBadges::ForContents(const Node* dir) {
    const ProtectHit& d = For(dir);
    if (d.level == P::None || d.rel == ProtectRel::Contains) return nullptr;
    if (d.rel == ProtectRel::Inside) return &d;
    // Protected itself: only worth a notice if the protection reaches the items inside (it doesn't
    // for a drive root or a user profile, whose contents have rules of their own).
    size_t checked = 0;
    for (auto& c : dir->children) {
        if (++checked > 8) break;
        const ProtectHit& h = For(c.get());
        if (h.rel == ProtectRel::Inside && SameSource(h, d)) return &d;
    }
    return nullptr;
}

std::wstring ProtectRulesFile() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) out = std::wstring(p) + L"\\WinNCDU\\protect.txt";
    CoTaskMemFree(p);
    return out;
}

void RefreshProtection() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    BuildLocked(s);
}

ProtectReport CheckProtection(const std::wstring& path, const Node* node) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    if (!s.built) BuildLocked(s);
    return CheckLocked(s, path, node);
}

std::vector<ProtectHit> ProtectedLocationsOnThisPC() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    if (!s.built) BuildLocked(s);
    std::vector<ProtectHit> out;
    for (auto& rv : s.resolved) {
        const Rule& r = s.rules[rv.rule];
        ProtectHit h;
        h.level = std::max(r.self, r.inside);
        h.group = r.group;
        h.name = r.name;
        h.reason = r.reason;
        h.path = rv.path;
        out.push_back(std::move(h));
    }
    std::stable_sort(out.begin(), out.end(), [](const ProtectHit& a, const ProtectHit& b) {
        if (a.group != b.group) return a.group < b.group;
        return a.path < b.path;
    });
    return out;
}

const wchar_t* ProtectLevelName(Protect p) {
    switch (p) {
        case Protect::Blocked: return L"Blocked";
        case Protect::Important: return L"Important";
        case Protect::Caution: return L"Caution";
        default: return L"";
    }
}

std::wstring DescribeHit(const ProtectHit& h) {
    std::wstring s = h.group.empty() ? h.name : h.group + L" \u00B7 " + h.name;
    switch (h.rel) {
        case ProtectRel::Self: break;
        case ProtectRel::Inside: s += L" \u00B7 inside " + h.path; break;
        case ProtectRel::Contains:
            s += h.count > 1 ? L" \u00B7 " + std::to_wstring(h.count) + L" inside, e.g. " + h.path : L" \u00B7 contains " + h.path;
            break;
    }
    return s;
}
