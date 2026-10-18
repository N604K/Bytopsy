#include "startup.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <taskschd.h>
#include <winevt.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <cwchar>

#include "runtool.h"
#include "i18n.h"
#include "util.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr const wchar_t* kApprovedBase = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";
constexpr DWORD kUserServiceBit = 0x40;  // SERVICE_USER_SERVICE: per-user instances of Windows' services

std::wstring Lower(const std::wstring& s) { return util::ToLower(s); }

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e + 1 - b);
}

std::wstring FileNameOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

std::wstring DirOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

std::wstring StripExt(const std::wstring& n) {
    size_t d = n.find_last_of(L'.');
    return d == std::wstring::npos || d == 0 ? n : n.substr(0, d);
}

bool FileExists(const std::wstring& p, uint64_t* bytes = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (p.empty() || !GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return false;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    if (bytes) *bytes = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return true;
}

std::wstring WindowsDir() {
    wchar_t b[MAX_PATH];
    UINT n = GetWindowsDirectoryW(b, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(b, n) : L"C:\\Windows";
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

// Ends of an executable or script name inside an unquoted command line.
const wchar_t* const kExts[] = {L".exe", L".com", L".bat", L".cmd", L".vbs", L".vbe", L".js", L".jse", L".wsf",
                                L".ps1", L".msc", L".lnk", L".dll", L".scr", L".cpl", L".hta"};

// First token of `s`: a quoted string, else up to the earliest known extension that ends a word,
// else up to the first space. `rest` gets what follows.
std::wstring FirstToken(const std::wstring& s, std::wstring& rest, const wchar_t* extraStops = L"") {
    std::wstring c = Trim(s);
    rest.clear();
    if (c.empty()) return L"";
    if (c[0] == L'"') {
        size_t q = c.find(L'"', 1);
        if (q == std::wstring::npos) return c.substr(1);
        rest = c.substr(q + 1);
        return c.substr(1, q - 1);
    }
    std::wstring lc = Lower(c);
    size_t best = std::wstring::npos;
    for (const wchar_t* ext : kExts) {
        size_t n = wcslen(ext);
        for (size_t at = lc.find(ext); at != std::wstring::npos; at = lc.find(ext, at + 1)) {
            size_t end = at + n;
            if (end == lc.size() || lc[end] == L' ' || lc[end] == L'\t' || lc[end] == L'"' || lc[end] == L',' ||
                wcschr(extraStops, lc[end])) {
                best = std::min(best, end);
                break;
            }
        }
    }
    size_t stop = c.find_first_of(std::wstring(L" \t") + extraStops);
    // ... unless options or quotes come before it ("C:\Tools\app -config C:\x.cmd" runs C:\Tools\app),
    // or the first word is a bare name ("rundll32 C:\Program Files\x.dll,Entry", "//B"): a path with
    // spaces always has a backslash before its first space.
    if (best != std::wstring::npos) {
        std::wstring head = c.substr(0, best);
        std::wstring first = c.substr(0, stop == std::wstring::npos ? c.size() : stop);
        if (head.find(L" -") != std::wstring::npos || head.find(L" /") != std::wstring::npos || head.find(L'"') != std::wstring::npos ||
            first.find_first_of(L"\\:") == std::wstring::npos)
            best = std::wstring::npos;
    }
    // An extension ending a word wins over the first space ("C:\Program Files\App\app.exe /x").
    size_t end = best != std::wstring::npos ? best : (stop == std::wstring::npos ? c.size() : stop);
    rest = c.substr(end);
    return c.substr(0, end);
}

bool IsHost(const std::wstring& program, const wchar_t* base) {
    std::wstring n = Lower(FileNameOf(program));
    std::wstring b = base;
    return n == b || n == b + L".exe";
}

// ---- Version info

struct VersionInfo {
    std::wstring company, description;
};

VersionInfo ReadVersionInfo(const std::wstring& file) {
    VersionInfo vi;
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(file.c_str(), &dummy);
    if (!size) return vi;
    std::vector<uint8_t> buf(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, buf.data())) return vi;
    std::vector<std::wstring> langs;
    struct Lang {
        WORD lang, cp;
    }* tr = nullptr;
    UINT len = 0;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) && tr)
        for (UINT i = 0; i < len / sizeof(Lang); ++i) {
            wchar_t b[16];
            swprintf(b, 16, L"%04x%04x", tr[i].lang, tr[i].cp);
            langs.push_back(b);
        }
    langs.push_back(L"040904b0");
    langs.push_back(L"040904e4");
    langs.push_back(L"000004b0");
    auto get = [&](const wchar_t* field) {
        for (auto& l : langs) {
            wchar_t* v = nullptr;
            UINT n = 0;
            std::wstring q = L"\\StringFileInfo\\" + l + L"\\" + field;
            if (VerQueryValueW(buf.data(), q.c_str(), reinterpret_cast<void**>(&v), &n) && v && n > 1) return Trim(std::wstring(v));
        }
        return std::wstring();
    };
    vi.company = get(L"CompanyName");
    vi.description = get(L"FileDescription");
    return vi;
}

// ---- Programs

// Absolute path of a program named on a command line, looking where CreateProcess would.
std::wstring ResolveOnDisk(std::wstring p) {
    if (p.empty()) return p;
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    bool rooted = (p.size() > 2 && p[1] == L':') || StartsWithI(p, L"\\\\");
    if (rooted) {
        if (!FileExists(p) && FileExists(p + L".exe")) p += L".exe";
        return p;
    }
    std::wstring win = WindowsDir();
    std::wstring dirs = win + L"\\System32;" + win + L";" + win + L"\\System32\\wbem";
    wchar_t pathEnv[8192];
    DWORD n = GetEnvironmentVariableW(L"PATH", pathEnv, 8192);
    if (n && n < 8192) dirs += L";" + std::wstring(pathEnv, n);
    wchar_t out[MAX_PATH];
    if (SearchPathW(dirs.c_str(), p.c_str(), L".exe", MAX_PATH, out, nullptr)) return out;
    return p;
}

// Fills program, programFound, programBytes, chromium, publisher, display, microsoft.
void Describe(StartupEntry& e, const std::wstring& expandedCommand, bool keepDisplay) {
    std::wstring args;
    std::wstring prog = ProgramFromCommand(expandedCommand, &args);
    prog = ResolveOnDisk(prog);
    // Squirrel launchers (Update.exe, or a stub named like the app) start the real app from the
    // newest app-* folder next to them.
    {
        std::wstring target = SquirrelTarget(args);
        if (!target.empty()) {
            std::wstring dir = DirOf(prog), best;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\app-*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && FileExists(dir + L"\\" + fd.cFileName + L"\\" + target) &&
                        (best.empty() || util::ToLower(fd.cFileName) > util::ToLower(best)))
                        best = fd.cFileName;
                while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            if (!best.empty())
                prog = dir + L"\\" + best + L"\\" + target;
            else if (FileExists(dir + L"\\" + target))
                prog = dir + L"\\" + target;
        }
    }
    e.program = prog;
    e.programFound = FileExists(prog, &e.programBytes);
    if (e.programFound) {
        std::wstring dir = DirOf(prog);
        e.chromium = FileExists(dir + L"\\resources\\app.asar") || FileExists(dir + L"\\chrome_100_percent.pak") ||
                     (FileExists(dir + L"\\resources.pak") && FileExists(dir + L"\\icudtl.dat"));
        VersionInfo vi = ReadVersionInfo(prog);
        e.publisher = vi.company;
        if (!keepDisplay && !vi.description.empty()) e.display = vi.description;
        std::wstring win = WindowsDir() + L"\\";
        e.microsoft = IsMicrosoftPublisher(vi.company) || (vi.company.empty() && StartsWithI(prog, win.c_str()));
    }
    if (e.display.empty()) e.display = e.name;
}

// ---- Run keys

struct RunSource {
    HKEY hive;
    const wchar_t* key;
    REGSAM view;
    StartupKind kind;
    bool machine, wow32;
    const wchar_t* label;
};

void ReadApproved(StartupEntry& e) {
    std::wstring key = ApprovedKey(e);
    if (key.empty()) return;
    uint8_t data[64];
    DWORD size = sizeof(data);
    DWORD flags = RRF_RT_REG_BINARY | (e.machine ? RRF_SUBKEY_WOW6464KEY : 0);
    LSTATUS st = RegGetValueW(e.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, key.c_str(), e.name.c_str(), flags, nullptr, data, &size);
    if (st == ERROR_SUCCESS) e.enabled = ApprovedEnabled(data, size, &e.disabledAt);
}

void ScanRunKeys(std::vector<StartupEntry>& out) {
    const RunSource sources[] = {
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, StartupKind::Run, false, false,
         L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_64KEY, StartupKind::Run, true, false,
         L"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_32KEY, StartupKind::Run, true, true,
         L"HKLM\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run"},
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0, StartupKind::RunOnce, false, false,
         L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", KEY_WOW64_64KEY, StartupKind::RunOnce, true, false,
         L"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", KEY_WOW64_32KEY, StartupKind::RunOnce, true, true,
         L"HKLM\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce"},
    };
    for (const RunSource& s : sources) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(s.hive, s.key, 0, KEY_READ | s.view, &k) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; ++i) {
            wchar_t name[16384];
            DWORD nameLen = 16384, type = 0, bytes = 0;
            LSTATUS st = RegEnumValueW(k, i, name, &nameLen, nullptr, &type, nullptr, &bytes);
            if (st == ERROR_NO_MORE_ITEMS) break;
            if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
            std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 2, 0);
            DWORD dataBytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
            nameLen = 16384;
            if (RegEnumValueW(k, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &dataBytes) != ERROR_SUCCESS)
                continue;
            StartupEntry e;
            e.kind = s.kind;
            e.machine = s.machine;
            e.wow32 = s.wow32;
            e.name = std::wstring(name, nameLen);
            e.where = s.label;
            e.command = data.data();
            Describe(e, ExpandEnv(e.command), false);
            if (e.kind == StartupKind::RunOnce) {
                e.canToggle = false;
                e.cantToggle = Tr(L"Runs once at the next sign-in, then Windows removes it");
            } else {
                ReadApproved(e);
            }
            out.push_back(std::move(e));
        }
        RegCloseKey(k);
    }
}

// ---- Startup folders

void ScanStartupFolder(const std::wstring& dir, bool machine, std::vector<StartupEntry>& out) {
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!_wcsicmp(n.c_str(), L"desktop.ini")) continue;
        StartupEntry e;
        e.kind = StartupKind::Folder;
        e.machine = machine;
        e.name = n;
        e.where = dir;
        std::wstring path = dir + L"\\" + n;
        e.command = L"\"" + path + L"\"";
        std::wstring ln = Lower(n);
        if (ln.size() > 4 && ln.compare(ln.size() - 4, 4, L".lnk") == 0) {
            ComPtr<IShellLinkW> link;
            ComPtr<IPersistFile> file;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
                SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Load(path.c_str(), STGM_READ))) {
                wchar_t target[MAX_PATH * 2] = {}, args[4096] = {};
                link->GetPath(target, MAX_PATH * 2, nullptr, SLGP_RAWPATH);
                link->GetArguments(args, 4096);
                if (target[0]) e.command = L"\"" + std::wstring(target) + L"\"" + (args[0] ? L" " + std::wstring(args) : L"");
            }
        }
        e.display = StripExt(n);
        Describe(e, ExpandEnv(e.command), true);
        ReadApproved(e);
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---- Scheduled tasks

std::wstring Bstr(BSTR b) {
    std::wstring s = b ? std::wstring(b, SysStringLen(b)) : L"";
    SysFreeString(b);
    return s;
}

void ScanTaskFolder(ITaskFolder* folder, std::vector<StartupEntry>& out, size_t& hidden, int depth) {
    BSTR bp = nullptr;
    folder->get_Path(&bp);
    std::wstring path = Bstr(bp);
    if (!_wcsicmp(path.c_str(), L"\\Microsoft\\Windows") || StartsWithI(path, L"\\Microsoft\\Windows\\")) {
        ComPtr<IRegisteredTaskCollection> tasks;
        LONG n = 0;
        if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && SUCCEEDED(tasks->get_Count(&n))) hidden += n;
        ComPtr<ITaskFolderCollection> subs;
        LONG sn = 0;
        if (depth < 12 && SUCCEEDED(folder->GetFolders(0, &subs)) && SUCCEEDED(subs->get_Count(&sn)))
            for (LONG i = 1; i <= sn; ++i) {
                VARIANT v;
                VariantInit(&v);
                v.vt = VT_I4;
                v.lVal = i;
                ComPtr<ITaskFolder> sub;
                if (SUCCEEDED(subs->get_Item(v, &sub))) ScanTaskFolder(sub.Get(), out, hidden, depth + 1);
            }
        return;
    }
    ComPtr<IRegisteredTaskCollection> tasks;
    LONG count = 0;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && SUCCEEDED(tasks->get_Count(&count))) {
        for (LONG i = 1; i <= count; ++i) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = i;
            ComPtr<IRegisteredTask> task;
            if (FAILED(tasks->get_Item(v, &task))) continue;
            ComPtr<ITaskDefinition> def;
            if (FAILED(task->get_Definition(&def))) continue;
            ComPtr<ITriggerCollection> triggers;
            LONG tn = 0;
            if (FAILED(def->get_Triggers(&triggers)) || FAILED(triggers->get_Count(&tn))) continue;
            bool logon = false, boot = false;
            for (LONG t = 1; t <= tn; ++t) {
                ComPtr<ITrigger> trig;
                if (FAILED(triggers->get_Item(t, &trig))) continue;
                TASK_TRIGGER_TYPE2 type{};
                VARIANT_BOOL on = VARIANT_TRUE;
                trig->get_Enabled(&on);
                if (FAILED(trig->get_Type(&type)) || on == VARIANT_FALSE) continue;
                if (type == TASK_TRIGGER_LOGON) logon = true;
                if (type == TASK_TRIGGER_BOOT) boot = true;
            }
            if (!logon && !boot) continue;
            StartupEntry e;
            e.kind = StartupKind::Task;
            BSTR b = nullptr;
            task->get_Path(&b);
            e.name = Bstr(b);
            b = nullptr;
            task->get_Name(&b);
            e.display = Bstr(b);
            e.where = path;
            e.trigger = logon && boot ? N_(L"at startup and log on") : logon ? N_(L"at log on") : N_(L"at startup");
            VARIANT_BOOL en = VARIANT_TRUE;
            task->get_Enabled(&en);
            e.enabled = en != VARIANT_FALSE;
            ComPtr<IActionCollection> actions;
            LONG an = 0;
            if (SUCCEEDED(def->get_Actions(&actions)) && SUCCEEDED(actions->get_Count(&an)))
                for (LONG a = 1; a <= an && e.command.empty(); ++a) {
                    ComPtr<IAction> act;
                    TASK_ACTION_TYPE at{};
                    if (FAILED(actions->get_Item(a, &act)) || FAILED(act->get_Type(&at))) continue;
                    if (at == TASK_ACTION_EXEC) {
                        ComPtr<IExecAction> ex;
                        if (FAILED(act.As(&ex))) continue;
                        BSTR p = nullptr, args = nullptr;
                        ex->get_Path(&p);
                        ex->get_Arguments(&args);
                        std::wstring prog = Trim(Bstr(p)), ar = Trim(Bstr(args));
                        if (!prog.empty() && prog[0] != L'"' && prog.find(L' ') != std::wstring::npos) prog = L"\"" + prog + L"\"";
                        e.command = prog + (ar.empty() ? L"" : L" " + ar);
                    } else if (at == TASK_ACTION_COM_HANDLER) {
                        e.command = L"(COM handler)";
                    }
                }
            std::wstring keepName = e.display;
            if (e.command != L"(COM handler)") Describe(e, ExpandEnv(e.command), true);
            e.display = keepName.empty() ? e.name : keepName;
            ComPtr<IRegistrationInfo> reg;
            if (!e.programFound && SUCCEEDED(def->get_RegistrationInfo(&reg))) {
                BSTR au = nullptr;
                reg->get_Author(&au);
                std::wstring author = Bstr(au);
                if (e.publisher.empty() && !author.empty() && author[0] != L'$') e.publisher = author;
                e.microsoft = e.microsoft || IsMicrosoftPublisher(author);
            }
            out.push_back(std::move(e));
        }
    }
    ComPtr<ITaskFolderCollection> subs;
    LONG sn = 0;
    if (depth < 12 && SUCCEEDED(folder->GetFolders(0, &subs)) && SUCCEEDED(subs->get_Count(&sn)))
        for (LONG i = 1; i <= sn; ++i) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = i;
            ComPtr<ITaskFolder> sub;
            if (SUCCEEDED(subs->get_Item(v, &sub))) ScanTaskFolder(sub.Get(), out, hidden, depth + 1);
        }
}

ComPtr<ITaskService> ConnectTaskScheduler() {
    ComPtr<ITaskService> svc;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc)))) return nullptr;
    VARIANT empty;
    VariantInit(&empty);
    if (FAILED(svc->Connect(empty, empty, empty, empty))) return nullptr;
    return svc;
}

void ScanTasks(std::vector<StartupEntry>& out, size_t& hidden) {
    ComPtr<ITaskService> svc = ConnectTaskScheduler();
    if (!svc) return;
    ComPtr<ITaskFolder> root;
    BSTR rootPath = SysAllocString(L"\\");
    HRESULT hr = svc->GetFolder(rootPath, &root);
    SysFreeString(rootPath);
    if (SUCCEEDED(hr)) ScanTaskFolder(root.Get(), out, hidden, 0);
}

// ---- Services

std::wstring StartWord(uint32_t startType, bool delayed) {
    if (startType == SERVICE_AUTO_START) return delayed ? L"delayed-auto" : L"auto";
    if (startType == SERVICE_DEMAND_START) return L"demand";
    if (startType == SERVICE_DISABLED) return L"disabled";
    return L"other";
}

const wchar_t* StartWordName(const std::wstring& w) {
    if (w == L"delayed-auto") return L"Automatic (delayed start)";
    if (w == L"auto") return L"Automatic";
    if (w == L"demand") return L"Manual";
    return L"Disabled";
}

std::wstring RegString(HKEY hive, const std::wstring& key, const wchar_t* value) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(hive, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    return buf;
}

void ScanServices(std::vector<StartupEntry>& out, const std::map<std::wstring, std::wstring>& turnedOff) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return;
    DWORD needed = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &needed, &count, &resume, nullptr);
    std::vector<uint8_t> buf(needed + 4096);
    resume = 0;
    if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf.data(), static_cast<DWORD>(buf.size()),
                               &needed, &count, &resume, nullptr)) {
        CloseServiceHandle(scm);
        return;
    }
    std::wstring win = WindowsDir();
    auto* svcs = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
    for (DWORD i = 0; i < count; ++i) {
        if (svcs[i].ServiceStatusProcess.dwServiceType & kUserServiceBit) continue;
        SC_HANDLE s = OpenServiceW(scm, svcs[i].lpServiceName, SERVICE_QUERY_CONFIG);
        if (!s) continue;
        DWORD cfgNeeded = 0;
        QueryServiceConfigW(s, nullptr, 0, &cfgNeeded);
        std::vector<uint8_t> cfgBuf(cfgNeeded + 16);
        auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cfgBuf.data());
        if (!cfgNeeded || !QueryServiceConfigW(s, cfg, static_cast<DWORD>(cfgBuf.size()), &cfgNeeded)) {
            CloseServiceHandle(s);
            continue;
        }
        std::wstring name = svcs[i].lpServiceName;
        auto off = turnedOff.find(Lower(name));
        bool ours = off != turnedOff.end() && cfg->dwStartType == SERVICE_DEMAND_START;
        if (cfg->dwStartType != SERVICE_AUTO_START && !ours) {
            CloseServiceHandle(s);
            continue;
        }
        StartupEntry e;
        e.kind = StartupKind::Service;
        e.machine = true;
        e.name = name;
        e.display = svcs[i].lpDisplayName ? svcs[i].lpDisplayName : name;
        e.where = L"Services";
        e.startType = cfg->dwStartType;
        e.command = cfg->lpBinaryPathName ? cfg->lpBinaryPathName : L"";
        SERVICE_DELAYED_AUTO_START_INFO dl{};
        DWORD dn = 0;
        if (QueryServiceConfig2W(s, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, reinterpret_cast<BYTE*>(&dl), sizeof(dl), &dn))
            e.delayed = dl.fDelayedAutostart != FALSE;
        CloseServiceHandle(s);
        e.enabled = cfg->dwStartType == SERVICE_AUTO_START;
        if (ours) e.restoreStart = off->second;
        std::wstring image = ServiceImagePath(ExpandEnv(e.command), win);
        // Shared svchost services: the DLL is what belongs to a publisher.
        std::wstring args;
        if (IsHost(ProgramFromCommand(image, &args), L"svchost")) {
            std::wstring key = L"SYSTEM\\CurrentControlSet\\Services\\" + name;
            std::wstring dll = RegString(HKEY_LOCAL_MACHINE, key + L"\\Parameters", L"ServiceDll");
            if (dll.empty()) dll = RegString(HKEY_LOCAL_MACHINE, key, L"ServiceDll");
            if (!dll.empty()) image = L"\"" + ServiceImagePath(ExpandEnv(dll), win) + L"\"";
        }
        Describe(e, image, true);
        if (e.microsoft) {
            e.canToggle = false;
            e.cantToggle = Tr(L"Windows' own services are listed for reference and are not changed here");
        }
        out.push_back(std::move(e));
    }
    CloseServiceHandle(scm);
}

// ---- Boot performance log

struct BootLog {
    std::map<std::wstring, uint32_t> apps;      // program file name (lowercase) -> slowest TotalTime
    std::map<std::wstring, uint32_t> services;  // service name (lowercase) -> slowest TotalTime
};

std::wstring EventXml(EVT_HANDLE ev) {
    DWORD used = 0, props = 0;
    EvtRender(nullptr, ev, EvtRenderEventXml, 0, nullptr, &used, &props);
    if (!used) return L"";
    std::wstring s(used / sizeof(wchar_t) + 1, L'\0');
    if (!EvtRender(nullptr, ev, EvtRenderEventXml, static_cast<DWORD>(s.size() * sizeof(wchar_t)), s.data(), &used, &props)) return L"";
    s.resize(wcslen(s.c_str()));
    return s;
}

bool ReadBootLog(BootLog& log, std::wstring& note) {
    EVT_HANDLE q = EvtQuery(nullptr, L"Microsoft-Windows-Diagnostics-Performance/Operational",
                            L"*[System[(EventID=101 or EventID=103)]]", EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!q) {
        DWORD e = GetLastError();
        note = e == ERROR_ACCESS_DENIED ? Tr(L"Windows' boot performance log needs administrator rights")
                                        : TrF(L"Windows' boot performance log can't be read: {0}", util::Win32ErrorMessage(e));
        return false;
    }
    size_t seen = 0;
    while (seen < 2000) {
        EVT_HANDLE evs[64];
        DWORD got = 0;
        if (!EvtNext(q, 64, evs, 2000, 0, &got) || !got) break;
        for (DWORD i = 0; i < got; ++i) {
            BootDelay d;
            if (ParseBootEvent(EventXml(evs[i]), d)) {
                auto put = [&](std::map<std::wstring, uint32_t>& m, const std::wstring& key) {
                    if (key.empty()) return;
                    uint32_t& v = m[Lower(key)];
                    v = std::max(v, d.totalMs);
                };
                if (d.eventId == 103) {
                    put(log.services, d.name);
                } else {
                    put(log.apps, FileNameOf(d.fileName));
                    put(log.apps, FileNameOf(d.name));
                }
            }
            EvtClose(evs[i]);
        }
        seen += got;
    }
    EvtClose(q);
    return true;
}

std::wstring XmlUnescape(std::wstring s) {
    const std::pair<const wchar_t*, wchar_t> ents[] = {{L"&lt;", L'<'}, {L"&gt;", L'>'}, {L"&quot;", L'"'}, {L"&apos;", L'\''}, {L"&amp;", L'&'}};
    for (auto& [ent, ch] : ents) {
        size_t n = wcslen(ent);
        for (size_t at = s.find(ent); at != std::wstring::npos; at = s.find(ent, at + 1)) s.replace(at, n, 1, ch);
    }
    return s;
}

}  // namespace

// =============================================================================================

const wchar_t* StartupKindName(StartupKind k) {
    switch (k) {
        case StartupKind::Run: return N_(L"Run key");
        case StartupKind::RunOnce: return N_(L"Run once");
        case StartupKind::Folder: return N_(L"Startup folder");
        case StartupKind::Task: return N_(L"Scheduled task");
        case StartupKind::Service: return N_(L"Service");
    }
    return L"";
}

const wchar_t* StartupImpactName(StartupImpact i) {
    switch (i) {
        case StartupImpact::Low: return L"low";
        case StartupImpact::Medium: return L"medium";
        case StartupImpact::High: return L"high";
        default: return L"unknown";
    }
}

bool ApprovedEnabled(const uint8_t* data, size_t size, int64_t* disabledAt) {
    if (disabledAt) *disabledAt = 0;
    if (!data || size == 0) return true;
    bool enabled = (data[0] & 1) == 0;
    if (!enabled && size >= 12 && disabledAt) {
        uint64_t ft = 0;
        memcpy(&ft, data + 4, 8);
        *disabledAt = static_cast<int64_t>(ft);
    }
    return enabled;
}

std::vector<uint8_t> MakeApproved(bool enabled, int64_t when) {
    std::vector<uint8_t> v(12, 0);
    v[0] = enabled ? 0x02 : 0x03;
    if (!enabled) {
        uint64_t ft = static_cast<uint64_t>(when);
        memcpy(v.data() + 4, &ft, 8);
    }
    return v;
}

std::wstring ApprovedKey(const StartupEntry& e) {
    if (e.kind == StartupKind::Run) return std::wstring(kApprovedBase) + (e.machine && e.wow32 ? L"Run32" : L"Run");
    if (e.kind == StartupKind::Folder) return std::wstring(kApprovedBase) + L"StartupFolder";
    return L"";
}

std::wstring ExpandEnv(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (!n) return s;
    std::wstring out(n, L'\0');
    n = ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
    if (!n) return s;
    out.resize(n - 1);
    return out;
}

std::wstring ProgramFromCommand(const std::wstring& command, std::wstring* argsOut) {
    std::wstring rest;
    std::wstring prog = FirstToken(command, rest);
    std::wstring args = Trim(rest);
    if (IsHost(prog, L"rundll32")) {
        // rundll32 "C:\x y\z.dll",Entry args  /  rundll32 C:\x\z.dll,Entry
        std::wstring after;
        std::wstring dll = FirstToken(args, after, L",");
        if (!dll.empty()) {
            if (argsOut) *argsOut = Trim(after);
            return dll;
        }
    } else if (IsHost(prog, L"wscript") || IsHost(prog, L"cscript") || IsHost(prog, L"mshta")) {
        std::wstring a = args;
        while (!a.empty()) {
            std::wstring after;
            std::wstring tok = FirstToken(a, after);
            if (tok.empty()) break;
            if (tok[0] != L'/') {
                if (argsOut) *argsOut = Trim(after);
                return tok;
            }
            a = after;
        }
    }
    if (argsOut) *argsOut = args;
    return prog;
}

std::wstring ServiceImagePath(const std::wstring& imagePath, const std::wstring& windowsDir) {
    std::wstring p = Trim(imagePath);
    bool quoted = !p.empty() && p[0] == L'"';
    std::wstring body = quoted ? p.substr(1) : p;
    if (StartsWithI(body, L"\\??\\")) body = body.substr(4);
    else if (StartsWithI(body, L"\\SystemRoot\\")) body = windowsDir + body.substr(11);
    else if (StartsWithI(body, L"system32\\") || StartsWithI(body, L"SysWOW64\\")) body = windowsDir + L"\\" + body;
    return quoted ? L"\"" + body : body;
}

std::wstring SquirrelTarget(const std::wstring& args) {
    std::wstring la = Lower(args);
    size_t at = la.find(L"--processstart");
    if (at == std::wstring::npos) return L"";
    std::wstring rest = args.substr(at + 14);
    if (!rest.empty() && rest[0] == L'=') rest = rest.substr(1);
    std::wstring after;
    std::wstring t = FirstToken(rest, after);
    return t.find_first_of(L"\\/") == std::wstring::npos ? t : L"";
}

bool IsMicrosoftPublisher(const std::wstring& company) { return util::ContainsI(company, L"microsoft"); }

void EstimateImpact(StartupEntry& e) {
    e.impactWhy.clear();
    if (e.measured) {
        e.impact = e.measuredMs >= 3000 ? StartupImpact::High : StartupImpact::Medium;
        e.impactWhy = TrF(L"Measured: Windows logged it slowing a boot down ({0} s, Diagnostics-Performance log)",
                          util::FormatDecimal(e.measuredMs / 1000.0, 1));
        return;
    }
    if (!e.programFound) {
        e.impact = StartupImpact::Unknown;
        e.impactWhy = e.program.empty() ? Tr(L"Unknown: no program to look at") : Tr(L"Unknown: the program file wasn't found");
        return;
    }
    if (e.programBytes == 0) {
        e.impact = StartupImpact::Unknown;
        e.impactWhy = Tr(L"Unknown: the program is an app alias or link (0 bytes), so its size says nothing");
        return;
    }
    std::wstring size = util::FormatSizeShort(e.programBytes);
    std::wstring file = Lower(FileNameOf(e.program));
    if (e.kind == StartupKind::Service && e.delayed) {
        e.impact = StartupImpact::Low;
        e.impactWhy = Tr(L"Estimate: delayed start, so Windows starts it after sign-in has settled");
    } else if (e.chromium) {
        e.impact = StartupImpact::High;
        e.impactWhy = TrF(L"Estimate: an Electron/Chromium app (it loads a whole browser engine), {0}", size);
    } else if (e.programBytes >= (100ull << 20)) {
        e.impact = StartupImpact::High;
        e.impactWhy = TrF(L"Estimate: a large program ({0})", size);
    } else if (file.find(L"update") != std::wstring::npos && e.programBytes < (10ull << 20)) {
        e.impact = StartupImpact::Low;
        e.impactWhy = TrF(L"Estimate: a small updater ({0}) that usually checks and exits", size);
    } else if (e.programBytes >= (20ull << 20)) {
        e.impact = StartupImpact::Medium;
        e.impactWhy = TrF(L"Estimate: a mid-sized program ({0})", size);
    } else {
        e.impact = StartupImpact::Low;
        e.impactWhy = TrF(L"Estimate: a small program ({0}); what it loads later isn't known", size);
    }
}

bool ParseBootEvent(const std::wstring& xml, BootDelay& out) {
    out = BootDelay{};
    size_t id = xml.find(L"<EventID");
    if (id == std::wstring::npos) return false;
    size_t gt = xml.find(L'>', id);
    if (gt == std::wstring::npos) return false;
    out.eventId = _wtoi(xml.c_str() + gt + 1);
    for (size_t at = xml.find(L"<Data Name="); at != std::wstring::npos; at = xml.find(L"<Data Name=", at + 1)) {
        size_t q = at + 11;
        if (q >= xml.size()) break;
        wchar_t quote = xml[q];
        size_t qe = xml.find(quote, q + 1);
        if (qe == std::wstring::npos) break;
        std::wstring key = xml.substr(q + 1, qe - q - 1);
        size_t vs = xml.find(L'>', qe);
        if (vs == std::wstring::npos) break;
        if (xml[vs - 1] == L'/') continue;  // <Data Name='X'/>
        size_t ve = xml.find(L"</Data>", vs);
        if (ve == std::wstring::npos) break;
        std::wstring val = XmlUnescape(xml.substr(vs + 1, ve - vs - 1));
        if (key == L"Name") out.name = val;
        else if (key == L"FileName") out.fileName = val;
        else if (key == L"FriendlyName") out.friendlyName = val;
        else if (key == L"TotalTime") out.totalMs = static_cast<uint32_t>(wcstoul(val.c_str(), nullptr, 10));
        else if (key == L"DegradationTime") out.degradationMs = static_cast<uint32_t>(wcstoul(val.c_str(), nullptr, 10));
    }
    return (out.eventId == 101 || out.eventId == 103) && (!out.name.empty() || !out.fileName.empty());
}

std::wstring ServiceJournalNote(const std::wstring& service, bool turnedOff, const std::wstring& startWord) {
    std::wstring human = turnedOff ? L"turned off at startup: start type set to Manual, was " + std::wstring(StartWordName(startWord))
                                   : L"turned on at startup: start type set back to " + std::wstring(StartWordName(startWord));
    return human + L" [startup service=" + service + (turnedOff ? L" was=" : L" now=") + startWord + L"]";
}

bool ParseServiceJournalNote(const std::wstring& note, std::wstring& service, bool& turnedOff, std::wstring& startWord) {
    size_t at = note.find(L"[startup service=");
    if (at == std::wstring::npos) return false;
    size_t end = note.find(L']', at);
    if (end == std::wstring::npos) return false;
    std::wstring body = note.substr(at + 17, end - at - 17);
    size_t w = body.rfind(L" was="), n = body.rfind(L" now=");
    size_t sep = w != std::wstring::npos ? w : n;
    if (sep == std::wstring::npos || sep == 0) return false;
    turnedOff = w != std::wstring::npos;
    service = body.substr(0, sep);
    startWord = body.substr(sep + 5);
    return !startWord.empty();
}

std::map<std::wstring, std::wstring> ServicesTurnedOff(const std::vector<JournalEntry>& journal) {
    std::map<std::wstring, std::wstring> out;
    std::map<std::wstring, bool> seen;
    for (const JournalEntry& e : journal) {
        std::wstring name, word;
        bool off = false;
        if (!ParseServiceJournalNote(e.note, name, off, word)) continue;
        std::wstring key = Lower(name);
        if (seen.count(key)) continue;  // only the newest record counts
        seen[key] = true;
        if (off && (word == L"auto" || word == L"delayed-auto")) out[key] = word;
    }
    return out;
}

std::wstring StartupAdminCommand(const StartupEntry& e, bool enable, int64_t now) {
    auto unsafe = [](const std::wstring& s) { return s.find_first_of(L"\"%\r\n") != std::wstring::npos; };
    if (unsafe(e.name)) return L"";
    auto quote = [](const std::wstring& s) { return L"\"" + s + L"\""; };
    switch (e.kind) {
        case StartupKind::Run:
        case StartupKind::Folder: {
            if (!e.machine) return L"";
            std::vector<uint8_t> v = MakeApproved(enable, now);
            std::wstring hex;
            for (uint8_t b : v) {
                wchar_t h[4];
                swprintf(h, 4, L"%02X", b);
                hex += h;
            }
            return quote(SystemTool(L"reg.exe")) + L" add " + quote(L"HKLM\\" + ApprovedKey(e)) +
                   (e.name.empty() ? std::wstring(L" /ve") : L" /v " + quote(e.name)) + L" /t REG_BINARY /d " + hex + L" /f /reg:64";
        }
        case StartupKind::Task:
            return quote(SystemTool(L"schtasks.exe")) + L" /change /tn " + quote(e.name) + (enable ? L" /enable" : L" /disable");
        case StartupKind::Service: {
            std::wstring word = enable ? (e.restoreStart.empty() ? std::wstring(L"auto") : e.restoreStart) : L"demand";
            return quote(SystemTool(L"sc.exe")) + L" config " + quote(e.name) + L" start= " + word;
        }
        default: return L"";
    }
}

bool StartupNeedsAdmin(const StartupEntry& e) {
    if (RunningElevated()) return false;
    return e.kind == StartupKind::Service || ((e.kind == StartupKind::Run || e.kind == StartupKind::Folder) && e.machine);
}

StartupList FindStartupEntries() {
    StartupList list;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ScanRunKeys(list.entries);
    ScanStartupFolder(KnownFolder(FOLDERID_Startup), false, list.entries);
    ScanStartupFolder(KnownFolder(FOLDERID_CommonStartup), true, list.entries);
    ScanTasks(list.entries, list.hiddenTasks);
    ScanServices(list.entries, ServicesTurnedOff(JournalRead(5000)));
    BootLog log;
    list.bootLogRead = ReadBootLog(log, list.bootLogNote);
    for (StartupEntry& e : list.entries) {
        if (list.bootLogRead) {
            const auto& m = e.kind == StartupKind::Service ? log.services : log.apps;
            auto it = m.find(Lower(e.kind == StartupKind::Service ? e.name : FileNameOf(e.program)));
            if (it != m.end() && it->second) {
                e.measured = true;
                e.measuredMs = it->second;
            }
        }
        EstimateImpact(e);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return list;
}

StartupChange SetStartupEnabled(const StartupEntry& e, bool enable) {
    StartupChange r;
    int64_t now = util::NowFileTime();
    std::wstring note;
    switch (e.kind) {
        case StartupKind::RunOnce: r.error = Tr(L"A Run once entry runs at the next sign-in and is then removed; it isn't changed here"); return r;
        case StartupKind::Run:
        case StartupKind::Folder: {
            std::vector<uint8_t> v = MakeApproved(enable, now);
            if (!e.machine || RunningElevated()) {
                HKEY k = nullptr;
                LSTATUS st = RegCreateKeyExW(e.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, ApprovedKey(e).c_str(), 0, nullptr, 0,
                                             KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr);
                if (st == ERROR_SUCCESS) {
                    st = RegSetValueExW(k, e.name.c_str(), 0, REG_BINARY, v.data(), static_cast<DWORD>(v.size()));
                    RegCloseKey(k);
                }
                if (st != ERROR_SUCCESS) {
                    r.error = TrF(L"Couldn't change it: {0}", util::Win32ErrorMessage(st));
                    return r;
                }
            } else {
                std::wstring cmd = StartupAdminCommand(e, enable, now);
                if (cmd.empty()) {
                    r.error = Tr(L"Its name can't be passed to reg.exe safely; use Task Manager for this one");
                    return r;
                }
                AdminRun a = RunAsAdmin(cmd);
                if (!a.error.empty() || a.code != 0) {
                    r.declined = a.declined;
                    r.error = !a.error.empty() ? a.error : TrF(L"reg.exe failed: {0}", LastLine(a.out));
                    return r;
                }
            }
            note = std::wstring(enable ? L"turned on at startup" : L"turned off at startup") + L" (" + StartupKindName(e.kind) + L", " +
                   e.where + L"\\" + e.name + L"; StartupApproved, as Task Manager does)";
            break;
        }
        case StartupKind::Task: {
            HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            HRESULT hr = E_FAIL;
            if (ComPtr<ITaskService> svc = ConnectTaskScheduler()) {
                ComPtr<ITaskFolder> root;
                BSTR rootPath = SysAllocString(L"\\"), path = SysAllocString(e.name.c_str());
                ComPtr<IRegisteredTask> task;
                hr = svc->GetFolder(rootPath, &root);
                if (SUCCEEDED(hr)) hr = root->GetTask(path, &task);
                if (SUCCEEDED(hr)) hr = task->put_Enabled(enable ? VARIANT_TRUE : VARIANT_FALSE);
                SysFreeString(rootPath);
                SysFreeString(path);
            }
            if (SUCCEEDED(co)) CoUninitialize();
            if (FAILED(hr)) {
                if (hr != E_ACCESSDENIED && hr != HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
                    r.error = TrF(L"Task Scheduler refused: {0}", util::Win32ErrorMessage(static_cast<unsigned long>(hr)));
                    return r;
                }
                std::wstring cmd = StartupAdminCommand(e, enable, now);
                if (cmd.empty()) {
                    r.error = Tr(L"Its name can't be passed to schtasks.exe safely; use Task Scheduler for this one");
                    return r;
                }
                AdminRun a = RunAsAdmin(cmd);
                if (!a.error.empty() || a.code != 0) {
                    r.declined = a.declined;
                    r.error = !a.error.empty() ? a.error : TrF(L"schtasks failed: {0}", LastLine(a.out));
                    return r;
                }
            }
            note = std::wstring(enable ? L"turned on at startup" : L"turned off at startup") + L" (scheduled task " + e.name +
                   L", " + (enable ? L"enabled" : L"disabled") + L" in Task Scheduler)";
            break;
        }
        case StartupKind::Service: {
            if (e.microsoft) {
                r.error = Tr(L"Windows' own services aren't changed here");
                return r;
            }
            std::wstring cmd = StartupAdminCommand(e, enable, now);
            if (cmd.empty()) {
                r.error = Tr(L"Its name can't be passed to sc.exe safely; use Services for this one");
                return r;
            }
            AdminRun a = RunAsAdmin(cmd);
            if (!a.error.empty() || a.code != 0) {
                r.declined = a.declined;
                r.error = !a.error.empty() ? a.error : TrF(L"sc.exe failed: {0}", LastLine(a.out));
                return r;
            }
            std::wstring word = enable ? (e.restoreStart.empty() ? std::wstring(L"auto") : e.restoreStart) : StartWord(e.startType, e.delayed);
            note = ServiceJournalNote(e.name, !enable, word);
            break;
        }
    }
    JournalEntry je;
    je.action = JournalAction::Clean;
    je.path = L"Startup: " + e.display;
    je.note = note;
    JournalAppend(je);
    r.ok = true;
    return r;
}
