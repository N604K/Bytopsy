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
#include "protect.h"
#include "sqlite3.h"
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

static void MkDirs(const std::wstring& path) {
    for (size_t i = path.find(L'\\', 3); ; i = path.find(L'\\', i + 1)) {
        CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
        if (i == std::wstring::npos) break;
    }
}

// Runs SQL on a fixture database (made if missing).
static bool Sql(const std::wstring& file, const char* sql) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open16(file.c_str(), &db);
    char* msg = nullptr;
    if (rc == SQLITE_OK) rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
    if (msg) {
        printf("SQL error: %s\n", msg);
        sqlite3_free(msg);
    }
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

// The rows of a query, one "a|b|c" string each.
static std::string Rows(const std::wstring& file, const char* sql) {
    sqlite3* db = nullptr;
    std::string out;
    if (sqlite3_open16(file.c_str(), &db) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK)
            while (sqlite3_step(st) == SQLITE_ROW) {
                for (int i = 0; i < sqlite3_column_count(st); ++i) {
                    const unsigned char* t = sqlite3_column_text(st, i);
                    out += (i ? "|" : "") + std::string(t ? reinterpret_cast<const char*>(t) : "NULL");
                }
                out += ";";
            }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

// Every file under `dir` with its size and time: measuring must leave all of it as it was.
static std::wstring Listing(const std::wstring& dir) {
    std::wstring out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            out += Listing(dir + L"\\" + fd.cFileName);
        else
            out += dir + L"\\" + fd.cFileName + L" " + std::to_wstring(fd.nFileSizeLow) + L" " + std::to_wstring(fd.ftLastWriteTime.dwLowDateTime) + L"\n";
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

static std::wstring W(const std::string& s) { return util::Utf8ToWide(s); }

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
        Check(ids == L"recycle temp wintemp browser cookies history chat chatmedia thumbs crash wu do shader launchers dev jvm conda conan "
                     L"winold winbt ",
              L"all categories, in order: " + ids);
        if (cats.size() == 20) {
            Check(cats[0].recycleBin && cats[18].external && cats[18].needsAdmin && cats[19].external && cats[19].targets.size() == 2,
                  L"kinds and admin flags");
            Check(cats[11].command.find(L"Delete-DeliveryOptimizationCache") != std::wstring::npos && cats[11].needsAdmin,
                  L"Delivery Optimization is emptied through its own command");
            Check(cats[17].command == L"conan remove \"*\" -c" && cats[16].targets[0].filePattern == L"*.conda;*.tar.bz2",
                  L"conan runs its own command; conda takes only archives");
            Check(cats[1].targets.size() == 1 && cats[1].targets[0].path == L"%TEMP%" && cats[1].targets[0].minAgeHours == 24,
                  L"temp: path and age");
            Check(cats[8].targets[0].filePattern == L"thumbcache_*.db", L"thumbs: file pattern");
            Check(cats[1].safe && !cats[14].safe && !cats[13].safe, L"temp preselected, developer and launcher caches not");
            Check(cats[3].targets.size() == 11 && cats[3].icon == 0xE774, L"browser: 11 paths and its icon");
            // Cookies and history: opt-in, cleaned in the databases of the same browser profiles.
            Check(cats[4].browserData == BrowserData::Cookies && cats[5].browserData == BrowserData::History && !cats[4].safe &&
                      !cats[5].safe && cats[4].targets.size() == 8 && cats[5].targets.size() == 8 && cats[3].browserData == BrowserData::None,
                  L"cookies and history: 8 browser profile paths each, never preselected");
            // Chat apps: only cache folders, never their sign-in, storage or message databases.
            bool onlyCaches = cats[6].targets.size() == 33;
            for (auto& t : cats[6].targets) {
                std::wstring last = util::ToLower(t.path.substr(t.path.rfind(L'\\') + 1));
                onlyCaches = onlyCaches && t.filePattern.empty() && t.minAgeHours == 0 &&
                             ((last.size() >= 5 && last.compare(last.size() - 5, 5, L"cache") == 0) || last == L"cachestorage");
            }
            Check(onlyCaches && cats[6].safe, L"chat: 33 cache folders (Teams, Discord, Slack), preselected like browser caches");
            Check(!cats[7].safe && cats[7].targets.size() == 4 && cats[7].desc.find(L"for good") != std::wstring::npos,
                  L"chat media: 4 folders, never preselected, says what is lost");
        }
    }

    // ---- WhatsApp's received media is open to Cleanup (with a heads-up); its chat history is not
    {
        wchar_t la[MAX_PATH];
        std::wstring local(la, GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH));
        std::wstring pkg = local + L"\\Packages\\5319275A.WhatsAppDesktop_cv1g1gvanyjgm\\LocalState";
        Protect media = CheckProtection(pkg + L"\\shared\\transfers\\2025_01\\IMG-1.jpg").level;
        Protect chats = CheckProtection(pkg + L"\\shared\\messages.db").level;
        Check(media == Protect::Caution && chats == Protect::Important, L"WhatsApp: received media Caution, the rest Important");
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

    // ---- Browser cookies and history, on fixture profiles made with SQLite: cookies of kept sites
    // survive, Firefox's bookmarks survive, measuring writes nothing, an open database is left alone.
    {
        std::wstring root = dir + L"\\browsers";
        std::wstring chrome = root + L"\\Chrome\\User Data\\Default", other = root + L"\\Chrome\\User Data\\Profile 2";
        std::wstring ff = root + L"\\Firefox\\Profiles\\abc.default-release";
        std::wstring keepFile = root + L"\\keep-signed-in.txt";
        MkDirs(chrome + L"\\Network");
        MkDirs(other);
        MkDirs(ff);
        SetEnvironmentVariableW(L"WINNCDU_KEEP_SIGNED_IN", keepFile.c_str());
        bool made =
            Sql(chrome + L"\\Network\\Cookies",
                "CREATE TABLE meta(key LONGVARCHAR NOT NULL UNIQUE PRIMARY KEY, value LONGVARCHAR);"
                "CREATE TABLE cookies(creation_utc INTEGER NOT NULL, host_key TEXT NOT NULL, name TEXT NOT NULL, value TEXT NOT NULL,"
                " path TEXT NOT NULL, encrypted_value BLOB);"
                "INSERT INTO cookies VALUES(1,'.google.com','SID','','/',x'00'),(2,'accounts.google.com','LSID','','/',x'00'),"
                "(3,'.notgoogle.com','a','','/',x'00'),(4,'.tracker.example','t','','/',x'00'),(5,'github.com','user_session','','/',x'00'),"
                "(6,'.github.com','_gh_sess','','/',x'00');") &&
            Sql(chrome + L"\\History",
                "CREATE TABLE meta(key LONGVARCHAR NOT NULL UNIQUE PRIMARY KEY, value LONGVARCHAR);"
                "CREATE TABLE urls(id INTEGER PRIMARY KEY, url LONGVARCHAR, title LONGVARCHAR, visit_count INTEGER DEFAULT 0 NOT NULL,"
                " typed_count INTEGER DEFAULT 0 NOT NULL, last_visit_time INTEGER NOT NULL, hidden INTEGER DEFAULT 0 NOT NULL);"
                "CREATE TABLE visits(id INTEGER PRIMARY KEY, url INTEGER NOT NULL, visit_time INTEGER NOT NULL);"
                "INSERT INTO urls VALUES(1,'https://mail.google.com/mail/u/0/','Mail',40,0,1,0),(2,'https://github.com/x/y','GitHub',25,0,1,0),"
                "(3,'https://www.bbc.co.uk/news','BBC',9,0,1,0),(4,'https://once.example/','Once',1,0,1,0),"
                "(5,'chrome://settings/','Settings',50,0,1,0),(6,'http://192.168.1.1/','Router',30,0,1,0);"
                "INSERT INTO visits VALUES(1,1,1),(2,1,2),(3,2,3),(4,3,4);") &&
            Sql(other + L"\\Cookies", "CREATE TABLE foo(host_key TEXT); INSERT INTO foo VALUES('.example.com');") &&
            Sql(ff + L"\\cookies.sqlite",
                "PRAGMA journal_mode=WAL;"
                "CREATE TABLE moz_cookies(id INTEGER PRIMARY KEY, originAttributes TEXT NOT NULL DEFAULT '', name TEXT, value TEXT,"
                " host TEXT, path TEXT);"
                "INSERT INTO moz_cookies(name, value, host, path) VALUES('a','1','.github.com','/'),('b','2','.ads.example','/'),"
                "('c','3','www.github.com','/');") &&
            Sql(ff + L"\\places.sqlite",
                "PRAGMA journal_mode=WAL;"
                "CREATE TABLE moz_origins(id INTEGER PRIMARY KEY, prefix TEXT NOT NULL, host TEXT NOT NULL, frecency INTEGER NOT NULL);"
                "CREATE TABLE moz_places(id INTEGER PRIMARY KEY, url LONGVARCHAR, title LONGVARCHAR, rev_host LONGVARCHAR,"
                " visit_count INTEGER DEFAULT 0, hidden INTEGER DEFAULT 0 NOT NULL, typed INTEGER DEFAULT 0 NOT NULL,"
                " frecency INTEGER DEFAULT -1 NOT NULL, last_visit_date INTEGER, guid TEXT, foreign_count INTEGER DEFAULT 0 NOT NULL,"
                " url_hash INTEGER DEFAULT 0 NOT NULL, origin_id INTEGER, recalc_frecency INTEGER NOT NULL DEFAULT 0);"
                "CREATE TABLE moz_historyvisits(id INTEGER PRIMARY KEY, from_visit INTEGER, place_id INTEGER, visit_date INTEGER,"
                " visit_type INTEGER, session INTEGER);"
                "CREATE TABLE moz_bookmarks(id INTEGER PRIMARY KEY, type INTEGER, fk INTEGER DEFAULT NULL, parent INTEGER,"
                " position INTEGER, title LONGVARCHAR, keyword_id INTEGER, folder_type TEXT, dateAdded INTEGER, lastModified INTEGER, guid TEXT);"
                "CREATE TABLE moz_keywords(id INTEGER PRIMARY KEY AUTOINCREMENT, keyword TEXT UNIQUE, place_id INTEGER, post_data TEXT);"
                "CREATE TABLE moz_annos(id INTEGER PRIMARY KEY, place_id INTEGER NOT NULL, anno_attribute_id INTEGER, content LONGVARCHAR);"
                "CREATE TABLE moz_inputhistory(place_id INTEGER NOT NULL, input LONGVARCHAR NOT NULL, use_count INTEGER,"
                " PRIMARY KEY (place_id, input));"
                "INSERT INTO moz_origins VALUES(1,'https://','bookmarked.example',100),(2,'https://','visited.example',100),"
                "(3,'https://','keyword.example',100);"
                "INSERT INTO moz_places(id, url, title, visit_count, typed, last_visit_date, foreign_count, origin_id) VALUES"
                "(1,'https://bookmarked.example/','Bookmarked',5,1,99,1,1),(2,'https://visited.example/','Visited',7,1,99,0,2),"
                "(3,'https://keyword.example/?q=%s','Keyword',0,0,NULL,1,3),(4,'https://visited.example/page','Page',3,0,99,0,2);"
                "INSERT INTO moz_bookmarks(id, type, fk, parent, position, title) VALUES(1,2,NULL,0,0,'root'),(2,1,1,1,0,'My bookmark');"
                "INSERT INTO moz_keywords(keyword, place_id) VALUES('k',3);"
                "INSERT INTO moz_historyvisits(place_id, visit_date, visit_type) VALUES(1,1,1),(2,2,1),(4,3,1);"
                "INSERT INTO moz_annos(place_id, anno_attribute_id, content) VALUES(1,1,'note'),(2,2,'file:///C:/Downloads/x.zip');"
                "INSERT INTO moz_inputhistory VALUES(2,'vis',1);");
        Check(made, L"browser fixtures made");
        Put(chrome + L"\\Visited Links", "links");
        Put(chrome + L"\\Top Sites", "tiles");
        Put(chrome + L"\\Bookmarks", "{\"roots\":{}}");
        Put(chrome + L"\\Login Data", "passwords");

        Check(KeepHost({L"google.com"}, L".google.com") && KeepHost({L"google.com"}, L"accounts.google.com") &&
                  !KeepHost({L"google.com"}, L".notgoogle.com") && !KeepHost({L"google.com"}, L"google.com.evil.example"),
              L"keep list: a site covers its subdomains, on a label boundary only");
        Check(SiteOf(L"mail.google.com") == L"google.com" && SiteOf(L"www.bbc.co.uk") == L"bbc.co.uk" && SiteOf(L"192.168.1.1") == L"192.168.1.1",
              L"sites of hosts");
        std::vector<std::wstring> top = MostVisitedSites({chrome, ff}, 15);
        std::wstring topList;
        for (auto& t : top) topList += t + L" ";
        Check(topList == L"google.com github.com visited.example bbc.co.uk bookmarked.example ",
              L"most visited sites, from Chromium and Firefox history (no IPs, no chrome://, no single visits): " + topList);

        std::wstring rules = L"[cookies]\nname = Browser cookies\nkind = cookies\npath = " + root + L"\\Chrome\\User Data\\*\npath = " + root +
                             L"\\Firefox\\Profiles\\*\n[history]\nname = Browser history\nkind = history\npath = " + root +
                             L"\\Chrome\\User Data\\*\npath = " + root + L"\\Firefox\\Profiles\\*\n";
        std::vector<std::wstring> errs;
        auto cats = ParseCleaners(rules, errs);
        Check(errs.empty() && cats.size() == 2 && cats[0].browserData == BrowserData::Cookies && cats[1].browserData == BrowserData::History,
              L"kind = cookies / history parsed");
        if (cats.size() == 2) {
            CleanCategory& ck = cats[0];
            CleanCategory& hi = cats[1];
            // First measure: no keep list yet, so it is made from the most visited sites.
            std::wstring before = Listing(root + L"\\Chrome") + Listing(root + L"\\Firefox");
            MeasureCategory(ck, cancel);
            MeasureCategory(hi, cancel);
            Check(Listing(root + L"\\Chrome") + Listing(root + L"\\Firefox") == before, L"measuring changed no file and made none");
            std::vector<std::wstring> made2 = LoadKeepList(keepFile);
            Check(made2.size() == 5 && made2[0] == L"google.com" && ck.keep == made2, L"keep list made from the most visited sites");
            Check(ck.denied && !Find(ck, other + L"\\Cookies"), L"a \"Cookies\" file that isn't a cookie database is left out");

            // The user's own list.
            Put(keepFile, "# comment\r\nhttps://www.GitHub.com/login\r\n  google.com  \r\n*.example.org\r\nnot a site\r\n");
            std::vector<std::wstring> keep = LoadKeepList(keepFile);
            Check(keep == std::vector<std::wstring>{L"github.com", L"google.com", L"example.org"}, L"keep list lines normalized");
            MeasureCategory(ck, cancel);
            CleanEntry* cc = Find(ck, chrome + L"\\Network\\Cookies");
            CleanEntry* fc = Find(ck, ff + L"\\cookies.sqlite");
            Check(ck.entries.size() == 2 && cc && cc->files == 2 && fc && fc->files == 1 && ck.files == 3,
                  L"cookies measured: 2 Chrome and 1 Firefox cookie of sites not kept");
            Check(cc && cc->note.find(L"4 kept for 2 sites") != std::wstring::npos, L"review note: " + (cc ? cc->note : L""));

            // An open database is skipped and the app named; nothing in it changes.
            HANDLE lock = CreateFileW((chrome + L"\\Network\\Cookies").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            CleanResult lr = CleanCategoryNow(ck, cancel, [](const std::wstring&) {});
            CloseHandle(lock);
            Check(lr.failed == 1 && !lr.lockedBy.empty() && lr.files == 1, L"a database in use is skipped, the app holding it named (" +
                                                                                   (lr.lockedBy.empty() ? L"none" : lr.lockedBy[0]) + L")");
            Check(Rows(chrome + L"\\Network\\Cookies", "SELECT COUNT(*) FROM cookies") == "6;", L"the database in use is unchanged");
            Check(Rows(ff + L"\\cookies.sqlite", "SELECT host FROM moz_cookies ORDER BY id") == ".github.com;www.github.com;",
                  L"Firefox: only the kept site's cookies remain");

            MeasureCategory(ck, cancel);
            CleanResult cr = CleanCategoryNow(ck, cancel, [](const std::wstring&) {});
            Check(cr.failed == 0 && cr.files == 2 && cr.error.empty(), L"Chrome cookies cleaned: " + cr.error);
            Check(Rows(chrome + L"\\Network\\Cookies", "SELECT host_key FROM cookies ORDER BY creation_utc") ==
                      ".google.com;accounts.google.com;github.com;.github.com;",
                  L"Chrome: cookies of google.com and github.com (and subdomains) remain, notgoogle.com went");
            Check(Rows(other + L"\\Cookies", "SELECT COUNT(*) FROM foo") == "1;", L"the unknown database was not touched");

            // History.
            CleanEntry* ch = Find(hi, chrome + L"\\History");
            CleanEntry* fh = Find(hi, ff + L"\\places.sqlite");
            Check(hi.entries.size() == 2 && ch && ch->files == 4 && fh && fh->files == 3 && fh->note.find(L"bookmarks stay") != std::wstring::npos,
                  L"history measured: 4 Chrome and 3 Firefox visits");
            CleanResult hr = CleanCategoryNow(hi, cancel, [](const std::wstring&) {});
            Check(hr.failed == 0 && hr.files == 7 && hr.error.empty(), L"history cleaned: " + hr.error);
            Check(!PathExists(chrome + L"\\History") && !PathExists(chrome + L"\\Visited Links") && !PathExists(chrome + L"\\Top Sites"),
                  L"Chrome: History and the files made from it are gone");
            Check(PathExists(chrome + L"\\Bookmarks") && PathExists(chrome + L"\\Login Data") && PathExists(chrome + L"\\Network\\Cookies"),
                  L"Chrome: bookmarks, passwords and cookies stay");
            std::wstring p = ff + L"\\places.sqlite";
            Check(Rows(p, "SELECT COUNT(*) FROM moz_historyvisits") == "0;", L"Firefox: visits gone");
            Check(Rows(p, "SELECT id, url, visit_count, last_visit_date, typed, recalc_frecency FROM moz_places ORDER BY id") ==
                      "1|https://bookmarked.example/|0|NULL|0|1;3|https://keyword.example/?q=%s|0|NULL|0|1;",
                  W("Firefox: the bookmarked and keyword pages stay, without visits: " +
                    Rows(p, "SELECT id, url, visit_count, last_visit_date, typed, recalc_frecency FROM moz_places ORDER BY id")));
            Check(Rows(p, "SELECT id, fk, title FROM moz_bookmarks ORDER BY id") == "1|NULL|root;2|1|My bookmark;",
                  L"Firefox: bookmarks untouched, still pointing at their page");
            Check(Rows(p, "SELECT id FROM moz_origins ORDER BY id") == "1;3;" && Rows(p, "SELECT place_id FROM moz_annos") == "1;" &&
                      Rows(p, "SELECT COUNT(*) FROM moz_inputhistory") == "0;" && Rows(p, "SELECT keyword, place_id FROM moz_keywords") == "k|3;",
                  L"Firefox: origins, annotations and address bar entries of deleted pages went with them");
            Check(Rows(p, "PRAGMA integrity_check") == "ok;", L"Firefox: database intact");
            MeasureCategory(hi, cancel);
            Check(hi.entries.empty(), L"nothing left to clean");
        }
        SetEnvironmentVariableW(L"WINNCDU_KEEP_SIGNED_IN", nullptr);
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
