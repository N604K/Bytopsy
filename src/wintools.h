#pragma once

// Space Windows itself keeps: the component store (WinSxS), old driver packages, System Restore
// storage, the hibernation file, Compact OS and event logs. Each is measured and changed with
// Windows' own tools (DISM, pnputil, vssadmin, powercfg, compact, wevtutil); changes need
// administrator rights and go through RunAsAdmin.

#include <cstdint>
#include <string>
#include <vector>

#include "runtool.h"

// ---- Component store (WinSxS)
struct ComponentStore {
    bool ok = false;
    uint64_t actual = 0;   // "Actual Size of Component Store"
    uint64_t backups = 0;  // "Backups and Disabled Features" (superseded components are part of it)
    uint64_t cache = 0;    // "Cache and Temporary Data"
    int reclaimablePackages = -1;
    bool recommended = false;
    std::wstring lastCleanup;
    std::wstring error;
};
ComponentStore ParseComponentStore(const std::string& dismOutput);
ComponentStore AnalyzeComponentStore();  // DISM /AnalyzeComponentStore, as administrator (takes minutes)
AdminRun CleanComponentStore();          // DISM /StartComponentCleanup (no /ResetBase: updates stay removable)

// ---- Driver packages (third-party drivers in the DriverStore)
struct DriverPackage {
    std::wstring oemInf;       // "oem12.inf": what pnputil deletes
    std::wstring originalInf;  // "nv_dispi.inf"
    std::wstring provider, className, version;
    int64_t date = 0;      // DriverVer date (FILETIME ticks)
    std::wstring folder;   // its DriverStore\FileRepository folder
    uint64_t bytes = 0;
    bool inUse = false;    // a device uses it (connected now or not)
    bool old = false;      // a newer version of the same driver is in the store, and nothing uses this one
};
std::vector<DriverPackage> ListDriverPackages();  // marks old ones (MarkOldDrivers)
// Same original .inf, provider and class = the same driver: all but the newest are old unless a
// device uses them. Printer drivers are never marked (print queues use them without a device).
void MarkOldDrivers(std::vector<DriverPackage>& pkgs);
bool ParseDriverVer(const std::wstring& driverVer, int64_t& date, std::wstring& version);  // "06/21/2024,32.0.15.6094"
int CompareVersions(const std::wstring& a, const std::wstring& b);
// pnputil /delete-driver for each (never /force: a driver in use stays).
AdminRun DeleteDriverPackages(const std::vector<DriverPackage>& pkgs);

// ---- System Restore (shadow copy storage)
struct ShadowStorage {
    std::wstring forDrive, onDrive;  // "C:"
    uint64_t used = 0, allocated = 0, max = 0;  // max: UINT64_MAX = no limit
    uint64_t volumeSize = 0;
};
std::vector<ShadowStorage> QueryShadowStorage(std::wstring& err);  // needs administrator rights
AdminRun LimitShadowStorage(const ShadowStorage& s, int percent);   // vssadmin resize shadowstorage

// ---- Hibernation file
struct Hibernation {
    bool on = false;
    bool reduced = false;      // only enough for Fast Startup
    bool fastStartup = false;
    uint64_t bytes = 0;        // hiberfil.sys
};
Hibernation QueryHibernation();
AdminRun SetHibernation(const wchar_t* mode);  // "off", "reduced", "on"

// ---- Compact OS
bool CompactOsOn();
AdminRun SetCompactOs(bool on);

// ---- Event logs
struct EventLogs {
    bool known = false;
    uint64_t bytes = 0, security = 0;  // all .evtx files, and the Security log (never cleared)
    size_t files = 0;
};
EventLogs QueryEventLogs();
AdminRun ClearEventLogs();  // every log but Security
