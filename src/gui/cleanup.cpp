#include "cleanup.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>

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

// Visits the top-level entries of a target (respecting the file pattern and the minimum age).
template <class F>
void ForEachEntry(const CleanTarget& t, const std::wstring& dir, F&& f) {
    std::wstring pattern = t.filePattern.empty() ? L"*" : t.filePattern;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((ToLongPath(dir) + L"\\" + pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    int64_t cutoff = t.minAgeHours ? util::NowFileTime() - static_cast<int64_t>(t.minAgeHours) * 36000000000LL : 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!t.filePattern.empty() && isDir) continue;
        if (cutoff && FtToI(fd.ftLastWriteTime) > cutoff) continue;
        f(dir + L"\\" + fd.cFileName, fd);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Size and file count of a path (recursive, links not followed). With `names`, returns false as
// soon as a name rule matches anything inside (a Git repository, a password database, a virtual
// disk, ...): deleting the folder by hand only asks for care, but Cleanup never takes it along.
bool SizeOf(const std::wstring& path, const WIN32_FIND_DATAW& top, uint64_t& bytes, uint64_t& files, bool& denied,
            std::atomic<bool>& cancel, const ProtectNames* names = nullptr) {
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
            if (names && names->Contains(fd.cFileName, isDir) != Protect::None) {
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

std::wstring Trim(const std::wstring& v) {
    size_t a = v.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = v.find_last_not_of(L" \t\r\n");
    return v.substr(a, b - a + 1);
}

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
    "# Turn a built-in category off (ids: recycle, temp, wintemp, browser, thumbs, crash, wu, do,\r\n"
    "# shader, dev, winold):\r\n"
    "# [dev]\r\n"
    "# enabled = false\r\n"
    "#\r\n"
    "# Protected items (see Protected locations) are never listed or deleted, whatever a rule says.\r\n";

}  // namespace

std::vector<CleanCategory> ParseCleaners(const std::wstring& text, std::vector<std::wstring>& errors) {
    std::vector<CleanCategory> out;
    CleanCategory* cur = nullptr;
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
            ok = cur->recycleBin || cur->external || k == L"files";
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
        } else {
            if (!c.icon) c.icon = 0xE74D;
            ++i;
        }
    }
    return out;
}

std::wstring UserCleanersFile(bool create) {
    PWSTR p = nullptr;
    std::wstring file;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) file = std::wstring(p) + L"\\WinNCDU\\cleaners.ini";
    CoTaskMemFree(p);
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
    if (errors) *errors = errs;
    return v;
}

std::vector<std::wstring> ExpandedPaths(const CleanCategory& c) {
    std::vector<std::wstring> out;
    for (auto& t : c.targets) Expand(Env(t.path), out);
    return out;
}

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
    } else {
        ProtectNames names;
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
                ForEachEntry(t, d, [&](const std::wstring& p, const WIN32_FIND_DATAW& fd) {
                    any = true;
                    if (CheckProtection(p).level >= Protect::Important) {
                        ++c.guarded;
                        return;
                    }
                    CleanEntry e;
                    e.path = p;
                    e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    e.mtime = FtToI(fd.ftLastWriteTime);
                    e.target = static_cast<int>(ti);
                    if (!SizeOf(p, fd, e.bytes, e.files, c.denied, cancel, &names)) {
                        ++c.guarded;
                        return;
                    }
                    c.bytes += e.bytes;
                    c.files += e.files;
                    c.entries.push_back(std::move(e));
                });
                if (!any && GetLastError() == ERROR_ACCESS_DENIED) c.denied = true;
            }
        }
    }
    std::stable_sort(c.entries.begin(), c.entries.end(), [](const CleanEntry& a, const CleanEntry& b) { return a.bytes > b.bytes; });
    c.measured = true;
}

CleanResult CleanCategoryNow(const CleanCategory& c, std::atomic<bool>& cancel,
                             const std::function<void(const std::wstring&)>& onItem) {
    CleanResult r;
    if (c.external) {
        ShellExecuteW(nullptr, L"open", L"cleanmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
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
        if (CheckProtection(e.path).level >= Protect::Important) {
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
