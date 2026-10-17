// Self-check of Cleanup's preview semantics: winncdu-cleanup-test.exe
// Exactly the measured entries are deleted, minus the kept ones; entries that changed since they
// were measured are left alone. Works in %TEMP% only; Recycle Bin items it creates are its own.
#include <windows.h>
#include <objbase.h>

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
    wchar_t b[MAX_PATH];
    std::wstring tmp(b, GetTempPathW(MAX_PATH, b));
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
        Check(ids == L"recycle temp wintemp browser thumbs crash wu do shader dev winold ", L"all categories, in order: " + ids);
        if (cats.size() == 11) {
            Check(cats[0].recycleBin && cats[10].external && cats[10].needsAdmin, L"kinds and admin flags");
            Check(cats[1].targets.size() == 1 && cats[1].targets[0].path == L"%TEMP%" && cats[1].targets[0].minAgeHours == 24,
                  L"temp: path and age");
            Check(cats[4].targets[0].filePattern == L"thumbcache_*.db", L"thumbs: file pattern");
            Check(cats[1].safe && !cats[9].safe, L"temp preselected, developer caches not");
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
        Check(gc.guarded == 2 && Find(gc, g + L"\\junk.tmp") && !Find(gc, g + L"\\passwords.kdbx") && !Find(gc, g + L"\\.git"),
              L"KeePass file and .git left out (" + std::to_wstring(gc.guarded) + L" guarded)");
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
