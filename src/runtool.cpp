#include "runtool.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "i18n.h"
#include "util.h"

namespace {

std::wstring TempDir() {
    wchar_t tmp[MAX_PATH];
    return std::wstring(tmp, GetTempPathW(MAX_PATH, tmp));
}

std::string ReadAll(const std::wstring& path) {
    std::string out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char b[8192];
    DWORD n = 0;
    while (out.size() < (8u << 20) && ReadFile(h, b, sizeof(b), &n, nullptr) && n) out.append(b, n);
    CloseHandle(h);
    return out;
}

}  // namespace

std::wstring SystemTool(const wchar_t* exe) {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(sys, n) + L"\\" + exe;
}

std::wstring FindOnPath(const wchar_t* exe) {
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, exe, nullptr, MAX_PATH, buf, nullptr);
    return n && n < MAX_PATH ? std::wstring(buf, n) : L"";
}

bool RunningElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

int RunTool(const std::wstring& app, const std::wstring& args, std::string& out, unsigned long timeoutMs, std::atomic<bool>* cancel) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nul;
    std::wstring line = L"\"" + app + L"\" " + args;
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(0);
    PROCESS_INFORMATION pi{};
    std::wstring cwd = TempDir();  // not the exe's folder
    BOOL ok = CreateProcessW(app.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    auto drain = [&] {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            char b[4096];
            DWORD n = 0;
            if (!ReadFile(rd, b, std::min<DWORD>(avail, sizeof(b)), &n, nullptr) || !n) break;
            out.append(b, n);
        }
    };
    int code = 0;
    ULONGLONG start = GetTickCount64();
    for (;;) {
        drain();
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            DWORD c = 1;
            GetExitCodeProcess(pi.hProcess, &c);
            code = static_cast<int>(c);
            break;
        }
        if ((cancel && *cancel) || GetTickCount64() - start > timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            code = -2;
            break;
        }
    }
    drain();  // never a blocking read: a child the tool started may still hold the pipe open
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return code;
}

std::wstring DecodeToolOutput(const std::string& raw) {
    // wsl.exe writes UTF-16LE.
    size_t zeros = 0;
    for (size_t i = 1; i < raw.size(); i += 2) zeros += raw[i] == 0;
    if (raw.size() >= 2 && zeros * 3 > raw.size() / 2) {
        size_t start = static_cast<uint8_t>(raw[0]) == 0xFF && static_cast<uint8_t>(raw[1]) == 0xFE ? 2 : 0;
        std::wstring w((raw.size() - start) / 2, L'\0');
        memcpy(w.data(), raw.data() + start, w.size() * 2);
        w.erase(std::remove(w.begin(), w.end(), L'\0'), w.end());
        return w;
    }
    // Linux tools and docker write UTF-8; Windows console tools writing to a pipe use the OEM code page.
    if (raw.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (n > 0) return util::Utf8ToWide(raw);
    n = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), w.data(), n);
    return w;
}

std::wstring LastLine(const std::string& raw) {
    std::wstring t = DecodeToolOutput(raw);
    size_t end = t.find_last_not_of(L" \t\r\n");
    if (end == std::wstring::npos) return L"";
    size_t start = t.find_last_of(L"\r\n", end);
    std::wstring l = t.substr(start == std::wstring::npos ? 0 : start + 1, end + 1 - (start == std::wstring::npos ? 0 : start + 1));
    size_t b = l.find_first_not_of(L" \t");
    return b == std::wstring::npos ? L"" : l.substr(b);
}

AdminRun RunAsAdmin(const std::wstring& commandLine) {
    AdminRun r;
    std::wstring cmd = SystemTool(L"cmd.exe");
    if (RunningElevated()) {
        r.code = RunTool(cmd, L"/d /s /c \"" + commandLine + L"\"", r.out, 0xFFFFFFFFu);
        if (r.code == -1) r.error = L"Couldn't start " + cmd;
        return r;
    }
    std::wstring log = TempDir() + L"winncdu-admin-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()) + L".log";
    // In parentheses so the log collects a whole `a & b` chain or `for` loop, not just its last part.
    std::wstring params = L"/d /s /c \"(" + commandLine + L") > \"" + log + L"\" 2>&1\"";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = cmd.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        DWORD e = GetLastError();
        r.declined = e == ERROR_CANCELLED;
        r.error = r.declined ? Tr(L"Administrator permission was declined") : TrF(L"Couldn't start: {0}", util::Win32ErrorMessage(e));
        return r;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    r.code = static_cast<int>(code);
    r.out = ReadAll(log);
    DeleteFileW(log.c_str());
    return r;
}
