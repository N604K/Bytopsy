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
// "" when App Installer is missing.
std::wstring FindWinget();
// The Microsoft Store page of App Installer (it brings winget).
extern const wchar_t* const kAppInstallerStoreLink;

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
