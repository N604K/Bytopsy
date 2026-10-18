// Self-check of Cleanup's preview semantics: winncdu-cleanup-test.exe
// Exactly the measured entries are deleted, minus the kept ones; entries that changed since they
// were measured are left alone. Works in %TEMP% only; Recycle Bin items it creates are its own.
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "fsops.h"
#include "gui/cleanup.h"
#include "journal.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static void Put(const std::wstring& path, const char* text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        fputs(text, f);
        fclose(f);
    }
}

static void Age(const std::wstring& path, int hours) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    int64_t t = util::NowFileTime() - static_cast<int64_t>(hours) * 36000000000LL;
    FILETIME ft{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

static CleanEntry* Find(CleanCategory& c, const std::wstring& path) {
    for (auto& e : c.entries)
        if (_wcsicmp(e.path.c_str(), path.c_str()) == 0) return &e;
    return nullptr;
}

int main() {
    // With long names: on GitHub's runners %TEMP% is C:\Users\RUNNER~1\..., and the Recycle Bin
    // reports long paths.
    wchar_t b[MAX_PATH], lb[MAX_PATH];
    DWORD tn = GetTempPathW(MAX_PATH, b), ln = tn ? GetLongPathNameW(b, lb, MAX_PATH) : 0;
    std::wstring tmp = ln && ln < MAX_PATH ? std::wstring(lb, ln) : std::wstring(b, tn);
    std::wstring pid = std::to_wstring(GetCurrentProcessId());
    std::wstring dir = tmp + L"winncdu-cleanup-test-" + pid;
    std::wstring journal = dir + L"-journal.log";  // restores are journaled; keep yours untouched
    SetEnvironmentVariableW(L"WINNCDU_JOURNAL", journal.c_str());
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\old-folder").c_str(), nullptr);
    Put(dir + L"\\old-folder\\a.tmp", "aaaa");
    Put(dir + L"\\old.tmp", "old");
    Put(dir + L"\\keep-me.tmp", "keep");
    Put(dir + L"\\changes-later.tmp", "x");
    Put(dir + L"\\new.tmp", "new");
    for (const wchar_t* f : {L"\\old-folder\\a.tmp", L"\\old-folder", L"\\old.tmp", L"\\keep-me.tmp", L"\\changes-later.tmp"})
        Age(dir + f, 48);

    CleanCategory c;
    c.name = L"Test";
    c.targets = {{dir, L"", 24}};
    std::atomic<bool> cancel{false};
    MeasureCategory(c, cancel);
    Check(c.entries.size() == 4, L"4 entries old enough (" + std::to_wstring(c.entries.size()) + L")");
    Check(!Find(c, dir + L"\\new.tmp"), L"a new file is not listed");
    if (CleanEntry* k = Find(c, dir + L"\\keep-me.tmp")) k->keep = true;
    Check(c.KeptCount() == 1 && c.ToClean() == c.bytes - 4, L"kept bytes are not counted");
    Age(dir + L"\\changes-later.tmp", 0);  // touched after the preview

    CleanResult r = CleanCategoryNow(c, cancel, [](const std::wstring&) {});
    Check(!PathExists(dir + L"\\old.tmp") && !PathExists(dir + L"\\old-folder"), L"listed entries deleted");
    Check(PathExists(dir + L"\\keep-me.tmp"), L"kept entry survives");
    Check(PathExists(dir + L"\\changes-later.tmp"), L"entry that changed since the preview survives");
    Check(PathExists(dir + L"\\new.tmp"), L"unlisted entry survives");
    Check(r.freed == 7 && r.files == 2 && r.failed == 0, L"result counts (freed " + std::to_wstring(r.freed) + L")");

    // ---- The shipped rule file (res/cleaners.ini) parses cleanly
    {
        FILE* f = nullptr;
        std::string data;
        if (_wfopen_s(&f, WINNCDU_SOURCE_DIR L"/res/cleaners.ini", L"rb") == 0 && f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
            fclose(f);
        }
        std::vector<std::wstring> errs;
        auto cats = ParseCleaners(util::Utf8ToWide(data), errs);
        Check(errs.empty(), L"res/cleaners.ini has no errors" + (errs.empty() ? L"" : L": " + errs[0]));
        std::wstring ids;
        for (auto& k : cats) ids += k.id + L" ";
        Check(ids == L"recycle temp wintemp browser thumbs crash wu do shader launchers dev jvm conda conan winold winbt ", L"all categories, in order: " + ids);
        if (cats.size() == 16) {
            Check(cats[0].recycleBin && cats[14].external && cats[14].needsAdmin && cats[15].external && cats[15].targets.size() == 2,
                  L"kinds and admin flags");
            Check(cats[7].command.find(L"Delete-DeliveryOptimizationCache") != std::wstring::npos && cats[7].needsAdmin,
                  L"Delivery Optimization is emptied through its own command");
            Check(cats[13].command == L"conan remove \"*\" -c" && cats[12].targets[0].filePattern == L"*.conda;*.tar.bz2",
                  L"conan runs its own command; conda takes only archives");
            Check(cats[1].targets.size() == 1 && cats[1].targets[0].path == L"%TEMP%" && cats[1].targets[0].minAgeHours == 24,
                  L"temp: path and age");
            Check(cats[4].targets[0].filePattern == L"thumbcache_*.db", L"thumbs: file pattern");
            Check(cats[1].safe && !cats[10].safe && !cats[9].safe, L"temp preselected, developer and launcher caches not");
            Check(cats[3].targets.size() == 11 && cats[3].icon == 0xE774, L"browser: 11 paths and its icon");
        }
    }

    // ---- Mistakes in user rules are reported, and never widen what is deleted
    {
        std::vector<std::wstring> errs;
        auto cats = ParseCleaners(L"[a]\nname = A\npath = C:\\\n"            // a whole drive: refused
                                  L"[b]\npath = %TEMP%\\x\n"                   // no name
                                  L"[c]\nname = C\nbogus = 1\npath = %TEMP%\\c | age=2d | wat\n"
                                  L"[dev]\nenabled = false\n",                 // turns a built-in off
                                  errs);
        Check(errs.size() == 5, L"5 problems reported (" + std::to_wstring(errs.size()) + L")");
        Check(cats.size() == 2 && cats[0].id == L"c" && cats[0].targets[0].minAgeHours == 48 && !cats[0].safe,
              L"valid category kept, not preselected by default");
        Check(cats.size() == 2 && cats[1].id == L"dev" && !cats[1].enabled, L"\"enabled = false\" stub kept");
    }

    // ---- Several file patterns, and "kind = command"
    {
        std::wstring p = dir + L"\\pkgs";
        CreateDirectoryW(p.c_str(), nullptr);
        CreateDirectoryW((p + L"\\numpy-1.0").c_str(), nullptr);
        Put(p + L"\\numpy-1.0\\lib.pyd", "extracted");
        Put(p + L"\\a.conda", "aa");
        Put(p + L"\\b.tar.bz2", "bb");
        Put(p + L"\\urls.txt", "u");
        CleanCategory pc;
        pc.targets = {{p, L"*.conda;*.tar.bz2", 0}};
        MeasureCategory(pc, cancel);
        Check(pc.entries.size() == 2 && Find(pc, p + L"\\a.conda") && Find(pc, p + L"\\b.tar.bz2"),
              L"files=*.conda;*.tar.bz2 lists only the archives (" + std::to_wstring(pc.entries.size()) + L")");

        std::vector<std::wstring> errs;
        auto bad = ParseCleaners(L"[a]\nname = A\nkind = command\npath = %TEMP%\\a\n[b]\nname = B\ncommand = x\npath = %TEMP%\\b\n", errs);
        Check(bad.empty() && errs.size() == 2, L"kind = command without a command, and a command without the kind, are refused");

        std::wstring cache = dir + L"\\toolcache";
        CreateDirectoryW(cache.c_str(), nullptr);
        Put(cache + L"\\one.bin", "1234");
        Put(cache + L"\\two.bin", "5678");
        std::string script = "@del /q \"" + util::WideToUtf8(cache) + "\\*.bin\"\r\n";
        Put(dir + L"\\clean tool.cmd", script.c_str());
        auto cats = ParseCleaners(L"[t]\nname = Tool cache\nkind = command\ncommand = \"" + dir + L"\\clean tool.cmd\" --all\npath = " + cache + L"\n", errs);
        if (cats.size() == 1) {
            CleanCategory& t = cats[0];
            MeasureCategory(t, cancel);
            Check(!t.toolMissing && t.bytes == 8, L"command category: tool found, cache measured");
            CleanResult tr = CleanCategoryNow(t, cancel, [](const std::wstring&) {});
            Check(tr.error.empty() && tr.freed == 8 && tr.files == 2 && !PathExists(cache + L"\\one.bin"),
                  L"command ran (a .cmd with spaces in its path) and what it freed was measured: " + tr.error);
        } else {
            Check(false, L"command category parsed");
        }
        CleanCategory missing;
        missing.command = L"winncdu-no-such-tool --clean";
        missing.targets = {{cache, L"", 0}};
        MeasureCategory(missing, cancel);
        CleanResult mr = CleanCategoryNow(missing, cancel, [](const std::wstring&) {});
        Check(missing.toolMissing && mr.error.find(L"was not found") != std::wstring::npos, L"a missing tool is reported: " + mr.error);
        Check(!FindTool(L"cmd /c exit").empty() && FindTool(L"winncdu-no-such-tool").empty(), L"tools are found on PATH");
    }

    // ---- Protected items are never listed, whatever a rule says
    {
        std::wstring g = dir + L"\\guarded";
        CreateDirectoryW(g.c_str(), nullptr);
        Put(g + L"\\passwords.kdbx", "secret");
        CreateDirectoryW((g + L"\\repo").c_str(), nullptr);
        CreateDirectoryW((g + L"\\repo\\.git").c_str(), nullptr);
        CreateDirectoryW((g + L"\\.git").c_str(), nullptr);
        Put(g + L"\\junk.tmp", "junk");
        CleanCategory gc;
        gc.targets = {{g, L"", 0}};
        MeasureCategory(gc, cancel);
        Check(gc.guarded == 3 && Find(gc, g + L"\\junk.tmp") && !Find(gc, g + L"\\passwords.kdbx") && !Find(gc, g + L"\\.git") &&
                  !Find(gc, g + L"\\repo"),
              L"KeePass file, .git and a folder with a repository inside left out (" + std::to_wstring(gc.guarded) + L" guarded)");
    }

    // ---- winapp2.ini: what is taken, what is skipped, and exactly what an entry deletes
    {
        std::wstring a = dir + L"\\w2app";
        for (const wchar_t* d : {L"", L"\\Cache", L"\\Cache\\sub", L"\\Logs", L"\\Logs\\sub", L"\\Logs\\repo", L"\\Logs\\repo\\.git"})
            CreateDirectoryW((a + d).c_str(), nullptr);
        for (const wchar_t* f : {L"\\Cache\\a.bin", L"\\Cache\\sub\\b.bin", L"\\Logs\\x.log", L"\\Logs\\sub\\y.txt", L"\\Logs\\keep.log",
                                 L"\\Logs\\z.dat", L"\\Logs\\History.log", L"\\Logs\\repo\\.git\\debug.log", L"\\t.tmp", L"\\u.dat"})
            Put(a + f, "data");
        std::wstring ini = L"; comment\r\n"
                           L"[Test App Cache *]\r\nLangSecRef=3021\r\nDetectFile=" + dir + L"\\w2a*\r\n"
                           L"FileKey1=" + a + L"\\Cache|*|REMOVESELF\r\n"
                           L"FileKey2=" + a + L"\\Logs|*.log;*.txt|RECURSE\r\n"
                           L"FileKey3=" + a + L"|*.tmp\r\n"
                           L"ExcludeKey1=FILE|" + a + L"\\Logs\\|keep.log\r\n"
                           L"[Test App Cookies *]\r\nFileKey1=" + a + L"|*\r\n"
                           L"[Test App Registry *]\r\nRegKey1=HKCU\\Software\\WinNCDU-Test\r\n"
                           L"[Whole Profile *]\r\nFileKey1=%UserProfile%|*|RECURSE\r\n"
                           L"[Not Installed *]\r\nDetect=HKCU\\Software\\WinNCDU-Test-Not-There\r\nFileKey1=" + a + L"\\Cache|*\r\n";
        Winapp2Stats st;
        auto w = ParseWinapp2(ini, st);
        Check(st.entries == 5 && st.usable == 2 && st.personal == 1 && st.broad == 1 && st.regOnly == 1,
              L"winapp2: 5 entries, 2 usable, 1 personal, 1 too broad, 1 registry only (" + std::to_wstring(st.usable) + L" usable)");
        if (w.size() == 2) {
            CleanCategory& app = w[0];
            Check(app.name == L"Test App Cache" && app.imported && !app.safe && app.targets.size() == 3 && app.excludes.size() == 1,
                  L"winapp2: entry parsed (name, not preselected, 3 rules, 1 exclude)");
            MeasureCategory(app, cancel);
            std::wstring listed;
            for (auto& e : app.entries) listed += e.path.substr(a.size()) + L" ";
            bool ok = app.present && app.entries.size() == 5 && Find(app, a + L"\\Cache\\a.bin") && Find(app, a + L"\\Cache\\sub\\b.bin") &&
                      Find(app, a + L"\\Logs\\x.log") && Find(app, a + L"\\Logs\\sub\\y.txt") && Find(app, a + L"\\t.tmp");
            Check(ok, L"winapp2: lists matching files, not the excluded one, not inside .git: " + listed);
            Check(app.guarded == 1 && !Find(app, a + L"\\Logs\\History.log"), L"winapp2: a file named like browser history is left out");
            CleanCategoryNow(app, cancel, [](const std::wstring&) {});
            Check(!PathExists(a + L"\\Cache\\a.bin") && !PathExists(a + L"\\Logs\\x.log") && !PathExists(a + L"\\t.tmp"),
                  L"winapp2: listed files deleted");
            Check(PathExists(a + L"\\Logs\\keep.log") && PathExists(a + L"\\Logs\\z.dat") && PathExists(a + L"\\Logs\\repo\\.git\\debug.log") &&
                      PathExists(a + L"\\Logs\\History.log") && PathExists(a + L"\\u.dat"),
                  L"winapp2: excluded, unmatched, repository and personal files survive");
            MeasureCategory(w[1], cancel);
            Check(!w[1].present && w[1].entries.empty(), L"winapp2: an app that isn't installed lists nothing");
        }

        // What a built-in category cleans is left to it; folders with personal data inside are left out.
        CreateDirectoryW((a + L"\\Profile").c_str(), nullptr);
        CreateDirectoryW((a + L"\\Profile\\Default").c_str(), nullptr);
        Put(a + L"\\Profile\\Default\\Cookies", "c");
        Put(a + L"\\Cache\\c.bin", "data");
        Put(a + L"\\v.tmp", "data");
        Put(a + L"\\w.log", "data");
        auto w2 = ParseWinapp2(L"[Overlap *]\r\nFileKey1=" + a + L"|*|RECURSE\r\n", st);
        if (w2.size() == 1) {
            w2[0].coveredBy = std::make_shared<std::vector<CleanExclude>>(
                std::vector<CleanExclude>{{a + L"\\Cache", L"*", true}, {a, L"*.tmp", false}});
            MeasureCategory(w2[0], cancel);
            std::wstring listed;
            for (auto& e : w2[0].entries) listed += e.path.substr(a.size()) + L" ";
            Check(!Find(w2[0], a + L"\\Cache") && !Find(w2[0], a + L"\\v.tmp") && Find(w2[0], a + L"\\w.log") && Find(w2[0], a + L"\\u.dat") &&
                      !Find(w2[0], a + L"\\Logs"),  // Logs has a repository inside
                  L"winapp2: what built-in rules clean is skipped: " + listed);
            Check(!Find(w2[0], a + L"\\Profile") && w2[0].guarded >= 1, L"winapp2: a folder with a cookie store inside is left out");
        }
    }

    // A real winapp2.ini (set WINNCDU_WINAPP2 to its path): parse it and measure what applies to
    // this PC. Only reads.
    if (const wchar_t* real = _wgetenv(L"WINNCDU_WINAPP2")) {
        FILE* f = nullptr;
        std::string data;
        if (_wfopen_s(&f, real, L"rb") == 0 && f) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
            fclose(f);
        }
        if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF) data.erase(0, 3);
        ULONGLONG t0 = GetTickCount64();
        Winapp2Stats st;
        auto cats = ParseWinapp2(util::Utf8ToWide(data), st);
        ULONGLONG t1 = GetTickCount64();
        // As in the app: what the shipped categories clean is left to them.
        {
            FILE* sf = nullptr;
            std::string shipped;
            if (_wfopen_s(&sf, WINNCDU_SOURCE_DIR L"/res/cleaners.ini", L"rb") == 0 && sf) {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), sf)) > 0) shipped.append(buf, n);
                fclose(sf);
            }
            std::vector<std::wstring> errs;
            auto covered = std::make_shared<std::vector<CleanExclude>>();
            for (auto& sc : ParseCleaners(util::Utf8ToWide(shipped), errs))
                if (!sc.recycleBin && !sc.external)
                    for (auto& t : sc.targets) covered->push_back({t.path, t.filePattern.empty() ? L"*" : t.filePattern, t.filePattern.empty()});
            for (auto& k : cats) k.coveredBy = covered;
        }
        uint64_t total = 0, guarded = 0;
        size_t present = 0, nonEmpty = 0;
        for (auto& k : cats) {
            MeasureCategory(k, cancel);
            present += k.present ? 1 : 0;
            nonEmpty += k.bytes ? 1 : 0;
            total += k.bytes;
            guarded += k.guarded;
        }
        ULONGLONG t2 = GetTickCount64();
        std::stable_sort(cats.begin(), cats.end(), [](const CleanCategory& x, const CleanCategory& y) { return x.bytes > y.bytes; });
        std::wstring s = L"\nwinapp2.ini: " + std::to_wstring(st.entries) + L" entries, " + std::to_wstring(st.usable) + L" usable, " +
                         std::to_wstring(st.personal) + L" personal, " + std::to_wstring(st.broad) + L" broad rules, " +
                         std::to_wstring(st.regOnly) + L" registry only; parsed in " + std::to_wstring(t1 - t0) + L" ms\n" +
                         std::to_wstring(present) + L" apps found, " + std::to_wstring(nonEmpty) + L" with something to clean, " +
                         util::FormatSizeShort(total) + L" total, " + std::to_wstring(guarded) + L" items left out; measured in " +
                         std::to_wstring(t2 - t1) + L" ms\n";
        for (size_t i = 0; i < cats.size() && i < 25 && cats[i].bytes; ++i)
            s += L"  " + util::FormatSizeShort(cats[i].bytes) + L"\t" + cats[i].name + L"  (" + std::to_wstring(cats[i].entries.size()) +
                 L" entries, " + std::to_wstring(cats[i].guarded) + L" left out)  " + cats[i].desc + L"\n";
        // WINNCDU_WINAPP2_SHOW="Name|Other name": the largest entries of those categories.
        if (const wchar_t* show = _wgetenv(L"WINNCDU_WINAPP2_SHOW"))
            for (auto& k : cats)
                if (std::wstring(L"|") + show + L"|" != L"" && (L"|" + std::wstring(show) + L"|").find(L"|" + k.name + L"|") != std::wstring::npos) {
                    s += L"\n[" + k.name + L"] " + std::to_wstring(k.entries.size()) + L" entries, " + std::to_wstring(k.guarded) + L" left out\n";
                    for (auto& t : k.targets)
                        s += L"  rule: " + t.path + L" | " + t.filePattern + (t.recurse ? L" | RECURSE" : L"") + (t.removeSelf ? L" | REMOVESELF" : L"") + L"\n";
                    for (size_t i = 0; i < k.entries.size() && i < 8; ++i)
                        s += L"    " + util::FormatSizeShort(k.entries[i].bytes) + L"  " + k.entries[i].path + L"\n";
                }
        std::string u = util::WideToUtf8(s);
        fputs(u.c_str(), stdout);
    }

    // Recycle Bin with one item kept: only the other one is removed (never the whole bin).
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::wstring err, r1 = dir + L"\\bin-remove.tmp", r2 = dir + L"\\bin-keep.tmp";
    Put(r1, "remove");
    Put(r2, "keep");
    if (DeleteToRecycleBin(r1, err) && DeleteToRecycleBin(r2, err)) {
        CleanCategory bin;
        bin.recycleBin = true;
        for (auto& it : ListRecycleBin()) {
            if (_wcsicmp(it.original.c_str(), r1.c_str()) && _wcsicmp(it.original.c_str(), r2.c_str())) continue;
            CleanEntry e;
            e.path = it.original;
            e.binData = it.dataFile;
            e.binInfo = it.infoFile;
            e.bytes = it.size;
            e.files = 1;
            e.keep = _wcsicmp(it.original.c_str(), r2.c_str()) == 0;
            bin.entries.push_back(e);
        }
        Check(bin.entries.size() == 2 && bin.KeptCount() == 1, L"both test items are in the Recycle Bin");
        if (bin.KeptCount() == 1) {  // with nothing kept the whole bin would be emptied
            CleanCategoryNow(bin, cancel, [](const std::wstring&) {});
            bool removedGone = true, keptThere = false;
            for (auto& it : ListRecycleBin()) {
                if (_wcsicmp(it.original.c_str(), r1.c_str()) == 0) removedGone = false;
                if (_wcsicmp(it.original.c_str(), r2.c_str()) == 0) {
                    keptThere = true;
                    RestoreRecycled(it, err);  // put it back so the folder can be removed below
                }
            }
            Check(removedGone, L"unkept bin item removed");
            Check(keptThere, L"kept bin item still in the bin");
        }
    } else {
        std::string u = util::WideToUtf8(L"skip Recycle Bin test: " + err + L"\n");
        fputs(u.c_str(), stdout);
    }
    CoUninitialize();

    std::atomic<uint64_t> removed{0};
    DeletePermanently(dir, cancel, removed, err);
    DeleteFileW(journal.c_str());
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
