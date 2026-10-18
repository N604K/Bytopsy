// Self-check of the apps list and leftover finder: bytopsy-apps-test.exe
// Uses synthetic Uninstall entries and fixture folders in %TEMP% to check which entries are listed
// and which folders count as an uninstalled app's leftovers (and which never do). Never starts an
// uninstaller, never deletes anything outside its own fixture, never writes the registry.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "apps.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static void Put(const std::wstring& path, size_t bytes) {
    for (size_t i = path.find(L'\\', 3); i != std::wstring::npos; i = path.find(L'\\', i + 1)) CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        std::string s(bytes, 'x');
        fwrite(s.data(), 1, s.size(), f);
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

static UninstallEntry App(const wchar_t* name, const wchar_t* pub, const std::wstring& location, const wchar_t* key = nullptr) {
    UninstallEntry e;
    e.key = key ? key : name;
    e.displayName = name;
    e.publisher = pub;
    e.installLocation = location;
    e.uninstallString = L"MsiExec.exe /X{00000000-0000-0000-0000-000000000000}";
    return e;
}

static bool Has(const std::vector<std::wstring>& v, const std::wstring& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

static const AppLeftover* Find(const std::vector<AppLeftover>& v, const std::wstring& path) {
    for (auto& l : v)
        if (!_wcsicmp(l.path.c_str(), path.c_str())) return &l;
    return nullptr;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    std::wstring base = std::wstring(tmp, GetTempPathW(MAX_PATH, tmp)) + L"bytopsy-apps-test";
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    RemoveTree(base);

    // ---- Which entries are apps
    UninstallEntry app = App(L"7-Zip 23.01 (x64)", L"Igor Pavlov", L"C:\\Program Files\\7-Zip\\");
    Check(IsListedApp(app), L"listed: an app with a name and an uninstaller");
    UninstallEntry e = app;
    e.systemComponent = true;
    Check(!IsListedApp(e), L"not listed: SystemComponent = 1");
    e = app;
    e.parentKeyName = L"OfficeProduct";
    Check(!IsListedApp(e), L"not listed: an update of another entry (ParentKeyName)");
    e = app;
    e.releaseType = L"Security Update";
    Check(!IsListedApp(e), L"not listed: ReleaseType = Security Update");
    e = App(L"Security Update for Microsoft Office (KB5002700)", L"Microsoft Corporation", L"");
    Check(!IsListedApp(e), L"not listed: a patch by its name");
    e = App(L"Some hotfix", L"Microsoft Corporation", L"", L"KB5034441");
    Check(!IsListedApp(e), L"not listed: a KB key");
    e = app;
    e.uninstallString.clear();
    Check(!IsListedApp(e), L"not listed: no uninstaller");
    e = app;
    e.displayName.clear();
    Check(!IsListedApp(e), L"not listed: no name");

    // ---- Names a folder of the app can have
    auto v = AppNameVariants(app);
    Check(Has(v, L"7-zip 23.01 (x64)") && Has(v, L"7-zip"), L"names: full name and without version/architecture");
    v = AppNameVariants(App(L"Mozilla Thunderbird (x64 en-US)", L"Mozilla", L""));
    Check(Has(v, L"mozilla thunderbird"), L"names: language and architecture group dropped");
    v = AppNameVariants(App(L"Malwarebytes version 5.7.2.358", L"Malwarebytes", L""));
    Check(Has(v, L"malwarebytes"), L"names: \"version 5.7\" dropped");
    v = AppNameVariants(App(L"Microsoft .NET Runtime - 6.0.36 (x86)", L"Microsoft Corporation", L""));
    Check(Has(v, L"microsoft .net runtime"), L"names: \"- 6.0.36 (x86)\" dropped");
    v = AppNameVariants(App(L"VLC media player", L"VideoLAN", L"C:\\Program Files\\VideoLAN\\VLC\\"));
    Check(Has(v, L"vlc media player") && Has(v, L"vlc"), L"names: the install folder's name");
    v = AppNameVariants(App(L"Google Chrome", L"Google LLC", L"C:\\Program Files\\Google\\Chrome\\Application"));
    Check(!Has(v, L"application"), L"names: a generic install folder name (\"Application\") is not one");
    v = AppNameVariants(App(L"2024", L"X", L""));
    Check(v.empty(), L"names: a bare number is not a name");
    auto p = PublisherVariants(L"Mozilla Corporation");
    Check(Has(p, L"mozilla corporation") && Has(p, L"mozilla"), L"publisher: \"Corporation\" dropped");
    p = PublisherVariants(L"The qBittorrent project");
    Check(Has(p, L"qbittorrent"), L"publisher: \"The ... project\" dropped");
    Check(PublisherVariants(L"").empty(), L"publisher: none");

    // ---- Install date
    Check(util::FormatTime(ParseInstallDate(L"20240131")).substr(0, 10) == L"2024-01-31", L"install date: YYYYMMDD");
    Check(util::FormatTime(ParseInstallDate(L"1/31/2024")).substr(0, 10) == L"2024-01-31", L"install date: M/D/YYYY");
    Check(ParseInstallDate(L"20241399") == 0 && ParseInstallDate(L"soon") == 0 && ParseInstallDate(L"") == 0, L"install date: not a date");

    // ---- Install folder and uninstall command
    Check(AppInstallFolder(App(L"A", L"", L"\"C:\\Program Files\\A\\\"")) == L"C:\\Program Files\\A", L"folder: InstallLocation, quotes and \\ trimmed");
    e = App(L"FileZilla", L"", L"");
    e.uninstallString = L"\"C:\\Program Files\\FileZilla FTP Client\\uninstall.exe\"";
    Check(AppInstallFolder(e) == L"C:\\Program Files\\FileZilla FTP Client", L"folder: from its uninstaller");
    e.uninstallString = L"\"C:\\Program Files\\Mozilla Thunderbird\\uninstall\\helper.exe\"";
    Check(AppInstallFolder(e) == L"C:\\Program Files\\Mozilla Thunderbird", L"folder: an \"uninstall\" subfolder is not the app's folder");
    e.uninstallString = L"MsiExec.exe /X{1234}";
    Check(AppInstallFolder(e).empty(), L"folder: not from MsiExec");
    e.uninstallString = L"\"C:\\ProgramData\\Package Cache\\{8c97}\\dotnet-runtime.exe\" /uninstall";
    Check(AppInstallFolder(e).empty(), L"folder: not from a package cache");
    e.uninstallString = L"C:\\WINDOWS\\SysWOW64\\RunDll32.EXE \"C:\\Program Files\\NVIDIA Corporation\\Installer2\\InstallerCore\\NVI2.DLL\",UninstallPackage";
    Check(AppInstallFolder(e).empty(), L"folder: not from RunDll32");
    Check(AppInstallFolder(App(L"A", L"", L"D:\\")).empty(), L"folder: never a drive root");

    std::wstring exe, args;
    Check(SplitCommand(L"\"C:\\GOG Games\\X\\unins000.exe\" /SILENT", exe, args) && exe == L"C:\\GOG Games\\X\\unins000.exe" && args == L"/SILENT",
          L"command: quoted program");
    Check(SplitCommand(L"C:\\Program Files\\Game\\uninstall.exe --remove now", exe, args) && exe == L"C:\\Program Files\\Game\\uninstall.exe" &&
              args == L"--remove now",
          L"command: unquoted program with spaces");
    Check(SplitCommand(L"winget uninstall --product-code Gyan.FFmpeg", exe, args) && exe == L"winget" &&
              args == L"uninstall --product-code Gyan.FFmpeg",
          L"command: a program without .exe");
    Check(SplitCommand(L"C:\\Apps\\tool.exe", exe, args) && exe == L"C:\\Apps\\tool.exe" && args.empty(), L"command: no arguments");
    Check(IsUriCommand(L"steam://uninstall/730") && IsUriCommand(L"ms-settings:appsfeatures") && !IsUriCommand(L"C:\\x\\u.exe") &&
              !IsUriCommand(L"\"C:\\x\\u.exe\""),
          L"command: URIs");
    UninstallEntry ghost = App(L"Bytopsy test ghost", L"", L"", L"Bytopsy-test-{6B1D3C55-0000-4000-8000-000000000000}");
    Check(!UninstallEntryExists(ghost), L"an entry that isn't in the registry is not installed");
    Check(!UninstallKeyPath(ghost).empty(), L"key path");

    // ---- Leftovers
    LeftoverRoots roots;
    std::wstring PF = base + L"\\Program Files", PD = base + L"\\ProgramData", RO = base + L"\\Roaming", LO = base + L"\\Local",
                 LL = base + L"\\LocalLow";
    roots.roots = {PF, PD, RO, LO, LL};
    roots.never = roots.roots;
    roots.never.push_back(base);
    for (auto& r : roots.roots) CreateDirectoryW(base.c_str(), nullptr), CreateDirectoryW(r.c_str(), nullptr);

    Put(PF + L"\\Foo Software\\Foo Editor\\editor.exe", 1000);
    Put(PF + L"\\Foo Software\\Foo Editor\\res\\icons.dat", 500);
    Put(RO + L"\\Foo Editor\\settings.json", 100);
    Put(LO + L"\\Foo Software\\Foo Editor\\cache.bin", 200);
    Put(LO + L"\\Foo Software\\Bar Tool\\bar.ini", 10);
    Put(PD + L"\\Foo\\shared.dat", 10);
    Put(PD + L"\\Foo Software\\license.dat", 10);
    Put(LL + L"\\Foo Editor\\wallet.dat", 30);
    Put(RO + L"\\Unrelated\\x.txt", 10);

    UninstallEntry foo = App(L"Foo Editor 2.1 (x64)", L"Foo Software Inc.", PF + L"\\Foo Software\\Foo Editor\\");
    UninstallEntry bar = App(L"Bar Tool", L"Foo Software Inc.", L"");
    UninstallEntry other = App(L"Other App", L"Someone", L"");
    RefreshProtection();
    auto left = FindAppLeftovers(foo, {bar, other}, roots);
    for (auto& l : left) {
        std::string line = util::WideToUtf8(L"     " + l.path + L"  " + util::FormatSizeShort(l.bytes) + (l.preselect ? L"  [x]" : L"  [ ]") +
                                            L"  " + l.why + (l.protectNote.empty() ? L"" : L"  (" + l.protectNote + L")") + L"\n");
        fputs(line.c_str(), stdout);
    }
    const AppLeftover* inst = Find(left, PF + L"\\Foo Software\\Foo Editor");
    Check(inst && inst->preselect && inst->bytes == 1500 && inst->Deletable(), L"leftovers: the former install folder, preselected");
    const AppLeftover* settings = Find(left, RO + L"\\Foo Editor");
    Check(settings && !settings->preselect && settings->bytes == 100 && settings->Deletable(), L"leftovers: a folder named after the app, not preselected");
    const AppLeftover* cache = Find(left, LO + L"\\Foo Software\\Foo Editor");
    Check(cache && !cache->preselect, L"leftovers: publisher\\app folder");
    Check(!Find(left, LO + L"\\Foo Software"), L"leftovers: never a publisher folder that holds another app's folder");
    Check(!Find(left, LO + L"\\Foo Software\\Bar Tool"), L"leftovers: never another app's folder");
    Check(!Find(left, PD + L"\\Foo") && !Find(left, PD + L"\\Foo Software"), L"leftovers: never a folder named only after the publisher");
    const AppLeftover* wallet = Find(left, LL + L"\\Foo Editor");
    Check(wallet && !wallet->Deletable() && !wallet->preselect && wallet->level >= Protect::Important,
          L"leftovers: a folder with a protected item inside is shown, never deletable");
    Check(!Find(left, RO + L"\\Unrelated"), L"leftovers: unrelated folders are not listed");
    Check(left.size() == 4, L"leftovers: nothing else (" + std::to_wstring(left.size()) + L")");
    Check(!left.empty() && left[0].preselect, L"leftovers: the preselected install folder comes first");

    // The publisher's folder holding only this app is offered whole (and inherits the preselection).
    auto alone = FindAppLeftovers(foo, {other}, roots);
    const AppLeftover* pubDir = Find(alone, PF + L"\\Foo Software");
    Check(pubDir && pubDir->preselect && !Find(alone, PF + L"\\Foo Software\\Foo Editor"),
          L"leftovers: a publisher folder that holds only this app is offered whole");
    Check(!Find(alone, PD + L"\\Foo Software"), L"leftovers: a publisher folder holding other things is not");

    // Another installed app with the same name (another version, the 32-bit build): its folders are shared.
    UninstallEntry foo2 = App(L"Foo Editor 1.9", L"Foo Software Inc.", L"");
    auto shared = FindAppLeftovers(foo, {foo2, bar}, roots);
    Check(!Find(shared, RO + L"\\Foo Editor") && !Find(shared, LO + L"\\Foo Software\\Foo Editor"),
          L"leftovers: folders named after an app that is still installed (other version) are not offered");
    const AppLeftover* own = Find(shared, PF + L"\\Foo Software\\Foo Editor");
    Check(own && !own->preselect, L"leftovers: its own install folder still is, but not preselected");

    // A name another installed app begins with ("Python" while "Python Launcher" is installed).
    Put(LO + L"\\Python\\Launcher\\py.exe", 10);
    UninstallEntry py = App(L"Python 3.10.5 (64-bit)", L"Python Software Foundation", L"");
    UninstallEntry launcher = App(L"Python Launcher", L"Python Software Foundation", L"");
    Check(!Find(FindAppLeftovers(py, {launcher}, roots), LO + L"\\Python"), L"leftovers: never a folder another app's name starts with");
    Check(Find(FindAppLeftovers(py, {other}, roots), LO + L"\\Python") != nullptr, L"leftovers: ...which is offered once no app uses that name");

    // An install location that holds other apps (Adobe registers "C:\Program Files\Adobe").
    Put(PF + L"\\Adobe\\Adobe Photoshop 2025\\ps.exe", 10);
    Put(PF + L"\\Adobe\\Adobe Premiere Pro 2026\\pr.exe", 10);
    Put(RO + L"\\Adobe Premiere Pro 2026\\prefs.txt", 10);
    UninstallEntry premiere = App(L"Adobe Premiere Pro 2026", L"Adobe Inc.", PF + L"\\Adobe");
    UninstallEntry photoshop = App(L"Adobe Photoshop 2025", L"Adobe Inc.", PF + L"\\Adobe\\Adobe Photoshop 2025");
    auto adobe = FindAppLeftovers(premiere, {photoshop}, roots);
    Check(!Find(adobe, PF + L"\\Adobe"), L"leftovers: never an install location that holds another installed app");
    Check(Find(adobe, RO + L"\\Adobe Premiere Pro 2026") != nullptr, L"leftovers: its own named folder still is");

    // An install folder inside another installed app's folder (a plug-in).
    Put(PF + L"\\Host App\\plugins\\Plug X\\x.dll", 10);
    UninstallEntry plug = App(L"Plug X", L"Someone", PF + L"\\Host App\\plugins\\Plug X");
    UninstallEntry host = App(L"Host App", L"Someone", PF + L"\\Host App");
    Check(!Find(FindAppLeftovers(plug, {host}, roots), PF + L"\\Host App\\plugins\\Plug X"),
          L"leftovers: never a folder inside another installed app's folder");

    // Install locations that are a root, above the roots, or a protected system folder.
    Check(FindAppLeftovers(App(L"Bad Root", L"", PF), {}, roots).empty(), L"leftovers: an install location that is Program Files itself");
    Check(FindAppLeftovers(App(L"Bad Base", L"", base), {}, roots).empty(), L"leftovers: an install location above the searched folders");
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    auto sys = FindAppLeftovers(App(L"Bad System", L"", win), {}, roots);
    Check(sys.empty() || (!sys[0].Deletable() && !sys[0].preselect), L"leftovers: the Windows folder is never deletable");
    Check(FindAppLeftovers(App(L"Gone Missing", L"", PF + L"\\Gone Missing"), {}, roots).empty(), L"leftovers: folders that don't exist");

    // The real PC, read-only.
    auto entries = ReadUninstallEntries();
    size_t listed = std::count_if(entries.begin(), entries.end(), [](const UninstallEntry& x) { return IsListedApp(x); });
    printf("     this PC: %zu Uninstall entries, %zu listed apps\n", entries.size(), listed);
    LeftoverRoots pc = LeftoverRoots::ThisPC();
    Check(!pc.roots.empty() && pc.never.size() > pc.roots.size(), L"this PC: leftover roots and never-offered folders");
    if (GetEnvironmentVariableW(L"BYTOPSY_APPS", nullptr, 0))
        for (auto& a : FindInstalledApps()) {
            std::string line = util::WideToUtf8(L"     " + util::FormatSizeShort(a.bytes) + (a.measured ? L"*  " : L"   ") + a.e.displayName + L"  " +
                                                a.folder + L"\n");
            fputs(line.c_str(), stdout);
        }

    // Read-only dry run on this PC: what each installed app would leave to review if it were gone.
    if (GetEnvironmentVariableW(L"BYTOPSY_APPS_LEFTOVERS", nullptr, 0)) {
        RefreshProtection();
        for (auto& a : entries) {
            if (!IsListedApp(a)) continue;
            std::vector<UninstallEntry> others;
            for (auto& o : entries)
                if (&o != &a) others.push_back(o);
            for (auto& l : FindAppLeftovers(a, others, pc)) {
                std::string line = util::WideToUtf8(L"     " + a.displayName + L"  ->  " + l.path + L"  " + util::FormatSizeShort(l.bytes) +
                                                    (l.preselect ? L"  [x]  " : L"  [ ]  ") + l.why +
                                                    (l.protectNote.empty() ? L"" : L"  (" + std::wstring(ProtectLevelName(l.level)) + L": " + l.protectNote + L")") +
                                                    L"\n");
                fputs(line.c_str(), stdout);
            }
        }
    }

    RemoveTree(base);
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
