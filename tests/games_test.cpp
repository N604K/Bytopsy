// Self-check of the games list: winncdu-games-test.exe
// Builds a fake Steam library and Epic manifests in %TEMP% and checks what is listed as a game,
// what as a leftover, and how uninstalls are routed. Never starts an uninstall.
#include <windows.h>

#include <cstdio>
#include <string>

#include "games.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static void Put(const std::wstring& path, const std::string& text) {
    for (size_t i = path.find(L'\\', 3); i != std::wstring::npos; i = path.find(L'\\', i + 1)) CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

static void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveTree(dir + L"\\" + n);
            else
                DeleteFileW((dir + L"\\" + n).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static const Game* Find(const std::vector<Game>& v, const std::wstring& name) {
    for (auto& g : v)
        if (g.name == name) return &g;
    return nullptr;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    std::wstring base = std::wstring(tmp, GetTempPathW(MAX_PATH, tmp)) + L"winncdu-games-test";
    RemoveTree(base);

    // ---- KeyValues
    Vdf v = ParseVdf(L"// comment\n\"libraryfolders\"\n{\n  \"0\"\n  {\n    \"path\"  \"D:\\\\Games\\\\Steam\"\n"
                     L"    \"apps\" { \"730\" \"123\" }\n  }\n  bare value\n}\n");
    auto lf = v.children.find(L"libraryfolders");
    Check(lf != v.children.end() && lf->second.children.count(L"0") && lf->second.children.at(L"0").Get(L"path") == L"D:\\Games\\Steam" &&
              lf->second.children.at(L"0").children.at(L"apps").Get(L"730") == L"123" && lf->second.Get(L"bare") == L"value",
          L"VDF: nesting, escapes, comments, bare words");
    Check(ParseVdf(L"\"a\" { \"b\" \"c\"").children.at(L"a").Get(L"b") == L"c", L"VDF: a cut-off file still gives what it has");

    // ---- Steam library
    std::wstring sa = base + L"\\steamapps";
    Put(sa + L"\\appmanifest_730.acf",
        "\"AppState\"\n{\n\t\"appid\"\t\t\"730\"\n\t\"name\"\t\t\"Counter-Strike 2\"\n\t\"installdir\"\t\t\"Counter-Strike Global Offensive\"\n"
        "\t\"LastPlayed\"\t\t\"1700000000\"\n\t\"SizeOnDisk\"\t\t\"35000000000\"\n}\n");
    Put(sa + L"\\appmanifest_228980.acf",
        "\"AppState\" { \"appid\" \"228980\" \"name\" \"Steamworks Common Redistributables\" \"installdir\" \"Steamworks Shared\" \"SizeOnDisk\" \"1\" }");
    Put(sa + L"\\appmanifest_1493710.acf",
        "\"AppState\" { \"appid\" \"1493710\" \"name\" \"Proton Experimental\" \"installdir\" \"Proton - Experimental\" }");
    Put(sa + L"\\common\\Counter-Strike Global Offensive\\cs2.exe", std::string(100, 'x'));
    Put(sa + L"\\common\\Steamworks Shared\\redist.exe", "x");
    Put(sa + L"\\common\\Proton - Experimental\\proton", "x");
    Put(sa + L"\\common\\Old Game\\data.pak", std::string(5000, 'x'));
    Put(sa + L"\\workshop\\content\\730\\item.bin", "x");
    Put(sa + L"\\workshop\\content\\440\\map.bsp", std::string(300, 'x'));
    Put(sa + L"\\shadercache\\730\\cache.bin", "x");
    Put(sa + L"\\shadercache\\2073850\\cache.bin", std::string(200, 'x'));
    std::vector<Game> games;
    ScanSteamLibrary(sa, games);
    const Game* cs = Find(games, L"Counter-Strike 2");
    Check(cs && !cs->leftover && cs->id == L"730" && cs->bytes == 35000000000ull && cs->uninstall == L"steam://uninstall/730" &&
              cs->lastPlayed == util::UnixToFileTime(1700000000) && cs->folder == sa + L"\\common\\Counter-Strike Global Offensive",
          L"Steam: a game with its size, last played and uninstall URI");
    Check(!Find(games, L"Steamworks Common Redistributables") && !Find(games, L"Proton Experimental"),
          L"Steam: runtimes other games need are not listed");
    const Game* old = Find(games, L"Old Game");
    Check(old && old->leftover && old->bytes == 5000, L"Steam: a common folder no game owns is a leftover");
    Check(!Find(games, L"Steamworks Shared") && !Find(games, L"Proton - Experimental"), L"Steam: runtime folders are not leftovers");
    const Game* ws = Find(games, L"Workshop items of an uninstalled game (app 440)");
    const Game* sc = Find(games, L"Shader cache of an uninstalled game (app 2073850)");
    Check(ws && ws->leftover && ws->bytes == 300 && sc && sc->leftover && sc->bytes == 200,
          L"Steam: workshop items and shader caches of uninstalled games are leftovers");
    Check(!Find(games, L"Workshop items of an uninstalled game (app 730)") && !Find(games, L"Shader cache of an uninstalled game (app 730)"),
          L"Steam: an installed game's workshop items and shader cache stay");
    Check(games.size() == 4, L"Steam: nothing else listed (" + std::to_wstring(games.size()) + L")");

    // ---- Epic
    std::wstring em = base + L"\\Manifests";
    Put(em + L"\\A1.item",
        "{\"DisplayName\": \"Fortnite\", \"InstallLocation\": \"C:\\\\Program Files\\\\Epic Games\\\\Fortnite\", \"AppName\": \"Fortnite\", "
        "\"InstallSize\": 41000000000, \"bIsApplication\": true}");
    Put(em + L"\\B2.item", "{\"DisplayName\": \"Some Plugin\", \"InstallLocation\": \"C:\\\\X\", \"AppName\": \"Plug\", \"bIsApplication\": false}");
    std::vector<Game> epic;
    ScanEpicManifests(em, epic);
    Check(epic.size() == 1 && epic[0].name == L"Fortnite" && epic[0].folder == L"C:\\Program Files\\Epic Games\\Fortnite" &&
              epic[0].bytes == 41000000000ull && epic[0].launcher == L"Epic Games",
          L"Epic: games from the launcher's manifests, plugins left out");

    // ---- Uninstall entries of other launchers
    auto launcher = [](const wchar_t* name, const wchar_t* pub, const wchar_t* cmd) {
        UninstallEntry e;
        e.displayName = name;
        e.publisher = pub;
        e.uninstallString = cmd;
        return GameLauncherOf(e);
    };
    Check(launcher(L"Assassin's Creed", L"Ubisoft", L"uplay://uninstall/720") == L"Ubisoft Connect", L"Ubisoft by its uninstall URI");
    Check(launcher(L"The Witcher 3", L"GOG.com", L"\"C:\\GOG Games\\The Witcher 3\\unins000.exe\"") == L"GOG", L"GOG by publisher");
    Check(launcher(L"Diablo IV", L"Blizzard Entertainment",
                   L"\"C:\\ProgramData\\Battle.net\\Agent\\Blizzard Uninstaller.exe\" --lang=enUS --uid=fenris") == L"Battle.net",
          L"Battle.net games by their uninstaller");
    Check(launcher(L"Battle.net", L"Blizzard Entertainment", L"\"C:\\Program Files (x86)\\Battle.net\\Battle.net Uninstaller.exe\"") == L"",
          L"the Battle.net app itself is not a game");
    Check(launcher(L"EA SPORTS FC 25", L"Electronic Arts",
                   L"\"C:\\Program Files\\Common Files\\EAInstaller\\EA SPORTS FC 25\\Cleanup.exe\" uninstall_game") == L"EA app",
          L"EA app games by their installer");
    Check(launcher(L"7-Zip", L"Igor Pavlov", L"\"C:\\Program Files\\7-Zip\\Uninstall.exe\"") == L"", L"other programs are not games");

    std::wstring exe, args;
    Check(SplitCommand(L"\"C:\\GOG Games\\X\\unins000.exe\" /SILENT", exe, args) && exe == L"C:\\GOG Games\\X\\unins000.exe" && args == L"/SILENT",
          L"uninstall command: quoted program");
    Check(SplitCommand(L"C:\\Program Files\\Game\\uninstall.exe --remove now", exe, args) && exe == L"C:\\Program Files\\Game\\uninstall.exe" &&
              args == L"--remove now",
          L"uninstall command: unquoted program with spaces");
    Check(SplitCommand(L"MsiExec.exe /X{1234}", exe, args) && exe == L"MsiExec.exe" && args == L"/X{1234}", L"uninstall command: MsiExec");

    std::wstring err;
    Game left;
    left.leftover = true;
    left.name = L"Old Game";
    Check(!StartUninstall(left, err) && !err.empty(), L"leftovers have no uninstaller to start");

    if (GetEnvironmentVariableW(L"WINNCDU_GAMES", nullptr, 0))
        for (auto& g : FindGames()) {
            std::string line = util::WideToUtf8(L"     " + g.launcher + L"  " + g.name + L"  " + util::FormatSizeShort(g.bytes) +
                                                (g.leftover ? L"  (leftover)" : L"") + L"  " + g.folder + L"\n");
            fputs(line.c_str(), stdout);
        }

    RemoveTree(base);
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
