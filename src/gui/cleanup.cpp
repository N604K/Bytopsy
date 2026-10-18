#include "cleanup.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <unordered_set>

#include "../fsops.h"
#include "../journal.h"
#include "../protect.h"
#include "../tree.h"
#include "../util.h"

namespace {

std::wstring Env(const std::wstring& s) {
    wchar_t buf[32768];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 32768);
    return n ? std::wstring(buf) : s;
}

// Expands '*' path segments against the file system (directories only). Relative paths and
// undefined %VARIABLES% (left as they are by Env) match nothing.
void Expand(const std::wstring& path, std::vector<std::wstring>& out) {
    bool absolute = (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') || path.rfind(L"\\\\", 0) == 0;
    if (!absolute || path.find(L'%') != std::wstring::npos) return;
    size_t star = path.find(L'*');
    if (star == std::wstring::npos) {
        DWORD a = GetFileAttributesW(ToLongPath(path).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(path);
        return;
    }
    size_t segStart = path.rfind(L'\\', star);
    size_t segEnd = path.find(L'\\', star);
    std::wstring base = path.substr(0, segStart);
    std::wstring pattern = path.substr(segStart + 1, segEnd == std::wstring::npos ? std::wstring::npos : segEnd - segStart - 1);
    std::wstring rest = segEnd == std::wstring::npos ? L"" : path.substr(segEnd);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((ToLongPath(base) + L"\\" + pattern).c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories,
                                nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        Expand(base + L"\\" + fd.cFileName + rest, out);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// "C:", "C:\", "\" and the like (also after %ENV% expansion).
bool IsDriveRoot(const std::wstring& p) {
    std::wstring s = p;
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s.empty() || (s.size() == 2 && s[1] == L':');
}

int64_t FtToI(const FILETIME& f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

std::wstring Trim(const std::wstring& v) {
    size_t a = v.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = v.find_last_not_of(L" \t\r\n");
    return v.substr(a, b - a + 1);
}

// File name against ';' separated patterns ("*.log;*.tmp"), case-insensitive.
bool MatchAny(const std::wstring& name, const std::wstring& patterns) {
    size_t i = 0;
    while (i <= patterns.size()) {
        size_t j = patterns.find(L';', i);
        if (j == std::wstring::npos) j = patterns.size();
        std::wstring p = util::ToLower(Trim(patterns.substr(i, j - i)));
        if (!p.empty() && (p == L"*" || p == L"*.*" || util::GlobMatchI(p, name))) return true;
        i = j + 1;
    }
    return false;
}

std::vector<std::wstring> LowerSegments(const std::wstring& path) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find(L'\\', i);
        if (j == std::wstring::npos) j = path.size();
        if (j > i) out.push_back(util::ToLower(path.substr(i, j - i)));
        i = j + 1;
    }
    return out;
}

// An exclude with its folder expanded into lowercase segments ('*' segments match any name).
struct Excl {
    std::vector<std::wstring> segs;
    std::wstring pattern;
    bool tree;
};

std::vector<Excl> PrepareExcludes(const std::vector<CleanExclude>& excludes) {
    std::vector<Excl> out;
    for (auto& e : excludes) {
        std::wstring folder = Env(e.folder);
        while (!folder.empty() && folder.back() == L'\\') folder.pop_back();
        if (folder.empty() || folder.find(L'%') != std::wstring::npos) continue;
        out.push_back({LowerSegments(folder), e.pattern, e.tree});
    }
    return out;
}

bool Excluded(const std::vector<Excl>& ex, const std::wstring& file) {
    size_t slash = file.rfind(L'\\');
    if (slash == std::wstring::npos) return false;
    std::vector<std::wstring> parent = LowerSegments(file.substr(0, slash));
    std::wstring name = file.substr(slash + 1);
    for (const Excl& e : ex) {
        if (e.segs.size() > parent.size() || (!e.tree && e.segs.size() != parent.size())) continue;
        bool ok = true;
        for (size_t i = 0; i < e.segs.size() && ok; ++i) ok = util::GlobMatchI(e.segs[i], parent[i]);
        if (ok && MatchAny(name, e.pattern)) return true;
    }
    return false;
}

// `path` is itself the folder of a whole-folder exclude.
bool ExcludedFolder(const std::vector<Excl>& ex, const std::wstring& path) {
    std::vector<std::wstring> segs = LowerSegments(path);
    for (const Excl& e : ex) {
        if (!e.tree || e.segs.size() != segs.size()) continue;
        bool ok = true;
        for (size_t i = 0; i < e.segs.size() && ok; ++i) ok = util::GlobMatchI(e.segs[i], segs[i]);
        if (ok) return true;
    }
    return false;
}

// Visits the top-level entries of a target (respecting the file pattern and the minimum age).
template <class F>
void ForEachEntry(const CleanTarget& t, const std::wstring& dir, F&& f) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((ToLongPath(dir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    int64_t cutoff = t.minAgeHours ? util::NowFileTime() - static_cast<int64_t>(t.minAgeHours) * 36000000000LL : 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!t.filePattern.empty() && (isDir || !MatchAny(fd.cFileName, t.filePattern))) continue;
        if (cutoff && FtToI(fd.ftLastWriteTime) > cutoff) continue;
        f(dir + L"\\" + fd.cFileName, fd);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Visits the matching files in `dir` and all its subfolders. Links are not followed, and folders
// a name rule protects (.git, ...) are not entered.
template <class F>
void ForEachFileDeep(const CleanTarget& t, const std::wstring& pattern, const std::wstring& dir, const ProtectNames& names,
                     bool& denied, std::atomic<bool>& cancel, F&& f) {
    int64_t cutoff = t.minAgeHours ? util::NowFileTime() - static_cast<int64_t>(t.minAgeHours) * 36000000000LL : 0;
    std::vector<std::wstring> stack{dir};
    while (!stack.empty() && !cancel) {
        std::wstring d = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((ToLongPath(d) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_ACCESS_DENIED) denied = true;
            continue;
        }
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && names.Contains(fd.cFileName, true) == Protect::None)
                    stack.push_back(d + L"\\" + fd.cFileName);
                continue;
            }
            if (!MatchAny(fd.cFileName, pattern)) continue;
            if (cutoff && FtToI(fd.ftLastWriteTime) > cutoff) continue;
            f(d + L"\\" + fd.cFileName, fd);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// Names of files and folders that hold personal data (browser profiles above all). Imported rules
// never list them, whatever the protection rules know about the app.
bool PersonalName(const std::wstring& name) {
    static const wchar_t* const kNames[] = {
        L"login data*", L"cookies*", L"*cookies", L"history*", L"*history.sqlite*", L"web data*", L"bookmarks*",
        L"top sites*", L"visited links*", L"current session", L"current tabs", L"last session", L"last tabs",
        L"sessions", L"session storage", L"sessionstore*", L"local storage", L"indexeddb", L"local extension settings",
        L"sync extension settings", L"extension state", L"preferences", L"secure preferences", L"local state",
        L"places.sqlite*", L"logins*.json", L"key3.db", L"key4.db", L"cert8.db", L"cert9.db", L"signons*",
        L"webappsstore.sqlite*", L"webcachev*.dat", L"*.automaticdestinations-ms", L"*.customdestinations-ms",
        L"*.kdbx", L"*.kdb", L"wallet.dat", L"*.wallet"};
    for (const wchar_t* p : kNames)
        if (util::GlobMatchI(p, name)) return true;
    return false;
}

// Whether `path` (absolute, '*' segments allowed) names something that exists; the last segment
// may be a file.
bool GlobExists(const std::wstring& path) {
    size_t star = path.find_first_of(L"*?");
    if (star == std::wstring::npos) return GetFileAttributesW(ToLongPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
    size_t segStart = path.rfind(L'\\', star);
    if (segStart == std::wstring::npos) return false;
    size_t segEnd = path.find(L'\\', star);
    std::wstring base = path.substr(0, segStart);
    std::wstring rest = segEnd == std::wstring::npos ? L"" : path.substr(segEnd);
    std::wstring pattern = path.substr(segStart + 1, segEnd == std::wstring::npos ? std::wstring::npos : segEnd - segStart - 1);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((ToLongPath(base) + L"\\" + pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (rest.empty()) found = true;
        else if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            found = GlobExists(base + L"\\" + fd.cFileName + rest);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// Size and file count of a path (recursive, links not followed). With `names`, returns false as
// soon as a name rule matches anything inside (a Git repository, a password database, a virtual
// disk, ...): deleting the folder by hand only asks for care, but Cleanup never takes it along.
// With `personal`, the same for files and folders that hold personal data (browser profiles).
bool SizeOf(const std::wstring& path, const WIN32_FIND_DATAW& top, uint64_t& bytes, uint64_t& files, bool& denied,
            std::atomic<bool>& cancel, const ProtectNames* names = nullptr, bool personal = false) {
    if (!(top.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        bytes += (static_cast<uint64_t>(top.nFileSizeHigh) << 32) | top.nFileSizeLow;
        ++files;
        return true;
    }
    if (top.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return true;
    std::vector<std::wstring> stack{path};
    while (!stack.empty() && !cancel) {
        std::wstring d = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((ToLongPath(d) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_ACCESS_DENIED) denied = true;
            continue;
        }
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if ((names && names->Contains(fd.cFileName, isDir) != Protect::None) || (personal && PersonalName(fd.cFileName))) {
                FindClose(h);
                return false;
            }
            if (isDir) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(d + L"\\" + fd.cFileName);
            } else {
                bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                ++files;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return true;
}

}  // namespace

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

bool CreateRestorePoint(const std::wstring& description, std::wstring& err) {
    // srclient.dll is loaded on demand: it doesn't exist on Windows Server without the feature.
    struct RpInfo {  // RESTOREPOINTINFOW
        DWORD dwEventType;
        DWORD dwRestorePtType;
        INT64 llSequenceNumber;
        WCHAR szDescription[256];
    };
    struct SmgrStatus {  // STATEMGRSTATUS
        UINT nStatus;
        INT64 llSequenceNumber;
    };
    using SetFn = BOOL(WINAPI*)(RpInfo*, SmgrStatus*);
    HMODULE lib = LoadLibraryExW(L"srclient.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto set = lib ? reinterpret_cast<SetFn>(GetProcAddress(lib, "SRSetRestorePointW")) : nullptr;
    if (!set) {
        err = L"System Restore is not available on this PC.";
        if (lib) FreeLibrary(lib);
        return false;
    }
    const DWORD kBegin = 100, kEnd = 101, kModifySettings = 12;  // BEGIN/END_SYSTEM_CHANGE, MODIFY_SETTINGS
    RpInfo info{};
    info.dwEventType = kBegin;
    info.dwRestorePtType = kModifySettings;
    wcsncpy_s(info.szDescription, description.c_str(), _TRUNCATE);
    SmgrStatus status{};
    bool ok = set(&info, &status) != FALSE;
    if (ok) {
        RpInfo end{};
        end.dwEventType = kEnd;
        end.llSequenceNumber = status.llSequenceNumber;
        SmgrStatus s2{};
        set(&end, &s2);
    } else if (status.nStatus == ERROR_SERVICE_DISABLED) {
        err = L"System Protection is turned off for the system drive (System Properties \u2192 System Protection).";
    } else {
        err = util::Win32ErrorMessage(status.nStatus ? status.nStatus : GetLastError());
    }
    FreeLibrary(lib);
    return ok;
}

namespace {

bool ParseBool(const std::wstring& v, bool& out) {
    std::wstring l = util::ToLower(v);
    if (l == L"true" || l == L"yes" || l == L"1") return out = true, true;
    if (l == L"false" || l == L"no" || l == L"0") return out = false, true;
    return false;
}

// "24h", "7d", "30" (hours)
bool ParseAge(const std::wstring& v, int& hours) {
    if (v.empty()) return false;
    wchar_t* end = nullptr;
    long n = wcstol(v.c_str(), &end, 10);
    if (end == v.c_str() || n < 0) return false;
    std::wstring unit = util::ToLower(Trim(end));
    if (unit.empty() || unit == L"h") hours = static_cast<int>(n);
    else if (unit == L"d") hours = static_cast<int>(n * 24);
    else return false;
    return true;
}

std::wstring Utf8File(const std::wstring& file, bool& found) {
    found = false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") != 0 || !f) return L"";
    found = true;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF) data.erase(0, 3);
    return util::Utf8ToWide(data);
}

// res/cleaners.ini, built into the windowed app as RCDATA 201.
std::wstring BuiltinCleaners() {
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(201), MAKEINTRESOURCEW(10));
    HGLOBAL g = r ? LoadResource(self, r) : nullptr;
    const char* p = g ? static_cast<const char*>(LockResource(g)) : nullptr;
    if (!p) return L"";
    std::string data(p, SizeofResource(self, r));
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF) data.erase(0, 3);
    return util::Utf8ToWide(data);
}

const char kUserTemplate[] =
    "# Your own WinNCDU cleanup categories. Same format as the built-in list (res/cleaners.ini in the\r\n"
    "# WinNCDU source). Click \"Measure\" in Cleanup after saving.\r\n"
    "#\r\n"
    "# Add a category:\r\n"
    "# [myapp]\r\n"
    "# name = MyApp cache\r\n"
    "# desc = MyApp downloads this again when needed.\r\n"
    "# path = %LOCALAPPDATA%\\MyApp\\Cache\r\n"
    "# path = %LOCALAPPDATA%\\MyApp\\Logs | files=*.log | age=7d\r\n"
    "#\r\n"
    "# Turn a built-in category off (ids: recycle, temp, wintemp, browser, cookies, history, chat,\r\n"
    "# chatmedia, thumbs, crash, wu, do, shader, launchers, dev, jvm, conda, conan, winold, winbt):\r\n"
    "# [dev]\r\n"
    "# enabled = false\r\n"
    "#\r\n"
    "# Protected items (see Protected locations) are never listed or deleted, whatever a rule says.\r\n";

}  // namespace

std::vector<CleanCategory> ParseCleaners(const std::wstring& text, std::vector<std::wstring>& errors) {
    std::vector<CleanCategory> out;
    CleanCategory* cur = nullptr;
    std::unordered_set<std::wstring> commandKind;  // ids with "kind = command"
    auto error = [&](size_t line, const std::wstring& msg) { errors.push_back(L"line " + std::to_wstring(line) + L": " + msg); };
    size_t pos = 0, lineNo = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        ++lineNo;
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        if (line[0] == L'[') {
            size_t close = line.find(L']');
            std::wstring id = close == std::wstring::npos ? L"" : util::ToLower(Trim(line.substr(1, close - 1)));
            if (id.empty()) {
                error(lineNo, L"expected [id]");
                cur = nullptr;
                continue;
            }
            out.emplace_back();
            cur = &out.back();
            cur->id = id;
            cur->safe = false;  // only explicitly marked categories are preselected
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            error(lineNo, L"expected key = value");
            continue;
        }
        if (!cur) {
            error(lineNo, L"setting outside a [section]");
            continue;
        }
        std::wstring key = util::ToLower(Trim(line.substr(0, eq))), val = Trim(line.substr(eq + 1));
        bool ok = true;
        if (key == L"name") cur->name = val;
        else if (key == L"desc") cur->desc = val;
        else if (key == L"icon") cur->icon = static_cast<wchar_t>(wcstoul(val.c_str(), nullptr, 16));
        else if (key == L"admin") ok = ParseBool(val, cur->needsAdmin);
        else if (key == L"safe") ok = ParseBool(val, cur->safe);
        else if (key == L"enabled") ok = ParseBool(val, cur->enabled);
        else if (key == L"kind") {
            std::wstring k = util::ToLower(val);
            cur->recycleBin = k == L"recyclebin";
            cur->external = k == L"external";
            cur->browserData = k == L"cookies" ? BrowserData::Cookies : k == L"history" ? BrowserData::History : BrowserData::None;
            if (k == L"command") commandKind.insert(cur->id);
            ok = cur->recycleBin || cur->external || cur->browserData != BrowserData::None || k == L"files" || k == L"command";
        } else if (key == L"command") {
            cur->command = val;
        } else if (key == L"path") {
            CleanTarget t;
            size_t bar = val.find(L'|');
            t.path = Trim(val.substr(0, bar));
            while (bar != std::wstring::npos) {
                size_t next = val.find(L'|', bar + 1);
                std::wstring opt = Trim(val.substr(bar + 1, next == std::wstring::npos ? std::wstring::npos : next - bar - 1));
                bar = next;
                size_t oe = opt.find(L'=');
                std::wstring ok2 = util::ToLower(Trim(opt.substr(0, oe))), ov = oe == std::wstring::npos ? L"" : Trim(opt.substr(oe + 1));
                if (ok2 == L"files" && !ov.empty()) t.filePattern = ov;
                else if (ok2 == L"age" && ParseAge(ov, t.minAgeHours)) {}
                else error(lineNo, L"unknown path option \"" + opt + L"\"");
            }
            // A cleaner must name a folder, never a whole drive.
            if (IsDriveRoot(t.path)) {
                error(lineNo, L"path must be a folder, not a drive, e.g. %LOCALAPPDATA%\\App\\Cache");
                continue;
            }
            cur->targets.push_back(t);
        } else {
            error(lineNo, L"unknown key \"" + key + L"\"");
            continue;
        }
        if (!ok) error(lineNo, L"bad value for " + key + L": \"" + val + L"\"");
    }
    // Drop incomplete categories (but keep "enabled = false" stubs: they switch built-ins off).
    for (size_t i = 0; i < out.size();) {
        CleanCategory& c = out[i];
        bool stub = !c.enabled;
        if (!stub && c.name.empty()) {
            errors.push_back(L"[" + c.id + L"]: missing name");
            out.erase(out.begin() + i);
        } else if (!stub && !c.recycleBin && c.targets.empty()) {
            errors.push_back(L"[" + c.id + L"]: no path");
            out.erase(out.begin() + i);
        } else if (!stub && commandKind.count(c.id) != !c.command.empty()) {
            errors.push_back(L"[" + c.id + (c.command.empty() ? L"]: kind = command needs a command" : L"]: command needs kind = command"));
            out.erase(out.begin() + i);
        } else {
            if (!c.icon) c.icon = 0xE74D;
            ++i;
        }
    }
    return out;
}

std::wstring UserCleanersFile(bool create) {
    std::wstring dir = util::DataDir();
    std::wstring file = dir.empty() ? L"" : dir + L"\\cleaners.ini";
    if (create && !file.empty() && GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(file.substr(0, file.rfind(L'\\')).c_str(), nullptr);
        FILE* f = nullptr;
        if (_wfopen_s(&f, file.c_str(), L"wb") == 0 && f) {
            fwrite(kUserTemplate, 1, sizeof(kUserTemplate) - 1, f);
            fclose(f);
        }
    }
    return file;
}

std::vector<CleanCategory> BuildCleanupCategories(std::vector<std::wstring>* errors) {
    std::vector<std::wstring> errs;
    // Tests: WINNCDU_CLEANERS names a cleaners.ini that replaces all rules (built-in, yours and
    // winapp2.ini), so a test can clean its own fixture folder and nothing else.
    wchar_t over[MAX_PATH];
    DWORD on = GetEnvironmentVariableW(L"WINNCDU_CLEANERS", over, MAX_PATH);
    if (on && on < MAX_PATH) {
        bool found = false;
        std::wstring text = Utf8File(over, found);
        std::vector<CleanCategory> v = ParseCleaners(text, errs);
        if (!found) errs.push_back(std::wstring(over) + L": not found");
        v.erase(std::remove_if(v.begin(), v.end(), [](const CleanCategory& c) { return !c.enabled; }), v.end());
        if (errors) *errors = errs;
        return v;
    }
    std::vector<CleanCategory> v = ParseCleaners(BuiltinCleaners(), errs);
    for (auto& e : errs) e = L"built-in cleaners.ini " + e;
    bool found = false;
    std::wstring user = Utf8File(UserCleanersFile(false), found);
    if (found) {
        std::vector<std::wstring> uerrs;
        for (auto& c : ParseCleaners(user, uerrs)) {
            c.user = true;
            auto it = std::find_if(v.begin(), v.end(), [&](const CleanCategory& b) { return b.id == c.id; });
            if (it != v.end() && !c.enabled)
                v.erase(it);
            else if (it != v.end())
                *it = c;
            else if (c.enabled)
                v.push_back(c);
        }
        for (auto& e : uerrs) errs.push_back(L"cleaners.ini " + e);
    }
    v.erase(std::remove_if(v.begin(), v.end(), [](const CleanCategory& c) { return !c.enabled; }), v.end());
    // An imported winapp2.ini, parsed again only when the file changes (it has thousands of entries).
    std::wstring w2 = Winapp2File();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!w2.empty() && GetFileAttributesExW(w2.c_str(), GetFileExInfoStandard, &fa)) {
        static int64_t cachedTime = -1;
        static std::vector<CleanCategory>* cached = new std::vector<CleanCategory>();
        if (FtToI(fa.ftLastWriteTime) != cachedTime) {
            Winapp2Stats st;
            *cached = ParseWinapp2(Utf8File(w2, found), st);
            cachedTime = FtToI(fa.ftLastWriteTime);
        }
        auto covered = std::make_shared<std::vector<CleanExclude>>();
        for (auto& c : v)
            if (!c.recycleBin && !c.external && c.browserData == BrowserData::None)
                for (auto& t : c.targets) covered->push_back({t.path, t.filePattern.empty() ? L"*" : t.filePattern, t.filePattern.empty()});
        size_t builtins = v.size();
        for (auto& c : *cached)
            if (std::none_of(v.begin(), v.begin() + builtins, [&](const CleanCategory& b) { return b.id == c.id; })) {
                v.push_back(c);
                v.back().coveredBy = covered;
            }
    }
    if (errors) *errors = errs;
    return v;
}

std::vector<std::wstring> ExpandedPaths(const CleanCategory& c) {
    std::vector<std::wstring> out;
    for (auto& t : c.targets) Expand(Env(t.path), out);
    return out;
}

namespace {

// The program of a command line: its first word, quotes allowed. `end`: where the arguments start
// in the trimmed command.
std::wstring FirstWord(const std::wstring& command, size_t* end = nullptr) {
    std::wstring s = Trim(command);
    size_t stop;
    std::wstring w;
    if (!s.empty() && s[0] == L'"') {
        stop = s.find(L'"', 1);
        w = s.substr(1, stop == std::wstring::npos ? std::wstring::npos : stop - 1);
        stop = stop == std::wstring::npos ? s.size() : stop + 1;
    } else {
        stop = s.find_first_of(L" \t");
        if (stop == std::wstring::npos) stop = s.size();
        w = s.substr(0, stop);
    }
    if (end) *end = stop;
    return w;
}

bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

std::wstring FindTool(const std::wstring& command) {
    std::wstring prog = Env(FirstWord(command));
    if (prog.empty()) return L"";
    size_t slash = prog.find_last_of(L"\\/");
    bool hasExt = prog.find(L'.', slash == std::wstring::npos ? 0 : slash) != std::wstring::npos;
    const wchar_t* exts[] = {L"", L".exe", L".cmd", L".bat", L".com"};
    auto tryAt = [&](const std::wstring& base) -> std::wstring {
        for (const wchar_t* e : exts) {
            if (hasExt && *e) break;
            if (IsFile(base + e)) return base + e;
        }
        return L"";
    };
    if (slash != std::wstring::npos) return tryAt(prog);  // a path: used as it is, never searched
    DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring path(n, L'\0');
    path.resize(GetEnvironmentVariableW(L"PATH", path.data(), n));
    for (size_t i = 0; i <= path.size();) {
        size_t j = path.find(L';', i);
        if (j == std::wstring::npos) j = path.size();
        std::wstring dir = Trim(path.substr(i, j - i));
        i = j + 1;
        if (dir.size() > 1 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
        // Only absolute folders: a relative PATH entry would resolve against the current folder.
        if (dir.size() < 3 || !(dir[1] == L':' || dir.rfind(L"\\\\", 0) == 0)) continue;
        if (dir.back() != L'\\') dir += L'\\';
        std::wstring hit = tryAt(dir + prog);
        if (!hit.empty()) return hit;
    }
    return L"";
}

int RunCommand(const std::wstring& command, std::atomic<bool>& cancel, std::wstring& err) {
    std::wstring tool = FindTool(command);
    if (tool.empty()) {
        err = FirstWord(command) + L" was not found. Is it installed and on PATH?";
        return -1;
    }
    std::wstring trimmed = Trim(command);
    size_t argsAt = 0;
    FirstWord(trimmed, &argsAt);
    std::wstring args = argsAt < trimmed.size() ? trimmed.substr(argsAt) : L"";
    std::wstring ext = util::ToLower(tool.substr(tool.size() >= 4 ? tool.size() - 4 : 0));
    std::wstring app, line;
    if (ext == L".cmd" || ext == L".bat") {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        app = std::wstring(sys) + L"\\cmd.exe";
        line = L"\"" + app + L"\" /d /s /c \"\"" + tool + L"\"" + args + L"\"";
    } else {
        app = tool;
        line = L"\"" + tool + L"\"" + args;
    }
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(0);
    wchar_t tmp[MAX_PATH];
    std::wstring cwd(tmp, GetTempPathW(MAX_PATH, tmp));  // not the exe's folder
    if (!CreateProcessW(app.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
        err = L"Couldn't run " + tool + L": " + util::Win32ErrorMessage(GetLastError());
        return -1;
    }
    while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT)
        if (cancel) {
            TerminateProcess(pi.hProcess, 1);
            err = L"Cancelled";
            break;
        }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

bool DetectPresent(const std::wstring& what) {
    static const std::pair<const wchar_t*, HKEY> kRoots[] = {
        {L"HKCU", HKEY_CURRENT_USER}, {L"HKEY_CURRENT_USER", HKEY_CURRENT_USER}, {L"HKLM", HKEY_LOCAL_MACHINE},
        {L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE}, {L"HKCR", HKEY_CLASSES_ROOT}, {L"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT},
        {L"HKU", HKEY_USERS}, {L"HKEY_USERS", HKEY_USERS}};
    size_t slash = what.find(L'\\');
    std::wstring head = what.substr(0, slash);
    for (auto& r : kRoots) {
        if (_wcsicmp(head.c_str(), r.first) != 0) continue;
        std::wstring sub = slash == std::wstring::npos ? L"" : what.substr(slash + 1);
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            HKEY k = nullptr;
            if (RegOpenKeyExW(r.second, sub.c_str(), 0, KEY_READ | view, &k) == ERROR_SUCCESS) {
                RegCloseKey(k);
                return true;
            }
        }
        return false;
    }
    std::wstring p = Env(what);
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    bool absolute = (p.size() >= 3 && p[1] == L':' && p[2] == L'\\') || p.rfind(L"\\\\", 0) == 0;
    return absolute && p.find(L'%') == std::wstring::npos && GlobExists(p);
}

namespace {

// kind = cookies / history: one entry per database of each profile folder. The profiles are
// browser profiles, which the protection rules mark Important; these categories exist to clean
// exactly that data, opt-in and reviewed, so only Blocked folders (yours, in protect.txt) are left out.
void MeasureBrowserData(CleanCategory& c, std::atomic<bool>& cancel) {
    std::vector<std::wstring> dirs;
    for (auto& t : c.targets) Expand(Env(t.path), dirs);
    dirs.erase(std::remove_if(dirs.begin(), dirs.end(), [&](const std::wstring& d) {
                   bool blocked = IsDriveRoot(d) || CheckProtection(d).level >= Protect::Blocked;
                   c.guarded += blocked ? 1 : 0;
                   return blocked;
               }),
               dirs.end());
    if (c.browserData == BrowserData::Cookies) {
        // The first time, the list starts with the most visited sites, so cleaning cookies doesn't
        // sign you out of the sites you use every day unless you say so.
        std::wstring file = KeepSignedInFile();
        if (!file.empty() && GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
            bool any = std::any_of(dirs.begin(), dirs.end(), [&](const std::wstring& d) { return !InspectProfile(d, BrowserData::Cookies, {}).empty(); });
            if (any) CreateKeepList(file, MostVisitedSites(dirs, 15));
        }
        c.keep = LoadKeepList(file);
    }
    for (auto& d : dirs) {
        if (cancel) return;
        std::wstring err;
        for (auto& b : InspectProfile(d, c.browserData, c.keep, &err)) {
            if (!b.rows) continue;
            CleanEntry e;
            e.path = b.file;
            e.bytes = b.bytes;
            e.files = b.rows;
            e.mtime = b.mtime;
            if (c.browserData == BrowserData::Cookies)
                e.note = util::FormatCount(b.rows) + (b.rows == 1 ? L" cookie" : L" cookies") +
                         (b.keptRows ? L"  ·  " + util::FormatCount(b.keptRows) + L" kept for " + util::FormatCount(b.keptSites) +
                                           (b.keptSites == 1 ? L" site" : L" sites")
                                     : L"");
            else
                e.note = util::FormatCount(b.rows) + (b.rows == 1 ? L" visit" : L" visits") +
                         (b.firefox ? L"  ·  bookmarks stay" : L"");
            c.bytes += e.bytes;
            c.files += e.files;
            c.entries.push_back(std::move(e));
        }
        if (!err.empty()) c.denied = true;
    }
}

}  // namespace

uint64_t CleanCategory::KeptBytes() const {
    uint64_t n = 0;
    for (auto& e : entries)
        if (e.keep) n += e.bytes;
    return n;
}

size_t CleanCategory::KeptCount() const {
    size_t n = 0;
    for (auto& e : entries) n += e.keep ? 1 : 0;
    return n;
}

uint64_t CleanCategory::ToClean() const {
    uint64_t kept = KeptBytes();
    return bytes > kept ? bytes - kept : 0;
}

void MeasureCategory(CleanCategory& c, std::atomic<bool>& cancel) {
    c.bytes = c.files = c.guarded = 0;
    c.denied = false;
    c.present = true;
    c.entries.clear();
    if (c.recycleBin) {
        SHQUERYRBINFO qi{sizeof(qi)};
        if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &qi))) {
            c.bytes = static_cast<uint64_t>(qi.i64Size);
            c.files = static_cast<uint64_t>(qi.i64NumItems);
        }
        for (auto& it : ListRecycleBin()) {
            CleanEntry e;
            e.path = it.original;
            e.binData = it.dataFile;
            e.binInfo = it.infoFile;
            e.bytes = it.size;
            e.files = 1;
            e.mtime = it.deleted;
            DWORD a = GetFileAttributesW(ToLongPath(it.dataFile).c_str());
            e.dir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
            c.entries.push_back(std::move(e));
        }
    } else if (c.browserData != BrowserData::None) {
        MeasureBrowserData(c, cancel);
    } else {
        c.toolMissing = !c.command.empty() && FindTool(c.command).empty();
        if (!c.detect.empty()) {
            c.present = std::any_of(c.detect.begin(), c.detect.end(), [](const std::wstring& d) { return DetectPresent(d); });
            if (!c.present) {
                c.measured = true;
                return;
            }
        }
        ProtectNames names;
        std::vector<Excl> excl = PrepareExcludes(c.excludes);
        std::vector<Excl> covered = c.coveredBy ? PrepareExcludes(*c.coveredBy) : std::vector<Excl>();
        Protect guard = c.imported ? Protect::Caution : Protect::Important;
        std::unordered_set<std::wstring> seen;  // a file can match several rules of one category
        for (size_t ti = 0; ti < c.targets.size(); ++ti) {
            const CleanTarget& t = c.targets[ti];
            std::vector<std::wstring> dirs;
            Expand(Env(t.path), dirs);
            for (auto& d : dirs) {
                if (cancel) return;
                if (IsDriveRoot(d)) continue;  // e.g. an undefined %VAR% at the start of a rule
                if (c.external) {
                    WIN32_FIND_DATAW top{};
                    top.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                    SizeOf(d, top, c.bytes, c.files, c.denied, cancel);
                    continue;
                }
                bool any = false;
                auto add = [&](const std::wstring& p, const WIN32_FIND_DATAW& fd) {
                    any = true;
                    if (!seen.insert(util::ToLower(p)).second || (!excl.empty() && Excluded(excl, p))) return;
                    if (!covered.empty() && (Excluded(covered, p) || ExcludedFolder(covered, p))) return;
                    if ((c.imported && PersonalName(fd.cFileName)) || CheckProtection(p).level >= guard) {
                        ++c.guarded;
                        return;
                    }
                    CleanEntry e;
                    e.path = p;
                    e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    e.mtime = FtToI(fd.ftLastWriteTime);
                    e.target = static_cast<int>(ti);
                    if (!SizeOf(p, fd, e.bytes, e.files, c.denied, cancel, &names, c.imported)) {
                        ++c.guarded;
                        return;
                    }
                    c.bytes += e.bytes;
                    c.files += e.files;
                    c.entries.push_back(std::move(e));
                };
                // With excludes, whole folders can't be taken: something inside may be excluded.
                bool fileLevel = !excl.empty();
                if (t.filePattern.empty() && t.removeSelf && !fileLevel) {
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileExW(ToLongPath(d).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
                    if (h != INVALID_HANDLE_VALUE) {
                        FindClose(h);
                        add(d, fd);
                    }
                } else if ((!t.filePattern.empty() && (t.recurse || t.removeSelf)) || (t.filePattern.empty() && fileLevel)) {
                    ForEachFileDeep(t, t.filePattern.empty() ? L"*" : t.filePattern, d, names, c.denied, cancel, add);
                } else {
                    ForEachEntry(t, d, add);
                }
                if (!any && GetLastError() == ERROR_ACCESS_DENIED) c.denied = true;
            }
        }
        // Rules can overlap (a whole cache folder, and log files in it): an entry inside a folder
        // entry is already covered by it.
        std::unordered_set<std::wstring> dirs;
        for (auto& e : c.entries)
            if (e.dir) dirs.insert(util::ToLower(e.path));
        if (!dirs.empty()) {
            auto inDirEntry = [&](const CleanEntry& e) {
                std::wstring p = util::ToLower(e.path);
                for (size_t s = p.rfind(L'\\'); s != std::wstring::npos && s > 0; s = p.rfind(L'\\', s - 1))
                    if (dirs.count(p.substr(0, s))) return true;
                return false;
            };
            c.entries.erase(std::remove_if(c.entries.begin(), c.entries.end(), inDirEntry), c.entries.end());
            c.bytes = c.files = 0;
            for (auto& e : c.entries) {
                c.bytes += e.bytes;
                c.files += e.files;
            }
        }
    }
    std::stable_sort(c.entries.begin(), c.entries.end(), [](const CleanEntry& a, const CleanEntry& b) { return a.bytes > b.bytes; });
    // Imported entries name many folders, most of them absent: say where the bulk actually is.
    if (c.imported && !c.entries.empty() && c.desc.rfind(L"Note: ", 0) != 0) {
        const CleanEntry& big = c.entries.front();
        std::wstring where = big.dir ? big.path : big.path.substr(0, big.path.rfind(L'\\'));
        size_t sep = c.desc.find(L"  ·  ");
        c.desc = (sep == std::wstring::npos ? L"" : c.desc.substr(0, sep + 5)) + where;
    }
    c.measured = true;
}

CleanResult CleanCategoryNow(const CleanCategory& c, std::atomic<bool>& cancel,
                             const std::function<void(const std::wstring&)>& onItem) {
    CleanResult r;
    if (c.external) {
        ShellExecuteW(nullptr, L"open", L"cleanmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
        return r;
    }
    if (!c.command.empty()) {
        // The tool cleans its own cache; what it freed is measured before and after.
        onItem(L"Running " + c.command);
        int code = RunCommand(c.command, cancel, r.error);
        if (code != 0 && r.error.empty()) r.error = c.command + L" failed (exit code " + std::to_wstring(code) + L")";
        CleanCategory after = c;
        std::atomic<bool> measureAll{false};
        MeasureCategory(after, measureAll);
        r.freed = c.bytes > after.bytes ? c.bytes - after.bytes : 0;
        r.files = c.files > after.files ? c.files - after.files : 0;
        return r;
    }
    if (c.browserData != BrowserData::None) {
        // Kept sites: those on the list when it was measured, and any added since.
        std::vector<std::wstring> keep = c.keep;
        if (c.browserData == BrowserData::Cookies)
            for (auto& k : LoadKeepList(KeepSignedInFile()))
                if (std::find(keep.begin(), keep.end(), k) == keep.end()) keep.push_back(k);
        for (auto& e : c.entries) {
            if (cancel) break;
            if (e.keep) continue;
            onItem(e.path);
            if (CheckProtection(e.path).level >= Protect::Blocked) {
                ++r.failed;
                continue;
            }
            if (!PathExists(e.path)) continue;
            // A running browser holds its databases open, and would undo or trip over the change.
            std::vector<AppUsingFile> apps = AppsUsing(e.path);
            if (apps.empty()) {
                BrowserCleanResult br;
                std::wstring err;
                if (CleanBrowserDb(e.path, c.browserData, keep, br, err)) {
                    r.freed += br.freed;
                    r.files += br.rows;
                    continue;
                }
                if (!br.busy && r.error.empty()) r.error = e.path + L": " + err;
                if (br.busy) apps = AppsUsing(e.path);
            }
            ++r.failed;
            for (auto& a : apps)
                if (std::find(r.lockedBy.begin(), r.lockedBy.end(), a.name) == r.lockedBy.end()) r.lockedBy.push_back(a.name);
        }
        return r;
    }
    if (c.recycleBin && c.KeptCount() == 0) {
        onItem(L"Emptying the Recycle Bin");
        SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        SHQUERYRBINFO qi{sizeof(qi)};
        uint64_t left = SUCCEEDED(SHQueryRecycleBinW(nullptr, &qi)) ? static_cast<uint64_t>(qi.i64Size) : 0;
        r.freed = c.bytes > left ? c.bytes - left : 0;
        r.files = c.files;
        return r;
    }
    int64_t now = util::NowFileTime();
    std::atomic<uint64_t> removed{0};
    for (auto& e : c.entries) {
        if (cancel) break;
        if (e.keep) continue;
        onItem(e.path);
        std::wstring err;
        if (c.recycleBin) {
            // Some items are kept, so the bin is emptied item by item: the $R file (or folder) is
            // the item itself, the $I file its record.
            if (DeletePermanently(e.binData, cancel, removed, err)) {
                DeleteFileW(ToLongPath(e.binInfo).c_str());
                r.freed += e.bytes;
                r.files += e.files;
            } else {
                ++r.failed;
            }
            continue;
        }
        // Safety net, also for user rules: never reach protected data.
        if (CheckProtection(e.path).level >= (c.imported ? Protect::Caution : Protect::Important)) {
            ++r.failed;
            continue;
        }
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(ToLongPath(e.path).c_str(), GetFileExInfoStandard, &fa)) continue;  // already gone
        // Changed since it was measured (e.g. a temp file in use again): it no longer qualifies.
        int minAge = e.target >= 0 && e.target < static_cast<int>(c.targets.size()) ? c.targets[e.target].minAgeHours : 0;
        if (minAge && FtToI(fa.ftLastWriteTime) > now - static_cast<int64_t>(minAge) * 36000000000LL) continue;
        if (DeletePermanently(e.path, cancel, removed, err)) {
            r.freed += e.bytes;
            r.files += e.files;
            continue;
        }
        ++r.failed;
        if (r.failed <= 20)  // name the apps in the way (checking every failure would take long)
            for (auto& a : AppsUsing(e.path, 200))
                if (std::find(r.lockedBy.begin(), r.lockedBy.end(), a.name) == r.lockedBy.end()) r.lockedBy.push_back(a.name);
        // Partly deleted: count what is gone.
        uint64_t leftBytes = 0, leftFiles = 0;
        bool denied = false;
        WIN32_FIND_DATAW top{};
        top.dwFileAttributes = fa.dwFileAttributes;
        top.nFileSizeHigh = fa.nFileSizeHigh;
        top.nFileSizeLow = fa.nFileSizeLow;
        if (GetFileAttributesW(ToLongPath(e.path).c_str()) != INVALID_FILE_ATTRIBUTES)
            SizeOf(e.path, top, leftBytes, leftFiles, denied, cancel);
        r.freed += e.bytes > leftBytes ? e.bytes - leftBytes : 0;
        r.files += e.files > leftFiles ? e.files - leftFiles : 0;
    }
    return r;
}

CleanResult CleanAndJournal(const CleanCategory& c, std::atomic<bool>& cancel, const std::function<void(const std::wstring&)>& onItem,
                            const std::wstring& via) {
    CleanResult r = CleanCategoryNow(c, cancel, onItem);
    if (c.external) return r;
    JournalEntry je;
    je.action = JournalAction::Clean;
    je.bytes = r.freed;
    je.items = r.files;
    je.path = c.name;
    std::wstring note;
    if (r.failed) note = util::FormatCount(r.failed) + L" items in use were skipped";
    if (!r.lockedBy.empty()) note += L" (by " + AppNames(r.lockedBy) + L")";
    if (size_t kept = c.KeptCount()) note += (note.empty() ? L"" : L"; ") + util::FormatCount(kept) + L" kept";
    if (c.browserData == BrowserData::Cookies)
        note += (note.empty() ? L"" : L"; ") + util::FormatCount(r.files) + L" cookies, " +
                util::FormatCount(c.keep.size()) + L" sites kept";
    if (c.browserData == BrowserData::History) note += (note.empty() ? L"" : L"; ") + util::FormatCount(r.files) + L" visits";
    if (!c.command.empty()) note += (note.empty() ? L"ran " : L"; ran ") + c.command + (r.error.empty() ? L"" : L": " + r.error);
    if (!via.empty()) note += (note.empty() ? L"" : L"; ") + via;
    je.note = note;
    JournalAppend(je);
    return r;
}
