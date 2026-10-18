#include "apps.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <map>
#include <set>

#include "tree.h"
#include "util.h"

namespace {

const wchar_t* kUninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

std::wstring Lower(std::wstring_view s) { return util::ToLower(s); }

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? std::wstring(buf, n) : L"";
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

DWORD Attrs(const std::wstring& p) { return GetFileAttributesW(ToLongPath(p).c_str()); }

bool IsDir(const std::wstring& p) {
    DWORD a = Attrs(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Trims spaces and quotes, uses '\', drops a trailing '\' (except after "X:").
std::wstring CleanPath(std::wstring p) {
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    auto junk = [](wchar_t c) { return c == L' ' || c == L'"' || c == L'\t'; };
    while (!p.empty() && junk(p.back())) p.pop_back();
    size_t b = 0;
    while (b < p.size() && junk(p[b])) ++b;
    p.erase(0, b);
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

bool IsAbsolute(const std::wstring& p) { return p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\'; }
bool IsDriveRoot(const std::wstring& p) { return p.size() <= 3 && p.size() >= 2 && p[1] == L':'; }

// `child` is `parent` or inside it (case-insensitive).
bool Within(const std::wstring& child, const std::wstring& parent) {
    if (parent.empty() || child.size() < parent.size()) return false;
    if (_wcsnicmp(child.c_str(), parent.c_str(), parent.size()) != 0) return false;
    return child.size() == parent.size() || parent.back() == L'\\' || child[parent.size()] == L'\\';
}

std::wstring Leaf(const std::wstring& p) {
    size_t s = p.find_last_of(L'\\');
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

std::wstring Parent(const std::wstring& p) {
    size_t s = p.find_last_of(L'\\');
    if (s == std::wstring::npos) return L"";
    return s == 2 ? p.substr(0, 3) : p.substr(0, s);
}

// Lowercase, trimmed, single spaces.
std::wstring Norm(std::wstring_view s) {
    std::wstring out;
    bool space = false;
    for (wchar_t c : s) {
        if (iswspace(c)) {
            space = !out.empty();
            continue;
        }
        if (space) out += L' ';
        space = false;
        out += util::LowerChar(c);
    }
    return out;
}

std::vector<std::wstring> Words(const std::wstring& s) {
    std::vector<std::wstring> w;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == L' ') ++i;
        size_t j = s.find(L' ', i);
        if (j == std::wstring::npos) j = s.size();
        if (j > i) w.push_back(s.substr(i, j - i));
        i = j;
    }
    return w;
}

std::wstring Join(const std::vector<std::wstring>& w, size_t n) {
    std::wstring s;
    for (size_t i = 0; i < n && i < w.size(); ++i) s += (i ? L" " : L"") + w[i];
    return s;
}

// A word that only says which build it is: a version, an architecture, a language.
bool NoiseWord(std::wstring w) {
    while (!w.empty() && (w.back() == L',' || w.back() == L')' || w.back() == L';')) w.pop_back();
    while (!w.empty() && w[0] == L'(') w.erase(0, 1);
    if (w.empty() || w == L"-" || w == L"\u2013") return true;
    if (iswdigit(w[0]) || (w.size() > 1 && w[0] == L'v' && iswdigit(w[1]))) return true;
    static const wchar_t* words[] = {L"x64", L"x86", L"amd64", L"arm64", L"win64", L"win32", L"64-bit", L"32-bit", L"version", L"build"};
    for (auto* x : words)
        if (w == x) return true;
    // "en-us", "de-de"
    return w.size() == 5 && w[2] == L'-' && iswalpha(w[0]) && iswalpha(w[1]) && iswalpha(w[3]) && iswalpha(w[4]);
}

// "7-zip 23.01 (x64)" -> "7-zip"; "mozilla thunderbird (x64 en-us)" -> "mozilla thunderbird".
std::wstring StripVersion(const std::wstring& name) {
    std::vector<std::wstring> w = Words(name);
    size_t n = w.size();
    while (n > 1) {
        // A trailing "( ... )" group made only of noise words.
        if (!w[n - 1].empty() && w[n - 1].back() == L')') {
            size_t open = n;
            for (size_t i = n; i-- > 0;)
                if (w[i].find(L'(') != std::wstring::npos) {
                    open = i;
                    break;
                }
            if (open < n && open >= 1 && w[open][0] == L'(') {
                bool noise = true;
                for (size_t i = open; i < n && noise; ++i) noise = NoiseWord(w[i]);
                if (noise) {
                    n = open;
                    continue;
                }
            }
        }
        if (NoiseWord(w[n - 1])) {
            --n;
            continue;
        }
        break;
    }
    return Join(w, n);
}

// Names too general to stand for one app (folders like these hold many apps' data).
bool Generic(const std::wstring& n) {
    static const std::set<std::wstring> g = {
        L"app", L"apps", L"application", L"applications", L"bin", L"program", L"programs", L"program files",
        L"program files (x86)", L"data", L"editor", L"client", L"common", L"common files", L"shared", L"files", L"installer",
        L"install", L"installation", L"installations", L"setup", L"uninstall", L"uninstaller", L"update", L"updater",
        L"updates", L"service", L"services", L"tools", L"tool", L"default", L"current", L"latest", L"temp", L"tmp", L"cache",
        L"caches", L"log", L"logs", L"microsoft", L"windows", L"packages", L"package cache", L"users", L"user", L"local",
        L"roaming", L"locallow", L"programdata", L"appdata", L"config", L"settings", L"plugins", L"driver", L"drivers",
        L"runtime", L"runtimes", L"sdk", L"lib", L"system", L"system32", L"documents", L"downloads", L"desktop", L"games",
        L"software", L"launcher", L"google", L"intel", L"nvidia", L"amd", L"adobe", L"apple", L"mozilla", L"asus", L"realtek",
        L"oracle", L"steam", L"steamapps", L"common7", L"x64", L"x86", L"crashpad", L"crashdumps", L"backup", L"backups"};
    if (n.size() < 3 || g.count(n)) return true;
    return std::none_of(n.begin(), n.end(), [](wchar_t c) { return iswalpha(c) != 0; });
}

void AddUnique(std::vector<std::wstring>& v, const std::wstring& s) {
    if (!s.empty() && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

// ---- Registry

std::wstring RegStr(HKEY k, const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(k, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < 2)
        return L"";
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    size = static_cast<DWORD>(s.size() * sizeof(wchar_t));
    if (RegGetValueW(k, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, s.data(), &size) != ERROR_SUCCESS) return L"";
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

DWORD RegDword(HKEY k, const wchar_t* name) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(k, nullptr, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return 0;
    return v;
}

struct Hive {
    HKEY root;
    REGSAM view;
    bool perUser, wow64;
};

const Hive kHives[] = {{HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, false, false},
                       {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, false, true},
                       {HKEY_CURRENT_USER, 0, true, false}};

int64_t Ft(const FILETIME& t) { return static_cast<int64_t>((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime); }

// Size and number of items of a folder; `names` (optional) checks every item's name against the
// protection name rules (Git repositories, password databases, wallets).
struct Walk {
    uint64_t bytes = 0, items = 0;
    Protect level = Protect::None;
    std::wstring found;  // the first item at `level`
    bool truncated = false;
};

void WalkDir(const std::wstring& dir, const ProtectNames* names, Walk& w, int depth = 0) {
    if (w.items > 500000) {
        w.truncated = true;
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(ToLongPath(dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        ++w.items;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::wstring path = dir + L"\\" + fd.cFileName;
        if (names && w.level != Protect::Blocked) {
            Protect p = names->Contains(fd.cFileName, isDir);
            if (p > w.level) {
                w.level = p;
                w.found = path;
            }
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // a link: removed as a link
        if (isDir) {
            if (depth < 128) WalkDir(path, names, w, depth + 1);
        } else {
            w.bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace

// =============================================================================================
// Uninstall entries

int64_t ParseInstallDate(const std::wstring& s0) {
    std::wstring s = Norm(s0);
    int y = 0, m = 0, d = 0;
    bool digits = s.size() == 8 && std::all_of(s.begin(), s.end(), [](wchar_t c) { return iswdigit(c) != 0; });
    if (digits) {
        y = _wtoi(s.substr(0, 4).c_str());
        m = _wtoi(s.substr(4, 2).c_str());
        d = _wtoi(s.substr(6, 2).c_str());
    } else if (swscanf_s(s.c_str(), L"%d-%d-%d", &y, &m, &d) == 3 && y > 1900) {
        // 2024-01-31
    } else if (swscanf_s(s.c_str(), L"%d/%d/%d", &m, &d, &y) == 3) {
        // 1/31/2024 (US order, as installers that write it this way use)
    } else {
        return 0;
    }
    if (y < 1990 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    SYSTEMTIME local{static_cast<WORD>(y), static_cast<WORD>(m), 0, static_cast<WORD>(d), 0, 0, 0, 0}, utc;
    FILETIME ft;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) return 0;
    return Ft(ft);
}

std::vector<UninstallEntry> ReadUninstallEntries() {
    std::vector<UninstallEntry> out;
    for (const Hive& hv : kHives) {
        HKEY base = nullptr;
        if (RegOpenKeyExW(hv.root, kUninstallKey, 0, KEY_READ | hv.view, &base) != ERROR_SUCCESS) continue;
        wchar_t name[256];
        for (DWORD i = 0;; ++i) {
            DWORD n = 256;
            LONG r = RegEnumKeyExW(base, i, name, &n, nullptr, nullptr, nullptr, nullptr);
            if (r == ERROR_MORE_DATA) continue;
            if (r != ERROR_SUCCESS) break;
            HKEY k = nullptr;
            if (RegOpenKeyExW(base, name, 0, KEY_READ | hv.view, &k) != ERROR_SUCCESS) continue;
            UninstallEntry e;
            e.key = name;
            e.perUser = hv.perUser;
            e.wow64 = hv.wow64;
            e.displayName = RegStr(k, L"DisplayName");
            e.publisher = RegStr(k, L"Publisher");
            e.displayVersion = RegStr(k, L"DisplayVersion");
            e.uninstallString = RegStr(k, L"UninstallString");
            e.installLocation = RegStr(k, L"InstallLocation");
            e.displayIcon = RegStr(k, L"DisplayIcon");
            e.parentKeyName = RegStr(k, L"ParentKeyName");
            e.releaseType = RegStr(k, L"ReleaseType");
            e.estimatedBytes = static_cast<uint64_t>(RegDword(k, L"EstimatedSize")) * 1024ull;
            e.systemComponent = RegDword(k, L"SystemComponent") != 0;
            e.windowsInstaller = RegDword(k, L"WindowsInstaller") != 0;
            e.noRemove = RegDword(k, L"NoRemove") != 0;
            e.installDate = ParseInstallDate(RegStr(k, L"InstallDate"));
            if (!e.installDate) {
                FILETIME wt{};
                if (RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &wt) ==
                    ERROR_SUCCESS)
                    e.installDate = Ft(wt);
            }
            RegCloseKey(k);
            while (!e.displayName.empty() && iswspace(e.displayName.back())) e.displayName.pop_back();
            if (!e.displayName.empty()) out.push_back(std::move(e));
        }
        RegCloseKey(base);
    }
    return out;
}

bool IsListedApp(const UninstallEntry& e) {
    if (e.displayName.empty() || e.uninstallString.empty()) return false;
    if (e.systemComponent || !e.parentKeyName.empty()) return false;  // hidden, or an update of another entry
    std::wstring rt = Lower(e.releaseType);
    if (rt == L"update" || rt == L"hotfix" || rt == L"security update" || rt == L"service pack" || rt == L"update rollup") return false;
    // Windows / Office patches: "KB1234567" keys, "Security Update for ..." names.
    if (e.key.size() > 2 && (e.key[0] == L'K' || e.key[0] == L'k') && (e.key[1] == L'B' || e.key[1] == L'b') &&
        std::all_of(e.key.begin() + 2, e.key.end(), [](wchar_t c) { return iswdigit(c) != 0; }))
        return false;
    std::wstring n = Lower(e.displayName);
    for (const wchar_t* p : {L"update for ", L"security update for ", L"hotfix for ", L"service pack "})
        if (n.rfind(p, 0) == 0) return false;
    return true;
}

bool UninstallEntryExists(const UninstallEntry& e) {
    for (const Hive& hv : kHives) {
        if (hv.perUser != e.perUser || hv.wow64 != e.wow64) continue;
        HKEY k = nullptr;
        std::wstring path = std::wstring(kUninstallKey) + L"\\" + e.key;
        if (RegOpenKeyExW(hv.root, path.c_str(), 0, KEY_READ | hv.view, &k) != ERROR_SUCCESS) return false;
        bool named = !RegStr(k, L"DisplayName").empty();
        RegCloseKey(k);
        return named;
    }
    return false;
}

std::wstring UninstallKeyPath(const UninstallEntry& e) {
    return std::wstring(e.perUser ? L"HKCU\\" : L"HKLM\\") + kUninstallKey + L"\\" + e.key + (e.wow64 ? L" (32-bit)" : L"");
}

std::wstring AppInstallFolder(const UninstallEntry& e) {
    std::wstring loc = CleanPath(e.installLocation);
    if (IsAbsolute(loc)) return IsDriveRoot(loc) ? L"" : loc;
    std::wstring exe, args;
    if (!SplitCommand(e.uninstallString, exe, args)) return L"";
    exe = CleanPath(exe);
    if (!IsAbsolute(exe)) return L"";
    std::wstring file = Lower(Leaf(exe));
    for (const wchar_t* tool : {L"msiexec.exe", L"rundll32.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"wscript.exe",
                                L"cscript.exe", L"sdbinst.exe", L"winget.exe", L"explorer.exe"})
        if (file == tool) return L"";
    std::wstring dir = Parent(exe), ld = Lower(dir) + L"\\";
    // Uninstallers kept in shared places: their folder is not the app's.
    for (const wchar_t* shared : {L"\\package cache\\", L"\\installshield installation information\\", L"\\common files\\", L"\\windows\\",
                                  L"\\temp\\", L"\\downloaded installations\\", L"\\installer\\", L"\\uninstall information\\"})
        if (ld.find(shared) != std::wstring::npos) return L"";
    if (ld.find(L"\\{") != std::wstring::npos) return L"";  // a GUID folder of an installer engine
    std::wstring leaf = Lower(Leaf(dir));
    if (leaf == L"uninstall" || leaf == L"uninst" || leaf == L"_uninst" || leaf == L"uninstaller") dir = Parent(dir);
    if (dir.empty() || IsDriveRoot(dir)) return L"";
    return dir;
}

bool SplitCommand(const std::wstring& command, std::wstring& exe, std::wstring& args) {
    std::wstring c = command;
    size_t b = c.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return false;
    c = c.substr(b);
    if (c[0] == L'"') {
        size_t q = c.find(L'"', 1);
        if (q == std::wstring::npos) return false;
        exe = c.substr(1, q - 1);
        args = c.substr(q + 1);
    } else {
        std::wstring lc = Lower(c);
        size_t at = lc.find(L".exe");
        while (at != std::wstring::npos && at + 4 < c.size() && c[at + 4] != L' ' && c[at + 4] != L'\t') at = lc.find(L".exe", at + 4);
        if (at != std::wstring::npos) {
            exe = c.substr(0, at + 4);
            args = c.substr(at + 4);
        } else {
            // No ".exe": the longest prefix (ending at a space) that is a file, else the first word.
            size_t cut = c.find(L' ');
            for (size_t sp = c.find(L' '); sp != std::wstring::npos; sp = c.find(L' ', sp + 1)) {
                DWORD a = GetFileAttributesW(c.substr(0, sp).c_str());
                if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) cut = sp;
            }
            exe = c.substr(0, cut);
            args = cut == std::wstring::npos ? L"" : c.substr(cut);
        }
    }
    size_t a = args.find_first_not_of(L" \t");
    args = a == std::wstring::npos ? L"" : args.substr(a);
    return !exe.empty();
}

bool IsUriCommand(const std::wstring& c) {
    size_t colon = c.find(L':');
    return colon != std::wstring::npos && colon > 1 && c.find(L'\\') > colon && c.find_first_of(L" \"") > colon;
}

bool LaunchCommand(const std::wstring& command, HANDLE* process, std::wstring& err) {
    if (process) *process = nullptr;
    std::wstring exe, args;
    if (IsUriCommand(command)) {
        exe = command;
    } else if (!SplitCommand(command, exe, args)) {
        err = L"Can't read the uninstall command: " + command;
        return false;
    }
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_FLAG_NO_UI | (process ? SEE_MASK_NOCLOSEPROCESS : 0);
    sei.lpVerb = L"open";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        err = e == ERROR_CANCELLED ? L"Cancelled" : L"Couldn't start the uninstaller: " + util::Win32ErrorMessage(e);
        return false;
    }
    if (process) *process = sei.hProcess;
    return true;
}

std::vector<InstalledApp> FindInstalledApps(bool measure) {
    std::vector<UninstallEntry> all = ReadUninstallEntries();
    LeftoverRoots pc = LeftoverRoots::ThisPC();
    std::set<std::wstring> registered;  // InstallLocations: a derived folder that is one of them is another app's
    for (auto& e : all) {
        std::wstring loc = CleanPath(e.installLocation);
        if (IsAbsolute(loc)) registered.insert(Lower(loc));
    }
    std::vector<InstalledApp> out;
    std::set<std::wstring> seen;
    for (auto& e : all) {
        if (!IsListedApp(e)) continue;
        if (!seen.insert(Lower(e.displayName + L"|" + e.displayVersion + L"|" + e.publisher + L"|" + e.uninstallString)).second) continue;
        InstalledApp a;
        a.e = e;
        a.folder = AppInstallFolder(e);
        bool derived = !IsAbsolute(CleanPath(e.installLocation));
        if (derived && registered.count(Lower(a.folder))) a.folder.clear();
        for (auto& n : pc.never)
            if (!a.folder.empty() && Within(n, a.folder)) a.folder.clear();  // "C:\Program Files", the profile, ...
        a.bytes = e.estimatedBytes;
        if (!a.bytes && measure && !a.folder.empty() && IsDir(a.folder)) {
            Walk w;
            WalkDir(a.folder, nullptr, w);
            a.bytes = w.bytes;
            a.measured = true;
        }
        out.push_back(std::move(a));
    }
    std::stable_sort(out.begin(), out.end(), [](const InstalledApp& a, const InstalledApp& b) { return a.bytes > b.bytes; });
    return out;
}

// =============================================================================================
// Leftovers

LeftoverRoots LeftoverRoots::ThisPC() {
    LeftoverRoots r;
    std::wstring local = Env(L"LOCALAPPDATA");
    std::wstring low = KnownFolder(FOLDERID_LocalAppDataLow);
    for (std::wstring p : {Env(L"ProgramFiles"), Env(L"ProgramW6432"), Env(L"ProgramFiles(x86)"), Env(L"ProgramData"), Env(L"APPDATA"),
                           local, local.empty() ? L"" : local + L"\\Programs", low}) {
        p = CleanPath(p);
        if (IsAbsolute(p) && std::none_of(r.roots.begin(), r.roots.end(), [&](const std::wstring& x) { return !_wcsicmp(x.c_str(), p.c_str()); }))
            r.roots.push_back(p);
    }
    r.never = r.roots;
    std::wstring prof = Env(L"USERPROFILE"), win = Env(L"SystemRoot"), sys = Env(L"SystemDrive");
    for (std::wstring p :
         {prof, win, sys.empty() ? L"" : sys + L"\\", Env(L"PUBLIC"), Env(L"CommonProgramFiles"), Env(L"CommonProgramFiles(x86)"),
          Env(L"CommonProgramW6432"), Env(L"TEMP"), KnownFolder(FOLDERID_Desktop), KnownFolder(FOLDERID_Documents),
          KnownFolder(FOLDERID_Downloads), KnownFolder(FOLDERID_Pictures), KnownFolder(FOLDERID_Music), KnownFolder(FOLDERID_Videos),
          KnownFolder(FOLDERID_UserProfiles), KnownFolder(FOLDERID_SkyDrive), KnownFolder(FOLDERID_SavedGames),
          local.empty() ? L"" : local + L"\\Microsoft", local.empty() ? L"" : local + L"\\Packages", local.empty() ? L"" : local + L"\\Temp",
          Env(L"APPDATA").empty() ? L"" : Env(L"APPDATA") + L"\\Microsoft", Env(L"ProgramData").empty() ? L"" : Env(L"ProgramData") + L"\\Microsoft"}) {
        p = CleanPath(p);
        if (IsAbsolute(p)) r.never.push_back(p);
    }
    return r;
}

std::vector<std::wstring> AppNameVariants(const UninstallEntry& e) {
    std::vector<std::wstring> v;
    std::wstring full = Norm(e.displayName);
    if (!Generic(full)) AddUnique(v, full);
    std::wstring stripped = StripVersion(full);
    if (!Generic(stripped)) AddUnique(v, stripped);
    std::wstring folder = AppInstallFolder(e);
    if (!folder.empty()) {
        std::wstring leaf = Norm(Leaf(folder));
        if (!leaf.empty() && !NoiseWord(leaf) && !Generic(leaf)) AddUnique(v, leaf);
    }
    return v;
}

std::vector<std::wstring> PublisherVariants(const std::wstring& publisher) {
    std::vector<std::wstring> v;
    std::wstring full = Norm(publisher);
    if (full.empty()) return v;
    AddUnique(v, full);
    static const std::set<std::wstring> suffix = {L"inc", L"inc.", L"incorporated", L"llc", L"l.l.c.", L"ltd", L"ltd.", L"limited",
                                                  L"gmbh", L"ag", L"corp", L"corp.", L"corporation", L"co", L"co.", L"company",
                                                  L"s.a.", L"sa", L"s.r.o.", L"b.v.", L"bv", L"oy", L"ab", L"pty", L"plc", L"srl",
                                                  L"s.l.", L"kk", L"team", L"project", L"developers", L"foundation", L"software"};
    std::vector<std::wstring> w = Words(full);
    for (auto& x : w)
        while (!x.empty() && (x.back() == L',' || x.back() == L';')) x.pop_back();
    if (!w.empty() && w[0] == L"the") w.erase(w.begin());
    // Every step: "foo software inc." -> "foo software" -> "foo".
    for (size_t n = w.size(); n >= 1; --n) {
        AddUnique(v, Join(w, n));
        if (n == 1 || !suffix.count(w[n - 1])) break;
    }
    return v;
}

Protect LeftoverProtection(const std::wstring& path, std::wstring* note) {
    ProtectReport pr = CheckProtection(path);
    if (pr.hits.empty()) return pr.level;
    // Everything inside Program Files counts as an installed program ("uninstall it in Settings"):
    // for the folder of an app whose uninstall entry is gone, that no longer holds. Every other
    // rule (user data, keys, shared components, the folders themselves) still does.
    Protect level = Protect::None;
    for (auto& hit : pr.hits) {
        bool programFolder =
            hit.rel == ProtectRel::Inside && hit.group == L"Programs" && (hit.name == L"Program Files" || hit.name == L"ProgramData");
        if (!programFolder && hit.level > level) {
            level = hit.level;
            if (note) *note = DescribeHit(hit);
        }
    }
    return level;
}

std::vector<AppLeftover> FindAppLeftovers(const UninstallEntry& gone, const std::vector<UninstallEntry>& installed,
                                          const LeftoverRoots& roots) {
    std::vector<std::wstring> appV = AppNameVariants(gone), pubV = PublisherVariants(gone.publisher);
    // Under the publisher's folder, the app may be named without the publisher: "Mozilla\Thunderbird".
    std::vector<std::wstring> underPubV = appV;
    for (auto& p : pubV)
        for (auto& a : appV)
            if (a.size() > p.size() + 1 && a.compare(0, p.size(), p) == 0 && a[p.size()] == L' ') {
                std::wstring rest = a.substr(p.size() + 1);
                if (!Generic(rest) && !NoiseWord(Words(rest)[0])) AddUnique(underPubV, rest);
            }

    // What the apps that are still installed are called, and where they are.
    std::vector<std::wstring> otherNames, otherDirs;
    for (auto& o : installed) {
        if (o.key == gone.key && o.perUser == gone.perUser && o.wow64 == gone.wow64) continue;
        AddUnique(otherNames, Norm(o.displayName));
        for (auto& s : AppNameVariants(o)) AddUnique(otherNames, s);
        for (auto& s : PublisherVariants(o.publisher)) AddUnique(otherNames, s);
        std::wstring d = AppInstallFolder(o);
        if (!d.empty()) AddUnique(otherDirs, d);
    }
    // A folder name another app is called, or begins with ("Python" while "Python Launcher" is installed).
    auto takenByOther = [&](const std::wstring& n) {
        for (auto& s : otherNames)
            if (s == n || (s.size() > n.size() && s.compare(0, n.size(), n) == 0 && s[n.size()] == L' ')) return true;
        return false;
    };
    auto rootOrAbove = [&](const std::wstring& d) {
        if (IsDriveRoot(d)) return true;
        for (auto& r : roots.roots)
            if (Within(r, d)) return true;
        for (auto& r : roots.never)
            if (Within(r, d)) return true;
        return false;
    };
    // Holds another app's folder, or lies inside one.
    auto overlapsOther = [&](const std::wstring& p) {
        for (auto& d : otherDirs)
            if (Within(d, p) || (Within(p, d) && !rootOrAbove(d))) return true;
        return false;
    };

    struct Cand {
        AppLeftover l;
        bool pubFolder = false;
    };
    std::map<std::wstring, Cand> cands;  // by lowercase path
    auto add = [&](std::wstring path, const std::wstring& why, bool preselect, bool pubFolder) {
        path = CleanPath(path);
        if (!IsAbsolute(path) || rootOrAbove(path) || overlapsOther(path)) return;
        DWORD a = Attrs(path);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return;
        Cand& c = cands[Lower(path)];
        if (c.l.path.empty()) {
            c.l.path = path;
            c.l.why = why;
        }
        c.l.preselect = c.l.preselect || preselect;
        c.pubFolder = c.pubFolder || pubFolder;
    };
    auto has = [](const std::vector<std::wstring>& v, const std::wstring& s) { return std::find(v.begin(), v.end(), s) != v.end(); };

    // 1. Where it was installed.
    std::wstring folder = AppInstallFolder(gone);
    if (!folder.empty()) {
        // Preselected only when it was registered as the install folder and no installed app goes
        // by the same name (another version or build may share the folder).
        bool registered = IsAbsolute(CleanPath(gone.installLocation));
        bool nameTaken = std::any_of(appV.begin(), appV.end(), [&](const std::wstring& n) { return takenByOther(n); });
        add(folder, registered ? L"Its install folder" : L"The folder its uninstaller was in", registered && !nameTaken, false);
    }
    // 2. Folders named after it in the usual places, directly or in its publisher's folder.
    for (auto& root : roots.roots) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(ToLongPath(root + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring n = Norm(fd.cFileName), dir = root + L"\\" + fd.cFileName;
            if (has(appV, n) && !takenByOther(n)) add(dir, L"Named after the app, in " + root, false, false);
            if (!has(pubV, n)) continue;
            std::vector<std::wstring> matched;
            bool other = false;
            WIN32_FIND_DATAW sd;
            HANDLE hs = FindFirstFileExW(ToLongPath(dir + L"\\*").c_str(), FindExInfoBasic, &sd, FindExSearchNameMatch, nullptr, 0);
            if (hs == INVALID_HANDLE_VALUE) continue;
            do {
                if (!wcscmp(sd.cFileName, L".") || !wcscmp(sd.cFileName, L"..")) continue;
                std::wstring sn = Norm(sd.cFileName);
                bool isDir = (sd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(sd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
                if (isDir && has(underPubV, sn) && !takenByOther(sn))
                    matched.push_back(dir + L"\\" + sd.cFileName);
                else
                    other = true;
            } while (FindNextFileW(hs, &sd));
            FindClose(hs);
            // The publisher's folder itself only when it holds nothing but this app, and no other
            // installed app has this publisher or a name starting with it.
            if (!other && !matched.empty() && !Generic(n) && !takenByOther(n)) {
                add(dir, L"Its publisher's folder, holding only this app", false, true);
            } else {
                for (auto& m : matched) add(m, L"Named after the app, in " + dir, false, false);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // A folder inside another candidate goes with it (outermost kept).
    std::vector<Cand> list;
    for (auto& [k, c] : cands) list.push_back(c);
    std::sort(list.begin(), list.end(), [](const Cand& a, const Cand& b) { return a.l.path.size() < b.l.path.size(); });
    std::vector<Cand> kept;
    for (auto& c : list) {
        auto outer = std::find_if(kept.begin(), kept.end(), [&](const Cand& k) { return Within(c.l.path, k.l.path); });
        if (outer == kept.end()) {
            kept.push_back(c);
        } else if (c.l.preselect && outer->pubFolder) {
            outer->l.preselect = true;  // the publisher's folder holds only the install folder
        }
    }

    ProtectNames names;
    std::vector<AppLeftover> out;
    for (auto& c : kept) {
        AppLeftover l = c.l;
        l.level = LeftoverProtection(l.path, &l.protectNote);
        Walk w;
        if (l.level != Protect::Blocked) WalkDir(l.path, &names, w);  // never deleted anyway: not measured
        l.bytes = w.bytes;
        l.items = w.items + 1;
        if (w.level > l.level) {
            l.level = w.level;
            l.protectNote = L"Holds a protected item: " + (w.found.size() > l.path.size() ? w.found.substr(l.path.size() + 1) : w.found);
        }
        if (w.truncated && l.level < Protect::Caution) {
            l.level = Protect::Caution;
            l.protectNote = L"Too large to check everything inside";
        }
        if (l.level != Protect::None) l.preselect = false;  // anything protected inside: the user decides
        out.push_back(std::move(l));
    }
    std::stable_sort(out.begin(), out.end(), [](const AppLeftover& a, const AppLeftover& b) {
        if (a.preselect != b.preselect) return a.preselect;
        return a.bytes > b.bytes;
    });
    return out;
}
