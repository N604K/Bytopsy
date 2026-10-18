#include "driverupdates.h"

#include <windows.h>
#include <initguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <wrl/client.h>
#include <wuapi.h>

#include <algorithm>
#include <cwctype>
#include <map>

#include "i18n.h"
#include "journal.h"
#include "util.h"
#include "wintools.h"

using Microsoft::WRL::ComPtr;

const wchar_t* const kDriverSearchCriteria = L"IsInstalled=0 and Type='Driver'";

namespace {

std::wstring Lower(std::wstring_view s) { return util::ToLower(s); }

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool StartsWith(const std::wstring& s, const std::wstring& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

constexpr int64_t kDay = 864000000000LL;  // FILETIME ticks

std::wstring DateIso(int64_t ft) {
    if (!ft) return L"";
    FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)};
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&f, &st)) return L"";
    wchar_t b[16];
    swprintf(b, 16, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return b;
}

std::wstring Hex(long hr) {
    wchar_t b[16];
    swprintf(b, 16, L"0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

// One line field of the result protocol: no tabs or line breaks.
std::wstring Field(std::wstring s) {
    for (auto& c : s)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return s;
}

// ---- SetupAPI properties

std::vector<BYTE> Prop(HDEVINFO devs, SP_DEVINFO_DATA& dev, const DEVPROPKEY& key, DEVPROPTYPE& type) {
    DWORD need = 0;
    type = 0;
    SetupDiGetDevicePropertyW(devs, &dev, &key, &type, nullptr, 0, &need, 0);
    if (!need) return {};
    std::vector<BYTE> b(need + sizeof(wchar_t) * 2, 0);
    if (!SetupDiGetDevicePropertyW(devs, &dev, &key, &type, b.data(), need, &need, 0)) return {};
    return b;
}

std::wstring PropStr(HDEVINFO devs, SP_DEVINFO_DATA& dev, const DEVPROPKEY& key) {
    DEVPROPTYPE t = 0;
    std::vector<BYTE> b = Prop(devs, dev, key, t);
    if (b.empty() || t != DEVPROP_TYPE_STRING) return L"";
    return reinterpret_cast<const wchar_t*>(b.data());
}

std::vector<std::wstring> PropList(HDEVINFO devs, SP_DEVINFO_DATA& dev, const DEVPROPKEY& key) {
    std::vector<std::wstring> out;
    DEVPROPTYPE t = 0;
    std::vector<BYTE> b = Prop(devs, dev, key, t);
    if (b.empty() || t != DEVPROP_TYPE_STRING_LIST) return out;
    const wchar_t* p = reinterpret_cast<const wchar_t*>(b.data());
    const wchar_t* end = p + b.size() / sizeof(wchar_t);
    while (p < end && *p) {
        std::wstring s = p;
        p += s.size() + 1;
        out.push_back(std::move(s));
    }
    return out;
}

// ---- COM helpers

struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

struct Bstr {
    BSTR b = nullptr;
    explicit Bstr(const std::wstring& s) : b(SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {}
    ~Bstr() { SysFreeString(b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};

std::wstring Take(BSTR b) {
    std::wstring s = b ? std::wstring(b, SysStringLen(b)) : L"";
    SysFreeString(b);
    return s;
}

template <class T>
std::wstring Str(T* obj, HRESULT (STDMETHODCALLTYPE T::*fn)(BSTR*)) {
    BSTR b = nullptr;
    if (FAILED((obj->*fn)(&b))) return L"";
    return Take(b);
}

template <class T>
bool Flag(T* obj, HRESULT (STDMETHODCALLTYPE T::*fn)(VARIANT_BOOL*), bool def) {
    VARIANT_BOOL v = def ? VARIANT_TRUE : VARIANT_FALSE;
    if (FAILED((obj->*fn)(&v))) return def;
    return v != VARIANT_FALSE;
}

int64_t DateToFileTime(DATE d) {
    SYSTEMTIME st{};
    if (d == 0 || !VariantTimeToSystemTime(d, &st)) return 0;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

DriverUpdate ReadUpdate(IUpdate* u) {
    DriverUpdate d;
    d.title = Str(u, &IUpdate::get_Title);
    d.description = Str(u, &IUpdate::get_Description);
    d.supportUrl = Str(u, &IUpdate::get_SupportUrl);
    ComPtr<IUpdateIdentity> id;
    if (SUCCEEDED(u->get_Identity(&id)) && id) {
        d.updateId = Lower(Str(id.Get(), &IUpdateIdentity::get_UpdateID));
        LONG rev = 0;
        if (SUCCEEDED(id->get_RevisionNumber(&rev))) d.revision = rev;
    }
    DECIMAL dec{};
    ULONG64 size = 0;
    if (SUCCEEDED(u->get_MaxDownloadSize(&dec)) && SUCCEEDED(VarUI8FromDec(&dec, &size))) d.bytes = size;
    ComPtr<IStringCollection> kbs;
    if (SUCCEEDED(u->get_KBArticleIDs(&kbs)) && kbs) {
        LONG n = 0;
        kbs->get_Count(&n);
        for (LONG i = 0; i < n; ++i) {
            BSTR b = nullptr;
            if (SUCCEEDED(kbs->get_Item(i, &b))) d.kbs.push_back(L"KB" + Take(b));
        }
    }
    d.downloaded = Flag(u, &IUpdate::get_IsDownloaded, false);
    d.eulaAccepted = Flag(u, &IUpdate::get_EulaAccepted, true);
    d.hidden = Flag(u, &IUpdate::get_IsHidden, false);
    ComPtr<IUpdate3> u3;
    if (SUCCEEDED(u->QueryInterface(IID_PPV_ARGS(&u3))) && u3) d.optional = Flag(u3.Get(), &IUpdate3::get_BrowseOnly, false);
    ComPtr<IInstallationBehavior> ib;
    if (SUCCEEDED(u->get_InstallationBehavior(&ib)) && ib) {
        InstallationRebootBehavior rb = irbNeverReboots;
        if (SUCCEEDED(ib->get_RebootBehavior(&rb))) d.reboot = static_cast<int>(rb);
    }
    ComPtr<IWindowsDriverUpdate> drv;
    if (SUCCEEDED(u->QueryInterface(IID_PPV_ARGS(&drv))) && drv) {
        d.driverClass = Str(drv.Get(), &IWindowsDriverUpdate::get_DriverClass);
        d.hardwareId = Str(drv.Get(), &IWindowsDriverUpdate::get_DriverHardwareID);
        d.manufacturer = Str(drv.Get(), &IWindowsDriverUpdate::get_DriverManufacturer);
        d.model = Str(drv.Get(), &IWindowsDriverUpdate::get_DriverModel);
        d.provider = Str(drv.Get(), &IWindowsDriverUpdate::get_DriverProvider);
        DATE date = 0;
        if (SUCCEEDED(drv->get_DriverVerDate(&date))) d.date = DateToFileTime(date);
    }
    d.version = VersionFromTitle(d.title);
    return d;
}

// BeginSearch needs a completion callback; the search is polled instead, so it can be cancelled.
class SearchCallback : public ISearchCompletedCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISearchCompletedCallback)) {
            *p = static_cast<ISearchCompletedCallback*>(this);
            AddRef();
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ISearchJob*, ISearchCompletedCallbackArgs*) override { return S_OK; }

private:
    virtual ~SearchCallback() = default;
    LONG ref_ = 1;
};

HRESULT RunSearch(IUpdateSearcher* searcher, std::atomic<bool>* cancel, ComPtr<ISearchResult>& result) {
    Bstr criteria(kDriverSearchCriteria);
    if (!cancel) return searcher->Search(criteria.b, &result);
    SearchCallback* cb = new SearchCallback();
    VARIANT state;
    VariantInit(&state);
    ComPtr<ISearchJob> job;
    HRESULT hr = searcher->BeginSearch(criteria.b, cb, state, &job);
    cb->Release();
    if (FAILED(hr)) return hr;
    bool aborted = false;
    for (;;) {
        VARIANT_BOOL done = VARIANT_FALSE;
        if (FAILED(job->get_IsCompleted(&done)) || done) break;
        if (*cancel && !aborted) {
            job->RequestAbort();
            aborted = true;
        }
        Sleep(100);
    }
    hr = searcher->EndSearch(job.Get(), &result);
    job->CleanUp();
    if (aborted) return static_cast<HRESULT>(0x8024000BL);  // WU_E_CALL_CANCELLED
    return hr;
}

struct Session {
    ComPtr<IUpdateSession> session;
    HRESULT hr = S_OK;
    Session() {
        hr = CoCreateInstance(__uuidof(UpdateSession), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&session));
        if (SUCCEEDED(hr)) {
            Bstr app(L"Bytopsy");
            session->put_ClientApplicationID(app.b);
        }
    }
};

// Searches and returns the IUpdate objects too (installing needs them).
HRESULT Search(Session& s, std::atomic<bool>* cancel, std::vector<ComPtr<IUpdate>>& items, std::wstring& err) {
    if (FAILED(s.hr)) {
        err = TrF(L"Windows Update isn't available: {0}", WuErrorText(s.hr));
        return s.hr;
    }
    ComPtr<IUpdateSearcher> searcher;
    HRESULT hr = s.session->CreateUpdateSearcher(&searcher);
    if (FAILED(hr)) {
        err = WuErrorText(hr);
        return hr;
    }
    ComPtr<ISearchResult> result;
    hr = RunSearch(searcher.Get(), cancel, result);
    if (FAILED(hr) || !result) {
        err = WuErrorText(FAILED(hr) ? hr : E_FAIL);
        return FAILED(hr) ? hr : E_FAIL;
    }
    OperationResultCode code = orcFailed;
    result->get_ResultCode(&code);
    if (code != orcSucceeded && code != orcSucceededWithErrors) {
        err = Tr(L"Windows Update couldn't finish the search");
        return E_FAIL;
    }
    ComPtr<IUpdateCollection> ups;
    if (FAILED(result->get_Updates(&ups)) || !ups) return S_OK;
    LONG n = 0;
    ups->get_Count(&n);
    for (LONG i = 0; i < n; ++i) {
        ComPtr<IUpdate> u;
        if (SUCCEEDED(ups->get_Item(i, &u)) && u) items.push_back(u);
    }
    return S_OK;
}

// ---- Vendor tools

struct ToolDef {
    const wchar_t* vendor;
    const wchar_t* name;
    std::vector<const wchar_t*> names;  // NormalizeToolName of its Uninstall DisplayName (a version may follow)
    const wchar_t* publisher;           // must also be in the publisher (generic names like "App Center")
    std::vector<const wchar_t*> store;  // Store package names
    std::vector<const wchar_t*> exes;   // under Program Files / Program Files (x86)
    const wchar_t* url;                 // its window is this web page
    const wchar_t* note;
};

const std::vector<ToolDef>& ToolDefs() {
    static const std::vector<ToolDef> defs = {
        // Graphics
        {L"nvidia", L"NVIDIA app", {L"nvidia app"}, nullptr, {}, {L"NVIDIA Corporation\\NVIDIA App\\CEF\\NVIDIA App.exe"}, nullptr, nullptr},
        {L"nvidia", L"GeForce Experience", {L"nvidia geforce experience"}, nullptr, {},
         {L"NVIDIA Corporation\\NVIDIA GeForce Experience\\NVIDIA GeForce Experience.exe"}, nullptr, nullptr},
        {L"amd", L"AMD Software: Adrenalin Edition",
         {L"amd software", L"amd software: adrenalin edition", L"amd software: pro edition", L"amd radeon software"}, nullptr, {},
         {L"AMD\\CNext\\CNext\\RadeonSoftware.exe"}, nullptr, nullptr},
        {L"intel", L"Intel Driver & Support Assistant", {L"intel driver & support assistant", L"intel driver and support assistant"}, nullptr,
         {}, {}, L"https://www.intel.com/content/www/us/en/support/intel-driver-support-assistant.html", nullptr},
        // PC makers
        {L"dell", L"Dell Command | Update",
         {L"dell command | update", L"dell command | update for windows universal", L"dell command | update for windows 10"}, nullptr,
         {L"DellInc.DellCommandUpdate"}, {L"Dell\\CommandUpdate\\DellCommandUpdate.exe"}, nullptr, nullptr},
        {L"dell", L"Dell SupportAssist", {L"dell supportassist", L"supportassist"}, nullptr, {L"DellInc.DellSupportAssistforPCs"}, {}, nullptr, nullptr},
        {L"lenovo", L"Lenovo Vantage", {}, nullptr, {L"E046963F.LenovoCompanion"}, {}, nullptr, nullptr},
        {L"lenovo", L"Lenovo Commercial Vantage", {}, nullptr, {L"E046963F.LenovoSettingsforEnterprise"}, {}, nullptr, nullptr},
        {L"lenovo", L"Lenovo System Update", {L"lenovo system update"}, nullptr, {}, {L"Lenovo\\System Update\\tvsu.exe"}, nullptr, nullptr},
        {L"hp", L"HP Support Assistant", {L"hp support assistant"}, nullptr, {L"AD2F1837.HPSupportAssistant"},
         {L"HP\\HP Support Framework\\HPSF.exe"}, nullptr, nullptr},
        {L"hp", L"HP Image Assistant", {L"hp image assistant"}, nullptr, {}, {L"HP\\HPIA\\HPImageAssistant.exe"}, nullptr, nullptr},
        {L"asus", L"MyASUS", {L"myasus"}, nullptr, {L"B9ECED6F.ASUSPCAssistant"}, {}, nullptr, nullptr},
        {L"asus", L"ASUS DriverHub", {L"asus driverhub"}, nullptr, {}, {}, L"https://driverhub.asus.com", nullptr},
        {L"asus", L"Armoury Crate", {L"armoury crate"}, nullptr, {L"B9ECED6F.ArmouryCrate"}, {}, nullptr, nullptr},
        {L"msi", L"MSI Center", {L"msi center"}, nullptr, {L"9426MICRO-STARINTERNATION.MSICenter"}, {}, nullptr, nullptr},
        {L"msi", L"MSI Driver & App Center", {L"msi driver & app center", L"msi driver and app center"}, nullptr, {}, {}, nullptr, nullptr},
        {L"gigabyte", L"GIGABYTE Control Center", {L"gigabyte control center"}, nullptr, {}, {}, nullptr, nullptr},
        {L"gigabyte", L"GIGABYTE App Center", {L"app center"}, L"gigabyte", {}, {}, nullptr, nullptr},
        {L"acer", L"Acer Care Center", {L"acer care center", L"care center"}, L"acer", {L"AcerIncorporated.AcerCareCenterS"}, {}, nullptr, nullptr},
        {L"asrock", L"ASRock APP Shop", {L"app shop", L"asrock app shop"}, L"asrock", {}, {}, nullptr, nullptr},
        {L"surface", L"Surface app", {}, nullptr, {L"Microsoft.SurfaceHub"}, {}, nullptr,
         N_(L"Surface drivers and firmware come through Windows Update; the Surface app shows the device's details and support.")},
    };
    return defs;
}

bool NameMatches(const std::wstring& normalized, const wchar_t* want) {
    std::wstring w = want;
    if (normalized == w) return true;
    // "nvidia app 11.0.9.565", "amd software 24.1": the name plus a version (not "nvidia app driver settings").
    if (normalized.size() > w.size() + 1 && StartsWith(normalized, w) && normalized[w.size()] == L' ') {
        wchar_t c = normalized[w.size() + 1];
        return iswdigit(c) || c == L'(' || c == L'v';
    }
    return false;
}

bool IsInstallerPath(const std::wstring& p) {
    std::wstring l = Lower(p);
    for (const wchar_t* bad : {L"package cache", L"installshield", L"installer", L"setup", L"uninst", L"nvi2.dll"})
        if (l.find(bad) != std::wstring::npos) return true;
    return false;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring EnvDir(const wchar_t* var) {
    wchar_t b[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(var, b, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(b, n) : L"";
}

std::wstring FindExe(const ToolDef& def, const UninstallEntry* e) {
    std::vector<std::wstring> roots;
    for (const wchar_t* v : {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"}) {
        std::wstring d = EnvDir(v);
        if (!d.empty() && std::find(roots.begin(), roots.end(), d) == roots.end()) roots.push_back(d);
    }
    for (const wchar_t* rel : def.exes)
        for (auto& r : roots)
            if (FileExists(r + L"\\" + rel)) return r + L"\\" + rel;
    if (e && !e->displayIcon.empty()) {
        std::wstring icon = e->displayIcon;
        size_t comma = icon.find_last_of(L',');
        if (comma != std::wstring::npos && comma > 2) icon = icon.substr(0, comma);
        icon.erase(std::remove(icon.begin(), icon.end(), L'"'), icon.end());
        icon = Trim(icon);
        if (Lower(icon).size() > 4 && Lower(icon).compare(icon.size() - 4, 4, L".exe") == 0 && !IsInstallerPath(icon) && FileExists(icon))
            return icon;
    }
    return L"";
}

std::wstring PackageName(const std::wstring& fullName) { return fullName.substr(0, fullName.find(L'_')); }
std::wstring PackageFamily(const std::wstring& fullName) {
    size_t last = fullName.find_last_of(L'_');
    return PackageName(fullName) + (last == std::wstring::npos ? L"" : fullName.substr(last));
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

}  // namespace

// =============================================================================================
// Devices

std::vector<DeviceDriver> ListDeviceDrivers() {
    std::vector<DeviceDriver> out;
    HDEVINFO devs = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (devs == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); ++i) {
        DeviceDriver d;
        d.hardwareIds = PropList(devs, dev, DEVPKEY_Device_HardwareIds);
        d.compatibleIds = PropList(devs, dev, DEVPKEY_Device_CompatibleIds);
        if (d.hardwareIds.empty() && d.compatibleIds.empty()) continue;
        d.instanceId = PropStr(devs, dev, DEVPKEY_Device_InstanceId);
        d.name = PropStr(devs, dev, DEVPKEY_Device_FriendlyName);
        if (d.name.empty()) d.name = PropStr(devs, dev, DEVPKEY_Device_DeviceDesc);
        d.className = PropStr(devs, dev, DEVPKEY_Device_Class);
        d.version = PropStr(devs, dev, DEVPKEY_Device_DriverVersion);
        d.provider = PropStr(devs, dev, DEVPKEY_Device_DriverProvider);
        DEVPROPTYPE t = 0;
        std::vector<BYTE> b = Prop(devs, dev, DEVPKEY_Device_DriverDate, t);
        if (t == DEVPROP_TYPE_FILETIME && b.size() >= sizeof(FILETIME)) {
            FILETIME ft;
            memcpy(&ft, b.data(), sizeof(ft));
            d.date = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
        }
        b = Prop(devs, dev, DEVPKEY_Device_IsPresent, t);
        if (t == DEVPROP_TYPE_BOOLEAN && !b.empty()) d.present = b[0] != 0;
        out.push_back(std::move(d));
    }
    SetupDiDestroyDeviceInfoList(devs);
    return out;
}

// =============================================================================================
// Pure helpers

std::wstring VersionFromTitle(const std::wstring& title) {
    std::wstring best;
    size_t i = 0;
    while (i < title.size()) {
        if (!iswdigit(title[i]) || (i > 0 && (iswalpha(title[i - 1]) || title[i - 1] == L'.'))) {
            ++i;
            continue;
        }
        size_t j = i;
        int dots = 0;
        while (j < title.size() && (iswdigit(title[j]) || (title[j] == L'.' && j + 1 < title.size() && iswdigit(title[j + 1])))) {
            dots += title[j] == L'.';
            ++j;
        }
        // Not part of a word ("AX211", "6E") either.
        bool word = j < title.size() && iswalpha(title[j]);
        if (dots >= 1 && !word) best = title.substr(i, j - i);
        i = j;
    }
    return best;
}

int MatchHardwareId(const std::wstring& id, const DeviceDriver& d) {
    if (id.empty()) return -1;
    std::wstring want = Lower(id);
    for (size_t i = 0; i < d.hardwareIds.size(); ++i)
        if (Lower(d.hardwareIds[i]) == want) return static_cast<int>(i);
    for (size_t i = 0; i < d.compatibleIds.size(); ++i)
        if (Lower(d.compatibleIds[i]) == want) return static_cast<int>(d.hardwareIds.size() + i);
    return -1;
}

bool SameCompany(const std::wstring& a, const std::wstring& b) {
    auto first = [](const std::wstring& s) {
        std::wstring l = Lower(s), w;
        for (wchar_t c : l) {
            if (iswalnum(c))
                w += c;
            else if (!w.empty())
                break;
        }
        return w;
    };
    std::wstring x = first(a), y = first(b);
    return !x.empty() && x == y;
}

OfferAge CompareOffer(const std::wstring& curVersion, int64_t curDate, const std::wstring& curProvider, const std::wstring& newVersion,
                      int64_t newDate, const std::wstring& newProvider) {
    bool comparable = curProvider.empty() || newProvider.empty() || SameCompany(curProvider, newProvider);
    if (!curVersion.empty() && !newVersion.empty() && comparable) {
        int c = CompareVersions(newVersion, curVersion);
        return c > 0 ? OfferAge::Newer : c == 0 ? OfferAge::Same : OfferAge::Older;
    }
    if (curDate && newDate) {
        int64_t a = curDate / kDay, b = newDate / kDay;
        return b > a ? OfferAge::Newer : b == a ? OfferAge::Same : OfferAge::Older;
    }
    return OfferAge::Unknown;
}

bool IsDisplayDriver(const DriverUpdate& u, const DeviceDriver* d) {
    if (d && Lower(d->className) == L"display") return true;
    std::wstring c = Lower(u.driverClass);
    return c == L"display" || c == L"video";
}

void MatchDriverUpdates(std::vector<DriverUpdate>& ups, const std::vector<DeviceDriver>& devs) {
    for (auto& u : ups) {
        u.device = -1;
        u.devices = 0;
        int bestRank = INT_MAX;
        bool bestPresent = false;
        for (size_t i = 0; i < devs.size(); ++i) {
            int rank = MatchHardwareId(u.hardwareId, devs[i]);
            if (rank < 0) continue;
            ++u.devices;
            // A connected device first, then the most specific match.
            bool present = devs[i].present;
            if (u.device < 0 || (present && !bestPresent) || (present == bestPresent && rank < bestRank)) {
                u.device = static_cast<int>(i);
                bestRank = rank;
                bestPresent = present;
            }
        }
        const DeviceDriver* d = u.device >= 0 ? &devs[u.device] : nullptr;
        u.age = d ? CompareOffer(d->version, d->date, d->provider, u.version, u.date, u.provider) : OfferAge::Unknown;
        u.display = IsDisplayDriver(u, d);
    }
}

bool DriverUpdate::Preselect(const std::vector<DeviceDriver>& devs) const {
    return device >= 0 && device < static_cast<int>(devs.size()) && devs[device].present && age == OfferAge::Newer && !display && !hidden &&
           eulaAccepted;
}

std::wstring WuErrorText(long hr) {
    struct Known {
        unsigned long hr;
        const wchar_t* text;
    };
    static const Known known[] = {
        {0x80240044, N_(L"Only an administrator can install updates for this PC")},
        {0x8024001E, N_(L"The Windows Update service stopped")},
        {0x80240016, N_(L"Windows Update is busy installing something else; try again when it's done")},
        {0x8024000B, N_(L"Cancelled")},
        {0x80240017, N_(L"The update doesn't apply to this PC any more")},
        {0x80240020, N_(L"Installing needs a signed-in user")},
        {0x8024002E, N_(L"Windows Update is turned off by a policy on this PC")},
        {0x8024402C, N_(L"Windows Update couldn't be reached (is the PC online?)")},
        {0x80244022, N_(L"Windows Update is busy; try again later")},
        {0x8024401C, N_(L"Windows Update didn't answer in time")},
        {0x80240438, N_(L"Windows Update couldn't be reached (is the PC online?)")},
        {0x80072EE7, N_(L"Windows Update couldn't be reached (is the PC online?)")},
        {0x80072EFD, N_(L"Windows Update couldn't be reached (is the PC online?)")},
        {0x80072EFE, N_(L"The connection to Windows Update was interrupted")},
        {0x80072F8F, N_(L"The secure connection to Windows Update failed (is the PC's clock right?)")},
        {0x8024200B, N_(L"The driver's installation failed")},
        {0x80242014, N_(L"A restart from an earlier update is still pending: restart Windows first")},
        {0x8024A000, N_(L"The Windows Update service isn't available")},
        {0x80070422, N_(L"The Windows Update service is turned off")},
        {0x80070005, N_(L"Access denied: run as administrator")},
    };
    for (auto& k : known)
        if (static_cast<unsigned long>(hr) == k.hr) return TrF(L"{0} ({1})", Tr(k.text), Hex(hr));
    std::wstring sys = util::Win32ErrorMessage(static_cast<unsigned long>(hr));
    if (sys.rfind(L"Error 0x", 0) == 0) return TrF(L"Windows Update error {0}", Hex(hr));
    return TrF(L"{0} ({1})", sys, Hex(hr));
}

// =============================================================================================
// Search

DriverSearch SearchDriverUpdates(std::atomic<bool>* cancel) {
    DriverSearch r;
    ComInit com;
    Session s;
    std::vector<ComPtr<IUpdate>> items;
    HRESULT hr = Search(s, cancel, items, r.error);
    if (FAILED(hr)) {
        r.hr = hr;
        return r;
    }
    for (auto& u : items) r.updates.push_back(ReadUpdate(u.Get()));
    r.devices = ListDeviceDrivers();
    MatchDriverUpdates(r.updates, r.devices);
    r.ok = true;
    return r;
}

// =============================================================================================
// Install

std::wstring InstallJournalNote(const DriverInstallResult& r, const DriverUpdate* u, const DeviceDriver* d) {
    i18n::EnglishScope english;
    std::wstring note = L"through Windows Update";
    if (u) {
        std::wstring what = !u->driverClass.empty() ? u->driverClass + L" driver" : L"driver";
        std::wstring dev = d && !d->name.empty() ? d->name : u->model;
        if (!dev.empty()) what += L" for " + dev;
        note += L"; " + what;
        std::wstring from = d ? (d->version.empty() ? L"unknown version" : d->version) + (d->date ? L" (" + DateIso(d->date) + L")" : L"")
                              : L"no driver found";
        std::wstring to = (u->version.empty() ? L"offered version" : u->version) + (u->date ? L" (" + DateIso(u->date) + L")" : L"");
        note += L"; " + from + L" -> " + to;
        if (!u->provider.empty()) note += L"; provider " + u->provider;
    }
    switch (r.status) {
        case InstallStatus::Installed: note += r.reboot ? L"; installed, restart required" : L"; installed"; break;
        case InstallStatus::Failed: note += L"; failed: " + r.message; break;
        case InstallStatus::Skipped: note += L"; skipped: " + r.message; break;
    }
    return note;
}

DriverInstallReport InstallDriverUpdates(const std::vector<std::wstring>& ids, const std::function<void(const std::wstring&)>& progress) {
    i18n::EnglishScope english;
    DriverInstallReport rep;
    auto say = [&](const std::wstring& s) {
        if (progress) progress(s);
    };
    ComInit com;
    Session s;
    say(L"Asking Windows Update which driver updates it offers…");
    std::vector<ComPtr<IUpdate>> items;
    if (FAILED(Search(s, nullptr, items, rep.error))) return rep;
    rep.ok = true;
    std::vector<DriverUpdate> infos;
    for (auto& u : items) infos.push_back(ReadUpdate(u.Get()));
    std::vector<DeviceDriver> devs = ListDeviceDrivers();
    MatchDriverUpdates(infos, devs);

    // Exactly the requested ones that Windows Update offers now.
    struct Pick {
        size_t req;     // index into rep.results
        size_t item;    // index into items / infos
    };
    std::vector<Pick> picks;
    for (auto& id : ids) {
        DriverInstallResult res;
        res.updateId = Lower(id);
        auto it = std::find_if(infos.begin(), infos.end(), [&](const DriverUpdate& u) { return u.updateId == res.updateId; });
        if (it == infos.end()) {
            res.status = InstallStatus::Skipped;
            res.message = L"Windows Update doesn't offer it any more";
        } else {
            res.title = it->title;
            if (!it->eulaAccepted) {
                res.status = InstallStatus::Skipped;
                res.message = L"its license terms must be accepted in Settings > Windows Update first";
            } else {
                picks.push_back({rep.results.size(), static_cast<size_t>(it - infos.begin())});
            }
        }
        rep.results.push_back(res);
    }
    auto fail = [&](const Pick& p, HRESULT hr, const std::wstring& why) {
        DriverInstallResult& res = rep.results[p.req];
        res.status = InstallStatus::Failed;
        res.hr = hr;
        res.message = why.empty() ? WuErrorText(hr) : why;
    };
    auto newCollection = [](ComPtr<IUpdateCollection>& c) {
        return CoCreateInstance(__uuidof(UpdateCollection), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&c));
    };

    if (!picks.empty()) {
        // Download (what is already downloaded is kept).
        ComPtr<IUpdateCollection> toGet;
        HRESULT hr = newCollection(toGet);
        std::vector<Pick> queued;
        if (SUCCEEDED(hr))
            for (auto& p : picks) {
                LONG at = 0;
                if (SUCCEEDED(toGet->Add(items[p.item].Get(), &at)))
                    queued.push_back(p);
                else
                    fail(p, E_FAIL, L"");
            }
        ComPtr<IUpdateDownloader> dl;
        if (SUCCEEDED(hr)) hr = s.session->CreateUpdateDownloader(&dl);
        if (SUCCEEDED(hr)) hr = dl->put_Updates(toGet.Get());
        ComPtr<IDownloadResult> dres;
        if (SUCCEEDED(hr)) {
            say(L"Windows Update is downloading " + std::to_wstring(queued.size()) + (queued.size() == 1 ? L" update…" : L" updates…"));
            hr = dl->Download(&dres);
        }
        std::vector<Pick> ready;
        for (size_t i = 0; i < queued.size(); ++i) {
            const Pick& p = queued[i];
            if (FAILED(hr) || !dres) {
                fail(p, FAILED(hr) ? hr : E_FAIL, L"");
                continue;
            }
            ComPtr<IUpdateDownloadResult> ur;
            OperationResultCode code = orcFailed;
            LONG uhr = 0;
            if (SUCCEEDED(dres->GetUpdateResult(static_cast<LONG>(i), &ur)) && ur) {
                ur->get_ResultCode(&code);
                ur->get_HResult(&uhr);
            }
            if (Flag(items[p.item].Get(), &IUpdate::get_IsDownloaded, false) || code == orcSucceeded || code == orcSucceededWithErrors)
                ready.push_back(p);
            else
                fail(p, uhr ? uhr : E_FAIL, L"download failed: " + WuErrorText(uhr ? uhr : E_FAIL));
        }

        // Install.
        if (!ready.empty()) {
            ComPtr<IUpdateCollection> toInstall;
            hr = newCollection(toInstall);
            if (SUCCEEDED(hr))
                for (auto& p : ready) {
                    LONG at = 0;
                    toInstall->Add(items[p.item].Get(), &at);
                }
            ComPtr<IUpdateInstaller> inst;
            if (SUCCEEDED(hr)) hr = s.session->CreateUpdateInstaller(&inst);
            if (SUCCEEDED(hr)) {
                Bstr app(L"Bytopsy");
                inst->put_ClientApplicationID(app.b);
                inst->put_AllowSourcePrompts(VARIANT_FALSE);
                if (Flag(inst.Get(), &IUpdateInstaller::get_IsBusy, false)) hr = static_cast<HRESULT>(0x80240016L);
            }
            if (SUCCEEDED(hr)) hr = inst->put_Updates(toInstall.Get());
            ComPtr<IInstallationResult> ires;
            if (SUCCEEDED(hr)) {
                say(L"Windows Update is installing " + std::to_wstring(ready.size()) + (ready.size() == 1 ? L" driver…" : L" drivers…"));
                hr = inst->Install(&ires);
            }
            if (SUCCEEDED(hr) && ires) {
                VARIANT_BOOL rb = VARIANT_FALSE;
                ires->get_RebootRequired(&rb);
                rep.rebootRequired = rb != VARIANT_FALSE;
            }
            for (size_t i = 0; i < ready.size(); ++i) {
                const Pick& p = ready[i];
                if (FAILED(hr) || !ires) {
                    fail(p, FAILED(hr) ? hr : E_FAIL, L"");
                    continue;
                }
                ComPtr<IUpdateInstallationResult> ur;
                OperationResultCode code = orcFailed;
                LONG uhr = 0;
                VARIANT_BOOL reboot = VARIANT_FALSE;
                if (SUCCEEDED(ires->GetUpdateResult(static_cast<LONG>(i), &ur)) && ur) {
                    ur->get_ResultCode(&code);
                    ur->get_HResult(&uhr);
                    ur->get_RebootRequired(&reboot);
                }
                DriverInstallResult& res = rep.results[p.req];
                if (code == orcSucceeded || code == orcSucceededWithErrors) {
                    res.status = InstallStatus::Installed;
                    res.reboot = reboot != VARIANT_FALSE;
                    if (code == orcSucceededWithErrors) res.message = L"installed with errors";
                } else {
                    fail(p, uhr ? uhr : E_FAIL, code == orcAborted ? std::wstring(L"cancelled") : L"");
                }
            }
        }
    }

    // Journal every attempt (skipped ones changed nothing).
    for (auto& p : picks) {
        const DriverInstallResult& res = rep.results[p.req];
        const DriverUpdate& u = infos[p.item];
        JournalEntry je;
        je.action = JournalAction::Clean;
        je.path = L"Driver update: " + u.title;
        je.note = InstallJournalNote(res, &u, u.device >= 0 ? &devs[u.device] : nullptr);
        JournalAppend(je);
    }
    return rep;
}

std::wstring FormatInstallResultLine(const DriverInstallResult& r) {
    const wchar_t* st = r.status == InstallStatus::Installed ? L"installed" : r.status == InstallStatus::Skipped ? L"skipped" : L"failed";
    return L"RESULT\t" + Field(r.updateId) + L"\t" + st + L"\t" + (r.reboot ? L"1" : L"0") + L"\t" + Hex(r.hr) + L"\t" + Field(r.title) +
           L"\t" + Field(r.message);
}

bool ParseInstallResultLine(const std::wstring& line, DriverInstallResult& r) {
    std::wstring l = line;
    while (!l.empty() && (l.back() == L'\r' || l.back() == L'\n')) l.pop_back();
    if (!StartsWith(l, L"RESULT\t")) return false;
    std::vector<std::wstring> f;
    size_t pos = 0;
    while (true) {
        size_t tab = l.find(L'\t', pos);
        f.push_back(l.substr(pos, tab == std::wstring::npos ? std::wstring::npos : tab - pos));
        if (tab == std::wstring::npos) break;
        pos = tab + 1;
    }
    if (f.size() < 7) return false;
    r = DriverInstallResult{};
    r.updateId = f[1];
    if (f[2] == L"installed")
        r.status = InstallStatus::Installed;
    else if (f[2] == L"skipped")
        r.status = InstallStatus::Skipped;
    else if (f[2] == L"failed")
        r.status = InstallStatus::Failed;
    else
        return false;
    r.reboot = f[3] == L"1";
    r.hr = static_cast<long>(wcstoul(f[4].c_str(), nullptr, 16));
    r.title = f[5];
    r.message = f[6];
    for (size_t i = 7; i < f.size(); ++i) r.message += L" " + f[i];
    return true;
}

std::vector<DriverInstallResult> ParseInstallOutput(const std::string& raw) {
    std::vector<DriverInstallResult> out;
    std::wstring text = DecodeToolOutput(raw);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        DriverInstallResult r;
        if (ParseInstallResultLine(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos), r)) out.push_back(r);
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    return out;
}

bool ParseUpdateIdList(const std::wstring& s, std::vector<std::wstring>& ids) {
    ids.clear();
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t c = s.find(L',', pos);
        std::wstring id = Lower(Trim(s.substr(pos, c == std::wstring::npos ? std::wstring::npos : c - pos)));
        if (!id.empty()) {
            // 8-4-4-4-12 hex digits
            if (id.size() != 36) return false;
            for (size_t i = 0; i < id.size(); ++i) {
                bool dash = i == 8 || i == 13 || i == 18 || i == 23;
                if (dash ? id[i] != L'-' : !iswxdigit(id[i])) return false;
            }
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        if (c == std::wstring::npos) break;
        pos = c + 1;
    }
    return !ids.empty();
}

// =============================================================================================
// Vendor tools

std::wstring VendorName(const std::wstring& v) {
    static const std::map<std::wstring, std::wstring> names = {
        {L"nvidia", L"NVIDIA"}, {L"amd", L"AMD"},       {L"intel", L"Intel"},         {L"dell", L"Dell"},
        {L"lenovo", L"Lenovo"}, {L"hp", L"HP"},         {L"asus", L"ASUS"},           {L"msi", L"MSI"},
        {L"gigabyte", L"GIGABYTE"}, {L"acer", L"Acer"}, {L"surface", L"Microsoft Surface"}, {L"framework", L"Framework"},
        {L"asrock", L"ASRock"},
    };
    auto it = names.find(v);
    return it == names.end() ? v : it->second;
}

std::wstring VendorSupportUrl(const std::wstring& v) {
    static const std::map<std::wstring, std::wstring> urls = {
        {L"nvidia", L"https://www.nvidia.com/en-us/software/nvidia-app/"},
        {L"amd", L"https://www.amd.com/en/support/download/drivers.html"},
        {L"intel", L"https://www.intel.com/content/www/us/en/support/intel-driver-support-assistant.html"},
        {L"dell", L"https://www.dell.com/support/home/"},
        {L"lenovo", L"https://pcsupport.lenovo.com/"},
        {L"hp", L"https://support.hp.com/drivers"},
        {L"asus", L"https://www.asus.com/support/download-center/"},
        {L"msi", L"https://www.msi.com/support/download"},
        {L"gigabyte", L"https://www.gigabyte.com/Support"},
        {L"acer", L"https://www.acer.com/support"},
        {L"surface", L"https://support.microsoft.com/surface/download-drivers-and-firmware-for-surface"},
        {L"framework", L"https://frame.work/support"},
        {L"asrock", L"https://www.asrock.com/support/index.asp"},
    };
    auto it = urls.find(v);
    return it == urls.end() ? L"" : it->second;
}

std::wstring GpuVendorFromHardwareId(const std::wstring& id) {
    std::wstring l = Lower(id);
    if (l.find(L"ven_10de") != std::wstring::npos) return L"nvidia";
    if (l.find(L"ven_1002") != std::wstring::npos || l.find(L"ven_1022") != std::wstring::npos) return L"amd";
    if (l.find(L"ven_8086") != std::wstring::npos) return L"intel";
    return L"";
}

std::wstring NormalizeToolName(const std::wstring& s) {
    std::wstring l = Lower(s);
    for (const wchar_t* drop : {L"(r)", L"(tm)", L"®", L"™"}) {
        std::wstring d = drop;
        for (size_t at; (at = l.find(d)) != std::wstring::npos;) l.erase(at, d.size());
    }
    std::wstring out;
    for (wchar_t c : l) {
        if (c == L' ' || c == L'\t') {
            if (!out.empty() && out.back() != L' ') out += L' ';
        } else {
            out += c;
        }
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

std::vector<StorePackage> ListStorePackages() {
    std::vector<StorePackage> out;
    HKEY base = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages", 0,
                      KEY_READ, &base) != ERROR_SUCCESS)
        return out;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD n = 512;
        if (RegEnumKeyExW(base, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        StorePackage p;
        p.fullName = name;
        HKEY pk = nullptr;
        if (RegOpenKeyExW(base, name, 0, KEY_READ, &pk) == ERROR_SUCCESS) {
            wchar_t app[256];
            DWORD an = 256;
            if (RegEnumKeyExW(pk, 0, app, &an, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) p.appId = app;
            RegCloseKey(pk);
        }
        out.push_back(std::move(p));
    }
    RegCloseKey(base);
    return out;
}

std::vector<DriverTool> FindDriverTools(const std::wstring& vendor, const std::vector<UninstallEntry>& entries,
                                        const std::vector<StorePackage>& store, bool resolveExe) {
    std::vector<DriverTool> out;
    const ToolDef* first = nullptr;
    for (const ToolDef& def : ToolDefs()) {
        if (vendor != def.vendor) continue;
        if (!first) first = &def;
        DriverTool t;
        t.vendor = vendor;
        t.name = def.name;
        if (def.note) t.note = Tr(def.note);
        const UninstallEntry* entry = nullptr;
        for (auto& e : entries) {
            if (def.publisher && !util::ContainsI(e.publisher, def.publisher)) continue;
            std::wstring n = NormalizeToolName(e.displayName);
            for (const wchar_t* want : def.names)
                if (NameMatches(n, want)) {
                    entry = &e;
                    break;
                }
            if (entry) break;
        }
        const StorePackage* pkg = nullptr;
        for (auto& p : store)
            for (const wchar_t* want : def.store)
                if (!pkg && !p.appId.empty() && Lower(PackageName(p.fullName)) == Lower(want)) pkg = &p;
        if (!entry && !pkg) continue;
        t.installed = true;
        if (def.url)
            t.launch = def.url;
        else if (pkg)
            t.launch = L"shell:AppsFolder\\" + PackageFamily(pkg->fullName) + L"!" + pkg->appId;
        else if (resolveExe)
            t.launch = FindExe(def, entry);
        out.push_back(std::move(t));
    }
    if (out.empty()) {
        DriverTool t;
        t.vendor = vendor;
        t.name = first ? first->name : L"";
        if (first && first->note) t.note = Tr(first->note);
        t.launch = first && first->url ? first->url : VendorSupportUrl(vendor);
        out.push_back(std::move(t));
    }
    return out;
}

bool IsPlaceholder(const std::wstring& s) {
    std::wstring l = Lower(Trim(s));
    static const wchar_t* const kPlaceholders[] = {
        L"",          L"system manufacturer", L"system product name", L"to be filled by o.e.m.", L"to be filled by o.e.m",
        L"default string", L"o.e.m.", L"oem", L"none", L"not applicable", L"not specified", L"system version",
        L"base board product name", L"default", L"invalid", L"unknown", L"x.x", L"123456789", L"type1productconfigid",
    };
    for (const wchar_t* p : kPlaceholders)
        if (l == p) return true;
    return false;
}

std::wstring MakerKey(const std::wstring& maker, const std::wstring& product) {
    std::wstring m = Lower(Trim(maker)), p = Lower(product);
    auto has = [&](const wchar_t* w) { return m.find(w) != std::wstring::npos; };
    if (has(L"dell")) return L"dell";
    if (has(L"lenovo")) return L"lenovo";
    if (has(L"hewlett") || m == L"hp" || StartsWith(m, L"hp ")) return L"hp";
    if (has(L"asus")) return L"asus";
    if (has(L"micro-star") || m == L"msi" || StartsWith(m, L"msi ")) return L"msi";
    if (has(L"gigabyte")) return L"gigabyte";
    if (StartsWith(m, L"acer")) return L"acer";
    if (has(L"asrock")) return L"asrock";
    if (has(L"framework")) return L"framework";
    if (has(L"microsoft") && p.find(L"surface") != std::wstring::npos) return L"surface";
    return L"";
}

PcMaker DetectPcMaker(const std::wstring& sysMaker, const std::wstring& sysProduct, const std::wstring& boardMaker,
                      const std::wstring& boardProduct) {
    PcMaker m;
    if (!IsPlaceholder(sysMaker) && !IsPlaceholder(sysProduct)) {
        m.name = Trim(sysMaker);
        m.model = Trim(sysProduct);
        m.vendor = MakerKey(sysMaker, sysProduct);
        return m;
    }
    // A self-built PC (or a board that leaves the system fields to the PC builder): the
    // motherboard's maker has the drivers.
    if (!IsPlaceholder(boardMaker)) {
        m.name = Trim(boardMaker);
        m.model = IsPlaceholder(boardProduct) ? L"" : Trim(boardProduct);
        m.vendor = MakerKey(boardMaker, boardProduct);
        m.selfBuilt = true;
        return m;
    }
    if (!IsPlaceholder(sysMaker)) {
        m.name = Trim(sysMaker);
        m.vendor = MakerKey(sysMaker, L"");
    }
    return m;
}

PcMaker ReadPcMaker() {
    const wchar_t* key = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    return DetectPcMaker(RegString(HKEY_LOCAL_MACHINE, key, L"SystemManufacturer"), RegString(HKEY_LOCAL_MACHINE, key, L"SystemProductName"),
                         RegString(HKEY_LOCAL_MACHINE, key, L"BaseBoardManufacturer"), RegString(HKEY_LOCAL_MACHINE, key, L"BaseBoardProduct"));
}

std::vector<GpuGroup> GroupGpus(const std::vector<DeviceDriver>& devs) {
    std::vector<GpuGroup> out;
    for (const wchar_t* v : {L"nvidia", L"amd", L"intel"}) {
        GpuGroup g;
        g.vendor = v;
        for (size_t i = 0; i < devs.size(); ++i) {
            const DeviceDriver& d = devs[i];
            if (!d.present || Lower(d.className) != L"display") continue;
            std::wstring vendor;
            for (auto& id : d.hardwareIds)
                if (vendor.empty()) vendor = GpuVendorFromHardwareId(id);
            if (vendor == v) g.adapters.push_back(static_cast<int>(i));
        }
        if (!g.adapters.empty()) out.push_back(std::move(g));
    }
    return out;
}

DriverToolsReport ReadDriverTools() {
    DriverToolsReport r;
    r.devices = ListDeviceDrivers();
    std::vector<UninstallEntry> entries = ReadUninstallEntries();
    std::vector<StorePackage> store = ListStorePackages();
    r.maker = ReadPcMaker();
    if (!r.maker.vendor.empty()) r.makerTools = FindDriverTools(r.maker.vendor, entries, store);
    r.gpus = GroupGpus(r.devices);
    for (auto& g : r.gpus) g.tools = FindDriverTools(g.vendor, entries, store);
    return r;
}
