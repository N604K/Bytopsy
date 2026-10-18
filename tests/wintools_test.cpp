// Self-check of the Windows tools: winncdu-wintools-test.exe
// Parses sample DISM output and decides which driver packages are old. Never changes anything:
// set WINNCDU_DRIVERS=1 to also list this PC's driver packages (read-only).
#include <windows.h>

#include <cstdio>
#include <string>

#include "util.h"
#include "wintools.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static DriverPackage Pkg(const wchar_t* oem, const wchar_t* orig, const wchar_t* ver, bool inUse = false,
                         const wchar_t* cls = L"Display", const wchar_t* provider = L"NVIDIA") {
    DriverPackage p;
    p.oemInf = oem;
    p.originalInf = orig;
    p.provider = provider;
    p.className = cls;
    ParseDriverVer(ver, p.date, p.version);
    p.inUse = inUse;
    return p;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // ---- DISM /AnalyzeComponentStore
    std::string dism =
        "\r\nDeployment Image Servicing and Management tool\r\nVersion: 10.0.26100.1150\r\n\r\nImage Version: 10.0.26200.6901\r\n\r\n"
        "[===========================100.0%==========================]\r\n\r\nComponent Store (WinSxS) information:\r\n\r\n"
        "Windows Explorer Reported Size of Component Store : 9.12 GB\r\n\r\nActual Size of Component Store : 8.84 GB\r\n\r\n"
        "    Shared with Windows : 6.01 GB\r\n    Backups and Disabled Features : 2.56 GB\r\n    Cache and Temporary Data :  278.13 MB\r\n\r\n"
        "Date of Last Cleanup : 2026-09-30 10:11:12\r\n\r\nNumber of Reclaimable Packages : 3\r\n"
        "Component Store Cleanup Recommended : Yes\r\n\r\nThe operation completed successfully.\r\n";
    ComponentStore cs = ParseComponentStore(dism);
    Check(cs.ok && cs.actual == static_cast<uint64_t>(8.84 * 1024 * 1024 * 1024 + 0.5) &&
              cs.backups == static_cast<uint64_t>(2.56 * 1024 * 1024 * 1024 + 0.5) &&
              cs.cache == static_cast<uint64_t>(278.13 * 1024 * 1024 + 0.5) && cs.reclaimablePackages == 3 && cs.recommended &&
              cs.lastCleanup == L"2026-09-30 10:11:12",
          L"DISM component store report: " + util::FormatSize(cs.actual) + L", " + std::to_wstring(cs.reclaimablePackages) + L" packages");
    ComponentStore none = ParseComponentStore("\r\nError: 740\r\n\r\nElevated permissions are required to run DISM.\r\n");
    Check(!none.ok && none.error == L"Elevated permissions are required to run DISM.", L"DISM errors come through: " + none.error);
    ComponentStore no = ParseComponentStore("Actual Size of Component Store : 5,5 GB\r\nComponent Store Cleanup Recommended : No\r\n");
    Check(no.ok && !no.recommended && no.actual == static_cast<uint64_t>(5.5 * 1024 * 1024 * 1024), L"decimal comma, not recommended");

    // ---- DriverVer and versions
    int64_t date = 0;
    std::wstring ver;
    Check(ParseDriverVer(L"06/21/2024,32.0.15.6094", date, ver) && ver == L"32.0.15.6094" && date > 0, L"DriverVer parsed");
    Check(!ParseDriverVer(L"garbage", date, ver), L"bad DriverVer refused");
    Check(CompareVersions(L"32.0.15.6094", L"32.0.15.10") > 0 && CompareVersions(L"1.2", L"1.2.0") == 0 &&
              CompareVersions(L"10.0.1", L"9.9.9") > 0 && CompareVersions(L"1.0", L"1.0.1") < 0,
          L"versions compare by number, not by text");

    // ---- Old drivers
    std::vector<DriverPackage> p = {
        Pkg(L"oem10.inf", L"nv_dispi.inf", L"01/10/2024,31.0.15.5000"),        // 0 old
        Pkg(L"oem11.inf", L"nv_dispi.inf", L"06/21/2024,32.0.15.6094", true),  // 1 newest, in use
        Pkg(L"oem12.inf", L"nv_dispi.inf", L"03/01/2024,31.0.15.5200", true),  // 2 older but a device uses it
        Pkg(L"oem20.inf", L"rt640x64.inf", L"05/05/2023,10.60.0", false, L"Net", L"Realtek"),  // 3 only version
        Pkg(L"oem30.inf", L"prnhp.inf", L"01/01/2020,1.0", false, L"Printer", L"HP"),          // 4 printer
        Pkg(L"oem31.inf", L"prnhp.inf", L"01/01/2022,2.0", false, L"Printer", L"HP"),          // 5 printer
        Pkg(L"oem40.inf", L"nv_dispi.inf", L"01/10/2023,30.0.1", false, L"Display", L"Other"),  // 6 other provider
        Pkg(L"oem50.inf", L"same.inf", L"01/01/2024,1.0.10", false, L"System", L"X"),          // 7 newer by version
        Pkg(L"oem51.inf", L"same.inf", L"01/01/2024,1.0.9", false, L"System", L"X"),           // 8 old: same date
    };
    MarkOldDrivers(p);
    Check(p[0].old && !p[1].old, L"an older version of a driver is old, the newest is kept");
    Check(!p[2].old, L"an older version a device uses is kept");
    Check(!p[3].old, L"a driver with one version is kept");
    Check(!p[4].old && !p[5].old, L"printer drivers are never marked (print queues use them)");
    Check(!p[6].old, L"the same .inf name from another provider is another driver");
    Check(!p[7].old && p[8].old, L"same date: the higher version wins");

    if (GetEnvironmentVariableW(L"WINNCDU_DRIVERS", nullptr, 0)) {
        auto all = ListDriverPackages();
        uint64_t old = 0;
        size_t n = 0;
        for (auto& d : all)
            if (d.old) {
                old += d.bytes;
                ++n;
                std::string line = util::WideToUtf8(L"     old: " + d.oemInf + L"  " + d.originalInf + L"  " + d.provider + L"  " + d.version +
                                                    L"  " + util::FormatSizeShort(d.bytes) + L"\n");
                fputs(line.c_str(), stdout);
            }
        std::string line = util::WideToUtf8(L"     " + std::to_wstring(all.size()) + L" packages, " + std::to_wstring(n) + L" old (" +
                                            util::FormatSizeShort(old) + L")\n");
        fputs(line.c_str(), stdout);
    }

    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
