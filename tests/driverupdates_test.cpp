// Self-check of driver updates: bytopsy-driverupdates-test.exe
// Synthetic devices, updates, Uninstall entries and SMBIOS strings: hardware id matching, version
// and date comparison, graphics drivers, the GPU vendors' and PC makers' tools, and the result
// lines the elevated bytopsy-cli prints. Never installs anything. Set BYTOPSY_WU_SEARCH=1 to also
// ask Windows Update what it offers this PC (a read-only search).
#include <windows.h>

#include <cstdio>
#include <string>

#include "driverupdates.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static int64_t Day(int y, int m, int d) {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

static DeviceDriver Dev(const wchar_t* name, const wchar_t* cls, std::vector<std::wstring> hw, std::vector<std::wstring> compat,
                        const wchar_t* ver, int64_t date, const wchar_t* provider, bool present = true) {
    DeviceDriver d;
    d.name = name;
    d.className = cls;
    d.hardwareIds = std::move(hw);
    d.compatibleIds = std::move(compat);
    d.version = ver;
    d.date = date;
    d.provider = provider;
    d.present = present;
    return d;
}

static UninstallEntry Entry(const wchar_t* name, const wchar_t* publisher) {
    UninstallEntry e;
    e.displayName = name;
    e.publisher = publisher;
    return e;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // ---- Versions in Windows Update's titles (both formats seen on a real PC)
    Check(VersionFromTitle(L"MediaTek Inc. - Ports - 3.0.1511.0") == L"3.0.1511.0", L"version from \"Provider - Class - Version\" title");
    Check(VersionFromTitle(L"Intel net Driver Update (24.40.0.4)") == L"24.40.0.4", L"version from \"... Driver Update (x.y)\" title");
    Check(VersionFromTitle(L"Intel(R) Wi-Fi 6E AX211 160MHz") == L"", L"no version in a model name");
    Check(VersionFromTitle(L"Realtek Semiconductor Corp. - Audio Device - Realtek High Definition Audio").empty(), L"no version: text only");
    Check(VersionFromTitle(L"NVIDIA - Display - 32.0.15.6094") == L"32.0.15.6094", L"display driver title");

    // ---- Hardware id matching: most specific id first, compatible ids after, case-insensitive
    DeviceDriver wifi = Dev(L"Intel(R) Wi-Fi 6E AX211 160MHz", L"Net",
                            {L"PCI\\VEN_8086&DEV_7A70&SUBSYS_00948086&REV_11", L"PCI\\VEN_8086&DEV_7A70&SUBSYS_00948086", L"PCI\\VEN_8086&DEV_7A70"},
                            {L"PCI\\VEN_8086&CC_028000", L"PCI\\CC_0280"}, L"23.60.1.2", Day(2024, 5, 2), L"Intel");
    Check(MatchHardwareId(L"pci\\ven_8086&dev_7a70&subsys_00948086", wifi) == 1, L"hardware id match is case-insensitive and ranked");
    Check(MatchHardwareId(L"PCI\\CC_0280", wifi) == 4, L"compatible ids rank after hardware ids");
    Check(MatchHardwareId(L"PCI\\VEN_8086&DEV_7A71", wifi) == -1, L"another device doesn't match");
    Check(MatchHardwareId(L"", wifi) == -1, L"an empty id matches nothing");

    // ---- Offered vs installed
    Check(CompareOffer(L"23.60.1.2", Day(2024, 5, 2), L"Intel", L"24.40.0.4", Day(2026, 4, 13), L"Intel") == OfferAge::Newer, L"newer version");
    Check(CompareOffer(L"24.40.0.4", Day(2026, 4, 13), L"Intel", L"24.40.0.4", Day(2026, 4, 13), L"Intel Corporation") == OfferAge::Same,
          L"same version (provider spelled differently)");
    Check(CompareOffer(L"32.0.16.1088", Day(2026, 7, 22), L"NVIDIA", L"32.0.15.6094", Day(2026, 8, 1), L"NVIDIA") == OfferAge::Older,
          L"an older version with a newer date counts as older (Windows Update sometimes offers those)");
    Check(CompareOffer(L"10.0.26100.1", Day(2006, 6, 21), L"Microsoft", L"3.0.1511.0", Day(2022, 7, 22), L"MediaTek Inc.") == OfferAge::Newer,
          L"Microsoft's generic driver vs a vendor's: dates decide, not unrelated version numbers");
    Check(CompareOffer(L"", 0, L"", L"1.0", Day(2022, 1, 1), L"X") == OfferAge::Unknown, L"nothing to compare: unknown");
    Check(CompareOffer(L"", Day(2022, 1, 1), L"A", L"", Day(2022, 1, 1) + 3600LL * 10000000, L"A") == OfferAge::Same, L"dates compare by day");
    Check(SameCompany(L"Intel", L"Intel Corporation") && !SameCompany(L"Microsoft", L"MediaTek Inc."), L"same company by first word");

    // ---- Matching a list of updates to devices
    std::vector<DeviceDriver> devs = {
        wifi,
        Dev(L"NVIDIA GeForce RTX 4060", L"Display", {L"PCI\\VEN_10DE&DEV_2882&SUBSYS_41071458&REV_A1", L"PCI\\VEN_10DE&DEV_2882&SUBSYS_41071458"},
            {L"PCI\\VEN_10DE&CC_030000"}, L"32.0.16.1088", Day(2026, 7, 22), L"NVIDIA"),
        Dev(L"USB Serial Device", L"Ports", {L"USB\\VID_0E8D&PID_0003&REV_0100", L"USB\\VID_0E8D&PID_0003"}, {L"USB\\Class_02"}, L"10.0.26100.1",
            Day(2006, 6, 21), L"Microsoft", false),
        Dev(L"USB Serial Device", L"Ports", {L"USB\\VID_0E8D&PID_0003&REV_0100", L"USB\\VID_0E8D&PID_0003"}, {L"USB\\Class_02"}, L"10.0.26100.1",
            Day(2006, 6, 21), L"Microsoft", true),
    };
    std::vector<DriverUpdate> ups(4);
    ups[0].title = L"Intel net Driver Update (24.40.0.4)";
    ups[0].version = VersionFromTitle(ups[0].title);
    ups[0].hardwareId = L"pci\\ven_8086&dev_7a70&subsys_00948086";
    ups[0].driverClass = L"Networking";
    ups[0].provider = L"Intel";
    ups[0].date = Day(2026, 4, 13);
    ups[1].title = L"NVIDIA - Display - 32.0.15.6094";
    ups[1].version = VersionFromTitle(ups[1].title);
    ups[1].hardwareId = L"pci\\ven_10de&dev_2882&subsys_41071458";
    ups[1].driverClass = L"Display";
    ups[1].provider = L"NVIDIA";
    ups[1].date = Day(2026, 8, 1);
    ups[2].title = L"MediaTek Inc. - Ports - 3.0.1511.0";
    ups[2].version = VersionFromTitle(ups[2].title);
    ups[2].hardwareId = L"usb\\vid_0e8d&pid_0003";
    ups[2].driverClass = L"OtherHardware";
    ups[2].provider = L"MediaTek Inc.";
    ups[2].date = Day(2022, 7, 22);
    ups[3].title = L"Some Vendor - System - 1.2.3";
    ups[3].version = L"1.2.3";
    ups[3].hardwareId = L"acpi\\ven_abcd&dev_0001";
    MatchDriverUpdates(ups, devs);
    Check(ups[0].device == 0 && ups[0].age == OfferAge::Newer && !ups[0].display && ups[0].Preselect(devs), L"Wi-Fi update: matched, newer, preselected");
    Check(ups[1].device == 1 && ups[1].display && ups[1].age == OfferAge::Older && !ups[1].Preselect(devs),
          L"graphics update: a display driver, older than installed, not preselected");
    Check(ups[2].device == 3 && ups[2].devices == 2 && ups[2].age == OfferAge::Newer, L"two matching devices: the connected one is shown");
    Check(ups[3].device == -1 && ups[3].age == OfferAge::Unknown && !ups[3].Preselect(devs), L"no matching device: shown, never preselected");
    ups[0].hidden = true;
    Check(!ups[0].Preselect(devs), L"hidden in Windows Update: not preselected");
    ups[0].hidden = false;
    ups[0].eulaAccepted = false;
    Check(!ups[0].Preselect(devs), L"license terms not accepted: not preselected");
    DriverUpdate video;
    video.driverClass = L"Display";
    Check(IsDisplayDriver(video, nullptr), L"a Display-class update is a graphics driver even without a device");

    // ---- The elevated CLI's result lines
    DriverInstallResult r;
    r.updateId = L"d1199072-9923-44f8-b64c-1e175eb8cf7f";
    r.title = L"Intel net Driver Update (24.40.0.4)\twith a tab";
    r.status = InstallStatus::Installed;
    r.reboot = true;
    std::wstring line = FormatInstallResultLine(r);
    DriverInstallResult back;
    Check(ParseInstallResultLine(line, back) && back.updateId == r.updateId && back.status == InstallStatus::Installed && back.reboot &&
              back.title == L"Intel net Driver Update (24.40.0.4) with a tab",
          L"result line round trip: " + line);
    r.status = InstallStatus::Failed;
    r.reboot = false;
    r.hr = static_cast<long>(0x8024200BL);
    r.message = WuErrorText(r.hr);
    std::string raw = "Asking Windows Update...\r\n" + util::WideToUtf8(FormatInstallResultLine(r)) + "\r\nRESULT\tbroken\r\n";
    auto parsed = ParseInstallOutput(raw);
    Check(parsed.size() == 1 && parsed[0].status == InstallStatus::Failed && parsed[0].hr == static_cast<long>(0x8024200BL) &&
              parsed[0].message.find(L"0x8024200B") != std::wstring::npos,
          L"failed result with its HRESULT text: " + (parsed.empty() ? L"" : parsed[0].message));
    Check(WuErrorText(static_cast<long>(0x80240044L)).find(L"administrator") != std::wstring::npos, L"Windows Update error in words");
    Check(WuErrorText(static_cast<long>(0x80070005L)).find(L"0x80070005") != std::wstring::npos, L"system error with its code");

    std::vector<std::wstring> ids;
    Check(ParseUpdateIdList(L"D1199072-9923-44f8-b64c-1e175eb8cf7f, 4093f717-f4a5-4e50-b67f-d0e3f49eee33,d1199072-9923-44f8-b64c-1e175eb8cf7f", ids) &&
              ids.size() == 2 && ids[0] == L"d1199072-9923-44f8-b64c-1e175eb8cf7f",
          L"update id list: lowercased, duplicates dropped");
    Check(!ParseUpdateIdList(L"d1199072-9923-44f8-b64c-1e175eb8cf7f & del x", ids) && !ParseUpdateIdList(L"", ids),
          L"anything but GUIDs is refused (the ids go on a command line)");

    std::wstring note = InstallJournalNote(back, &ups[0], &devs[0]);
    Check(note.find(L"23.60.1.2 (2024-05-02) -> 24.40.0.4 (2026-04-13)") != std::wstring::npos && note.find(L"restart required") != std::wstring::npos,
          L"journal note: " + note);

    // ---- Graphics: vendors from hardware ids, adapters grouped
    Check(GpuVendorFromHardwareId(L"PCI\\VEN_10DE&DEV_2882") == L"nvidia" && GpuVendorFromHardwareId(L"PCI\\VEN_1002&DEV_73BF") == L"amd" &&
              GpuVendorFromHardwareId(L"PCI\\VEN_8086&DEV_A780") == L"intel" && GpuVendorFromHardwareId(L"ROOT\\BasicDisplay").empty(),
          L"GPU vendor from the PCI vendor id");
    std::vector<DeviceDriver> gpus = {
        Dev(L"Intel(R) UHD Graphics 770", L"Display", {L"PCI\\VEN_8086&DEV_A780"}, {}, L"31.0.101.5590", 0, L"Intel Corporation"),
        Dev(L"NVIDIA GeForce RTX 4060", L"Display", {L"PCI\\VEN_10DE&DEV_2882"}, {}, L"32.0.16.1088", 0, L"NVIDIA"),
        Dev(L"Microsoft Basic Display Adapter", L"Display", {L"ROOT\\BasicDisplay"}, {}, L"10.0.26100.1", 0, L"Microsoft"),
        Dev(L"AMD Radeon RX 6800 (unplugged)", L"Display", {L"PCI\\VEN_1002&DEV_73BF"}, {}, L"31.0.1", 0, L"AMD", false),
        Dev(L"NVIDIA High Definition Audio", L"MEDIA", {L"HDAUDIO\\FUNC_01&VEN_10DE&DEV_00A3"}, {}, L"1.4", 0, L"NVIDIA"),
    };
    auto groups = GroupGpus(gpus);
    Check(groups.size() == 2 && groups[0].vendor == L"nvidia" && groups[1].vendor == L"intel" && groups[1].adapters[0] == 0,
          L"connected display adapters grouped by vendor (unplugged and generic ones left out)");

    // ---- Vendor tools from (fake) Uninstall entries and Store packages
    std::vector<UninstallEntry> entries = {
        Entry(L"NVIDIA App 11.0.9.565", L"NVIDIA Corporation"),
        Entry(L"NVIDIA App driver settings", L"NVIDIA Corporation"),
        Entry(L"Intel® Driver & Support Assistant", L"Intel"),
        Entry(L"Armoury Crate Service", L"ASUSTeK COMPUTER INC."),
        Entry(L"ASUS DriverHub", L"ASUSTeK COMPUTER INC."),
        Entry(L"App Center", L"Some Other Company"),
        Entry(L"Dell Command | Update for Windows Universal", L"Dell Inc."),
    };
    std::vector<StorePackage> store = {{L"E046963F.LenovoCompanion_10.2401.29.0_x64__k1h2ywk1493x8", L"App"},
                                       {L"B9ECED6F.ArmouryCrate_6.1.18.0_x64__qmba6cd70vzyy", L"App"},
                                       {L"Microsoft.WindowsCalculator_11.2607.0.0_x64__8wekyb3d8bbwe", L"App"}};
    auto nv = FindDriverTools(L"nvidia", entries, store, false);
    Check(nv.size() == 1 && nv[0].installed && nv[0].name == L"NVIDIA app", L"NVIDIA app found by its versioned name (not \"driver settings\")");
    auto amd = FindDriverTools(L"amd", entries, store, false);
    Check(amd.size() == 1 && !amd[0].installed && amd[0].launch == VendorSupportUrl(L"amd") && amd[0].name.find(L"Adrenalin") != std::wstring::npos,
          L"AMD Software missing: its official download page");
    auto intel = FindDriverTools(L"intel", entries, store, false);
    Check(intel.size() == 1 && intel[0].installed && intel[0].launch.rfind(L"https://www.intel.com/", 0) == 0,
          L"Intel DSA (\"®\" in its name) opens its web page");
    Check(NormalizeToolName(L"Intel(R)  Driver &  Support Assistant™") == L"intel driver & support assistant", L"tool names normalized");
    auto asus = FindDriverTools(L"asus", entries, store, false);
    Check(asus.size() == 2 && asus[0].name == L"ASUS DriverHub" && asus[0].launch == L"https://driverhub.asus.com" && asus[1].name == L"Armoury Crate" &&
              asus[1].launch == L"shell:AppsFolder\\B9ECED6F.ArmouryCrate_qmba6cd70vzyy!App",
          L"ASUS: DriverHub (web page) and Armoury Crate (Store app); \"Armoury Crate Service\" alone is not the app");
    auto lenovo = FindDriverTools(L"lenovo", entries, store, false);
    Check(lenovo.size() == 1 && lenovo[0].name == L"Lenovo Vantage" && lenovo[0].launch == L"shell:AppsFolder\\E046963F.LenovoCompanion_k1h2ywk1493x8!App",
          L"Lenovo Vantage from its Store package");
    auto dell = FindDriverTools(L"dell", entries, store, false);
    Check(dell.size() == 1 && dell[0].installed && dell[0].name == L"Dell Command | Update", L"Dell Command | Update from its Uninstall entry");
    auto giga = FindDriverTools(L"gigabyte", entries, store, false);
    Check(giga.size() == 1 && !giga[0].installed && giga[0].launch == VendorSupportUrl(L"gigabyte"),
          L"another publisher's \"App Center\" isn't GIGABYTE's");
    auto surface = FindDriverTools(L"surface", entries, store, false);
    Check(surface.size() == 1 && !surface[0].installed && !surface[0].note.empty(), L"Surface: drivers come through Windows Update (noted)");
    auto fw = FindDriverTools(L"framework", entries, store, false);
    Check(fw.size() == 1 && fw[0].name.empty() && fw[0].launch == VendorSupportUrl(L"framework"), L"Framework: no tool, its driver bundle page");

    // ---- PC maker from SMBIOS strings
    PcMaker m = DetectPcMaker(L"ASUS", L"System Product Name", L"ASUSTeK COMPUTER INC.", L"PRIME Z790-A WIFI");
    Check(m.vendor == L"asus" && m.selfBuilt && m.model == L"PRIME Z790-A WIFI", L"self-built PC: the motherboard's maker and model");
    m = DetectPcMaker(L"Dell Inc.", L"XPS 13 9340", L"Dell Inc.", L"0X8DXD");
    Check(m.vendor == L"dell" && !m.selfBuilt && m.model == L"XPS 13 9340", L"Dell laptop");
    m = DetectPcMaker(L"LENOVO", L"21K5CTO1WW", L"LENOVO", L"21K5CTO1WW");
    Check(m.vendor == L"lenovo", L"Lenovo");
    m = DetectPcMaker(L"HP", L"HP EliteBook 840 G10", L"HP", L"8B41");
    Check(m.vendor == L"hp", L"HP");
    m = DetectPcMaker(L"Micro-Star International Co., Ltd.", L"MS-7D25", L"Micro-Star International Co., Ltd.", L"PRO Z690-A WIFI DDR4(MS-7D25)");
    Check(m.vendor == L"msi", L"MSI");
    m = DetectPcMaker(L"To be filled by O.E.M.", L"To be filled by O.E.M.", L"Gigabyte Technology Co., Ltd.", L"B650 AORUS ELITE AX");
    Check(m.vendor == L"gigabyte" && m.selfBuilt, L"Gigabyte board in a self-built PC");
    m = DetectPcMaker(L"System manufacturer", L"System Product Name", L"ASRock", L"B650M Pro RS");
    Check(m.vendor == L"asrock" && m.selfBuilt && m.model == L"B650M Pro RS", L"ASRock board");
    m = DetectPcMaker(L"Microsoft Corporation", L"Surface Laptop 7", L"Microsoft Corporation", L"Surface Laptop 7");
    Check(m.vendor == L"surface", L"Microsoft Surface");
    m = DetectPcMaker(L"Microsoft Corporation", L"Virtual Machine", L"Microsoft Corporation", L"Virtual Machine");
    Check(m.vendor.empty(), L"a Hyper-V virtual machine has no PC maker tool");
    m = DetectPcMaker(L"Framework", L"Laptop 13 (AMD Ryzen 7040Series)", L"Framework", L"FRANMDCP07");
    Check(m.vendor == L"framework", L"Framework");
    m = DetectPcMaker(L"Acer", L"Swift SFG14-73", L"IL", L"Lily_MTH");
    Check(m.vendor == L"acer", L"Acer");
    m = DetectPcMaker(L"Default string", L"Default string", L"Default string", L"Default string");
    Check(m.vendor.empty() && m.name.empty(), L"nothing but placeholders: unknown");
    Check(IsPlaceholder(L"  To be filled by O.E.M. ") && !IsPlaceholder(L"Dell Inc."), L"placeholder strings");

    // ---- Optional: what Windows Update offers this PC (read-only search)
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"BYTOPSY_WU_SEARCH", env, 8) && env[0] == L'1') {
        DriverSearch s = SearchDriverUpdates();
        Check(s.ok, L"Windows Update search: " + (s.ok ? std::to_wstring(s.updates.size()) + L" driver updates offered" : s.error));
        for (auto& u : s.updates) {
            std::wstring cur = u.device >= 0 ? s.devices[u.device].version + L" on " + s.devices[u.device].name : L"no device";
            fputs(util::WideToUtf8(L"      " + u.title + L"  [" + u.updateId + L"]  installed: " + cur + L"\n").c_str(), stdout);
        }
    }

    if (g_fail) {
        printf("%d FAILED\n", g_fail);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
