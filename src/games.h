#pragma once

// Installed games across launchers (Steam, Epic, GOG, EA app, Ubisoft Connect, Battle.net, Xbox),
// with their size and when they were last played. Uninstalling always goes through the game's own
// launcher or uninstaller, so the launcher's library, cloud saves and shared runtimes stay right.
// Folders a launcher left behind after an uninstall are listed as leftovers.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "apps.h"  // UninstallEntry, SplitCommand

struct Game {
    std::wstring launcher;  // "Steam", "Epic Games", "GOG", "EA app", "Ubisoft Connect", "Battle.net", "Xbox"
    std::wstring id;        // Steam app id, Epic AppName, ...
    std::wstring name;
    std::wstring folder;
    uint64_t bytes = 0;
    int64_t lastPlayed = 0;     // FILETIME ticks; 0 = unknown
    bool lastPlayedGuess = false;  // from the last time one of its programs was opened, not the launcher
    std::wstring uninstall;     // launcher URI ("steam://uninstall/730") or the registered uninstall command
    std::wstring uninstallHow;  // what happens: "Steam asks to confirm", ...
    bool leftover = false;      // a folder of a game that is no longer installed: deleted like any folder
};

// Every launcher found on this PC. Folder sizes are measured (can take a few seconds).
std::vector<Game> FindGames();

// Starts the uninstall through the launcher or the game's uninstaller (which asks the user).
bool StartUninstall(const Game& g, std::wstring& err);

// ---- Exposed for tests
struct Vdf {
    std::map<std::wstring, std::wstring> values;  // keys lowercase
    std::map<std::wstring, Vdf> children;
    std::wstring Get(const wchar_t* key) const;
};
Vdf ParseVdf(const std::wstring& text);  // Valve KeyValues text (libraryfolders.vdf, appmanifest_*.acf)
// One Steam library's steamapps folder: installed games, and leftovers (common folders, workshop
// items and shader caches of games that are not installed).
void ScanSteamLibrary(const std::wstring& steamapps, std::vector<Game>& out);
void ScanEpicManifests(const std::wstring& manifestsDir, std::vector<Game>& out);

// The launcher an Uninstall registry entry belongs to ("GOG", "Ubisoft Connect", "EA app",
// "Battle.net"), or "" if it is not a game from one of them.
std::wstring GameLauncherOf(const UninstallEntry& e);
