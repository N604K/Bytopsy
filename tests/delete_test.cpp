// Self-check of permanent deletion: bytopsy-delete-test.exe
// Works in %TEMP% only. Checks that links inside a deleted folder are removed as links (their
// targets survive), read-only and hidden items go too, locked files are reported, and long paths work.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "fsops.h"
#include "tree.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static void Put(const std::wstring& path, const char* text, DWORD attrs = 0) {
    HANDLE h = CreateFileW(ToLongPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(h, text, static_cast<DWORD>(strlen(text)), &n, nullptr);
    CloseHandle(h);
    if (attrs) SetFileAttributesW(ToLongPath(path).c_str(), attrs);
}

static void Dir(const std::wstring& path) { CreateDirectoryW(ToLongPath(path).c_str(), nullptr); }

// Directory junctions need no special rights (unlike symbolic links).
static bool Junction(const std::wstring& link, const std::wstring& target) {
    std::wstring cmd = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\" >nul";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> b(cmd.begin(), cmd.end());
    b.push_back(0);
    if (!CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DWORD a = GetFileAttributesW(link.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

int main() {
    // With long names: on GitHub's runners %TEMP% is C:\Users\RUNNER~1\..., and Windows reports
    // where a junction leads with long names.
    wchar_t b[MAX_PATH], lb[MAX_PATH];
    DWORD tn = GetTempPathW(MAX_PATH, b), ln = tn ? GetLongPathNameW(b, lb, MAX_PATH) : 0;
    std::wstring tmp = ln && ln < MAX_PATH ? std::wstring(lb, ln) : std::wstring(b, tn);
    std::wstring base = tmp + L"bytopsy-delete-test-" + std::to_wstring(GetCurrentProcessId());
    Dir(base);
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> removed{0};
    std::wstring err;

    // ---- A tree with read-only, hidden and system items
    std::wstring t = base + L"\\tree";
    Dir(t);
    Dir(t + L"\\a");
    Dir(t + L"\\a\\b");
    Put(t + L"\\a\\b\\deep.txt", "deep");
    Put(t + L"\\readonly.txt", "ro", FILE_ATTRIBUTE_READONLY);
    Put(t + L"\\hidden.txt", "h", FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    Dir(t + L"\\rodir");
    SetFileAttributesW((t + L"\\rodir").c_str(), FILE_ATTRIBUTE_READONLY);
    Check(DeletePermanently(t, cancel, removed, err), L"tree deleted" + (err.empty() ? L"" : L": " + err));
    Check(!PathExists(t), L"tree is gone");
    Check(removed == 7, L"7 items removed (" + std::to_wstring(removed.load()) + L")");

    // ---- A junction inside the tree: the link goes, its target stays
    std::wstring outside = base + L"\\outside";
    Dir(outside);
    Put(outside + L"\\precious.txt", "keep me");
    std::wstring t2 = base + L"\\tree2";
    Dir(t2);
    Put(t2 + L"\\file.txt", "x");
    if (Junction(t2 + L"\\link", outside)) {
        err.clear();
        Check(DeletePermanently(t2, cancel, removed, err), L"tree with a junction deleted" + (err.empty() ? L"" : L": " + err));
        Check(!PathExists(t2), L"tree with a junction is gone");
        Check(PathExists(outside + L"\\precious.txt"), L"the junction's target is untouched");
        // A path through a junction leads elsewhere (as administrator, deletes refuse such paths).
        std::wstring actual;
        Check(PathLeadsElsewhere(base + L"\\outside\\precious.txt", actual) == false, L"a plain path leads where it says");
        Junction(base + L"\\via", outside);
        Check(PathLeadsElsewhere(base + L"\\via\\precious.txt", actual) && actual == outside + L"\\precious.txt",
              L"a path through a junction is detected: " + actual);
        RemoveDirectoryW((base + L"\\via").c_str());
        // A junction as the delete target itself.
        Junction(base + L"\\rootlink", outside);
        err.clear();
        Check(DeletePermanently(base + L"\\rootlink", cancel, removed, err) && !PathExists(base + L"\\rootlink"),
              L"a junction as the target removes only the link");
        Check(PathExists(outside + L"\\precious.txt"), L"its target is untouched");
    } else {
        fputs("skip junction tests: mklink /J failed\n", stdout);
    }

    // ---- A locked file: everything else goes, the error names the file
    std::wstring t3 = base + L"\\tree3";
    Dir(t3);
    Put(t3 + L"\\free.txt", "free");
    Put(t3 + L"\\locked.txt", "locked");
    HANDLE lock = CreateFileW((t3 + L"\\locked.txt").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    err.clear();
    Check(!DeletePermanently(t3, cancel, removed, err), L"a locked file makes the delete report failure");
    Check(err.find(L"locked.txt") != std::wstring::npos, L"the error names the locked file: " + err);
    Check(!PathExists(t3 + L"\\free.txt") && PathExists(t3 + L"\\locked.txt"), L"the rest was deleted");
    CloseHandle(lock);
    err.clear();
    Check(DeletePermanently(t3, cancel, removed, err) && !PathExists(t3), L"after closing it, it deletes");

    // ---- Paths longer than MAX_PATH
    std::wstring deep = base + L"\\long";
    Dir(deep);
    std::wstring p = deep;
    for (int i = 0; i < 12; ++i) {
        p += L"\\" + std::wstring(30, static_cast<wchar_t>(L'a' + i));
        Dir(p);
    }
    Put(p + L"\\end.txt", "end");
    Check(p.size() > 300 && PathExists(p + L"\\end.txt"), L"long path created (" + std::to_wstring(p.size()) + L" chars)");
    err.clear();
    Check(DeletePermanently(deep, cancel, removed, err) && !PathExists(deep), L"long path deleted" + (err.empty() ? L"" : L": " + err));

    // ---- Single files, missing paths, cancel
    Put(base + L"\\single.txt", "1", FILE_ATTRIBUTE_READONLY);
    err.clear();
    Check(DeletePermanently(base + L"\\single.txt", cancel, removed, err) && !PathExists(base + L"\\single.txt"),
          L"a read-only file on its own");
    Check(DeletePermanently(base + L"\\does-not-exist", cancel, removed, err), L"a missing path counts as deleted");
    std::wstring t4 = base + L"\\tree4";
    Dir(t4);
    Put(t4 + L"\\x.txt", "x");
    std::atomic<bool> stop{true};
    Check(!DeletePermanently(t4, stop, removed, err) && PathExists(t4), L"cancel stops before deleting");

    err.clear();
    Check(DeletePermanently(base, cancel, removed, err) && !PathExists(base), L"test folder removed");
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
