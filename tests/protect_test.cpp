// Small self-check for the protection rules: bytopsy-protect-test.exe
// Checks well-known paths against the rules. It creates nothing; your protect.txt is restored.
#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>

#include "protect.h"
#include "tree.h"
#include "util.h"

static int g_fail = 0;

static std::wstring Env(const wchar_t* v) {
    wchar_t b[MAX_PATH * 4];
    DWORD n = GetEnvironmentVariableW(v, b, MAX_PATH * 4);
    return n ? std::wstring(b, n) : L"";
}

static void Expect(const std::wstring& path, Protect want, const Node* node = nullptr) {
    ProtectReport r = CheckProtection(path, node);
    bool ok = r.level == want;
    if (!ok) ++g_fail;
    std::wstring line = std::wstring(ok ? L"ok   " : L"FAIL ") + path + L" -> " + ProtectLevelName(r.level) +
                        (ok ? L"" : std::wstring(L" (want ") + ProtectLevelName(want) + L")");
    for (auto& h : r.hits) line += L"\n        " + std::wstring(ProtectLevelName(h.level)) + L": " + DescribeHit(h);
    std::string u = util::WideToUtf8(line + L"\n");
    fputs(u.c_str(), stdout);
}

static void ExpectBadge(ProtectBadges& b, const Node* n, Protect want, ProtectRel rel, uint64_t count = 1) {
    const ProtectHit& h = b.For(n);
    bool ok = h.level == want && (want == Protect::None || (h.rel == rel && h.count == count));
    if (!ok) ++g_fail;
    std::wstring line = std::wstring(ok ? L"ok   badge " : L"FAIL badge ") + NodePath(n) + L" -> " + ProtectLevelName(h.level);
    if (h.level != Protect::None) line += L": " + DescribeHit(h);
    if (line.size() > 160) line = line.substr(0, 150) + L"…";
    std::string u = util::WideToUtf8(line + L"\n");
    fputs(u.c_str(), stdout);
}

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

int main() {
    std::wstring home = Env(L"USERPROFILE"), roaming = Env(L"APPDATA"), local = Env(L"LOCALAPPDATA");
    std::wstring sys = Env(L"SystemRoot"), pf = Env(L"ProgramFiles");
    // Path rules don't need the folders to exist; only the "contains" check below does.
    RefreshProtection();

    Expect(L"C:\\", Protect::Blocked);
    Expect(sys, Protect::Blocked);
    Expect(sys + L"\\System32\\drivers", Protect::Blocked);
    Expect(sys + L"\\Temp", Protect::Caution);
    Expect(sys + L"\\SoftwareDistribution\\Download\\abc", Protect::Caution);
    Expect(sys + L"\\Minidump", Protect::None);
    Expect(sys + L"\\Installer", Protect::Blocked);
    Expect(pf, Protect::Blocked);
    Expect(pf + L"\\SomeApp", Protect::Important);
    Expect(home, Protect::Blocked);
    Expect(roaming, Protect::Blocked);
    Expect(home + L"\\.claude", Protect::Important);
    Expect(home + L"\\.claude\\projects", Protect::Important);
    Expect(home + L"\\.codex", Protect::Important);
    Expect(roaming + L"\\Cursor", Protect::Important);
    Expect(roaming + L"\\Cursor\\Cache", Protect::None);
    Expect(roaming + L"\\Cursor\\User\\globalStorage", Protect::Important);
    Expect(home + L"\\.ssh", Protect::Important);
    Expect(local + L"\\Google\\Chrome\\User Data\\Default\\Cache", Protect::None);
    Expect(local + L"\\Google\\Chrome\\User Data\\Default", Protect::Important);
    if (Exists(local + L"\\Google\\Chrome\\User Data"))
        Expect(local + L"\\Google", Protect::Important);  // contains the Chrome profile
    Expect(local + L"\\Temp\\foo", Protect::None);
    Expect(L"D:\\Projects\\app\\.git", Protect::Important);
    Expect(L"D:\\Projects\\app\\.git\\objects", Protect::Important);
    Expect(L"D:\\Projects\\app\\node_modules", Protect::None);
    Expect(L"D:\\Videos\\movie.mkv", Protect::None);
    Expect(L"D:\\stuff\\passwords.kdbx", Protect::Important);
    Expect(L"D:\\$Recycle.Bin", Protect::Blocked);
    Expect(L"C:\\pagefile.sys", Protect::Blocked);

    // A scanned tree: deleting D:\Projects contains a Git repository (caution) and a KeePass file.
    auto root = std::make_unique<Node>();
    root->name = L"D:\\Projects";
    root->flags = NF_DIR;
    auto add = [](Node* p, const wchar_t* name, bool dir) {
        auto n = std::make_unique<Node>();
        n->name = name;
        n->parent = p;
        n->flags = dir ? NF_DIR : 0;
        Node* raw = n.get();
        p->children.push_back(std::move(n));
        return raw;
    };
    Node* app = add(root.get(), L"app", true);
    add(app, L".git", true);
    add(app, L"main.cpp", false);
    Expect(L"D:\\Projects", Protect::Caution, root.get());
    Node* vault = add(root.get(), L"vault.KDBX", false);
    Expect(L"D:\\Projects", Protect::Important, root.get());

    // Shield badges: the same answers per item, summarized bottom-up.
    {
        Node* git = app->children[0].get();
        Node* objects = add(git, L"objects", true);
        Node* lib = add(root.get(), L"lib", true);
        add(add(lib, L"one", true), L".git", true);
        add(add(lib, L"two", true), L".git", true);
        Node* deep = add(root.get(), L"deep", true);
        Node* d = deep;
        for (int i = 0; i < 5000; ++i) d = add(d, L"d", true);  // no recursion in the summary
        add(d, L".git", true);
        ProtectBadges b;
        ExpectBadge(b, root.get(), Protect::Important, ProtectRel::Contains);  // the KeePass file wins
        ExpectBadge(b, vault, Protect::Important, ProtectRel::Self);
        ExpectBadge(b, app, Protect::Caution, ProtectRel::Contains);
        ExpectBadge(b, git, Protect::Important, ProtectRel::Self);
        ExpectBadge(b, objects, Protect::Important, ProtectRel::Inside);
        ExpectBadge(b, lib, Protect::Caution, ProtectRel::Contains, 2);
        ExpectBadge(b, deep, Protect::Caution, ProtectRel::Contains);
        ExpectBadge(b, app->children[1].get(), Protect::None, ProtectRel::Self);  // main.cpp
        // In a listing of .git, "objects" is unmarked and the folder's notice covers it.
        auto check = [](bool ok, const char* what) {
            if (!ok) ++g_fail;
            printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        };
        check(b.InList(objects) == nullptr, " .git\\objects unmarked inside a listing of .git");
        check(b.InList(git) != nullptr, " .git marked in a listing of app");
        check(b.ForContents(git) != nullptr, " notice above a listing of .git");
        check(b.ForContents(root.get()) == nullptr, " no notice above D:\\Projects (it only contains protected items)");
        // Reloaded rules make cached badges stale by themselves.
        RefreshProtection();
        ExpectBadge(b, lib, Protect::Caution, ProtectRel::Contains, 2);
    }

    // User rules in protect.txt (the existing file is restored afterwards)
    {
        std::wstring file = ProtectRulesFile();
        std::string saved;
        bool had = false;
        if (FILE* in = _wfopen(file.c_str(), L"rb")) {
            had = true;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0) saved.append(buf, n);
            fclose(in);
        }
        FILE* f = _wfopen(file.c_str(), L"wb");
        if (f) {
            fputs("important D:\\Work ; client work\r\nallow {HOME}\\.ssh\r\ncaution *.blend\r\nblock \"D:\\My Backups\"\r\n", f);
            fclose(f);
        }
        RefreshProtection();
        Expect(L"D:\\Work\\client\\file.txt", Protect::Important);
        Expect(home + L"\\.ssh", Protect::None);
        Expect(L"D:\\art\\scene.blend", Protect::Caution);
        Expect(L"D:\\My Backups", Protect::Blocked);
        if (had) {
            if (FILE* out = _wfopen(file.c_str(), L"wb")) {
                fwrite(saved.data(), 1, saved.size(), out);
                fclose(out);
            }
        } else {
            _wremove(file.c_str());
        }
        RefreshProtection();
    }

    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
