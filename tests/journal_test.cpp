// Self-check of the deletion journal and Recycle Bin restore: winncdu-journal-test.exe
// Uses a journal file in %TEMP% (not yours) and a temporary folder that it recycles and restores.
// Also checks where WinNCDU keeps its files (portable mode).
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <string>

#include "fsops.h"
#include "journal.h"
#include "protect.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

// With long names: on GitHub's runners %TEMP% is C:\Users\RUNNER~1\..., and the Recycle Bin
// reports long paths.
static std::wstring TempDir() {
    wchar_t b[MAX_PATH], lb[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, b);
    DWORD m = n ? GetLongPathNameW(b, lb, MAX_PATH) : 0;
    std::wstring s = m && m < MAX_PATH ? std::wstring(lb, m) : std::wstring(b, n);
    if (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}

static bool PutText(const std::wstring& path, const char* text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static std::string GetText(const std::wstring& path) {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return s;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

int main() {
    std::wstring tmp = TempDir();
    std::wstring pid = std::to_wstring(GetCurrentProcessId());

    // ---- Where WinNCDU keeps its files: %LOCALAPPDATA%\WinNCDU, or next to the exe in portable mode
    {
        std::wstring exeDir = tmp + L"\\winncdu-portable-test-" + pid;
        CreateDirectoryW(exeDir.c_str(), nullptr);
        Check(util::DataDirFor(exeDir, L"C:\\Users\\me\\AppData\\Local") == L"C:\\Users\\me\\AppData\\Local\\WinNCDU",
              L"data folder: %LOCALAPPDATA%\\WinNCDU by default");
        std::wstring marker = exeDir + L"\\" + util::kPortableMarker;
        CreateDirectoryW(marker.c_str(), nullptr);
        Check(util::DataDirFor(exeDir, L"C:\\L") == L"C:\\L\\WinNCDU", L"data folder: a folder named winncdu.portable doesn't count");
        RemoveDirectoryW(marker.c_str());
        PutText(marker, "");
        Check(util::DataDirFor(exeDir, L"C:\\L") == exeDir + L"\\WinNCDU-data", L"data folder: WinNCDU-data next to the exe with winncdu.portable");
        DeleteFileW(marker.c_str());
        Check(util::DataDirFor(exeDir, L"").empty(), L"data folder: none without %LOCALAPPDATA% or a marker");
        RemoveDirectoryW(exeDir.c_str());

        wchar_t exe[MAX_PATH], lad[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring here(exe, n);
        here = here.substr(0, here.rfind(L'\\'));
        DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
        Check(util::DataDir() == util::DataDirFor(here, std::wstring(lad, ln < MAX_PATH ? ln : 0)) && !util::DataDir().empty(),
              L"data folder of this run: " + util::DataDir() + (util::PortableMode() ? L" (portable)" : L""));
        SetEnvironmentVariableW(L"WINNCDU_JOURNAL", nullptr);
        Check(JournalFile() == util::DataDir() + L"\\journal.log" && ProtectRulesFile() == util::DataDir() + L"\\protect.txt",
              L"journal.log and protect.txt are in it");
    }

    std::wstring journal = tmp + L"\\winncdu-journal-test-" + pid + L".log";
    SetEnvironmentVariableW(L"WINNCDU_JOURNAL", journal.c_str());
    DeleteFileW(journal.c_str());
    Check(JournalFile() == journal, L"journal file can be redirected for tests");

    // ---- Format round trip
    JournalEntry a;
    a.time = util::NowFileTime();
    a.action = JournalAction::Delete;
    a.bytes = 1234567;
    a.items = 42;
    a.path = L"D:\\Old\\project";
    a.note = L"partly deleted";
    JournalAppend(a);
    JournalEntry b = a;
    b.action = JournalAction::Clean;
    b.path = L"Browser caches";
    b.note = L"line\tbreak\nhere";
    JournalAppend(b);
    auto read = JournalRead();
    Check(read.size() == 2, L"two entries read back");
    if (read.size() == 2) {
        Check(read[0].action == JournalAction::Clean && read[1].action == JournalAction::Delete, L"newest first");
        Check(read[1].bytes == 1234567 && read[1].items == 42 && read[1].path == a.path && read[1].note == a.note,
              L"fields survive");
        Check(read[0].note == L"line break here", L"tabs and line breaks are flattened");
        Check(read[1].time / 10000000 == a.time / 10000000, L"time to the second");
    }

    // ---- Who is locking this? (Restart Manager): this process holds a file open
    {
        std::wstring lockDir = tmp + L"\\winncdu-lock-test-" + pid;
        CreateDirectoryW(lockDir.c_str(), nullptr);
        std::wstring f = lockDir + L"\\locked.txt";
        HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        auto holds = [&](const std::wstring& p) {
            for (auto& a : AppsUsing(p))
                if (a.pid == GetCurrentProcessId()) return !a.name.empty();
            return false;
        };
        Check(holds(f), L"the app holding a file is named");
        Check(holds(lockDir), L"the app holding a file inside a folder is named");
        CloseHandle(h);
        Check(!holds(f), L"nobody once it is closed");
        Check(AppNames({L"A", L"B", L"A", L"C", L"D", L"E"}) == L"A, B, C and 2 more" && AppNames({L"A", L"B"}) == L"A and B",
              L"app names are joined readably");
        DeleteFileW(f.c_str());
        RemoveDirectoryW(lockDir.c_str());
    }

    // ---- Recycle and restore a real folder
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::wstring dir = tmp + L"\\winncdu-restore-test-" + pid;
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\sub").c_str(), nullptr);
    PutText(dir + L"\\sub\\file.txt", "hello");
    std::wstring err;
    if (!DeleteToRecycleBin(dir, err)) {
        std::string u = util::WideToUtf8(L"skip recycle test: " + err + L"\n");
        fputs(u.c_str(), stdout);
    } else {
        JournalEntry r;
        r.time = util::NowFileTime();
        r.action = JournalAction::Recycle;
        r.path = dir;
        JournalAppend(r);
        Check(!PathExists(dir), L"folder is in the Recycle Bin");
        auto bin = ListRecycleBin();
        const RecycledItem* it = FindRecycled(bin, r);
        Check(it != nullptr, L"journal entry found in the Recycle Bin");
        if (it) {
            // Never overwrite: something new at the old path blocks the restore.
            PutText(dir, "new file with the same name");
            err.clear();
            bool refused = !RestoreRecycled(*it, err);
            Check(refused, L"restore refuses to overwrite: " + err);
            DeleteFileW(dir.c_str());
            err.clear();
            bool restored = RestoreRecycled(*it, err);
            Check(restored, L"restore" + (err.empty() ? std::wstring() : L" (" + err + L")"));
            Check(GetText(dir + L"\\sub\\file.txt") == "hello", L"content is back");
            auto bin2 = ListRecycleBin();
            Check(FindRecycled(bin2, r) == nullptr, L"gone from the Recycle Bin");
            auto after = JournalRead(1);
            Check(!after.empty() && after[0].action == JournalAction::Restore && after[0].path == dir, L"restore is journaled");
        }
        std::atomic<bool> cancel{false};
        std::atomic<uint64_t> removed{0};
        DeletePermanently(dir, cancel, removed, err);
    }
    CoUninitialize();
    DeleteFileW(journal.c_str());

    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
