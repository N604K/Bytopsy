#pragma once

// App updates through winget (App Installer): `winget upgrade` lists what has a newer version, and
// each chosen app is updated with `winget upgrade --id <Id> --exact`, which runs that app's own
// installer. winget contacts its sources (the winget community repository, the Microsoft Store);
// Bytopsy itself contacts nothing. winget prints a table only (no JSON), so it is parsed from the
// header's column positions, whatever language the headers are in.

#include <atomic>
#include <string>
#include <vector>

#include "apps.h"

struct WingetUpdate {
    std::wstring name, id, version, available, source;  // as winget shows them ("< 3.10.8" included)
    bool nameCut = false;         // the name ends in "…" (winget shortened it to fit)
    bool idCut = false;           // the id too: it can't be updated from here
    bool unknownVersion = false;  // installed version "Unknown" (listed with --include-unknown)
    bool explicitOnly = false;    // in the "require explicit targeting" table (pinned)
    bool Updatable() const;
};

struct WingetList {
    bool ok = false;  // winget answered: a table, or "no updates"
    std::vector<WingetUpdate> updates;
    int unknownCount = 0;  // "N package(s) have version numbers that cannot be determined"
    int pinnedCount = 0;   // "N package(s) have pins that prevent upgrade" (not listed)
    std::wstring error;    // why there is no list (winget's last line, or a known exit code)
};

// winget.exe: BYTOPSY_WINGET (tests: a .cmd that prints a fixture), then PATH, then
// %LOCALAPPDATA%\Microsoft\WindowsApps\winget.exe (an app execution alias, a 0-byte reparse point).
// "" when it can't be found (DiagnoseWinget says why).
std::wstring FindWinget();
// The Microsoft Store page of App Installer (it brings winget).
extern const wchar_t* const kAppInstallerStoreLink;

// ---- When winget can't be found
// winget comes with App Installer (package family Microsoft.DesktopAppInstaller_8wekyb3d8bbwe). It
// can be missing in several ways, each with its own fix.
enum class WingetState {
    Found,          // winget.exe is there
    AliasOff,       // App Installer is registered for this user and has its winget.exe link, but the
                    // App execution alias in %LOCALAPPDATA%\Microsoft\WindowsApps is turned off
    NotReachable,   // App Installer is registered for this user, but no winget.exe link exists (a
                    // fresh profile before the Store finished setting it up)
    NotRegistered,  // App Installer is on this PC (another user's, or provisioned for new accounts)
                    // but not registered for this user
    NotInstalled,   // App Installer isn't on this PC (LTSC, Server, Store removed)
};
// What DiagnoseWinget looks at (read-only).
struct WingetProbe {
    std::wstring winget;                        // FindWinget()
    std::vector<std::wstring> userPackages;     // package full names registered for this user
    std::vector<std::wstring> machinePackages;  // on this PC: any user's, and the provisioned ones
    bool packageAlias = false;                  // ...\WindowsApps\Microsoft.DesktopAppInstaller_8wekyb3d8bbwe\winget.exe
};
struct WingetDiagnosis {
    WingetState state = WingetState::NotInstalled;
    std::wstring winget;   // when found
    std::wstring package;  // App Installer's package full name ("" when not on this PC)
    // Re-registering App Installer for this user can bring winget back.
    bool CanRepair() const { return state == WingetState::AliasOff || state == WingetState::NotReachable || state == WingetState::NotRegistered; }
};
// Reads FindWinget(), the current user's package repository (HKCU) and this PC's (HKLM: every
// package installed, and the provisioned ones), without admin. BYTOPSY_APPINSTALLER (tests) stands in
// for the packages: "user" (registered, no winget.exe link), "alias" (registered, alias off), "pc"
// (on this PC, not registered for this user) or "none".
WingetProbe ProbeWinget();
WingetDiagnosis DiagnoseWinget(const WingetProbe& p);
inline WingetDiagnosis DiagnoseWinget() { return DiagnoseWinget(ProbeWinget()); }
// "Microsoft.DesktopAppInstaller_1.29.379.0_x64__8wekyb3d8bbwe" (or its bundle).
bool IsAppInstallerPackage(const std::wstring& fullName);

extern const wchar_t* const kAppInstallerFamily;  // Microsoft.DesktopAppInstaller_8wekyb3d8bbwe
extern const wchar_t* const kWingetReleasesLink;  // Microsoft's winget releases (for PCs without the Store)
extern const wchar_t* const kAppAliasesSettings;  // Settings > Apps > Advanced app settings
// The PowerShell command that registers App Installer for this user (as this user, no admin,
// nothing downloaded): Add-AppxPackage -RegisterByFamilyName -MainPackage <family>.
std::wstring WingetRepairCommand();
// What `powershell.exe` is given: -NoProfile -NonInteractive -Command "<WingetRepairCommand()>".
std::wstring WingetRepairArgs();

enum class WingetRepairOutcome {
    Fixed,         // winget is found afterwards
    StillAlias,    // registered, but winget.exe still isn't reachable: the App execution alias is off
    Failed,        // PowerShell reported an error (detail: its first line)
    StillMissing,  // PowerShell succeeded but App Installer still isn't registered
    NoPowerShell,  // powershell.exe couldn't be started
    TimedOut,      // stopped after 5 minutes
    NotRun,        // a test setting stands in for winget and no stand-in repair was given
};
struct WingetRepairResult {
    WingetRepairOutcome outcome = WingetRepairOutcome::NotRun;
    int code = -1;          // PowerShell's exit code
    std::wstring detail;    // PowerShell's first line of output, when it failed
    WingetDiagnosis after;  // winget looked for again
};
// Runs WingetRepairCommand() through powershell.exe (BYTOPSY_WINGET_REPAIR: a test's .cmd instead;
// while BYTOPSY_WINGET or BYTOPSY_APPINSTALLER is set nothing else runs), then diagnoses again.
// Waits up to 5 minutes.
WingetRepairResult RepairWinget();
// The journal note for an attempt (English).
std::wstring WingetRepairJournalNote(const WingetRepairResult& r);
// What `bytopsy-cli --app-updates` prints when winget is missing: the diagnosis and its fix (English).
std::wstring WingetMissingText(const WingetDiagnosis& d);

// Runs `winget upgrade --disable-interactivity` (read-only: it only lists) and parses it. Apps whose
// installed version winget can't tell are only counted (updating them could reinstall the same
// version).
WingetList ListWingetUpdates(const std::wstring& winget, std::atomic<bool>* cancel = nullptr);

struct WingetUpgradeResult {
    int code = -1;           // winget's exit code (-1 couldn't start, -2 cancelled or timed out)
    bool ok = false;         // updated (maybe needing a restart)
    bool restart = false;    // restart Windows to finish
    std::wstring message;    // English (N_), translate with Tr(); for failures
};
// `winget upgrade --id <Id> --exact ...` for one app: its installer runs (and may show its own UAC
// prompt). Waits up to 30 minutes.
WingetUpgradeResult RunWingetUpgrade(const std::wstring& winget, const WingetUpdate& u, std::atomic<bool>* cancel = nullptr);
// The arguments it passes (shown in the confirmation, journaled).
std::wstring WingetUpgradeArgs(const WingetUpdate& u);

// For each update, the index into `apps` of the installed app it updates, or -1 (an MSIX/Store app,
// or one the Apps list leaves out). winget shows an app's own registered name (cut with "…" when
// long), so names are matched first, versions break ties.
std::vector<int> MatchUpdatesToApps(const std::vector<WingetUpdate>& updates, const std::vector<InstalledApp>& apps);

// ---- Exposed for tests
WingetList ParseWingetUpgrade(const std::wstring& text);
// Console output as lines: escape sequences dropped, a bare '\r' starts the line over (winget's
// progress spinner), '\b' erases.
std::vector<std::wstring> ConsoleLines(const std::wstring& text);
// What a winget exit code means (English, N_), "" if unknown.
std::wstring WingetCodeText(int code);
// An id safe to pass on a command line (no spaces, quotes or shell characters, not cut).
bool ValidWingetId(const std::wstring& id);
