#include "diskhealth.h"

#include <windows.h>
#include <shlobj.h>
#include <wbemidl.h>
#include <winioctl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>

#include "runtool.h"
#include "util.h"

namespace {

constexpr int64_t kDay = 864000000000LL;  // FILETIME ticks
constexpr int64_t kSecond = 10000000LL;

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

const std::wstring* Prop(const WmiProps& p, const wchar_t* key) {
    auto it = p.find(key);
    return it == p.end() || it->second.empty() ? nullptr : &it->second;
}

// The first number in a value ("4", "{2}", "1000204886016"), or `def`.
int64_t Num(const WmiProps& p, const wchar_t* key, int64_t def = -1) {
    const std::wstring* v = Prop(p, key);
    if (!v) return def;
    size_t i = v->find_first_of(L"-0123456789");
    if (i == std::wstring::npos) return def;
    return _wcstoi64(v->c_str() + i, nullptr, 10);
}

std::wstring Str(const WmiProps& p, const wchar_t* key) {
    const std::wstring* v = Prop(p, key);
    return v ? Trim(*v) : L"";
}

std::vector<int> NumList(const WmiProps& p, const wchar_t* key) {
    std::vector<int> out;
    const std::wstring* v = Prop(p, key);
    if (!v) return out;
    const wchar_t* s = v->c_str();
    while (*s) {
        if (iswdigit(*s)) {
            wchar_t* end = nullptr;
            out.push_back(static_cast<int>(wcstol(s, &end, 10)));
            s = end;
        } else {
            ++s;
        }
    }
    return out;
}

uint64_t Le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

int LevelRank(DiskHealthLevel l) { return static_cast<int>(l); }

std::wstring Count(int64_t n) { return util::FormatCount(static_cast<uint64_t>(n)); }

// ---- WMI

std::wstring VarText(const VARIANT& v) {
    if (v.vt == VT_EMPTY || v.vt == VT_NULL) return L"";
    if (v.vt & VT_ARRAY) {
        SAFEARRAY* sa = v.parray;
        VARTYPE t = static_cast<VARTYPE>(v.vt & VT_TYPEMASK);
        LONG lo = 0, hi = -1;
        if (!sa || FAILED(SafeArrayGetLBound(sa, 1, &lo)) || FAILED(SafeArrayGetUBound(sa, 1, &hi))) return L"";
        std::wstring out;
        for (LONG i = lo; i <= hi && i - lo < 256; ++i) {
            std::wstring item;
            if (t == VT_BSTR) {
                BSTR b = nullptr;
                if (SUCCEEDED(SafeArrayGetElement(sa, &i, &b)) && b) item = b;
                SysFreeString(b);
            } else {
                uint64_t raw = 0;
                if (FAILED(SafeArrayGetElement(sa, &i, &raw))) continue;
                int64_t n = 0;
                switch (t) {
                    case VT_I1: n = static_cast<int8_t>(raw); break;
                    case VT_UI1: n = static_cast<uint8_t>(raw); break;
                    case VT_I2: n = static_cast<int16_t>(raw); break;
                    case VT_UI2: n = static_cast<uint16_t>(raw); break;
                    case VT_I4:
                    case VT_INT: n = static_cast<int32_t>(raw); break;
                    case VT_UI4:
                    case VT_UINT: n = static_cast<uint32_t>(raw); break;
                    case VT_BOOL: n = static_cast<int16_t>(raw) ? 1 : 0; break;
                    default: n = static_cast<int64_t>(raw);
                }
                item = std::to_wstring(n);
            }
            out += (out.empty() ? L"" : L",") + item;
        }
        return out;
    }
    VARIANT c;
    VariantInit(&c);
    std::wstring out;
    if (SUCCEEDED(VariantChangeTypeEx(&c, &v, LOCALE_INVARIANT, VARIANT_ALPHABOOL, VT_BSTR)) && c.bstrVal) out = c.bstrVal;
    VariantClear(&c);
    return out;
}

WmiProps ObjectProps(IWbemClassObject* obj) {
    WmiProps p;
    if (FAILED(obj->BeginEnumeration(0))) return p;
    BSTR name = nullptr;
    VARIANT v;
    VariantInit(&v);
    while (obj->Next(0, &name, &v, nullptr, nullptr) == WBEM_S_NO_ERROR) {
        p[util::ToLower(name ? name : L"")] = VarText(v);
        SysFreeString(name);
        name = nullptr;
        VariantClear(&v);
    }
    obj->EndEnumeration();
    return p;
}

void SetBlanket(IUnknown* u) {
    CoSetProxyBlanket(u, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                      EOAC_NONE);
}

// Runs a WQL query; HRESULT of the first failure (the query itself or fetching a result).
HRESULT WmiQuery(IWbemServices* svc, const std::wstring& wql, std::vector<WmiProps>& out) {
    IEnumWbemClassObject* en = nullptr;
    BSTR lang = SysAllocString(L"WQL");
    BSTR q = SysAllocString(wql.c_str());
    HRESULT hr = svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang);
    SysFreeString(q);
    if (FAILED(hr) || !en) return FAILED(hr) ? hr : E_FAIL;
    SetBlanket(en);
    for (;;) {
        IWbemClassObject* obj = nullptr;
        ULONG n = 0;
        hr = en->Next(20000, 1, &obj, &n);
        if (FAILED(hr)) break;
        if (n == 0 || !obj) {
            hr = hr == WBEM_S_TIMEDOUT ? WBEM_E_TIMED_OUT : S_OK;
            break;
        }
        out.push_back(ObjectProps(obj));
        VARIANT path;
        VariantInit(&path);
        if (SUCCEEDED(obj->Get(L"__PATH", 0, &path, nullptr, nullptr)) && path.vt == VT_BSTR) out.back()[L"__path"] = path.bstrVal;
        VariantClear(&path);
        obj->Release();
    }
    en->Release();
    return hr;
}

// ---- IOCTL_STORAGE_QUERY_PROPERTY (handles opened with no access: queries only)

HANDLE OpenDisk(int number) {
    wchar_t path[64];
    swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", number);
    return CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

void QueryTemperature(HANDLE h, PhysicalDisk& d) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceTemperatureProperty;
    q.QueryType = PropertyStandardQuery;
    alignas(8) BYTE buf[1024]{};
    DWORD n = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &n, nullptr)) return;
    if (n < sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR)) return;
    auto* t = reinterpret_cast<STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buf);
    if (t->InfoCount > 0 && d.temperature < 0 && t->TemperatureInfo[0].Temperature > 0) d.temperature = t->TemperatureInfo[0].Temperature;
    if (d.temperatureMax < 0 && t->WarningTemperature > 0) d.temperatureMax = t->WarningTemperature;
}

void QueryNvmeLog(HANDLE h, PhysicalDisk& d) {
    constexpr DWORD kLog = 512;
    std::vector<BYTE> b(FIELD_OFFSET(STORAGE_PROPERTY_QUERY, AdditionalParameters) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + kLog);
    auto* q = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(b.data());
    auto* pd = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(q->AdditionalParameters);
    q->PropertyId = StorageDeviceProtocolSpecificProperty;
    q->QueryType = PropertyStandardQuery;
    pd->ProtocolType = ProtocolTypeNvme;
    pd->DataType = NVMeDataTypeLogPage;
    pd->ProtocolDataRequestValue = 2;  // SMART / Health Information
    pd->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    pd->ProtocolDataLength = kLog;
    DWORD n = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, b.data(), static_cast<DWORD>(b.size()), b.data(), static_cast<DWORD>(b.size()),
                         &n, nullptr))
        return;
    auto* desc = reinterpret_cast<STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(b.data());
    const STORAGE_PROTOCOL_SPECIFIC_DATA& data = desc->ProtocolSpecificData;
    size_t start = reinterpret_cast<const BYTE*>(&data) - b.data() + data.ProtocolDataOffset;
    if (data.ProtocolDataOffset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) || start + std::min<size_t>(data.ProtocolDataLength, kLog) > b.size())
        return;
    ApplyNvmeHealthLog(d, b.data() + start, std::min<size_t>(data.ProtocolDataLength, kLog));
}

TrimStatus QueryTrim() {
    std::string out;
    int code = RunTool(SystemTool(L"fsutil.exe"), L"behavior query DisableDeleteNotify", out, 15000);
    TrimStatus t = ParseFsutilTrim(DecodeToolOutput(out));
    if (code != 0 && t.error.empty()) t.error = L"fsutil: " + LastLine(out);
    if (t.ntfs < 0) {
        // The value fsutil reads for NTFS.
        DWORD v = 0, size = sizeof(v);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"DisableDeleteNotification",
                         RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS) {
            t.ntfs = v == 0 ? 1 : 0;
            t.error.clear();
        }
    }
    return t;
}

std::wstring LocalDataDir() {
    return util::DataDir();  // %LOCALAPPDATA%\WinNCDU, or WinNCDU-data next to a portable exe
}

std::string ReadSmallFile(const std::wstring& path) {
    std::string out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char b[16384];
    DWORD n = 0;
    while (out.size() < (4u << 20) && ReadFile(h, b, sizeof(b), &n, nullptr) && n) out.append(b, n);
    CloseHandle(h);
    return out;
}

std::wstring DaysText(double days) {
    if (days < 60) return std::to_wstring(std::max<long long>(1, std::llround(days))) + L" days";
    if (days < 730) return std::to_wstring(std::llround(days / 30.4)) + L" months";
    wchar_t b[32];
    swprintf(b, 32, L"%.1f years", days / 365.25);
    return b;
}

}  // namespace

// =============================================================================================
// Names and verdicts

std::wstring BusName(int busType) {
    switch (busType) {
        case 1: return L"SCSI";
        case 2: return L"ATAPI";
        case 3: return L"ATA";
        case 4: return L"FireWire";
        case 6: return L"Fibre Channel";
        case 7: return L"USB";
        case 8: return L"RAID";
        case 9: return L"iSCSI";
        case 10: return L"SAS";
        case 11: return L"SATA";
        case 12: return L"SD card";
        case 13: return L"MMC";
        case 14:
        case 15: return L"Virtual";
        case 16: return L"Storage Spaces";
        case 17: return L"NVMe";
        case 18: return L"SCM";
        case 19: return L"UFS";
        default: return L"";
    }
}

std::wstring MediaName(const PhysicalDisk& d) {
    std::wstring bus = BusName(d.busType);
    if (d.busType == 17) return L"NVMe SSD";
    if (d.mediaType == 3) return bus == L"USB" ? L"USB hard disk" : L"Hard disk";
    if (d.mediaType == 4) return bus.empty() ? L"SSD" : bus + L" SSD";
    if (d.mediaType == 5) return L"Storage-class memory";
    return bus.empty() ? L"Disk" : bus + L" disk";
}

std::wstring OpStatusName(int code) {
    switch (code) {
        case 0: return L"Unknown";
        case 1: return L"Other";
        case 2: return L"OK";
        case 3: return L"Degraded";
        case 4: return L"Stressed";
        case 5: return L"Predictive Failure";
        case 6: return L"Error";
        case 7: return L"Non-Recoverable Error";
        case 8: return L"Starting";
        case 9: return L"Stopping";
        case 10: return L"Stopped";
        case 11: return L"In Service";
        case 12: return L"No Contact";
        case 13: return L"Lost Communication";
        case 14: return L"Aborted";
        case 15: return L"Dormant";
        case 16: return L"Supporting Entity in Error";
        case 17: return L"Completed";
        case 18: return L"Power Mode";
        case 0xD010: return L"In Maintenance";
        case 0xD011: return L"Not Usable";
        case 0xD012: return L"Transient Error";
        case 0xD013: return L"Failed Media";
        default: return L"status " + std::to_wstring(code);
    }
}

const wchar_t* HealthLevelName(DiskHealthLevel l) {
    switch (l) {
        case DiskHealthLevel::Healthy: return L"Healthy";
        case DiskHealthLevel::Warning: return L"Warning";
        case DiskHealthLevel::Unhealthy: return L"Unhealthy";
        default: return L"Unknown";
    }
}

DiskVerdict AssessDisk(const PhysicalDisk& d) {
    DiskVerdict v;
    auto raise = [&](DiskHealthLevel l, std::wstring why) {
        if (LevelRank(l) > LevelRank(v.level)) v.level = l;
        if (!why.empty()) v.problems.push_back(std::move(why));
    };
    switch (d.healthStatus) {
        case 0: raise(DiskHealthLevel::Healthy, L""); break;
        case 1: raise(DiskHealthLevel::Warning, L"Windows reports a warning for this disk"); break;
        case 2: raise(DiskHealthLevel::Unhealthy, L"Windows reports this disk as unhealthy: back up what is on it now"); break;
        default: break;
    }
    for (int s : d.opStatus) {
        switch (s) {
            case 5: raise(DiskHealthLevel::Warning, L"Windows predicts this disk will fail (Predictive Failure): back it up now"); break;
            case 3: raise(DiskHealthLevel::Warning, L"Windows reports it as degraded"); break;
            case 12:
            case 13:
            case 0xD012: raise(DiskHealthLevel::Warning, L"Windows reports: " + OpStatusName(s)); break;
            case 6:
            case 7:
            case 0xD011:
            case 0xD013: raise(DiskHealthLevel::Unhealthy, L"Windows reports: " + OpStatusName(s)); break;
            default: break;
        }
    }
    if (d.criticalWarning > 0) {
        int c = d.criticalWarning;
        if (c & 0x01)
            raise(DiskHealthLevel::Warning, L"The drive's spare blocks are below its threshold (" + std::to_wstring(d.availableSpare) + L"% left)");
        if (c & 0x02) raise(DiskHealthLevel::Warning, L"The drive reports a temperature over its limit");
        if (c & 0x04) raise(DiskHealthLevel::Unhealthy, L"The drive reports degraded reliability: back up what is on it now");
        if (c & 0x08) raise(DiskHealthLevel::Unhealthy, L"The drive has switched itself to read-only");
        if (c & 0x10) raise(DiskHealthLevel::Warning, L"The drive's backup power failed");
    }
    if (d.wear >= 100)
        raise(DiskHealthLevel::Warning, L"It has used up its rated endurance (" + std::to_wstring(d.wear) +
                                            L"%): it may keep working, but plan to replace it");
    else if (d.wear >= 90)
        v.notes.push_back(std::to_wstring(d.wear) + L"% of its rated endurance is used up");
    if (d.temperature > 0 && d.temperatureMax > 0 && d.temperature >= d.temperatureMax)
        raise(DiskHealthLevel::Warning, L"It is running hot: " + std::to_wstring(d.temperature) + L" °C (rated for " +
                                            std::to_wstring(d.temperatureMax) + L" °C)");
    else if (d.temperature >= 70 && d.temperatureMax <= 0)
        v.notes.push_back(L"It is warm: " + std::to_wstring(d.temperature) + L" °C");
    int64_t uncorrected = std::max<int64_t>(0, d.readErrorsUncorrected) + std::max<int64_t>(0, d.writeErrorsUncorrected);
    std::wstring rated = v.level == DiskHealthLevel::Healthy ? L"Windows still rates it healthy; " : L"";
    if (d.mediaErrors > 0)
        v.notes.push_back(Count(d.mediaErrors) + (d.mediaErrors == 1 ? L" media error" : L" media errors") +
                          L" logged over its life (data the drive couldn't read back). " + rated + L"keep backups current");
    else if (uncorrected > 0)
        v.notes.push_back(Count(uncorrected) + (uncorrected == 1 ? L" read/write error" : L" read/write errors") +
                          L" the drive couldn't correct. " + rated + L"keep backups current");
    if (d.availableSpare >= 0 && d.spareThreshold > 0 && !(d.criticalWarning > 0 && (d.criticalWarning & 1)) &&
        d.availableSpare < 100 && d.availableSpare <= d.spareThreshold + 10)
        v.notes.push_back(L"Only " + std::to_wstring(d.availableSpare) + L"% spare blocks left (the drive warns at " +
                          std::to_wstring(d.spareThreshold) + L"%)");
    return v;
}

// =============================================================================================
// Parsing

std::vector<WmiProps> ParseFormatList(const std::wstring& text) {
    std::vector<WmiProps> out;
    WmiProps cur;
    std::wstring lastKey;
    auto flush = [&] {
        if (!cur.empty()) out.push_back(std::move(cur));
        cur.clear();
        lastKey.clear();
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring t = Trim(line);
        if (t.empty()) {
            flush();
            continue;
        }
        if ((line[0] == L' ' || line[0] == L'\t') && !lastKey.empty()) {
            cur[lastKey] += t;  // a long value wrapped onto the next line
            continue;
        }
        size_t colon = line.find(L':');
        if (colon == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, colon));
        if (key.empty() || key.find_first_of(L" \t") != std::wstring::npos) continue;
        lastKey = util::ToLower(key);
        cur[lastKey] = Trim(line.substr(colon + 1));
    }
    flush();
    // "{2, 53264}" -> "2,53264"
    for (auto& o : out)
        for (auto& kv : o) {
            std::wstring& v = kv.second;
            if (v.size() >= 2 && v.front() == L'{' && v.back() == L'}' && v.find(L'\\') == std::wstring::npos) {
                std::wstring inner = v.substr(1, v.size() - 2), r;
                for (wchar_t c : inner)
                    if (c != L' ') r += c;
                v = r;
            }
        }
    return out;
}

PhysicalDisk DiskFromWmi(const WmiProps& p) {
    PhysicalDisk d;
    d.number = static_cast<int>(Num(p, L"deviceid"));
    d.model = Str(p, L"friendlyname");
    if (d.model.empty()) d.model = Str(p, L"model");
    if (d.model.empty()) d.model = L"Disk " + std::to_wstring(d.number);
    d.firmware = Str(p, L"firmwareversion");
    d.busType = static_cast<int>(Num(p, L"bustype", 0));
    d.mediaType = static_cast<int>(Num(p, L"mediatype", 0));
    d.size = static_cast<uint64_t>(std::max<int64_t>(0, Num(p, L"size", 0)));
    d.healthStatus = static_cast<int>(Num(p, L"healthstatus", 5));
    d.opStatus = NumList(p, L"operationalstatus");
    return d;
}

void ApplyReliabilityCounter(PhysicalDisk& d, const WmiProps& p) {
    auto get = [&](const wchar_t* key) { return Num(p, key); };
    bool any = false;
    auto set = [&](auto& field, int64_t v, bool zeroUnknown = false) {
        if (v < 0 || (zeroUnknown && v == 0)) return;
        field = static_cast<std::remove_reference_t<decltype(field)>>(v);
        any = true;
    };
    set(d.wear, get(L"wear"));
    set(d.temperature, get(L"temperature"), true);
    set(d.temperatureMax, get(L"temperaturemax"), true);
    set(d.powerOnHours, get(L"poweronhours"));
    set(d.readErrors, get(L"readerrorstotal"));
    set(d.readErrorsUncorrected, get(L"readerrorsuncorrected"));
    set(d.writeErrors, get(L"writeerrorstotal"));
    set(d.writeErrorsUncorrected, get(L"writeerrorsuncorrected"));
    set(d.startStopCycles, get(L"startstopcyclecount"));
    if (any) d.countersRead = true;
}

bool ApplyNvmeHealthLog(PhysicalDisk& d, const uint8_t* log, size_t size) {
    if (!log || size < 192) return false;
    bool any = false;
    for (size_t i = 0; i < 192 && !any; ++i) any = log[i] != 0;
    if (!any) return false;
    d.criticalWarning = log[0];
    int kelvin = log[1] | log[2] << 8;
    if (kelvin > 273 && d.temperature < 0) d.temperature = kelvin - 273;
    d.availableSpare = log[3];
    d.spareThreshold = log[4];
    if (d.wear < 0) d.wear = log[5];
    // Data units are thousands of 512-byte blocks.
    d.bytesRead = Le64(log + 32) * 512000ull;
    d.bytesWritten = Le64(log + 48) * 512000ull;
    if (d.powerOnHours < 0) d.powerOnHours = static_cast<int64_t>(Le64(log + 128));
    d.unsafeShutdowns = static_cast<int64_t>(Le64(log + 144));
    d.mediaErrors = static_cast<int64_t>(Le64(log + 160));
    d.nvmeLogRead = true;
    return true;
}

void AssignDrives(std::vector<PhysicalDisk>& disks, const std::vector<VolumeDisks>& volumes) {
    for (auto& d : disks) {
        d.drives.clear();
        d.fileSystems.clear();
    }
    for (auto& v : volumes)
        for (int n : v.disks)
            for (auto& d : disks)
                if (d.number == n && std::find(d.drives.begin(), d.drives.end(), v.drive) == d.drives.end()) {
                    d.drives.push_back(v.drive);
                    d.fileSystems.push_back(v.fs);
                }
}

TrimStatus ParseFsutilTrim(const std::wstring& out) {
    TrimStatus t;
    bool seen = false;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find(L'\n', pos);
        std::wstring line = out.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? out.size() : nl + 1;
        size_t k = line.find(L"DisableDeleteNotify");
        if (k == std::wstring::npos) continue;
        seen = true;
        // "NTFS DisableDeleteNotify = 0  (...)", "ReFS DisableDeleteNotify is not currently set",
        // older Windows: "DisableDeleteNotify = 0" (NTFS).
        int* field = line.find(L"ReFS") != std::wstring::npos ? &t.refs : &t.ntfs;
        size_t eq = line.find(L'=', k);
        if (eq == std::wstring::npos) continue;
        size_t dg = line.find_first_of(L"0123456789", eq);
        if (dg == std::wstring::npos) continue;
        *field = line[dg] == L'0' ? 1 : 0;
    }
    if (!seen) {
        std::wstring last = Trim(out);
        size_t nl = last.find_last_of(L"\r\n");
        t.error = nl == std::wstring::npos ? last : Trim(last.substr(nl + 1));
        if (t.error.empty()) t.error = L"fsutil printed nothing";
    }
    return t;
}

int TrimFor(const TrimStatus& t, const std::wstring& fs) {
    std::wstring f = util::ToLower(fs);
    if (f == L"ntfs") return t.ntfs;
    if (f == L"refs") return t.refs;
    return -1;
}

// =============================================================================================
// Trends

UsageTrend ComputeTrend(std::vector<UsagePoint> pts, int64_t now, uint64_t freeNow, int windowDays) {
    UsageTrend t;
    std::sort(pts.begin(), pts.end(), [](const UsagePoint& a, const UsagePoint& b) { return a.time < b.time; });
    pts.erase(std::remove_if(pts.begin(), pts.end(), [now](const UsagePoint& p) { return p.time <= 0 || p.time > now + kDay; }),
              pts.end());
    if (pts.size() < 2) return t;
    const UsagePoint& last = pts.back();
    int64_t windowStart = now - windowDays * kDay;
    size_t base = 0;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        if (pts[i].time <= windowStart) base = i;
    int64_t span = last.time - pts[base].time;
    if (span < kDay) {
        // Too short to say anything; maybe an older point gives a longer span.
        if (base == 0) return t;
        base = 0;
        span = last.time - pts[0].time;
        if (span < kDay) return t;
    }
    t.ok = true;
    t.delta = static_cast<int64_t>(last.used) - static_cast<int64_t>(pts[base].used);
    t.days = static_cast<double>(span) / kDay;
    t.recent = now - last.time < 2 * kDay;
    if (t.delta > 0 && t.days >= 7) {
        double perDay = t.delta / t.days;
        t.daysToFull = static_cast<int64_t>(freeNow / perDay);
    }
    // At most 90 points for the sparkline: evenly spaced picks, always keeping both ends.
    size_t n = pts.size() - base;
    size_t keep = std::min<size_t>(n, 90);
    for (size_t k = 0; k < keep; ++k) t.points.push_back(pts[base + (keep == 1 ? 0 : k * (n - 1) / (keep - 1))]);
    return t;
}

std::wstring TrendText(const UsageTrend& t, const std::wstring& what, uint64_t total) {
    if (!t.ok) return L"";
    std::wstring span = t.recent ? L"in the last " + DaysText(t.days) : L"over " + DaysText(t.days) + L" (up to " +
                                                                             util::FormatTime(t.points.back().time).substr(0, 10) + L")";
    if (t.days >= 0.5 && t.days < 1.5) span = t.recent ? L"in the last day" : L"in one day";
    uint64_t mag = static_cast<uint64_t>(std::llabs(t.delta));
    uint64_t steady = std::max<uint64_t>(256ull << 20, total / 1000);
    if (mag < steady) return what + L" stayed about the same " + span;
    std::wstring s = what + (t.delta > 0 ? L" grew " : L" shrank ") + util::FormatSizeShort(mag) + L" " + span;
    if (t.daysToFull >= 0 && t.daysToFull < 3 * 365) s += L"; at this rate the drive is full in about " + DaysText(static_cast<double>(t.daysToFull));
    return s;
}

std::wstring FreeSpaceLogPath() {
    wchar_t over[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"WINNCDU_FREESPACE_LOG", over, MAX_PATH);  // tests
    if (n && n < MAX_PATH) return over;
    std::wstring dir = LocalDataDir();
    return dir.empty() ? L"" : dir + L"\\freespace.log";
}

std::vector<FreeSample> ParseFreeSpaceLog(const std::string& text) {
    std::vector<FreeSample> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        long long secs = 0;
        char drive[8] = {};
        unsigned serial = 0;
        unsigned long long total = 0, free = 0;
        if (sscanf_s(line.c_str(), "%lld\t%7s\t%x\t%llu\t%llu", &secs, drive, static_cast<unsigned>(sizeof(drive)), &serial, &total, &free) != 5)
            continue;
        if (secs <= 0 || !total || free > total || strlen(drive) != 2 || drive[1] != ':') continue;
        FreeSample s;
        s.time = util::UnixToFileTime(secs);
        s.drive = std::wstring(1, static_cast<wchar_t>(towupper(static_cast<wint_t>(drive[0])))) + L":";
        s.serial = serial;
        s.total = total;
        s.free = free;
        out.push_back(s);
    }
    return out;
}

std::string FormatFreeSample(const FreeSample& s) {
    char b[160];
    snprintf(b, sizeof(b), "%lld\t%s\t%08x\t%llu\t%llu\n", static_cast<long long>(util::FileTimeToUnix(s.time)),
             util::WideToUtf8(s.drive).c_str(), s.serial, static_cast<unsigned long long>(s.total), static_cast<unsigned long long>(s.free));
    return b;
}

std::vector<FreeSample> LoadFreeSpaceLog() {
    std::wstring path = FreeSpaceLogPath();
    return path.empty() ? std::vector<FreeSample>{} : ParseFreeSpaceLog(ReadSmallFile(path));
}

uint32_t VolumeSerial(const std::wstring& root) {
    DWORD serial = 0;
    std::wstring r = root;
    if (!r.empty() && r.back() != L'\\') r += L'\\';
    if (!GetVolumeInformationW(r.c_str(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) return 0;
    return serial;
}

void RecordFreeSpace(const std::vector<std::wstring>& roots, int64_t minGapSeconds) {
    std::wstring path = FreeSpaceLogPath();
    if (path.empty()) return;
    std::vector<FreeSample> log = LoadFreeSpaceLog();
    int64_t now = util::NowFileTime();
    std::string add;
    for (auto& root : roots) {
        if (root.size() < 2 || root[1] != L':') continue;
        std::wstring r = root.substr(0, 2) + L"\\";
        if (GetDriveTypeW(r.c_str()) != DRIVE_FIXED) continue;
        FreeSample s;
        s.time = now;
        s.drive = std::wstring(1, static_cast<wchar_t>(towupper(r[0]))) + L":";
        s.serial = VolumeSerial(r);
        if (!QueryFreeSpace(r, s.total, s.free) || !s.total) continue;
        bool recent = false;
        for (auto& o : log)
            if (o.drive == s.drive && o.serial == s.serial && now - o.time < minGapSeconds * kSecond) recent = true;
        if (recent) continue;
        add += FormatFreeSample(s);
        log.push_back(s);
    }
    if (add.empty()) return;
    size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    // Keep the file small: past ~5000 samples, rewrite it with the newest 3000.
    if (log.size() > 5000) {
        std::sort(log.begin(), log.end(), [](const FreeSample& a, const FreeSample& b) { return a.time < b.time; });
        std::string all;
        for (size_t i = log.size() - 3000; i < log.size(); ++i) all += FormatFreeSample(log[i]);
        std::wstring tmp = path + L".tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w = 0;
        BOOL ok = WriteFile(h, all.data(), static_cast<DWORD>(all.size()), &w, nullptr) && w == all.size();
        CloseHandle(h);
        if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        DeleteFileW(tmp.c_str());
        return;
    }
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, add.data(), static_cast<DWORD>(add.size()), &w, nullptr);
    CloseHandle(h);
}

DriveTrend BuildDriveTrend(const DriveInfo& d, uint32_t serial, const std::vector<FreeSample>& log, const std::vector<UsagePoint>& scans,
                           int64_t now) {
    DriveTrend r;
    r.drive = d.root.substr(0, 2);
    r.label = d.label;
    r.fs = d.fs;
    r.total = d.total;
    r.free = d.free;
    std::vector<UsagePoint> pts;
    for (auto& s : log)
        if (_wcsicmp(s.drive.c_str(), r.drive.c_str()) == 0 && (!serial || !s.serial || s.serial == serial))
            pts.push_back({s.time, s.total - s.free});
    if (d.ready && d.total >= d.free && d.total) {
        bool same = false;
        for (auto& p : pts) same |= now - p.time < 600 * kSecond;
        if (!same) pts.push_back({now, d.total - d.free});
    }
    r.trend = ComputeTrend(pts, now, d.free);
    r.samples = pts.size();
    if (r.trend.ok) {
        r.text = TrendText(r.trend, L"Used space", d.total);
        return r;
    }
    if (scans.size() >= 2) {
        UsageTrend s = ComputeTrend(scans, now, d.free);
        if (s.ok) {
            r.trend = s;
            r.fromScans = true;
            r.samples = scans.size();
            r.text = TrendText(s, L"Scanned size", d.total);
            return r;
        }
    }
    r.text = L"Scan this drive a few times to see a trend";
    return r;
}

// =============================================================================================
// Queries

std::vector<VolumeDisks> ListVolumeDisks(const std::vector<DriveInfo>& drives) {
    std::vector<VolumeDisks> out;
    for (auto& d : drives) {
        if (!d.ready || (d.type != DRIVE_FIXED && d.type != DRIVE_REMOVABLE) || d.root.size() < 2) continue;
        std::wstring dev = L"\\\\.\\" + d.root.substr(0, 2);
        HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        alignas(8) BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + 31 * sizeof(DISK_EXTENT)]{};
        DWORD n = 0;
        if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf, sizeof(buf), &n, nullptr)) {
            auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf);
            VolumeDisks v;
            v.drive = d.root.substr(0, 2);
            v.fs = d.fs;
            for (DWORD i = 0; i < ext->NumberOfDiskExtents && i < 32; ++i) {
                int num = static_cast<int>(ext->Extents[i].DiskNumber);
                if (std::find(v.disks.begin(), v.disks.end(), num) == v.disks.end()) v.disks.push_back(num);
            }
            out.push_back(v);
        }
        CloseHandle(h);
    }
    return out;
}

DiskHealthReport QueryDiskHealth() {
    DiskHealthReport r;
    r.elevated = RunningElevated();
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWbemLocator* loc = nullptr;
    IWbemServices* svc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&loc));
    if (SUCCEEDED(hr)) {
        BSTR ns = SysAllocString(L"ROOT\\Microsoft\\Windows\\Storage");
        hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
        SysFreeString(ns);
    }
    if (SUCCEEDED(hr) && svc) {
        SetBlanket(svc);
        std::vector<WmiProps> objs;
        hr = WmiQuery(svc, L"SELECT * FROM MSFT_PhysicalDisk", objs);
        for (auto& o : objs) {
            PhysicalDisk d = DiskFromWmi(o);
            auto path = o.find(L"__path");
            if (path != o.end()) {
                std::vector<WmiProps> rc;
                HRESULT h2 = WmiQuery(svc, L"ASSOCIATORS OF {" + path->second + L"} WHERE AssocClass = MSFT_PhysicalDiskToStorageReliabilityCounter",
                                      rc);
                if (SUCCEEDED(h2) && !rc.empty())
                    ApplyReliabilityCounter(d, rc[0]);
                else
                    d.countersDenied = !r.elevated;
            }
            r.disks.push_back(std::move(d));
        }
        if (FAILED(hr) && objs.empty()) r.error = L"Windows' storage provider didn't answer (WMI error 0x" + [&] {
            wchar_t b[16];
            swprintf(b, 16, L"%08lX", static_cast<unsigned long>(hr));
            return std::wstring(b);
        }() + L")";
    } else {
        wchar_t b[96];
        swprintf(b, 96, L"Couldn't connect to Windows' storage provider (WMI error 0x%08lX)", static_cast<unsigned long>(hr));
        r.error = b;
    }
    if (svc) svc->Release();
    if (loc) loc->Release();
    if (SUCCEEDED(init)) CoUninitialize();

    // What the drives answer themselves (no administrator rights needed).
    for (auto& d : r.disks) {
        if (d.number < 0) continue;
        HANDLE h = OpenDisk(d.number);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (d.busType == 17) QueryNvmeLog(h, d);
        QueryTemperature(h, d);
        CloseHandle(h);
    }
    AssignDrives(r.disks, ListVolumeDisks(ListDrives()));
    std::sort(r.disks.begin(), r.disks.end(), [](const PhysicalDisk& a, const PhysicalDisk& b) { return a.number < b.number; });
    r.trim = QueryTrim();
    return r;
}
