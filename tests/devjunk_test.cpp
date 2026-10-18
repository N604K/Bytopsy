// Self-check of the developer junk finder: bytopsy-devjunk-test.exe
// Builds small fake projects next to the exe (a folder outside AppData), scans them and checks
// what is found, how it is grouped, and what must never be listed. Only reads after setup.
#include <windows.h>

#include <cstdio>
#include <string>

#include "devjunk.h"
#include "fsops.h"
#include "scanner.h"
#include "tree.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

// Creates the file and its folders.
static void Put(const std::wstring& path, const char* text = "x", int ageDays = 0) {
    for (size_t i = path.find(L'\\', 3); i != std::wstring::npos; i = path.find(L'\\', i + 1)) CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        fputs(text, f);
        fclose(f);
    }
    if (!ageDays) return;
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    int64_t t = util::NowFileTime() - static_cast<int64_t>(ageDays) * 864000000000LL;
    FILETIME ft{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

static void Fixture(const std::wstring& d) {
    std::string big(100000, 'n');
    Put(d + L"\\web\\package.json", "{}", 400);
    Put(d + L"\\web\\src\\app.js", "x", 400);
    Put(d + L"\\web\\node_modules\\left-pad\\index.js", big.c_str());
    Put(d + L"\\web\\node_modules\\left-pad\\package.json");
    Put(d + L"\\web\\node_modules\\left-pad\\node_modules\\inner\\i.js");
    Put(d + L"\\web\\.next\\cache.bin");
    Put(d + L"\\web\\.git\\HEAD");                    // new, but Git data isn't "a change to the project"
    Put(d + L"\\loose\\node_modules\\a.js");           // no package.json: not a project
    Put(d + L"\\rust\\Cargo.toml");
    Put(d + L"\\rust\\target\\debug\\app.exe");
    Put(d + L"\\dotnet\\App.csproj");
    Put(d + L"\\dotnet\\bin\\Debug\\App.dll");
    Put(d + L"\\dotnet\\obj\\project.assets.json");
    Put(d + L"\\cmake\\CMakeLists.txt");
    Put(d + L"\\cmake\\build\\CMakeCache.txt");
    Put(d + L"\\cmake\\build\\main.obj");
    Put(d + L"\\unity\\Assets\\Player.cs");
    Put(d + L"\\unity\\ProjectSettings\\ProjectVersion.txt");
    Put(d + L"\\unity\\Library\\ArtifactDB");
    Put(d + L"\\py\\pyproject.toml");
    Put(d + L"\\py\\pkg\\mod.py");
    Put(d + L"\\py\\pkg\\__pycache__\\mod.cpython-312.pyc");
    Put(d + L"\\py\\.venv\\pyvenv.cfg");
    Put(d + L"\\py\\.venv\\Lib\\site-packages\\x.py");
    Put(d + L"\\random\\build\\output.txt");            // "build" without a build system: kept
    Put(d + L"\\tools\\.vscode\\ext\\package.json");     // an editor's extension, not your project
    Put(d + L"\\tools\\.vscode\\ext\\node_modules\\m.js");
}

static std::unique_ptr<Node> Scan(const std::wstring& path) {
    auto root = std::make_unique<Node>();
    root->name = path;
    root->flags = NF_DIR;
    ReadNodeMeta(root.get(), path);
    ScanOptions opt;
    ScanStats stats;
    ScanDirectory(root.get(), path, opt, stats);
    RecomputeTotals(root.get());
    return root;
}

static const DevProject* Project(const std::vector<DevProject>& v, const std::wstring& name) {
    for (auto& p : v)
        if (p.dir->name == name) return &p;
    return nullptr;
}

static std::wstring Names(const DevProject* p) {
    std::wstring s;
    if (p)
        for (auto& a : p->artifacts) s += a.node->name + L" ";
    return s;
}

int main() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring here(exe);
    here = here.substr(0, here.rfind(L'\\'));
    std::wstring pid = std::to_wstring(GetCurrentProcessId());
    std::wstring dir = here + L"\\devjunk-test-" + pid;
    Fixture(dir);

    auto root = Scan(dir);
    auto found = FindDevJunk(root.get());
    std::wstring all;
    for (auto& p : found) all += p.dir->name + L"(" + Names(&p) + L") ";
    // A build inside a dot folder (.claude\worktrees\...) counts as a tool's folder: nothing is found.
    Check(found.size() == 6, L"6 projects (build outside folders like .git or AppData): " + all);

    const DevProject* web = Project(found, L"web");
    Check(web && Names(web) == L"node_modules .next ", L"web: node_modules (largest first) and .next, nothing nested: " + Names(web));
    Check(web && web->kinds == L"Node.js, Next.js", L"web: kinds listed once each");
    int64_t days = web ? (util::NowFileTime() - web->lastChange) / 864000000000LL : 0;
    Check(web && days >= 399 && days <= 401, L"web: last change is the project's own files, not node_modules or .git (" +
                                                 std::to_wstring(days) + L" days)");
    Check(Names(Project(found, L"rust")) == L"target ", L"rust: target");
    std::wstring dn = Names(Project(found, L"dotnet"));
    Check(dn == L"bin obj " || dn == L"obj bin ", L".NET: bin and obj");
    Check(Names(Project(found, L"cmake")) == L"build ", L"CMake: build with a CMakeCache.txt inside");
    Check(Names(Project(found, L"unity")) == L"Library ", L"Unity: Library");
    std::wstring py = Names(Project(found, L"py"));
    Check(py.find(L"__pycache__") != std::wstring::npos && py.find(L".venv") != std::wstring::npos,
          L"Python: __pycache__ grouped under the project, .venv: " + py);
    Check(!Project(found, L"loose") && !Project(found, L"random") && !Project(found, L"ext"),
          L"not listed: node_modules without package.json, a plain build folder, an editor extension");
    Check(web && DevProjectSize(*web, false) >= 100000, L"sizes add up");

    // The same projects inside AppData belong to installed programs: nothing is listed.
    wchar_t tb[MAX_PATH];
    std::wstring tmp(tb, GetTempPathW(MAX_PATH, tb));
    std::wstring appDir = tmp + L"bytopsy-devjunk-test-" + pid;
    Fixture(appDir);
    auto appRoot = Scan(appDir);
    Check(FindDevJunk(appRoot.get()).empty(), L"nothing found when the scan is inside AppData");

    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> removed{0};
    std::wstring err;
    DeletePermanently(dir, cancel, removed, err);
    DeletePermanently(appDir, cancel, removed, err);
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
