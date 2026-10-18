#include "wintools.h"

#include <windows.h>
#include <initguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <wofapi.h>

#include <algorithm>
#include <map>
#include <set>

#include "util.h"

namespace {

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

std::wstring WinDir() {
    wchar_t buf[MAX_PATH];
    UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
    return std::wstring(buf, n);
}

std::wstring Lower(std::wstring s) { return util::ToLower(s); }

std::vector<std::wstring> Lines(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find(L'\n', pos);
        std::wstring l = s.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        while (!l.empty() && (l.back() == L'\r' || l.back() == L' ' || l.back() == L'\t')) l.pop_back();
        size_t b = l.find_first_not_of(L" \t");
        if (b != std::wstring::npos) out.push_back(l.substr(b));
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    return out;
}

uint64_t FolderSize(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += FolderSize(dir + L"\\" + fd.cFileName);
        else
            total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD def) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return def;
    return v;
}

// "7.95 GB", "86.15 MB", "7,95 GB" (DISM counts in 1024s).
uint64_t ParseDismSize(const std::wstring& s) {
    std::wstring num;
    size_t i = 0;
    for (; i < s.size() && (iswdigit(s[i]) || s[i] == L'.' || s[i] == L','); ++i) num += s[i] == L',' ? L'.' : s[i];
    if (num.empty()) return 0;
    while (i < s.size() && s[i] == L' ') ++i;
    double mul = 1;
    if (i < s.size()) switch (towupper(s[i])) {
            case L'K': mul = 1024.0; break;
            case L'M': mul = 1024.0 * 1024; break;
            case L'G': mul = 1024.0 * 1024 * 1024; break;
            case L'T': mul = 1024.0 * 1024 * 1024 * 1024; break;
            default: break;
        }
    return static_cast<uint64_t>(_wtof(num.c_str()) * mul + 0.5);
}

std::wstring InfField(HINF inf, const wchar_t* section, const wchar_t* key, DWORD field) {
    INFCONTEXT ctx;
    if (!SetupFindFirstLineW(inf, section, key, &ctx)) return L"";
    wchar_t buf[512];
    DWORD need = 0;
    if (!SetupGetStringFieldW(&ctx, field, buf, 512, &need)) return L"";
    return buf;
}

// The .inf files (oemNN.inf) that a device on this PC is set up with, connected or not.
std::set<std::wstring> InfsInUse() {
    std::set<std::wstring> used;
    HDEVINFO devs = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (devs == INVALID_HANDLE_VALUE) return used;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); ++i) {
        wchar_t buf[MAX_PATH];
        DEVPROPTYPE type = 0;
        if (SetupDiGetDevicePropertyW(devs, &dev, &DEVPKEY_Device_DriverInfPath, &type, reinterpret_cast<BYTE*>(buf), sizeof(buf), nullptr, 0) &&
            type == DEVPROP_TYPE_STRING)
            used.insert(Lower(buf));
    }
    SetupDiDestroyDeviceInfoList(devs);
    return used;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Component store

ComponentStore ParseComponentStore(const std::string& dismOutput) {
    ComponentStore cs;
    std::map<std::wstring, std::wstring> v;
    for (auto& l : Lines(DecodeToolOutput(dismOutput))) {
        size_t at = l.find(L" : ");
        if (at == std::wstring::npos) continue;
        std::wstring key = Lower(l.substr(0, at));
        while (!key.empty() && key.back() == L' ') key.pop_back();
        std::wstring val = l.substr(at + 3);
        size_t b = val.find_first_not_of(L' ');
        v[key] = b == std::wstring::npos ? L"" : val.substr(b);
    }
    auto get = [&](const wchar_t* k) { return v.count(k) ? v[k] : L""; };
    std::wstring actual = get(L"actual size of component store");
    if (actual.empty()) {
        cs.error = LastLine(dismOutput);
        if (cs.error.empty()) cs.error = L"DISM gave no report";
        return cs;
    }
    cs.ok = true;
    cs.actual = ParseDismSize(actual);
    cs.backups = ParseDismSize(get(L"backups and disabled features"));
    cs.cache = ParseDismSize(get(L"cache and temporary data"));
    cs.lastCleanup = get(L"date of last cleanup");
    std::wstring n = get(L"number of reclaimable packages");
    cs.reclaimablePackages = n.empty() ? -1 : _wtoi(n.c_str());
    cs.recommended = Lower(get(L"component store cleanup recommended")) == L"yes";
    return cs;
}

ComponentStore AnalyzeComponentStore() {
    AdminRun r = RunAsAdmin(Quote(SystemTool(L"dism.exe")) + L" /English /Online /Cleanup-Image /AnalyzeComponentStore");
    if (!r.error.empty()) {
        ComponentStore cs;
        cs.error = r.error;
        return cs;
    }
    return ParseComponentStore(r.out);
}

AdminRun CleanComponentStore() {
    return RunAsAdmin(Quote(SystemTool(L"dism.exe")) + L" /English /Online /Cleanup-Image /StartComponentCleanup");
}

// ---------------------------------------------------------------------------------------------
// Drivers

bool ParseDriverVer(const std::wstring& driverVer, int64_t& date, std::wstring& version) {
    int m = 0, d = 0, y = 0;
    if (swscanf_s(driverVer.c_str(), L"%d/%d/%d", &m, &d, &y) != 3 || m < 1 || m > 12 || d < 1 || d > 31 || y < 1980) return false;
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    date = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    size_t comma = driverVer.find(L',');
    version.clear();
    if (comma != std::wstring::npos) {
        version = driverVer.substr(comma + 1);
        size_t b = version.find_first_not_of(L' ');
        version = b == std::wstring::npos ? L"" : version.substr(b);
    }
    return true;
}

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        uint64_t x = 0, y = 0;
        while (i < a.size() && iswdigit(a[i])) x = x * 10 + (a[i++] - L'0');
        while (j < b.size() && iswdigit(b[j])) y = y * 10 + (b[j++] - L'0');
        if (x != y) return x < y ? -1 : 1;
        while (i < a.size() && !iswdigit(a[i])) ++i;
        while (j < b.size() && !iswdigit(b[j])) ++j;
    }
    return 0;
}

void MarkOldDrivers(std::vector<DriverPackage>& pkgs) {
    std::map<std::wstring, std::vector<size_t>> groups;
    for (size_t i = 0; i < pkgs.size(); ++i) {
        pkgs[i].old = false;
        if (pkgs[i].originalInf.empty() || Lower(pkgs[i].className) == L"printer") continue;
        groups[Lower(pkgs[i].originalInf) + L"|" + Lower(pkgs[i].provider) + L"|" + Lower(pkgs[i].className)].push_back(i);
    }
    for (auto& [key, idx] : groups) {
        if (idx.size() < 2) continue;
        size_t newest = idx[0];
        for (size_t i : idx) {
            const DriverPackage& a = pkgs[i];
            const DriverPackage& n = pkgs[newest];
            int c = a.date != n.date ? (a.date < n.date ? -1 : 1) : CompareVersions(a.version, n.version);
            if (c > 0) newest = i;
        }
        for (size_t i : idx)
            if (i != newest && !pkgs[i].inUse) pkgs[i].old = true;
    }
}

std::vector<DriverPackage> ListDriverPackages() {
    std::vector<DriverPackage> out;
    std::wstring infDir = WinDir() + L"\\INF\\";
    std::set<std::wstring> used = InfsInUse();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((infDir + L"oem*.inf").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        DriverPackage p;
        p.oemInf = Lower(fd.cFileName);
        std::wstring path = infDir + fd.cFileName;
        wchar_t loc[MAX_PATH * 2];
        DWORD need = 0;
        if (SetupGetInfDriverStoreLocationW(path.c_str(), nullptr, nullptr, loc, MAX_PATH * 2, &need)) {
            std::wstring l = loc;
            size_t slash = l.find_last_of(L'\\');
            if (slash != std::wstring::npos) {
                p.folder = l.substr(0, slash);
                p.originalInf = Lower(l.substr(slash + 1));
            }
        }
        HINF inf = SetupOpenInfFileW(path.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
        if (inf != INVALID_HANDLE_VALUE) {
            p.provider = InfField(inf, L"Version", L"Provider", 1);
            p.className = InfField(inf, L"Version", L"Class", 1);
            ParseDriverVer(InfField(inf, L"Version", L"DriverVer", 1) + L"," + InfField(inf, L"Version", L"DriverVer", 2), p.date, p.version);
            SetupCloseInfFile(inf);
        }
        p.inUse = used.count(p.oemInf) > 0;
        if (!p.folder.empty()) p.bytes = FolderSize(p.folder);
        out.push_back(std::move(p));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    MarkOldDrivers(out);
    std::stable_sort(out.begin(), out.end(), [](const DriverPackage& a, const DriverPackage& b) { return a.bytes > b.bytes; });
    return out;
}

AdminRun DeleteDriverPackages(const std::vector<DriverPackage>& pkgs) {
    std::wstring line;
    for (auto& p : pkgs) {
        // Only third-party packages by their published name, never /force.
        if (p.oemInf.rfind(L"oem", 0) != 0 || p.oemInf.find_first_of(L" \"&|<>^%") != std::wstring::npos) continue;
        line += (line.empty() ? L"" : L" & ") + Quote(SystemTool(L"pnputil.exe")) + L" /delete-driver " + p.oemInf;
    }
    if (line.empty()) return AdminRun{0, "", false, L""};
    return RunAsAdmin(line);
}

// ---------------------------------------------------------------------------------------------
// System Restore storage

std::vector<ShadowStorage> QueryShadowStorage(std::wstring& err) {
    std::vector<ShadowStorage> out;
    std::string raw;
    int code = RunTool(SystemTool(L"WindowsPowerShell\\v1.0\\powershell.exe"),
                       L"-NoProfile -NonInteractive -Command \"Get-CimInstance Win32_ShadowStorage | ForEach-Object { "
                       L"'{0}|{1}|{2}|{3}|{4}' -f $_.UsedSpace,$_.AllocatedSpace,$_.MaxSpace,$_.Volume.DeviceID,$_.DiffVolume.DeviceID }\"",
                       raw, 60000);
    if (code != 0) {
        err = RunningElevated() ? LastLine(raw) : L"needs administrator rights";
        return out;
    }
    auto drive = [](const std::wstring& volumeId) {
        wchar_t names[1024];
        DWORD n = 0;
        if (!GetVolumePathNamesForVolumeNameW(volumeId.c_str(), names, 1024, &n) || !names[0]) return std::wstring();
        std::wstring d = names;
        if (d.size() >= 2 && d[1] == L':') return d.substr(0, 2);
        return std::wstring();
    };
    for (auto& l : Lines(DecodeToolOutput(raw))) {
        std::vector<std::wstring> f;
        size_t pos = 0;
        for (size_t bar; (bar = l.find(L'|', pos)) != std::wstring::npos; pos = bar + 1) f.push_back(l.substr(pos, bar - pos));
        f.push_back(l.substr(pos));
        if (f.size() != 5) continue;
        ShadowStorage s;
        s.used = _wcstoui64(f[0].c_str(), nullptr, 10);
        s.allocated = _wcstoui64(f[1].c_str(), nullptr, 10);
        s.max = _wcstoui64(f[2].c_str(), nullptr, 10);
        s.forDrive = drive(f[3]);
        s.onDrive = drive(f[4]);
        if (s.forDrive.empty() || s.onDrive.empty()) continue;
        ULARGE_INTEGER total{};
        if (GetDiskFreeSpaceExW((s.onDrive + L"\\").c_str(), nullptr, &total, nullptr)) s.volumeSize = total.QuadPart;
        out.push_back(s);
    }
    return out;
}

AdminRun LimitShadowStorage(const ShadowStorage& s, int percent) {
    percent = std::clamp(percent, 1, 100);
    return RunAsAdmin(Quote(SystemTool(L"vssadmin.exe")) + L" resize shadowstorage /for=" + s.forDrive + L" /on=" + s.onDrive +
                      L" /maxsize=" + std::to_wstring(percent) + L"%");
}

// ---------------------------------------------------------------------------------------------
// Hibernation, Compact OS, event logs

Hibernation QueryHibernation() {
    Hibernation h;
    const wchar_t* power = L"SYSTEM\\CurrentControlSet\\Control\\Power";
    h.on = RegDword(HKEY_LOCAL_MACHINE, power, L"HibernateEnabled", 0) != 0;
    h.reduced = RegDword(HKEY_LOCAL_MACHINE, power, L"HiberFileType", 2) == 1;
    h.fastStartup = h.on && RegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", L"HiberbootEnabled", 0) != 0;
    std::wstring file = WinDir().substr(0, 2) + L"\\hiberfil.sys";
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(file.c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        h.bytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        FindClose(f);
    }
    return h;
}

AdminRun SetHibernation(const wchar_t* mode) {
    std::wstring args = !wcscmp(mode, L"reduced") ? L"/hibernate /type reduced" : !wcscmp(mode, L"on") ? L"/hibernate on" : L"/hibernate off";
    return RunAsAdmin(Quote(SystemTool(L"powercfg.exe")) + L" " + args);
}

bool CompactOsOn() {
    // With Compact OS, Windows' own binaries are WOF-compressed.
    std::wstring sys = SystemTool(L"");
    int compressed = 0;
    for (const wchar_t* f : {L"kernel32.dll", L"shell32.dll", L"user32.dll"}) {
        BOOL external = FALSE;
        ULONG provider = 0;
        if (SUCCEEDED(WofIsExternalFile((sys + f).c_str(), &external, &provider, nullptr, nullptr)) && external &&
            provider == WOF_PROVIDER_FILE)
            ++compressed;
    }
    return compressed >= 2;
}

AdminRun SetCompactOs(bool on) {
    return RunAsAdmin(Quote(SystemTool(L"compact.exe")) + (on ? L" /compactos:always" : L" /compactos:never"));
}

EventLogs QueryEventLogs() {
    EventLogs e;
    std::wstring dir = SystemTool(L"winevt\\Logs\\");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.evtx").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return e;
    e.known = true;
    do {
        uint64_t b = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.bytes += b;
        ++e.files;
        if (!_wcsicmp(fd.cFileName, L"Security.evtx")) e.security = b;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return e;
}

AdminRun ClearEventLogs() {
    // The Security log stays: it is the audit trail, and clearing it is what intruders do.
    return RunAsAdmin(L"for /f \"delims=\" %G in ('wevtutil el') do @if /i not \"%G\"==\"Security\" wevtutil cl \"%G\" 2>nul");
}
