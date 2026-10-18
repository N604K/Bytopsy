#pragma once

// Installed programs, as Settings > Installed apps lists them: read from the Uninstall registry keys
// (HKLM 64-bit and 32-bit views, HKCU). Uninstalling always runs the program's own registered
// uninstaller, which asks the user what to do. Afterwards, folders the program left behind are
// found by conservative name matching and offered for review; registry keys are never touched.

#include <cstdint>
#include <string>
#include <vector>

#include "protect.h"

typedef void* HANDLE;

struct UninstallEntry {
    std::wstring key;  // subkey name under ...\CurrentVersion\Uninstall
    std::wstring displayName, publisher, displayVersion, uninstallString, installLocation, displayIcon;
    std::wstring parentKeyName, releaseType;
    uint64_t estimatedBytes = 0;
    int64_t installDate = 0;  // FILETIME ticks: InstallDate, else when the key was last written; 0 = unknown
    bool systemComponent = false, windowsInstaller = false, noRemove = false;
    bool perUser = false;  // HKCU (else HKLM)
    bool wow64 = false;    // HKLM's 32-bit view
};

// Every Uninstall entry with a DisplayName, hidden ones included (they still own their folders).
std::vector<UninstallEntry> ReadUninstallEntries();
// Is this entry an app people uninstall (not a system component, update, patch or part of another app)?
bool IsListedApp(const UninstallEntry& e);
// Re-reads the registry: does the entry still exist (the app is still installed)?
bool UninstallEntryExists(const UninstallEntry& e);
// "HKLM\SOFTWARE\...\Uninstall\<key>" (32-bit view noted).
std::wstring UninstallKeyPath(const UninstallEntry& e);

// The app's own folder: InstallLocation, else the folder its uninstaller is in, when that is the
// app's own (not msiexec, a package cache or a shared folder). "" if unknown.
std::wstring AppInstallFolder(const UninstallEntry& e);

// Splits a registered uninstall command into program and arguments.
bool SplitCommand(const std::wstring& command, std::wstring& exe, std::wstring& args);
// "steam://uninstall/730", "ms-settings:appsfeatures" (not "C:\...").
bool IsUriCommand(const std::wstring& command);
// Starts a registered command (program + arguments, or a URI) through the shell, so an uninstaller
// that needs administrator rights gets its UAC prompt. `process` (optional) gets the process
// handle when there is one; the caller closes it.
bool LaunchCommand(const std::wstring& command, HANDLE* process, std::wstring& err);

struct InstalledApp {
    UninstallEntry e;
    std::wstring folder;    // AppInstallFolder
    uint64_t bytes = 0;     // EstimatedSize, else the measured folder
    bool measured = false;  // bytes come from measuring the folder
};

// The listed apps, largest first. `measure`: size folders of apps that don't register a size.
std::vector<InstalledApp> FindInstalledApps(bool measure = true);

// ---- Leftovers

struct LeftoverRoots {
    // Where folders named after an app are looked for (Program Files, ProgramData, AppData, ...).
    std::vector<std::wstring> roots;
    // Folders that are never offered, nor anything that contains them (profile, Documents, ...).
    std::vector<std::wstring> never;
    static LeftoverRoots ThisPC();
};

struct AppLeftover {
    std::wstring path;
    std::wstring why;           // "Its install folder", "Named after the app in ...", ...
    uint64_t bytes = 0;
    uint64_t items = 0;         // files and folders, the folder itself included
    bool preselect = false;     // only the former install folder, and only with nothing protected in it
    Protect level = Protect::None;
    std::wstring protectNote;   // DescribeHit of the worst hit, or what the walk found inside
    // Important and Blocked leftovers (user data, keys, wallets, system folders) are shown, never
    // deleted from here.
    bool Deletable() const { return level != Protect::Important && level != Protect::Blocked; }
};

// CheckProtection for the folder of an uninstalled app: the "inside Program Files / ProgramData:
// an installed program" rule is left out (its app is gone); every other rule applies.
Protect LeftoverProtection(const std::wstring& path, std::wstring* note = nullptr);

// Folders the uninstalled app `gone` left behind. `installed`: every Uninstall entry that still
// exists (folders another app may use are never offered). Each candidate is checked against the
// protection rules (also what is inside it) and measured. Call RefreshProtection() first.
std::vector<AppLeftover> FindAppLeftovers(const UninstallEntry& gone, const std::vector<UninstallEntry>& installed,
                                          const LeftoverRoots& roots);

// ---- Exposed for tests
// Lowercase names a folder of this app could have: "7-zip 23.01 (x64)", "7-zip", the install folder's name.
std::vector<std::wstring> AppNameVariants(const UninstallEntry& e);
// "mozilla corporation" -> "mozilla corporation", "mozilla".
std::vector<std::wstring> PublisherVariants(const std::wstring& publisher);
// "20240131" -> FILETIME ticks (local midnight), 0 if not a date.
int64_t ParseInstallDate(const std::wstring& s);
