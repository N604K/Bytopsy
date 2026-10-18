#include "games.h"

#include <windows.h>

#include <algorithm>
#include <functional>
#include <set>

#include "aimodels.h"
#include "tree.h"
#include "util.h"

namespace {

std::wstring Lower(std::wstring s) { return util::ToLower(s); }

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? std::wstring(buf, n) : L"";
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(ToLongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ReadFileBytes(const std::wstring& path, size_t max = 4 << 20) {
    std::string out;
    HANDLE h = CreateFileW(ToLongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char b[8192];
    DWORD n = 0;
    while (out.size() < max && ReadFile(h, b, sizeof(b), &n, nullptr) && n) out.append(b, n);
    CloseHandle(h);
    return out;
}

std::wstring ReadText(const std::wstring& path) {
    std::string raw = ReadFileBytes(path);
    if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) raw.erase(0, 3);
    return util::Utf8ToWide(raw);
}

template <class F>
void ForEachChild(const std::wstring& dir, const wchar_t* pattern, F f) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(ToLongPath(dir + L"\\" + pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        f(fd);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int64_t Ft(const FILETIME& t) { return static_cast<int64_t>((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime); }

// Size of a folder, and the last time one of its programs was opened (if Windows records it).
void Measure(const std::wstring& dir, uint64_t& bytes, int64_t& exeUsed) {
    ForEachChild(dir, L"*", [&](const WIN32_FIND_DATAW& fd) {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Measure(dir + L"\\" + fd.cFileName, bytes, exeUsed);
            return;
        }
        bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        size_t len = wcslen(fd.cFileName);
        if (len > 4 && !_wcsicmp(fd.cFileName + len - 4, L".exe")) exeUsed = std::max(exeUsed, Ft(fd.ftLastAccessTime));
    });
}

std::wstring JsonValue(const std::wstring& json, const wchar_t* key) {
    std::wstring k = std::wstring(L"\"") + key + L"\"";
    size_t at = json.find(k);
    if (at == std::wstring::npos) return L"";
    at = json.find(L':', at + k.size());
    if (at == std::wstring::npos) return L"";
    ++at;
    while (at < json.size() && iswspace(json[at])) ++at;
    std::wstring v;
    if (at < json.size() && json[at] == L'"') {
        for (size_t i = at + 1; i < json.size() && json[i] != L'"'; ++i) {
            if (json[i] == L'\\' && i + 1 < json.size()) ++i;
            v += json[i];
        }
        return v;
    }
    while (at < json.size() && json[at] != L',' && json[at] != L'}' && !iswspace(json[at])) v += json[at++];
    return v;
}

const wchar_t* HowFor(const std::wstring& launcher) {
    if (launcher == L"Steam") return L"Steam asks you to confirm, then removes it";
    if (launcher == L"Epic Games") return L"Opens the Epic Games Launcher: uninstall it from your Library";
    if (launcher == L"Ubisoft Connect") return L"Ubisoft Connect asks you to confirm";
    if (launcher == L"Battle.net") return L"Battle.net asks you to confirm";
    if (launcher == L"Xbox") return L"Opens Settings > Installed apps: uninstall it there";
    return L"Its uninstaller asks you to confirm";
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Valve KeyValues

std::wstring Vdf::Get(const wchar_t* key) const {
    auto it = values.find(Lower(key));
    return it == values.end() ? L"" : it->second;
}

Vdf ParseVdf(const std::wstring& t) {
    size_t i = 0;
    // A token: a quoted or bare string, or a brace (returned as "{" / "}" with brace = true).
    auto next = [&](std::wstring& tok, bool& brace) {
        for (;;) {
            while (i < t.size() && iswspace(t[i])) ++i;
            if (i + 1 < t.size() && t[i] == L'/' && t[i + 1] == L'/') {
                while (i < t.size() && t[i] != L'\n') ++i;
                continue;
            }
            break;
        }
        if (i >= t.size()) return false;
        tok.clear();
        brace = t[i] == L'{' || t[i] == L'}';
        if (brace) {
            tok = t[i++];
            return true;
        }
        if (t[i] == L'"') {
            for (++i; i < t.size() && t[i] != L'"'; ++i) {
                if (t[i] == L'\\' && i + 1 < t.size()) {
                    ++i;
                    tok += t[i] == L'n' ? L'\n' : t[i] == L't' ? L'\t' : t[i];
                } else {
                    tok += t[i];
                }
            }
            ++i;
        } else {
            while (i < t.size() && !iswspace(t[i]) && t[i] != L'{' && t[i] != L'}' && t[i] != L'"') tok += t[i++];
        }
        return true;
    };
    std::function<void(Vdf&, int)> block = [&](Vdf& v, int depth) {
        std::wstring key, val;
        bool kb = false, vb = false;
        while (next(key, kb)) {
            if (kb) {
                if (key == L"}") return;
                continue;  // stray '{'
            }
            if (!next(val, vb)) return;
            if (vb && val == L"{") {
                if (depth < 64) block(v.children[Lower(key)], depth + 1);
            } else if (!vb) {
                v.values[Lower(key)] = val;
            } else {
                return;
            }
        }
    };
    Vdf root;
    block(root, 0);
    return root;
}

// ---------------------------------------------------------------------------------------------
// Launchers

void ScanSteamLibrary(const std::wstring& steamapps, std::vector<Game>& out) {
    std::set<std::wstring> installDirs, installed;
    ForEachChild(steamapps, L"appmanifest_*.acf", [&](const WIN32_FIND_DATAW& fd) {
        Vdf m = ParseVdf(ReadText(steamapps + L"\\" + fd.cFileName));
        auto it = m.children.find(L"appstate");
        if (it == m.children.end()) return;
        const Vdf& a = it->second;
        std::wstring id = a.Get(L"appid"), dir = a.Get(L"installdir"), name = a.Get(L"name");
        if (id.empty() || dir.empty()) return;
        installDirs.insert(Lower(dir));
        installed.insert(id);
        // Runtimes other games need (redistributables, Proton, the Linux runtime) are not games.
        if (id == L"228980" || name.rfind(L"Steamworks Common", 0) == 0 || name.rfind(L"Proton", 0) == 0 ||
            name.rfind(L"Steam Linux Runtime", 0) == 0)
            return;
        Game g;
        g.launcher = L"Steam";
        g.id = id;
        g.name = name.empty() ? L"App " + id : name;
        g.folder = steamapps + L"\\common\\" + dir;
        g.bytes = _wcstoui64(a.Get(L"sizeondisk").c_str(), nullptr, 10);
        int64_t played = _wtoi64(a.Get(L"lastplayed").c_str());
        g.lastPlayed = played > 0 ? util::UnixToFileTime(played) : 0;
        g.uninstall = L"steam://uninstall/" + id;
        out.push_back(std::move(g));
    });
    auto leftover = [&](const std::wstring& path, const std::wstring& name) {
        Game g;
        g.launcher = L"Steam";
        g.name = name;
        g.folder = path;
        g.leftover = true;
        int64_t ignore = 0;
        Measure(path, g.bytes, ignore);
        out.push_back(std::move(g));
    };
    ForEachChild(steamapps + L"\\common", L"*", [&](const WIN32_FIND_DATAW& fd) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            !installDirs.count(Lower(fd.cFileName)))
            leftover(steamapps + L"\\common\\" + fd.cFileName, fd.cFileName);
    });
    for (const wchar_t* sub : {L"workshop\\content", L"shadercache"}) {
        bool workshop = sub[0] == L'w';
        ForEachChild(steamapps + L"\\" + sub, L"*", [&](const WIN32_FIND_DATAW& fd) {
            std::wstring id = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || id.find_first_not_of(L"0123456789") != std::wstring::npos ||
                installed.count(id))
                return;
            leftover(steamapps + L"\\" + sub + L"\\" + id,
                     (workshop ? L"Workshop items of an uninstalled game (app " : L"Shader cache of an uninstalled game (app ") + id + L")");
        });
    }
}

void ScanEpicManifests(const std::wstring& manifestsDir, std::vector<Game>& out) {
    ForEachChild(manifestsDir, L"*.item", [&](const WIN32_FIND_DATAW& fd) {
        std::wstring j = ReadText(manifestsDir + L"\\" + fd.cFileName);
        Game g;
        g.launcher = L"Epic Games";
        g.id = JsonValue(j, L"AppName");
        g.name = JsonValue(j, L"DisplayName");
        g.folder = JsonValue(j, L"InstallLocation");
        if (g.name.empty() || g.folder.empty() || JsonValue(j, L"bIsApplication") == L"false") return;
        g.bytes = _wcstoui64(JsonValue(j, L"InstallSize").c_str(), nullptr, 10);
        g.uninstall = L"com.epicgames.launcher://library";
        out.push_back(std::move(g));
    });
}

std::wstring GameLauncherOf(const UninstallEntry& e) {
    std::wstring u = Lower(e.uninstallString), pub = Lower(e.publisher), name = Lower(e.displayName);
    if (u.rfind(L"uplay://uninstall/", 0) == 0) return L"Ubisoft Connect";
    if (pub == L"gog.com" || pub == L"gog ltd.") return L"GOG";
    if (u.find(L"battle.net") != std::wstring::npos && pub.find(L"blizzard") != std::wstring::npos && name != L"battle.net")
        return L"Battle.net";
    if (pub.find(L"electronic arts") != std::wstring::npos && u.find(L"eainstaller") != std::wstring::npos) return L"EA app";
    return L"";
}

std::vector<Game> FindGames() {
    std::vector<Game> out;
    // Steam: its libraries (libraryfolders.vdf).
    std::wstring steam;
    {
        wchar_t buf[MAX_PATH];
        DWORD size = sizeof(buf);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            steam = buf;
        for (auto& c : steam)
            if (c == L'/') c = L'\\';
        if (steam.empty() || !IsDir(steam)) steam = Env(L"ProgramFiles(x86)") + L"\\Steam";
    }
    if (IsDir(steam + L"\\steamapps")) {
        // libraryfolders.vdf first: it spells the paths as they are (SteamPath is lowercase).
        std::vector<std::wstring> libs;
        auto add = [&](std::wstring p) {
            while (!p.empty() && p.back() == L'\\') p.pop_back();
            if (!p.empty() && std::none_of(libs.begin(), libs.end(), [&](const std::wstring& x) { return !_wcsicmp(x.c_str(), p.c_str()); }))
                libs.push_back(p);
        };
        Vdf lf = ParseVdf(ReadText(steam + L"\\steamapps\\libraryfolders.vdf"));
        auto root = lf.children.find(L"libraryfolders");
        if (root != lf.children.end())
            for (auto& [k, lib] : root->second.children) add(lib.Get(L"path"));
        add(steam);
        for (auto& l : libs)
            if (IsDir(l + L"\\steamapps")) ScanSteamLibrary(l + L"\\steamapps", out);
    }
    // Epic Games: the launcher's install manifests.
    ScanEpicManifests(Env(L"ProgramData") + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests", out);
    // GOG, Ubisoft Connect, EA app, Battle.net: their games register an uninstaller.
    for (auto& e : ReadUninstallEntries()) {
        if (e.uninstallString.empty()) continue;
        std::wstring launcher = GameLauncherOf(e);
        if (launcher.empty()) continue;
        Game g;
        g.launcher = launcher;
        g.id = e.key;
        g.name = e.displayName;
        g.folder = e.installLocation;
        while (!g.folder.empty() && (g.folder.back() == L'\\' || g.folder.back() == L'"')) g.folder.pop_back();
        if (!g.folder.empty() && g.folder[0] == L'"') g.folder.erase(0, 1);
        g.bytes = e.estimatedBytes;
        g.uninstall = e.uninstallString;
        if (std::none_of(out.begin(), out.end(), [&](const Game& x) { return x.launcher == g.launcher && x.name == g.name; }))
            out.push_back(std::move(g));
    }
    // Xbox app / Microsoft Store games: <drive>:\XboxGames\<game>.
    DWORD drives = GetLogicalDrives();
    for (wchar_t d = L'C'; d <= L'Z'; ++d) {
        if (!(drives & (1u << (d - L'A')))) continue;
        std::wstring root = std::wstring(1, d) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        ForEachChild(root + L"XboxGames", L"*", [&](const WIN32_FIND_DATAW& fd) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsDir(root + L"XboxGames\\" + fd.cFileName + L"\\Content")) return;
            Game g;
            g.launcher = L"Xbox";
            g.name = fd.cFileName;
            g.folder = root + L"XboxGames\\" + fd.cFileName;
            g.uninstall = L"ms-settings:appsfeatures";
            out.push_back(std::move(g));
        });
    }
    // Sizes the launcher didn't record, and when a game was last played if the launcher doesn't say.
    bool atime = LastAccessTracked();
    for (auto& g : out) {
        if (g.leftover) continue;
        g.uninstallHow = HowFor(g.launcher);
        if (g.folder.empty() || !IsDir(g.folder)) continue;
        uint64_t bytes = 0;
        int64_t exeUsed = 0;
        if (!g.bytes || !g.lastPlayed) Measure(g.folder, bytes, exeUsed);
        if (!g.bytes) g.bytes = bytes;
        if (!g.lastPlayed && atime && exeUsed) {
            g.lastPlayed = exeUsed;
            g.lastPlayedGuess = true;
        }
    }
    for (auto& g : out)
        if (g.leftover) g.uninstallHow = L"Moved to the Recycle Bin";
    std::stable_sort(out.begin(), out.end(), [](const Game& a, const Game& b) { return a.bytes > b.bytes; });
    return out;
}

bool StartUninstall(const Game& g, std::wstring& err) {
    if (g.uninstall.empty() || g.leftover) {
        err = L"No uninstaller is registered for " + g.name;
        return false;
    }
    std::wstring launchErr;
    if (LaunchCommand(g.uninstall, nullptr, launchErr)) return true;
    // "steam://...", "ms-settings:...": the launcher isn't there to open it.
    err = IsUriCommand(g.uninstall) && launchErr != L"Cancelled" ? L"Couldn't open " + g.launcher + L" (is it installed?)" : launchErr;
    return false;
}
