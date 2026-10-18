#include "fsops.h"

#include <windows.h>
#include <winternl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <restartmanager.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "tree.h"
#include "i18n.h"
#include "util.h"

namespace {

template <class T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::wstring HrMessage(HRESULT hr) { return util::Win32ErrorMessage(static_cast<unsigned long>(hr)); }

bool QueryDrive(DriveInfo& d) {
    ULARGE_INTEGER avail{}, total{}, totalFree{};
    if (!GetDiskFreeSpaceExW(d.root.c_str(), &avail, &total, &totalFree)) return false;
    d.ready = true;
    d.total = total.QuadPart;
    d.free = avail.QuadPart;
    wchar_t label[MAX_PATH + 1] = {}, fs[MAX_PATH + 1] = {};
    if (GetVolumeInformationW(d.root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH)) {
        d.label = label;
        d.fs = fs;
    }
    return true;
}

}  // namespace

const wchar_t* DriveTypeName(unsigned type) {
    switch (type) {
        case DRIVE_REMOVABLE: return N_(L"Removable");
        case DRIVE_FIXED: return N_(L"Local");
        case DRIVE_REMOTE: return N_(L"Network");
        case DRIVE_CDROM: return N_(L"Optical");
        case DRIVE_RAMDISK: return N_(L"RAM disk");
        default: return N_(L"Unknown");
    }
}

std::vector<DriveInfo> ListDrives() {
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    std::vector<std::shared_ptr<DriveInfo>> infos;
    std::vector<std::shared_ptr<std::atomic<bool>>> done;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        auto d = std::make_shared<DriveInfo>();
        d->root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        d->type = GetDriveTypeW(d->root.c_str());
        if (d->type == DRIVE_NO_ROOT_DIR) continue;
        auto flag = std::make_shared<std::atomic<bool>>(false);
        // Query each drive on its own thread so a dead network share can't freeze startup.
        std::thread([d, flag] {
            QueryDrive(*d);
            flag->store(true);
        }).detach();
        infos.push_back(d);
        done.push_back(flag);
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    std::vector<DriveInfo> out;
    for (size_t i = 0; i < infos.size(); ++i) {
        while (!done[i]->load() && std::chrono::steady_clock::now() < deadline) Sleep(10);
        if (done[i]->load())
            out.push_back(*infos[i]);
        else {
            DriveInfo d;
            d.root = infos[i]->root;
            d.type = infos[i]->type;
            d.label = N_(L"(not responding)");
            out.push_back(d);
        }
    }
    SetErrorMode(oldMode);
    return out;
}

bool QueryFreeSpace(const std::wstring& path, uint64_t& total, uint64_t& free) {
    ULARGE_INTEGER avail{}, tot{}, totalFree{};
    std::wstring p = path;
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    if (!GetDiskFreeSpaceExW(p.c_str(), &avail, &tot, &totalFree)) return false;
    total = tot.QuadPart;
    free = avail.QuadPart;
    return true;
}

bool PathExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(ToLongPath(path).c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return true;
    DWORD e = GetLastError();
    // Anything other than "not found" (e.g. access denied) means something is still there.
    return !(e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME ||
             e == ERROR_BAD_NETPATH);
}

std::wstring NormalizeDirPath(const std::wstring& input, std::wstring& err) {
    std::wstring in = input;
    while (!in.empty() && (in.back() == L'"' || in.back() == L' ')) in.pop_back();
    while (!in.empty() && (in.front() == L'"' || in.front() == L' ')) in.erase(in.begin());
    if (in.empty()) {
        err = Tr(L"Empty path");
        return L"";
    }
    for (auto& c : in)
        if (c == L'/') c = L'\\';
    if (in.size() == 2 && in[1] == L':') in += L'\\';
    std::vector<wchar_t> buf(32768);
    DWORD n = GetFullPathNameW(in.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (!n || n >= buf.size()) {
        err = util::Win32ErrorMessage(GetLastError());
        return L"";
    }
    std::wstring p(buf.data(), n);
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0)
        p = L"\\\\" + p.substr(8);
    else if (p.rfind(L"\\\\?\\", 0) == 0)
        p = p.substr(4);
    DWORD a = GetFileAttributesW(ToLongPath(p).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        // Drive roots of some file systems need the trailing backslash form.
        err = util::Win32ErrorMessage(GetLastError());
        return L"";
    }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        err = Tr(L"Not a directory");
        return L"";
    }
    bool isDriveRoot = p.size() == 3 && p[1] == L':';
    if (!isDriveRoot && p.size() > 3 && p.back() == L'\\') {
        // Keep "\\server\share\" style roots intact but trim normal trailing slashes.
        size_t slashes = 0;
        for (wchar_t c : p)
            if (c == L'\\') ++slashes;
        if (!(p.rfind(L"\\\\", 0) == 0 && slashes <= 4)) p.pop_back();
    }
    return p;
}

namespace {

// Handle-based deletion. Every item below the delete target is opened relative to its parent's
// handle, with reparse points opened as themselves, so a folder that is swapped for a junction or
// symlink while we work can never redirect the delete (this matters most when running as
// administrator). Deleting goes through the handle too (FileDispositionInfoEx).

#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif
#ifndef FILE_OPEN_FOR_BACKUP_INTENT
#define FILE_OPEN_FOR_BACKUP_INTENT 0x00004000
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001
#endif

constexpr ACCESS_MASK kDeleteAccess = DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
constexpr ULONG kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// Opens `name` inside the directory `dir` (or `dir` itself again when `name` is empty).
HANDLE OpenRelative(HANDLE dir, const std::wstring& name, ACCESS_MASK access, ULONG options, DWORD& err) {
    UNICODE_STRING us;
    us.Buffer = const_cast<PWSTR>(name.c_str());
    us.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    us.MaximumLength = us.Length;
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, dir, nullptr);
    IO_STATUS_BLOCK io{};
    HANDLE h = nullptr;
    NTSTATUS st = NtCreateFile(&h, access, &oa, &io, nullptr, 0, kShareAll, FILE_OPEN,
                               options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT,
                               nullptr, 0);
    if (st < 0) {
        err = RtlNtStatusToDosError(st);
        return nullptr;
    }
    return h;
}

bool AttrTag(HANDLE h, DWORD& attrs) {
    FILE_ATTRIBUTE_TAG_INFO ti{};
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof(ti))) return false;
    attrs = ti.FileAttributes;
    return true;
}

bool IsRealDir(DWORD attrs) { return (attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT); }

// Marks the open item for deletion; it disappears when the handle is closed (at once, with POSIX
// semantics). Read-only items are deleted too.
bool DeleteByHandle(HANDLE h, DWORD attrs, DWORD& err) {
    FILE_DISPOSITION_INFO_EX ex{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    if (SetFileInformationByHandle(h, FileDispositionInfoEx, &ex, sizeof(ex))) return true;
    err = GetLastError();
    if (err != ERROR_INVALID_PARAMETER && err != ERROR_NOT_SUPPORTED && err != ERROR_INVALID_FUNCTION) return false;
    // Older Windows and file systems without the extended form (FAT, some network shares).
    if (attrs & FILE_ATTRIBUTE_READONLY) {
        FILE_BASIC_INFO bi{};  // zero times: unchanged
        bi.FileAttributes = (attrs & ~FILE_ATTRIBUTE_READONLY) ? (attrs & ~FILE_ATTRIBUTE_READONLY) : FILE_ATTRIBUTE_NORMAL;
        SetFileInformationByHandle(h, FileBasicInfo, &bi, sizeof(bi));
    }
    FILE_DISPOSITION_INFO d{TRUE};
    if (SetFileInformationByHandle(h, FileDispositionInfo, &d, sizeof(d))) return true;
    err = GetLastError();
    return false;
}

// Names in a directory, read completely before anything in it is deleted.
bool ListDirectory(HANDLE h, std::vector<std::wstring>& names, DWORD& err) {
    std::vector<unsigned char> buf(64 * 1024);
    FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
    while (true) {
        if (!GetFileInformationByHandleEx(h, cls, buf.data(), static_cast<DWORD>(buf.size()))) {
            DWORD e = GetLastError();
            if (e == ERROR_NO_MORE_FILES) return true;
            err = e;
            return false;
        }
        cls = FileIdBothDirectoryInfo;
        auto* p = buf.data();
        while (true) {
            auto* info = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(p);
            std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") names.push_back(std::move(name));
            if (!info->NextEntryOffset) break;
            p += info->NextEntryOffset;
        }
    }
}

bool ProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

std::wstring Shown(const std::wstring& p) { return p.rfind(L"\\\\?\\", 0) == 0 ? p.substr(4) : p; }

// True if the opened item `h` is not the one `path` names (a folder above it was swapped for a
// link or mount point). `actual` gets where it really is. Unknown counts as "same".
bool OpenedElsewhere(HANDLE h, const std::wstring& path, std::wstring& actual) {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= buf.size()) return false;
    actual = Shown(std::wstring(buf.data(), n));
    if (actual.rfind(L"UNC\\", 0) == 0) actual = L"\\" + actual.substr(3);
    // Compare long names: the path may carry 8.3 short names (C:\Users\NATIV~1\...).
    std::wstring want = Shown(path);
    n = GetLongPathNameW(ToLongPath(want).c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    if (n && n < buf.size()) want = Shown(std::wstring(buf.data(), n));
    while (want.size() > 3 && want.back() == L'\\') want.pop_back();
    return _wcsicmp(actual.c_str(), want.c_str()) != 0;
}

}  // namespace

bool PathLeadsElsewhere(const std::wstring& path, std::wstring& actual) {
    HANDLE h = CreateFileW(ToLongPath(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool r = OpenedElsewhere(h, path, actual);
    CloseHandle(h);
    return r;
}

bool DeletePermanently(const std::wstring& path, std::atomic<bool>& cancel, std::atomic<uint64_t>& removed,
                       std::wstring& err) {
    std::wstring shown = Shown(path);
    HANDLE root = CreateFileW(ToLongPath(path).c_str(), kDeleteAccess, kShareAll, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (root == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
        err = shown + L": " + util::Win32ErrorMessage(e);
        return false;
    }
    // As administrator, also make sure no folder above the item was swapped for a link since it was
    // chosen: the opened item must still be the one at that path.
    std::wstring actual;
    if (ProcessElevated() && OpenedElsewhere(root, path, actual)) {
        CloseHandle(root);
        err = TrF(L"{0}: this path now leads to {1} (a link or mount point changed). Nothing was deleted.", shown, actual);
        return false;
    }

    DWORD attrs = 0, e = 0;
    if (!AttrTag(root, attrs)) {
        e = GetLastError();
        CloseHandle(root);
        err = shown + L": " + util::Win32ErrorMessage(e);
        return false;
    }
    if (!IsRealDir(attrs)) {
        // Files, and links to directories: remove the entry itself, never the target.
        bool ok = DeleteByHandle(root, attrs, e);
        CloseHandle(root);
        if (!ok) {
            err = shown + L": " + util::Win32ErrorMessage(e);
            return false;
        }
        ++removed;
        return true;
    }

    struct Frame {
        HANDLE h;
        std::wstring path;  // for messages only; never opened by name
        DWORD attrs;
        std::vector<std::wstring> dirs;  // subfolders still to visit
        size_t next = 0;
        bool listed = false;
    };
    std::vector<Frame> stack;
    HANDLE listable = OpenRelative(root, L"", kDeleteAccess | FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE, e);
    CloseHandle(root);
    if (!listable) {
        err = shown + L": " + util::Win32ErrorMessage(e);
        return false;
    }
    stack.push_back({listable, shown, attrs, {}, 0, false});
    bool allOk = true;
    auto fail = [&](const std::wstring& p, DWORD code) {
        allOk = false;
        if (err.empty()) err = p + L": " + util::Win32ErrorMessage(code);
    };
    while (!stack.empty()) {
        if (cancel.load()) {
            for (auto& f : stack) CloseHandle(f.h);
            return false;
        }
        Frame& top = stack.back();
        if (!top.listed) {
            top.listed = true;
            std::vector<std::wstring> names;
            if (!ListDirectory(top.h, names, e)) fail(top.path, e);
            for (auto& name : names) {
                if (cancel.load()) break;
                std::wstring childPath = top.path + L"\\" + name;
                HANDLE ch = OpenRelative(top.h, name, kDeleteAccess, 0, e);
                if (!ch) {
                    if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) fail(childPath, e);
                    continue;
                }
                DWORD ca = 0;
                if (!AttrTag(ch, ca)) {
                    fail(childPath, GetLastError());
                } else if (IsRealDir(ca)) {
                    top.dirs.push_back(name);  // visited one at a time, so few handles stay open
                } else if (DeleteByHandle(ch, ca, e)) {
                    ++removed;
                } else {
                    fail(childPath, e);
                }
                CloseHandle(ch);
            }
            continue;
        }
        if (top.next < top.dirs.size()) {
            std::wstring name = top.dirs[top.next++];
            std::wstring childPath = top.path + L"\\" + name;
            HANDLE ch = OpenRelative(top.h, name, kDeleteAccess | FILE_LIST_DIRECTORY, 0, e);
            DWORD ca = 0;
            if (!ch) {
                if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) fail(childPath, e);
            } else if (!AttrTag(ch, ca)) {
                fail(childPath, GetLastError());
                CloseHandle(ch);
            } else if (!IsRealDir(ca)) {
                // It turned into a link (or file) since it was listed: remove that entry, don't enter it.
                if (DeleteByHandle(ch, ca, e))
                    ++removed;
                else
                    fail(childPath, e);
                CloseHandle(ch);
            } else {
                stack.push_back({ch, childPath, ca, {}, 0, false});
            }
            continue;
        }
        // Everything inside is handled: remove the folder itself.
        Frame f = std::move(stack.back());
        stack.pop_back();
        if (DeleteByHandle(f.h, f.attrs, e))
            ++removed;
        else if (e != ERROR_DIR_NOT_EMPTY || allOk)
            fail(f.path, e);  // "not empty" is only news if nothing else failed inside it
        CloseHandle(f.h);
    }
    return allOk;
}

bool DeleteToRecycleBin(const std::wstring& path, std::wstring& err) {
    IFileOperation* op = nullptr;
    IShellItem* item = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (FAILED(hr)) {
        err = TrF(L"Cannot create file operation: {0}", HrMessage(hr));
        return false;
    }
    // FOF_WANTNUKEWARNING: Windows asks before permanently deleting items that can't be recycled.
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING |
                          FOFX_RECYCLEONDELETE);
    hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) hr = SHCreateItemFromParsingName(ToLongPath(path).c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) {
        err = path + L": " + HrMessage(hr);
        SafeRelease(op);
        return false;
    }
    hr = op->DeleteItem(item, nullptr);
    if (SUCCEEDED(hr)) hr = op->PerformOperations();
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    SafeRelease(item);
    SafeRelease(op);
    if (FAILED(hr)) {
        err = path + L": " + HrMessage(hr);
        return false;
    }
    if (aborted) {
        err = TrF(L"{0}: operation was cancelled", path);
        return false;
    }
    return true;
}

std::vector<AppUsingFile> AppsUsing(const std::wstring& path, size_t maxFiles) {
    std::vector<AppUsingFile> out;
    // The Restart Manager wants plain paths, not "\\?\" ones.
    std::wstring p = path.rfind(L"\\\\?\\", 0) == 0 ? path.substr(4) : path;
    std::vector<std::wstring> files;
    DWORD a = GetFileAttributesW(ToLongPath(p).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return out;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        files.push_back(p);
    } else if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        std::vector<std::wstring> stack{p};
        while (!stack.empty() && files.size() < maxFiles) {
            std::wstring d = stack.back();
            stack.pop_back();
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW((ToLongPath(d) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                        FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                std::wstring child = d + L"\\" + fd.cFileName;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    files.push_back(child);
                else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    stack.push_back(child);
            } while (files.size() < maxFiles && FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (files.empty()) return out;

    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return out;
    std::vector<LPCWSTR> ptrs;
    for (auto& f : files) ptrs.push_back(f.c_str());
    if (RmRegisterResources(session, static_cast<UINT>(ptrs.size()), ptrs.data(), 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
        UINT needed = 0, count = 0;
        DWORD reasons = 0;
        std::vector<RM_PROCESS_INFO> info;
        DWORD rc = RmGetList(session, &needed, &count, nullptr, &reasons);
        for (int tries = 0; rc == ERROR_MORE_DATA && tries < 3; ++tries) {  // the list can grow in between
            info.resize(needed);
            count = needed;
            rc = RmGetList(session, &needed, &count, info.data(), &reasons);
        }
        if (rc == ERROR_SUCCESS)
            for (UINT i = 0; i < count && i < info.size(); ++i) {
                AppUsingFile app;
                app.pid = info[i].Process.dwProcessId;
                app.name = info[i].strAppName;
                if (app.name.empty()) app.name = TrF(L"process {0}", std::to_wstring(app.pid));
                out.push_back(std::move(app));
            }
    }
    RmEndSession(session);
    return out;
}

std::wstring AppNames(const std::vector<std::wstring>& names, size_t max) {
    std::vector<std::wstring> u;
    for (auto& n : names)
        if (std::find(u.begin(), u.end(), n) == u.end()) u.push_back(n);
    std::wstring s;
    size_t shown = std::min(u.size(), max);
    for (size_t i = 0; i < shown; ++i)
        s = i == 0 ? u[i] : (i + 1 == shown && u.size() <= max) ? TrF(L"{0} and {1}", s, u[i]) : TrF(L"{0}, {1}", s, u[i]);
    if (u.size() > max) s = TrN(u.size() - max, L"{1} and {0} more", L"{1} and {0} more", s);
    return s;
}

bool RevealInExplorer(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr)) return true;
    }
    std::wstring args = L"/select,\"" + path + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                                                   SW_SHOWNORMAL)) > 32;
}

bool OpenWithDefault(const std::wstring& path) {
    INT_PTR r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, nullptr, path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r == SE_ERR_NOASSOC)
        r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"openas", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32;
}

bool OpenTerminalAt(const std::wstring& dir) {
    std::wstring d = dir;
    if (!d.empty() && d.back() == L'\\') d += L'.';  // avoid a trailing backslash escaping the quote
    wchar_t wt[MAX_PATH] = {};
    std::wstring cmd;
    if (SearchPathW(nullptr, L"wt.exe", nullptr, MAX_PATH, wt, nullptr))
        cmd = L"\"" + std::wstring(wt) + L"\" -d \"" + d + L"\"";
    else
        cmd = L"powershell.exe -NoLogo -NoExit";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si,
                        &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool CopyTextToClipboard(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem) {
        void* p = GlobalLock(mem);
        memcpy(p, text.c_str(), bytes);
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

std::wstring ReadClipboardText() {
    std::wstring out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

std::wstring CurrentDirectory() {
    DWORD n = GetCurrentDirectoryW(0, nullptr);
    std::wstring s(n, L'\0');
    n = GetCurrentDirectoryW(n, s.data());
    s.resize(n);
    return s;
}

std::wstring HomeDirectory() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}
