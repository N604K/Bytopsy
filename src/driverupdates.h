#pragma once

// Driver updates through Windows Update: Windows Update (the Windows Update Agent API, the same
// service Settings > Windows Update uses) is asked which driver updates it has for this PC; each
// one is shown next to the driver its device has now, and the chosen ones are downloaded and
// installed by Windows Update itself (Microsoft-signed, matched to the hardware), never from
// anywhere else. Graphics drivers and the drivers a PC maker ships outside Windows Update come
// from the vendor's own tool, which is only detected and opened here.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "apps.h"

// ---- Devices and the drivers they have now (SetupAPI)

struct DeviceDriver {
    std::wstring instanceId, name, className;  // className: "Display", "Net", "Bluetooth", ...
    std::vector<std::wstring> hardwareIds, compatibleIds;  // most specific first
    std::wstring version, provider;  // the installed driver ("32.0.16.1088", "NVIDIA")
    int64_t date = 0;                // its DriverVer date (FILETIME ticks); 0 = unknown
    bool present = true;             // connected now
};
// Every device Windows knows, connected or not. Read-only.
std::vector<DeviceDriver> ListDeviceDrivers();

// ---- What Windows Update offers

// How the offered driver compares with the one the device has.
enum class OfferAge { Unknown, Newer, Same, Older };

struct DriverUpdate {
    std::wstring updateId;  // IUpdate's identity (a GUID): what installing names
    int revision = 0;
    std::wstring title, description, supportUrl;
    std::wstring driverClass, manufacturer, model, provider, hardwareId;  // IWindowsDriverUpdate
    std::vector<std::wstring> kbs;  // "KB5031234" (driver updates rarely have one)
    std::wstring version;           // from the title ("Intel net Driver Update (24.40.0.4)"); may be ""
    int64_t date = 0;               // DriverVerDate (FILETIME ticks)
    uint64_t bytes = 0;             // MaxDownloadSize
    bool downloaded = false, eulaAccepted = true, hidden = false;
    bool optional = false;  // BrowseOnly: Settings lists it under "Optional updates"
    int reboot = 0;         // InstallationBehavior.RebootBehavior: 0 never, 1 always, 2 can ask for one

    // Matched to this PC (MatchDriverUpdates)
    int device = -1;   // index of the device it is for (best match), -1 = none found
    size_t devices = 0;  // how many devices have the hardware id
    OfferAge age = OfferAge::Unknown;
    bool display = false;  // a graphics driver: the GPU vendor's own tool is recommended

    // Checked in the windowed app when the list arrives: a newer driver for a device that is
    // connected, not a graphics driver, not hidden and with no license terms left to accept.
    bool Preselect(const std::vector<DeviceDriver>& devs) const;
};

struct DriverSearch {
    bool ok = false;
    long hr = 0;            // HRESULT of a failed search
    std::wstring error;     // why it failed (WuErrorText)
    std::vector<DriverUpdate> updates;
    std::vector<DeviceDriver> devices;  // what the updates were matched against
};
// Windows Update's own search ("IsInstalled=0 and Type='Driver'"): Windows contacts Windows
// Update (or the PC's update server) for it. Takes up to a minute or two; `cancel` stops it.
// Initializes COM on the calling thread itself. Read-only: nothing is downloaded or installed.
DriverSearch SearchDriverUpdates(std::atomic<bool>* cancel = nullptr);
extern const wchar_t* const kDriverSearchCriteria;

// Finds each update's device and compares the drivers (pure: for tests).
void MatchDriverUpdates(std::vector<DriverUpdate>& ups, const std::vector<DeviceDriver>& devs);

// ---- Installing (needs administrator rights): `bytopsy-cli --install-driver-updates`

enum class InstallStatus { Installed, Failed, Skipped };
struct DriverInstallResult {
    std::wstring updateId, title;
    InstallStatus status = InstallStatus::Failed;
    bool reboot = false;  // installed, but takes effect after a restart
    long hr = 0;
    std::wstring message;  // why it failed or was skipped (English)
};
struct DriverInstallReport {
    bool ok = false;      // Windows Update could be asked at all
    std::wstring error;   // ... and why not
    bool rebootRequired = false;
    std::vector<DriverInstallResult> results;  // one per requested id, in order
};
// Searches again and installs exactly the requested updates that Windows Update still offers:
// IUpdateDownloader, then IUpdateInstaller. Updates no longer offered, or whose license terms
// weren't accepted in Settings, are skipped. Each attempt is journaled. `progress` gets English
// status lines. The caller creates the restore point first and must be elevated.
DriverInstallReport InstallDriverUpdates(const std::vector<std::wstring>& ids, const std::function<void(const std::wstring&)>& progress);

// The protocol between the elevated bytopsy-cli and the windowed app: one line per update,
// "RESULT<tab>id<tab>installed|failed|skipped<tab>reboot 0/1<tab>hr (hex)<tab>title<tab>message".
std::wstring FormatInstallResultLine(const DriverInstallResult& r);
bool ParseInstallResultLine(const std::wstring& line, DriverInstallResult& r);
std::vector<DriverInstallResult> ParseInstallOutput(const std::string& raw);
// "id1,id2": update ids are GUIDs; anything else is refused (false).
bool ParseUpdateIdList(const std::wstring& s, std::vector<std::wstring>& ids);
// The journal note for an install (English): what it updated, from which version to which.
std::wstring InstallJournalNote(const DriverInstallResult& r, const DriverUpdate* u, const DeviceDriver* d);

// ---- Pure helpers (exposed for tests)

// The last version-looking token of a title: "MediaTek Inc. - Ports - 3.0.1511.0" -> "3.0.1511.0".
std::wstring VersionFromTitle(const std::wstring& title);
// How well a hardware id matches a device: 0 = its most specific hardware id, higher = less
// specific (compatible ids come after hardware ids), -1 = no match. Case-insensitive.
int MatchHardwareId(const std::wstring& id, const DeviceDriver& d);
// Offered vs installed. Versions are compared when both are known and the providers are the same
// company (an Intel driver replacing Microsoft's generic one has unrelated numbers); dates (by day)
// otherwise.
OfferAge CompareOffer(const std::wstring& curVersion, int64_t curDate, const std::wstring& curProvider, const std::wstring& newVersion,
                      int64_t newDate, const std::wstring& newProvider);
bool SameCompany(const std::wstring& a, const std::wstring& b);
// A graphics driver: its device is a display adapter, or Windows Update files it under Display.
bool IsDisplayDriver(const DriverUpdate& u, const DeviceDriver* d);
// An HRESULT as text: the common Windows Update errors in words, else the system message, plus "0x...".
std::wstring WuErrorText(long hr);

// ---- The vendors' own driver tools: GPU vendors and PC makers

// Vendor keys: "nvidia", "amd", "intel" (graphics); "dell", "lenovo", "hp", "asus", "msi",
// "gigabyte", "acer", "surface", "framework", "asrock" (PC and motherboard makers).
std::wstring VendorName(const std::wstring& vendor);        // "NVIDIA", "Dell", "Microsoft Surface"
std::wstring VendorSupportUrl(const std::wstring& vendor);  // the official driver download / support page
// "PCI\VEN_10DE&DEV_2882..." -> "nvidia" ("amd", "intel"; "" for others).
std::wstring GpuVendorFromHardwareId(const std::wstring& id);

// A Store app package as registered for this user.
struct StorePackage {
    std::wstring fullName;  // "E046963F.LenovoCompanion_10.2401.29.0_x64__k1h2ywk1493x8"
    std::wstring appId;     // its first application ("App")
};
std::vector<StorePackage> ListStorePackages();  // the current user's (registry, read-only)

struct DriverTool {
    std::wstring vendor;  // key
    std::wstring name;    // "NVIDIA app", "Dell Command | Update"
    bool installed = false;
    // How to open it: an .exe path, "shell:AppsFolder\<family>!<app>" for a Store app, or an https
    // URL (tools whose window is a web page: Intel DSA, ASUS DriverHub). Not installed: the
    // vendor's official download / support page. Empty: installed, but how to start it is unknown.
    std::wstring launch;
    std::wstring note;  // e.g. "Surface drivers come through Windows Update"
};
// The vendor's tools that are installed (Uninstall entries, Store packages), in order of
// preference; none installed: one entry, not installed, with the vendor's download page. Exe
// paths are only filled in when `resolveExe` (it looks at the disk).
std::vector<DriverTool> FindDriverTools(const std::wstring& vendor, const std::vector<UninstallEntry>& entries,
                                        const std::vector<StorePackage>& store, bool resolveExe = true);
// Lowercase, "®", "(R)", "™", "(TM)" dropped, spaces collapsed: "Intel® Driver & Support Assistant" ->
// "intel driver & support assistant".
std::wstring NormalizeToolName(const std::wstring& s);

// The PC's maker from SMBIOS (HKLM\HARDWARE\DESCRIPTION\System\BIOS).
struct PcMaker {
    std::wstring vendor;  // key, "" = not one Bytopsy knows (or a virtual machine)
    std::wstring name;    // as the firmware says ("Dell Inc.", "ASUSTeK COMPUTER INC.")
    std::wstring model;   // "XPS 13 9340", or the motherboard ("PRIME Z790-A WIFI") of a self-built PC
    bool selfBuilt = false;  // the system fields are placeholders: the motherboard's maker counts
};
PcMaker DetectPcMaker(const std::wstring& sysMaker, const std::wstring& sysProduct, const std::wstring& boardMaker,
                      const std::wstring& boardProduct);
PcMaker ReadPcMaker();
// "Dell Inc." -> "dell", "ASUSTeK COMPUTER INC." -> "asus", "Micro-Star International Co., Ltd." -> "msi".
std::wstring MakerKey(const std::wstring& maker, const std::wstring& product);
bool IsPlaceholder(const std::wstring& smbiosText);  // "To be filled by O.E.M.", "System Product Name", ...

// Everything the driver-tool cards show: read quickly, without Windows Update.
struct GpuGroup {
    std::wstring vendor;  // "nvidia"
    std::vector<int> adapters;  // indices into devices (connected display adapters of this vendor)
    std::vector<DriverTool> tools;
};
struct DriverToolsReport {
    PcMaker maker;
    std::vector<DriverTool> makerTools;
    std::vector<GpuGroup> gpus;
    std::vector<DeviceDriver> devices;
};
// Groups the connected display adapters by vendor (pure).
std::vector<GpuGroup> GroupGpus(const std::vector<DeviceDriver>& devs);
DriverToolsReport ReadDriverTools();
