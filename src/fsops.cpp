#include "fsops.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "tree.h"
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
        case DRIVE_REMOVABLE: return L"Removable";
        case DRIVE_FIXED: return L"Local";
        case DRIVE_REMOTE: return L"Network";
        case DRIVE_CDROM: return L"Optical";
        case DRIVE_RAMDISK: return L"RAM disk";
        default: return L"Unknown";
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
            d.label = L"(not responding)";
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
        err = L"Empty path";
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
        err = L"Not a directory";
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

bool RemoveOne(const std::wstring& lp, DWORD attrs, bool isDir, std::wstring& err) {
    if (attrs & FILE_ATTRIBUTE_READONLY) {
        DWORD na = attrs & (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                            FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_TEMPORARY);
        SetFileAttributesW(lp.c_str(), na ? na : FILE_ATTRIBUTE_NORMAL);
    }
    BOOL ok = isDir ? RemoveDirectoryW(lp.c_str()) : DeleteFileW(lp.c_str());
    if (!ok) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
        if (err.empty()) {
            std::wstring shown = lp.rfind(L"\\\\?\\", 0) == 0 ? lp.substr(4) : lp;
            err = shown + L": " + util::Win32ErrorMessage(e);
        }
        return false;
    }
    return true;
}

}  // namespace

bool DeletePermanently(const std::wstring& path, std::atomic<bool>& cancel, std::atomic<uint64_t>& removed,
                       std::wstring& err) {
    std::wstring root = ToLongPath(path);
    DWORD a = GetFileAttributesW(root.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
        err = path + L": " + util::Win32ErrorMessage(e);
        return false;
    }
    bool isDir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!isDir || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        // Files, and links to directories: remove the entry itself, never the target.
        bool ok = RemoveOne(root, a, isDir, err);
        if (ok) ++removed;
        return ok;
    }

    struct Frame {
        std::wstring path;
        DWORD attrs;
        bool expanded;
    };
    std::vector<Frame> stack;
    stack.push_back({root, a, false});
    bool allOk = true;
    while (!stack.empty()) {
        if (cancel.load()) return false;
        if (!stack.back().expanded) {
            stack.back().expanded = true;
            std::wstring dir = stack.back().path;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                        FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) {
                DWORD e = GetLastError();
                if (e != ERROR_FILE_NOT_FOUND) {
                    allOk = false;
                    if (err.empty()) err = dir.substr(4) + L": " + util::Win32ErrorMessage(e);
                }
                continue;
            }
            do {
                if (fd.cFileName[0] == L'.' &&
                    (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
                    continue;
                std::wstring child = dir + L"\\" + fd.cFileName;
                bool cDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                bool cReparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                if (cDir && !cReparse) {
                    stack.push_back({child, fd.dwFileAttributes, false});
                } else if (RemoveOne(child, fd.dwFileAttributes, cDir, err)) {
                    ++removed;
                } else {
                    allOk = false;
                }
            } while (!cancel.load() && FindNextFileW(h, &fd));
            FindClose(h);
            continue;
        }
        Frame f = std::move(stack.back());
        stack.pop_back();
        if (RemoveOne(f.path, f.attrs, true, err))
            ++removed;
        else
            allOk = false;
    }
    return allOk;
}

bool DeleteToRecycleBin(const std::wstring& path, std::wstring& err) {
    IFileOperation* op = nullptr;
    IShellItem* item = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (FAILED(hr)) {
        err = L"Cannot create file operation: " + HrMessage(hr);
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
        err = path + L": operation was cancelled";
        return false;
    }
    return true;
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
