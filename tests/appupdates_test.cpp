// Self-check of app updates through winget: bytopsy-appupdates-test.exe
// Parses `winget upgrade` tables (real output, shortened names, unknown versions, pinned packages,
// nothing to update, localized headers, spinner and escape-sequence garbage, wide characters),
// matches them to installed apps, and runs a fake winget (.cmd in %TEMP%, via BYTOPSY_WINGET). Tells
// why winget is missing from injected inputs (BYTOPSY_APPINSTALLER) and runs Repair winget with a
// stand-in .cmd (BYTOPSY_WINGET_REPAIR), and checks bytopsy-cli --app-updates' advice. Never runs the
// real winget or Add-AppxPackage, never updates, registers or installs anything.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "appupdates.h"
#include "runtool.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

// A winget table: each column padded to its width, one space between (the last not padded).
static std::wstring Table(const std::vector<int>& widths, const std::vector<std::vector<std::wstring>>& rows, const wchar_t* eol = L"\r\n") {
    std::wstring out;
    int total = 0;
    for (int w : widths) total += w + 1;
    for (size_t r = 0; r < rows.size(); ++r) {
        std::wstring line;
        for (size_t c = 0; c < rows[r].size(); ++c) {
            line += rows[r][c];
            if (c + 1 < rows[r].size())
                for (int k = static_cast<int>(rows[r][c].size()); k < widths[c] + 1; ++k) line += L' ';
        }
        out += line + eol;
        if (r == 0) out += std::wstring(total - 1, L'-') + eol;
    }
    return out;
}

static const WingetUpdate* ById(const WingetList& l, const std::wstring& id) {
    for (auto& u : l.updates)
        if (u.id == id) return &u;
    return nullptr;
}

static InstalledApp Installed(const wchar_t* name, const wchar_t* version, const wchar_t* publisher) {
    InstalledApp a;
    a.e.displayName = name;
    a.e.displayVersion = version;
    a.e.publisher = publisher;
    return a;
}

static void WriteFile(const std::wstring& path, const std::string& bytes) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // ---- Real output (winget 1.29, redirected), lines kept as they were
    {
        std::wstring real =
            L"Name                                                         Id                                     Version              Available             Source\r\n"
            L"-----------------------------------------------------------------------------------------------------------------------------------------------------\r\n"
            L"App Installer                                                Microsoft.AppInstaller                 1.29.379.0           1.29.380              winget\r\n"
            L"Git                                                          Git.Git                                2.51.0.2             2.55.0.5              winget\r\n"
            L"Lens 2026.6.260931-latest                                    Mirantis.Lens                          2026.6.260931-latest 2026.10.121159-latest winget\r\n"
            L"Microsoft Visual C++ 2012 Redistributable (x64) - 11.0.51106 Microsoft.VCRedist.2012.x64            11.0.51106.1         11.0.61030.0          winget\r\n"
            L"NVIDIA Nsight Compute 2025.2.0                               Nvidia.Nsight.Compute                  < 2026.3.1           2026.3.1              winget\r\n"
            L"Node.js                                                      OpenJS.NodeJS.22                       22.20.0              22.23.2               winget\r\n"
            L"Visual Studio Community 2022                                 Microsoft.VisualStudio.2022.Community  17.14.37             17.14.42              winget\r\n"
            L"WireGuard                                                    WireGuard.WireGuard                    1.1                  1.1.1                 winget\r\n"
            L"32 upgrades available.\r\n";
        WingetList l = ParseWingetUpgrade(real);
        Check(l.ok && l.updates.size() == 8, L"real output: 8 rows, the footer isn't one");
        const WingetUpdate* lens = ById(l, L"Mirantis.Lens");
        Check(lens && lens->version == L"2026.6.260931-latest" && lens->available == L"2026.10.121159-latest" && lens->source == L"winget",
              L"real output: columns that touch (one space apart) split right");
        const WingetUpdate* vc = ById(l, L"Microsoft.VCRedist.2012.x64");
        Check(vc && vc->name == L"Microsoft Visual C++ 2012 Redistributable (x64) - 11.0.51106" && !vc->nameCut,
              L"real output: a name that fills its column");
        const WingetUpdate* ns = ById(l, L"Nvidia.Nsight.Compute");
        Check(ns && ns->version == L"< 2026.3.1" && ns->available == L"2026.3.1", L"real output: \"< version\" kept");
        Check(l.updates[0].name == L"App Installer" && !l.updates[0].explicitOnly && l.updates[0].Updatable(), L"real output: first row");
    }

    // ---- Shortened names and ids ("…"), LF line ends
    {
        std::wstring t = Table({34, 30, 14, 14},
                               {{L"Name", L"Id", L"Version", L"Available", L"Source"},
                                {L"Microsoft Visual C++ 2015-2022 Re…", L"Microsoft.VCRedist.2015+.x64", L"14.36.32532.0", L"14.38.33130.0", L"winget"},
                                {L"Microsoft Visual C++ 2015-2022 Re…", L"Microsoft.VCRedist.2015+.x86", L"14.36.32532.0", L"14.38.33130.0", L"winget"},
                                {L"Some Very Long Application Name W…", L"SomeVeryLongPublisherName.Ap…", L"1.0", L"2.0", L"winget"},
                                {L"7-Zip 22.01 (x64)", L"7zip.7zip", L"22.01", L"23.01", L"winget"}},
                               L"\n") +
                         L"4 upgrades available.\n";
        WingetList l = ParseWingetUpgrade(t);
        Check(l.ok && l.updates.size() == 4, L"cut names: 4 rows");
        Check(l.updates.size() == 4 && l.updates[0].nameCut && !l.updates[0].idCut && l.updates[0].Updatable(), L"cut name, whole id: updatable");
        Check(l.updates.size() == 4 && l.updates[2].idCut && !l.updates[2].Updatable(), L"cut id: not updatable");
        Check(l.updates.size() == 4 && l.updates[3].version == L"22.01" && l.updates[3].available == L"23.01", L"7-Zip row");

        std::vector<InstalledApp> apps = {Installed(L"Microsoft Visual C++ 2015-2022 Redistributable (x86) - 14.36.32532", L"14.36.32532.0", L"Microsoft Corporation"),
                                          Installed(L"Microsoft Visual C++ 2015-2022 Redistributable (x64) - 14.36.32532", L"14.36.32532.0", L"Microsoft Corporation"),
                                          Installed(L"7-Zip 22.01 (x64)", L"22.01", L"Igor Pavlov")};
        std::vector<int> m = MatchUpdatesToApps(l.updates, apps);
        Check(m.size() == 4 && m[0] == 1 && m[1] == 0, L"cut names: x64 and x86 told apart by the id");
        Check(m.size() == 4 && m[2] == -1 && m[3] == 2, L"cut names: no match for an unknown app; exact name matches");
    }

    // ---- --include-unknown: "Unknown" versions
    {
        std::wstring t = Table({20, 24, 10, 10},
                               {{L"Name", L"Id", L"Version", L"Available", L"Source"},
                                {L"Discord", L"Discord.Discord", L"Unknown", L"1.0.9170", L"winget"},
                                {L"Notepad++ (64-bit)", L"Notepad++.Notepad++", L"8.6", L"8.7.1", L"winget"}}) +
                         L"2 upgrades available.\r\n";
        WingetList l = ParseWingetUpgrade(t);
        const WingetUpdate* d = ById(l, L"Discord.Discord");
        Check(d && d->unknownVersion, L"unknown version flagged");
        Check(d && WingetUpgradeArgs(*d).find(L"--include-unknown") != std::wstring::npos, L"unknown version: upgrade passes --include-unknown");
        const WingetUpdate* n = ById(l, L"Notepad++.Notepad++");
        Check(n && !n->unknownVersion && n->Updatable(), L"id with + is fine");
    }

    // ---- Pinned: a second table, and the notes
    {
        std::wstring t = Table({16, 18, 8, 9},
                               {{L"Name", L"Id", L"Version", L"Available", L"Source"}, {L"Mozilla Firefox", L"Mozilla.Firefox", L"128.0", L"131.0.3", L"winget"}}) +
                         L"1 upgrades available.\r\n\r\n"
                         L"1 package(s) have version numbers that cannot be determined. Use --include-unknown to see all results.\r\n"
                         L"2 package(s) have pins that prevent upgrade. Use the 'winget pin' command to view and edit pins. Using the --include-pinned argument may show more results.\r\n"
                         L"The following packages have an upgrade available, but require explicit targeting for upgrade:\r\n" +
                         Table({14, 22, 8, 9}, {{L"Name", L"Id", L"Version", L"Available", L"Source"}, {L"PowerToys", L"Microsoft.PowerToys", L"0.84.1", L"0.85.1", L"winget"}});
        WingetList l = ParseWingetUpgrade(t);
        Check(l.updates.size() == 2, L"pinned: both tables read");
        const WingetUpdate* p = ById(l, L"Microsoft.PowerToys");
        const WingetUpdate* f = ById(l, L"Mozilla.Firefox");
        Check(p && p->explicitOnly && f && !f->explicitOnly, L"pinned: the second table's rows need explicit targeting");
        Check(l.unknownCount == 1 && l.pinnedCount == 2, L"pinned: unknown and pinned counts from the notes");
    }

    // ---- Nothing to update
    {
        WingetList l = ParseWingetUpgrade(L"   - \r   \\ \r   | \r                                                                                                                        \r"
                                          L"No installed package found matching input criteria.\r\n");
        Check(l.ok && l.updates.empty(), L"no updates: ok and empty");
        WingetList bad = ParseWingetUpgrade(L"Failed in attempting to update the source: winget\r\nAn unexpected error occurred while executing the command:\r\n0x8a15000f : Data required by the source is missing\r\n");
        Check(!bad.ok && bad.updates.empty(), L"source error: not ok");
    }

    // ---- Localized headers (German), and a label with a space in it
    {
        std::wstring de = Table({22, 24, 10, 10},
                                {{L"Name", L"ID", L"Version", L"Verfügbar", L"Quelle"},
                                 {L"Mozilla Thunderbird", L"Mozilla.Thunderbird.de", L"115.9.0", L"128.3.1", L"winget"},
                                 {L"Microsoft Teams", L"Microsoft.Teams", L"24193.1", L"24243.1", L"winget"}}) +
                          L"2 Aktualisierungen verfügbar.\r\n";
        WingetList l = ParseWingetUpgrade(de);
        const WingetUpdate* t = ById(l, L"Mozilla.Thunderbird.de");
        Check(l.ok && l.updates.size() == 2 && t && t->available == L"128.3.1" && t->source == L"winget", L"German headers");

        std::wstring es = L"Nombre        Id               Versión  Versión disponible Origen\r\n"
                          L"------------------------------------------------------------------\r\n"
                          L"Google Chrome Google.Chrome    129.0    130.0.6723.59      winget\r\n"
                          L"Zoom          Zoom.Zoom        6.1.0    6.2.3              winget\r\n";
        WingetList s = ParseWingetUpgrade(es);
        const WingetUpdate* c = ById(s, L"Google.Chrome");
        Check(s.updates.size() == 2 && c && c->version == L"129.0" && c->available == L"130.0.6723.59" && c->source == L"winget",
              L"a header label with a space is one column");
    }

    // ---- Console garbage: spinner, progress bar, escape sequences, backspaces, a BOM
    {
        std::wstring t = L"\xFEFF   - \r   \\ \r   | \r   / \r"
                         L"  \x2588\x2588\x2588\x2588\x2588\x2588\x2592\x2592\x2592\x2592  1.00 MB / 2.00 MB\r"
                         L"                                                  \r"
                         L"\x1B[2K\x1B[0G\x1B]0;winget\x07" +
                         Table({10, 12, 8, 9}, {{L"Name", L"Id", L"Version", L"Available", L"Source"},
                                                {L"Git", L"Git.Git", L"2.41.0", L"2.42.0.2", L"winget"},
                                                {L"curl", L"cURL.cURL", L"8.4.0", L"8.5.0", L"winget"}}) +
                         L"x\bx\b2 upgrades available.\r\n";
        std::vector<std::wstring> lines = ConsoleLines(L"ab\r\ncd\refg\bh\x1B[31mi\n");
        Check(lines.size() == 2 && lines[0] == L"ab" && lines[1] == L"efhi", L"console lines: CRLF, bare CR, backspace, escapes");
        WingetList l = ParseWingetUpgrade(t);
        Check(l.ok && l.updates.size() == 2 && ById(l, L"Git.Git") && ById(l, L"Git.Git")->available == L"2.42.0.2" && ById(l, L"cURL.cURL"),
              L"spinner and escape sequences before the table");
    }

    // ---- Wide characters take two cells
    {
        // "微信" is 4 cells; the Name column is 10 cells wide.
        // Columns at cells 0, 11, 26, 34, 44: "微信" (4 cells) + 7 spaces reaches the Id column.
        std::wstring t = L"Name       Id             Version Available Source\r\n"
                         L"--------------------------------------------------\r\n"
                         L"\x5FAE\x4FE1       Tencent.WeChat 3.9.10  3.9.12    winget\r\n"
                         L"Foo        Foo.Foo        1.0     1.1       winget\r\n";
        WingetList l = ParseWingetUpgrade(t);
        const WingetUpdate* w = ById(l, L"Tencent.WeChat");
        Check(w && w->name == L"\x5FAE\x4FE1" && w->version == L"3.9.10", L"wide characters counted as two cells");
    }

    // ---- Matching real names to installed apps
    {
        std::vector<WingetUpdate> ups(7);
        ups[0].name = L"Git", ups[0].id = L"Git.Git", ups[0].version = L"2.51.0.2";
        ups[1].name = L"Microsoft .NET Runtime - 8.0.21 (x64)", ups[1].id = L"Microsoft.DotNet.Runtime.8", ups[1].version = L"8.0.21";
        ups[2].name = L"Python 3.10.5 (64-bit)", ups[2].id = L"Python.Python.3.10", ups[2].version = L"3.10.5";
        ups[3].name = L"App Installer", ups[3].id = L"Microsoft.AppInstaller", ups[3].version = L"1.29.379.0";
        ups[4].name = L"Unity 6000.0.58f1", ups[4].id = L"Unity.Unity.6000", ups[4].version = L"6000.0.58f1";
        ups[5].name = L"WireGuard VPN client", ups[5].id = L"WireGuard.WireGuard", ups[5].version = L"1.1";  // a name that differs: by id + publisher
        ups[6].name = L"git", ups[6].id = L"Other.Git", ups[6].version = L"1.0";         // same name, but Git is taken
        std::vector<InstalledApp> apps = {Installed(L"Unity 6000.4.0f1", L"", L"Unity Technologies ApS"),
                                          Installed(L"Unity 6000.0.58f1", L"", L"Unity Technologies ApS"),
                                          Installed(L"Git", L"2.51.0.2", L"The Git Development Community"),
                                          Installed(L"Microsoft .NET Runtime - 8.0.21 (x64)", L"8.0.21.35325", L"Microsoft Corporation"),
                                          Installed(L"Python  3.10.5 (64-bit)", L"3.10.5150.0", L"Python Software Foundation"),
                                          Installed(L"Wire Guard", L"0.5", L"WireGuard LLC")};
        std::vector<int> m = MatchUpdatesToApps(ups, apps);
        Check(m[0] == 2 && m[1] == 3 && m[2] == 4, L"match: exact names (case, spacing), versions written differently");
        Check(m[3] == -1, L"match: an MSIX app stays unmatched");
        Check(m[4] == 1, L"match: the right Unity of two");
        Check(m[5] == 5, L"match: id and publisher when the name differs");
        Check(m[6] == -1, L"match: one app is matched once (the better version wins)");
    }

    // ---- Ids on a command line, exit codes
    {
        Check(ValidWingetId(L"Microsoft.VCRedist.2015+.x64") && ValidWingetId(L"9NBLGGH4NNS1"), L"valid ids");
        Check(!ValidWingetId(L"Foo.Bar & calc") && !ValidWingetId(L"Foo\"Bar") && !ValidWingetId(L"Foo.Ba…") && !ValidWingetId(L""),
              L"ids with spaces, quotes, shell characters or cut are refused");
        WingetUpdate u;
        u.id = L"Git.Git";
        u.source = L"winget";
        Check(WingetUpgradeArgs(u) == L"upgrade --id Git.Git --exact --source winget --disable-interactivity --accept-package-agreements --accept-source-agreements",
              L"upgrade arguments");
        Check(WingetCodeText(0) == L"Updated" && !WingetCodeText(static_cast<int>(0x8A15002Bu)).empty() && WingetCodeText(12345).empty(), L"exit codes described");
    }

    // ---- A fake winget (.cmd): FindWinget, listing, an update's exit code
    {
        wchar_t tmp[MAX_PATH];
        std::wstring dir = std::wstring(tmp, GetTempPathW(MAX_PATH, tmp)) + L"bytopsy-appupdates-test";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring fixture = dir + L"\\upgrade.txt", cmd = dir + L"\\winget.cmd";
        std::wstring table = Table({12, 20, 8, 9}, {{L"Name", L"Id", L"Version", L"Available", L"Source"},
                                                    {L"Café App", L"Cafe.App", L"1.0", L"1.2", L"winget"},
                                                    {L"Other", L"Other.Thing", L"3.0", L"3.1", L"msstore"}}) +
                             L"2 upgrades available.\r\n";
        WriteFile(fixture, util::WideToUtf8(table));
        // (exit /b inside a parenthesized block loses its code: plain lines and a label instead)
        std::string script = "@echo off\r\nif not \"%2\"==\"--id\" goto list\r\nif \"%3\"==\"Cafe.App\" exit /b " +
                             std::to_string(static_cast<int>(0x8A150109u)) +
                             "\r\necho Installer failed with exit code: 1603\r\nexit /b 1603\r\n:list\r\ntype \"" + util::WideToUtf8(fixture) + "\"\r\n";
        WriteFile(cmd, script);
        SetEnvironmentVariableW(L"BYTOPSY_WINGET", cmd.c_str());
        Check(_wcsicmp(FindWinget().c_str(), cmd.c_str()) == 0, L"BYTOPSY_WINGET is used");
        WingetList l = ListWingetUpdates(FindWinget());
        Check(l.ok && l.updates.size() == 2 && l.updates[0].name == L"Café App" && l.updates[1].source == L"msstore", L"fake winget listed (UTF-8)");
        if (l.updates.size() == 2) {
            WingetUpgradeResult a = RunWingetUpgrade(cmd, l.updates[0]);
            Check(a.ok && a.restart, L"fake update: restart needed to finish (code " + std::to_wstring(a.code) + L")");
            WingetUpgradeResult b = RunWingetUpgrade(cmd, l.updates[1]);
            Check(!b.ok && b.code == 1603 && b.message.find(L"1603") != std::wstring::npos, L"fake update: failure with winget's last line");
        }
        WingetUpdate cut;
        cut.id = L"Foo.Ba…";
        cut.idCut = true;
        WingetUpgradeResult c = RunWingetUpgrade(cmd, cut);
        Check(!c.ok && c.code == -1, L"a cut id is never run");
        SetEnvironmentVariableW(L"BYTOPSY_WINGET", (dir + L"\\missing.cmd").c_str());
        Check(FindWinget().empty(), L"missing winget: empty path");
        SetEnvironmentVariableW(L"BYTOPSY_WINGET", nullptr);
        DeleteFileW(fixture.c_str());
        DeleteFileW(cmd.c_str());
        RemoveDirectoryW(dir.c_str());
    }

    // ---- winget missing: the diagnosis from injected inputs (the real system is not read)
    {
        const std::wstring app = L"Microsoft.DesktopAppInstaller_1.29.379.0_x64__8wekyb3d8bbwe",
                           bundle = L"Microsoft.DesktopAppInstaller_2026.917.151.0_neutral_~_8wekyb3d8bbwe";
        Check(IsAppInstallerPackage(app) && IsAppInstallerPackage(bundle) && IsAppInstallerPackage(L"microsoft.desktopappinstaller_1.0.0.0_arm64__8WEKYB3D8BBWE"),
              L"App Installer package names");
        Check(!IsAppInstallerPackage(L"Microsoft.WindowsStore_22409.1401.5.0_x64__8wekyb3d8bbwe") &&
                  !IsAppInstallerPackage(L"Microsoft.DesktopAppInstaller_1.0_x64__otherpublisher") && !IsAppInstallerPackage(L"Microsoft.DesktopAppInstaller_8wekyb3d8bbwe"),
              L"other packages aren't App Installer");

        WingetProbe p;
        p.winget = L"C:\\x\\winget.exe";
        p.userPackages = {app};
        WingetDiagnosis d = DiagnoseWinget(p);
        Check(d.state == WingetState::Found && d.winget == p.winget && !d.CanRepair(), L"winget found");
        p.winget.clear();
        p.packageAlias = true;
        d = DiagnoseWinget(p);
        Check(d.state == WingetState::AliasOff && d.package == app && d.CanRepair(), L"registered, package link there: the alias is off");
        p.packageAlias = false;
        d = DiagnoseWinget(p);
        Check(d.state == WingetState::NotReachable && d.CanRepair(), L"registered, no winget.exe: not reachable");
        p.userPackages.clear();
        p.machinePackages = {bundle, app};
        d = DiagnoseWinget(p);
        Check(d.state == WingetState::NotRegistered && d.package == app && d.CanRepair(), L"on this PC, not for this user (the app package named, not the bundle)");
        p.machinePackages.clear();
        d = DiagnoseWinget(p);
        Check(d.state == WingetState::NotInstalled && d.package.empty() && !d.CanRepair(), L"not on this PC");

        // What the CLI prints.
        const std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -Command \"Add-AppxPackage -RegisterByFamilyName -MainPackage "
                                 L"Microsoft.DesktopAppInstaller_8wekyb3d8bbwe\"";
        Check(L"powershell.exe " + WingetRepairArgs() == cmd, L"the repair command");
        std::wstring t = WingetMissingText(d);
        Check(t.find(L"isn't on this PC") != std::wstring::npos && t.find(kAppInstallerStoreLink) != std::wstring::npos &&
                  t.find(L"https://github.com/microsoft/winget-cli/releases/latest") != std::wstring::npos && t.find(L"Add-AppxPackage") == std::wstring::npos,
              L"not installed: the Store and Microsoft's release page, no repair");
        p.machinePackages = {app};
        t = WingetMissingText(DiagnoseWinget(p));
        Check(t.find(L"isn't registered for your account") != std::wstring::npos && t.find(cmd) != std::wstring::npos && t.find(app) != std::wstring::npos,
              L"not registered: the exact PowerShell command");
        p.userPackages = {app};
        t = WingetMissingText(DiagnoseWinget(p));
        Check(t.find(cmd) != std::wstring::npos && t.find(L"ms-settings:advanced-apps") != std::wstring::npos, L"not reachable: repair, then the alias");
        p.packageAlias = true;
        t = WingetMissingText(DiagnoseWinget(p));
        Check(t.find(L"alias is turned off") != std::wstring::npos && t.find(L"ms-settings:advanced-apps") < t.find(cmd), L"alias off: Settings first, then repair");
    }

    // ---- ProbeWinget and RepairWinget with stand-ins (BYTOPSY_APPINSTALLER, BYTOPSY_WINGET_REPAIR):
    // the real App Installer is never registered.
    {
        wchar_t tmp[MAX_PATH];
        std::wstring dir = std::wstring(tmp, GetTempPathW(MAX_PATH, tmp)) + L"bytopsy-wingetrepair-test";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring winget = dir + L"\\winget.cmd", fail = dir + L"\\fail.cmd", noop = dir + L"\\noop.cmd", fix = dir + L"\\fix.cmd",
                     journal = dir + L"\\journal.log";
        DeleteFileW(winget.c_str());
        SetEnvironmentVariableW(L"BYTOPSY_JOURNAL", journal.c_str());
        SetEnvironmentVariableW(L"BYTOPSY_WINGET", winget.c_str());  // not there yet
        for (auto [env, want] : std::initializer_list<std::pair<const wchar_t*, WingetState>>{{L"user", WingetState::NotReachable}, {L"alias", WingetState::AliasOff},
                                 {L"pc", WingetState::NotRegistered}, {L"none", WingetState::NotInstalled}}) {
            SetEnvironmentVariableW(L"BYTOPSY_APPINSTALLER", env);
            Check(DiagnoseWinget().state == want, std::wstring(L"BYTOPSY_APPINSTALLER=") + env);
        }

        // Simulated, no stand-in: nothing runs.
        SetEnvironmentVariableW(L"BYTOPSY_APPINSTALLER", L"user");
        SetEnvironmentVariableW(L"BYTOPSY_WINGET_REPAIR", nullptr);
        WingetRepairResult r = RepairWinget();
        Check(r.outcome == WingetRepairOutcome::NotRun && r.code == -1, L"simulated winget: the real repair is never run");

        WriteFile(fail, std::string("@echo off\r\necho.\r\necho Add-AppxPackage : Deployment failed with HRESULT: 0x80073CF9 1>&2\r\necho second line\r\nexit /b 1\r\n"));
        SetEnvironmentVariableW(L"BYTOPSY_WINGET_REPAIR", fail.c_str());
        r = RepairWinget();
        Check(r.outcome == WingetRepairOutcome::Failed && r.code == 1 && r.detail.find(L"0x80073CF9") != std::wstring::npos,
              L"repair failed: PowerShell's first line (" + r.detail + L")");
        std::wstring note = WingetRepairJournalNote(r);
        Check(note.find(L"Add-AppxPackage -RegisterByFamilyName") != std::wstring::npos && note.find(L"failed: ") != std::wstring::npos &&
                  note.find(L"exit code 1") != std::wstring::npos,
              L"journal note: the command and the error");

        WriteFile(noop, std::string("@echo off\r\nexit /b 0\r\n"));
        SetEnvironmentVariableW(L"BYTOPSY_WINGET_REPAIR", noop.c_str());
        r = RepairWinget();
        Check(r.outcome == WingetRepairOutcome::StillAlias && r.detail.empty(), L"registered but still no winget: the alias is what's left");
        SetEnvironmentVariableW(L"BYTOPSY_APPINSTALLER", L"pc");
        r = RepairWinget();
        Check(r.outcome == WingetRepairOutcome::StillMissing, L"succeeded but still not registered");

        // The stand-in "repair" brings winget (a .cmd) back.
        WriteFile(fix, "@echo off\r\n> \"" + util::WideToUtf8(winget) + "\" echo @echo off\r\nexit /b 0\r\n");
        SetEnvironmentVariableW(L"BYTOPSY_WINGET_REPAIR", fix.c_str());
        r = RepairWinget();
        Check(r.outcome == WingetRepairOutcome::Fixed && r.after.state == WingetState::Found && _wcsicmp(r.after.winget.c_str(), winget.c_str()) == 0,
              L"repaired: winget found again");
        Check(WingetRepairJournalNote(r).find(L"winget found again") != std::wstring::npos, L"journal note: fixed");
        DeleteFileW(winget.c_str());
        SetEnvironmentVariableW(L"BYTOPSY_WINGET_REPAIR", (dir + L"\\missing.cmd").c_str());
        r = RepairWinget();
        Check(r.outcome == WingetRepairOutcome::NoPowerShell, L"a missing stand-in runs nothing else");

        // bytopsy-cli --app-updates prints the diagnosis (it runs nothing: winget is missing).
        wchar_t self[MAX_PATH];
        std::wstring exe(self, GetModuleFileNameW(nullptr, self, MAX_PATH));
        std::wstring cli = exe.substr(0, exe.rfind(L'\\')) + L"\\bytopsy-cli.exe";
        if (GetFileAttributesW(cli.c_str()) != INVALID_FILE_ATTRIBUTES) {
            SetEnvironmentVariableW(L"BYTOPSY_WINGET_REPAIR", nullptr);
            std::string out;
            SetEnvironmentVariableW(L"BYTOPSY_APPINSTALLER", L"pc");
            int code = RunTool(cli, L"--app-updates", out, 60000);
            std::wstring o = DecodeToolOutput(out);
            Check(code == 1 && o.find(L"Add-AppxPackage -RegisterByFamilyName -MainPackage Microsoft.DesktopAppInstaller_8wekyb3d8bbwe") != std::wstring::npos &&
                      o.find(L"Repair winget") != std::wstring::npos && o.find(L"Nothing was changed") != std::wstring::npos,
                  L"cli --app-updates: not registered, the PowerShell command");
            SetEnvironmentVariableW(L"BYTOPSY_APPINSTALLER", L"none");
            code = RunTool(cli, L"--app-updates", out = {}, 60000);
            o = DecodeToolOutput(out);
            Check(code == 1 && o.find(L"https://github.com/microsoft/winget-cli/releases/latest") != std::wstring::npos &&
                      o.find(L"ms-windows-store://") != std::wstring::npos && o.find(L"Add-AppxPackage") == std::wstring::npos,
                  L"cli --app-updates: not installed, the two links");
        } else {
            Check(false, L"bytopsy-cli.exe next to this test");
        }
        for (const wchar_t* v : {L"BYTOPSY_APPINSTALLER", L"BYTOPSY_WINGET", L"BYTOPSY_WINGET_REPAIR", L"BYTOPSY_JOURNAL"}) SetEnvironmentVariableW(v, nullptr);
        for (auto& f : {winget, fail, noop, fix, journal}) DeleteFileW(f.c_str());
        RemoveDirectoryW(dir.c_str());
    }

    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
