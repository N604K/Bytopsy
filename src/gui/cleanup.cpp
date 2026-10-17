#include "cleanup.h"

#include <windows.h>
#include <shellapi.h>

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

// Expands '*' path segments against the file system (directories only).
void Expand(const std::wstring& path, std::vector<std::wstring>& out) {
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

// Size and file count of a path (recursive, links not followed).
void SizeOf(const std::wstring& path, const WIN32_FIND_DATAW& top, uint64_t& bytes, uint64_t& files, bool& denied,
            std::atomic<bool>& cancel) {
    if (!(top.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        bytes += (static_cast<uint64_t>(top.nFileSizeHigh) << 32) | top.nFileSizeLow;
        ++files;
        return;
    }
    if (top.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return;
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
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(d + L"\\" + fd.cFileName);
            } else {
                bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                ++files;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

CleanCategory Cat(const wchar_t* id, const wchar_t* name, const wchar_t* desc, wchar_t icon, std::vector<CleanTarget> t,
                  bool admin = false, bool safe = true) {
    CleanCategory c;
    c.id = id;
    c.name = name;
    c.desc = desc;
    c.icon = icon;
    c.targets = std::move(t);
    c.needsAdmin = admin;
    c.safe = safe;
    return c;
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

std::vector<CleanCategory> BuildCleanupCategories() {
    std::vector<CleanCategory> v;
    {
        CleanCategory c = Cat(L"recycle", L"Recycle Bin", L"Files you deleted earlier still use space until the bin is emptied.",
                              0xE74D, {});
        c.recycleBin = true;
        v.push_back(c);
    }
    v.push_back(Cat(L"temp", L"Temporary files", L"Leftovers from apps and installers (older than 1 day).", 0xE7C3,
                    {{L"%TEMP%", L"", 24}}));
    v.push_back(Cat(L"wintemp", L"Windows temporary files", L"System temp folder (older than 1 day).", 0xE7C3,
                    {{L"%SystemRoot%\\Temp", L"", 24}}, true));
    v.push_back(Cat(L"browser", L"Browser caches",
                    L"Chrome, Edge, Brave, Vivaldi, Opera and Firefox caches. Close the browsers first for best results.", 0xE774,
                    {{L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Cache"},
                     {L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Code Cache"},
                     {L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\GPUCache"},
                     {L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\Cache"},
                     {L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\Code Cache"},
                     {L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\GPUCache"},
                     {L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\*\\Cache"},
                     {L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\*\\Code Cache"},
                     {L"%LOCALAPPDATA%\\Vivaldi\\User Data\\*\\Cache"},
                     {L"%LOCALAPPDATA%\\Opera Software\\Opera Stable\\Cache"},
                     {L"%LOCALAPPDATA%\\Mozilla\\Firefox\\Profiles\\*\\cache2"}}));
    v.push_back(Cat(L"thumbs", L"Thumbnail cache", L"Explorer rebuilds thumbnails automatically when needed.", 0xE91B,
                    {{L"%LOCALAPPDATA%\\Microsoft\\Windows\\Explorer", L"thumbcache_*.db"},
                     {L"%LOCALAPPDATA%\\Microsoft\\Windows\\Explorer", L"iconcache_*.db"}}));
    v.push_back(Cat(L"crash", L"Crash dumps & error reports", L"Memory dumps and Windows Error Reporting archives.", 0xE7BA,
                    {{L"%LOCALAPPDATA%\\CrashDumps"},
                     {L"%LOCALAPPDATA%\\Microsoft\\Windows\\WER\\ReportArchive"},
                     {L"%LOCALAPPDATA%\\Microsoft\\Windows\\WER\\ReportQueue"},
                     {L"%ProgramData%\\Microsoft\\Windows\\WER\\ReportArchive"},
                     {L"%ProgramData%\\Microsoft\\Windows\\WER\\ReportQueue"},
                     {L"%SystemRoot%\\Minidump"},
                     {L"%SystemRoot%\\LiveKernelReports"},
                     {L"%SystemRoot%", L"MEMORY.DMP"}}));
    v.push_back(Cat(L"wu", L"Windows Update downloads", L"Update packages that were already installed.", 0xE895,
                    {{L"%SystemRoot%\\SoftwareDistribution\\Download", L"", 24}}, true));
    v.push_back(Cat(L"do", L"Delivery Optimization cache", L"Update pieces shared with other PCs.", 0xE895,
                    {{L"%SystemRoot%\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization\\Cache"}},
                    true));
    v.push_back(Cat(L"shader", L"Graphics shader caches", L"DirectX / NVIDIA / AMD shader caches; games rebuild them.", 0xE7F4,
                    {{L"%LOCALAPPDATA%\\D3DSCache"},
                     {L"%LOCALAPPDATA%\\NVIDIA\\DXCache"},
                     {L"%LOCALAPPDATA%\\NVIDIA\\GLCache"},
                     {L"%LOCALAPPDATA%\\AMD\\DxCache"},
                     {L"%LOCALAPPDATA%\\AMD\\DxcCache"}}));
    v.push_back(Cat(L"dev", L"Developer package caches",
                    L"npm, Yarn, pnpm, pip, NuGet, Gradle, Go and Cargo caches. Packages download again when needed.", 0xE943,
                    {{L"%LOCALAPPDATA%\\npm-cache"},
                     {L"%APPDATA%\\npm-cache"},
                     {L"%LOCALAPPDATA%\\Yarn\\Cache"},
                     {L"%LOCALAPPDATA%\\pnpm\\store"},
                     {L"%LOCALAPPDATA%\\pip\\Cache"},
                     {L"%USERPROFILE%\\.nuget\\packages"},
                     {L"%USERPROFILE%\\.gradle\\caches"},
                     {L"%LOCALAPPDATA%\\go-build"},
                     {L"%USERPROFILE%\\.cargo\\registry\\cache"}},
                    false, false));
    {
        CleanCategory c = Cat(L"winold", L"Previous Windows installation",
                              L"Windows.old from an upgrade. Removed with Windows Disk Cleanup.", 0xE7F8,
                              {{L"%SystemDrive%\\Windows.old"}}, true, false);
        c.external = true;
        v.push_back(c);
    }
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
    c.bytes = c.files = 0;
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
        for (size_t ti = 0; ti < c.targets.size(); ++ti) {
            const CleanTarget& t = c.targets[ti];
            std::vector<std::wstring> dirs;
            Expand(Env(t.path), dirs);
            for (auto& d : dirs) {
                if (cancel) return;
                if (c.external) {
                    WIN32_FIND_DATAW top{};
                    top.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                    SizeOf(d, top, c.bytes, c.files, c.denied, cancel);
                    continue;
                }
                bool any = false;
                ForEachEntry(t, d, [&](const std::wstring& p, const WIN32_FIND_DATAW& fd) {
                    any = true;
                    CleanEntry e;
                    e.path = p;
                    e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    e.mtime = FtToI(fd.ftLastWriteTime);
                    e.target = static_cast<int>(ti);
                    SizeOf(p, fd, e.bytes, e.files, c.denied, cancel);
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
        // Safety net: categories are curated, but never let one reach a blocked location.
        if (CheckProtection(e.path).level == Protect::Blocked) {
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
