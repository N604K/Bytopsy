// Self-check of the analysis views: winncdu-analysis-test.exe
// Search syntax and matching, largest files and file types run on an in-memory tree; duplicates
// run on real files in %TEMP% (identical, same size but different, hard links, files that vanished).
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "analysis.h"
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

constexpr uint64_t KB = 1024, MB = 1024 * KB, GB = 1024 * MB;
constexpr int64_t kDay = 864000000000LL;

static Node* Add(Node* parent, const std::wstring& name, uint64_t size, int ageDays, uint16_t flags = 0) {
    auto c = std::make_unique<Node>();
    c->name = name;
    c->parent = parent;
    c->size = size;
    c->disk = (size + 4095) / 4096 * 4096;
    c->mtime = util::NowFileTime() - ageDays * kDay;
    c->flags = flags;
    Node* raw = c.get();
    parent->children.push_back(std::move(c));
    return raw;
}

static std::unique_ptr<Node> Fixture() {
    auto root = std::make_unique<Node>();
    root->name = L"C:\\fixture";
    root->flags = NF_DIR;
    Node* movies = Add(root.get(), L"Movies", 0, 10, NF_DIR);
    Add(movies, L"film.MP4", 2 * GB, 400);
    Add(movies, L"film copy.mp4", 2 * GB, 400, NF_HARDLINK);  // second link to film.MP4
    Add(movies, L"clip.mkv", 600 * MB, 3);
    Add(movies, L"trailer.mp4", 1 * GB, 30);
    Add(movies, L"notes.txt", 100, 2);
    Node* iso = Add(root.get(), L"iso", 0, 100, NF_DIR);
    Add(iso, L"ubuntu.iso", 5 * GB, 800);
    Add(iso, L"small.ISO", 10 * MB, 1);
    Add(root.get(), L"Empty Folder", 0, 100, NF_DIR);
    Add(root.get(), L"zero.log", 0, 100);
    Add(root.get(), L"two words.doc", 5 * KB, 100);
    Add(root.get(), L".bashrc", 1 * KB, 100);
    Add(root.get(), L"archive.tar.gz", 9 * MB, 100);
    Add(root.get(), L"noext", 50, 100);
    RecomputeTotals(root.get());
    return root;
}

// Names of the matches, sorted; "error: ..." if the query doesn't parse.
static std::wstring Find(Node* root, const std::wstring& query) {
    SearchQuery q;
    std::wstring err;
    if (!ParseQuery(query, q, err)) return L"error: " + err;
    std::vector<std::wstring> names;
    for (Node* n : RunSearch(root, q, false)) names.push_back(n->name);
    std::sort(names.begin(), names.end());
    std::wstring s;
    for (auto& n : names) s += (s.empty() ? L"" : L"|") + n;
    return s;
}

static void Expect(Node* root, const std::wstring& query, const std::wstring& want) {
    std::wstring got = Find(root, query);
    Check(got == want, L"search [" + query + L"] -> " + got + (got == want ? L"" : L"   (want " + want + L")"));
}

static void ExpectError(const std::wstring& query, const std::wstring& mention) {
    SearchQuery q;
    std::wstring err;
    bool ok = ParseQuery(query, q, err);
    Check(!ok && err.find(mention) != std::wstring::npos, L"rejected [" + query + L"]: " + err);
}

static void SearchTests() {
    auto root = Fixture();
    Node* r = root.get();
    // Names and wildcards (case-insensitive).
    Expect(r, L"*.iso", L"small.ISO|ubuntu.iso");
    Expect(r, L"*.mp4", L"film copy.mp4|film.MP4|trailer.mp4");
    Expect(r, L"?ero.log", L"zero.log");
    Expect(r, L"MOVIES", L"Movies");
    Expect(r, L"film", L"film copy.mp4|film.MP4");
    Expect(r, L"words two", L"two words.doc");
    Expect(r, L"\"words two\"", L"");
    Expect(r, L"\"film copy\"", L"film copy.mp4");
    Expect(r, L"nothing-like-this", L"");
    // Sizes: ">" and "<" are strict, ">=" and "<=" include the limit.
    Expect(r, L">1G type:f", L"film copy.mp4|film.MP4|ubuntu.iso");
    Expect(r, L">=1G type:f", L"film copy.mp4|film.MP4|trailer.mp4|ubuntu.iso");
    Expect(r, L"<10M type:f", L".bashrc|archive.tar.gz|noext|notes.txt|two words.doc|zero.log");
    Expect(r, L"<=10M type:f", L".bashrc|archive.tar.gz|noext|notes.txt|small.ISO|two words.doc|zero.log");
    Expect(r, L">1G", L"Movies|film copy.mp4|film.MP4|iso|ubuntu.iso");
    Expect(r, L"size:>5G", L"iso");
    Expect(r, L">500M <2G", L"clip.mkv|trailer.mp4");
    Expect(r, L">1.5g <=2gb", L"film copy.mp4|film.MP4");
    Expect(r, L"<0", L"");
    Expect(r, L"<1", L"Empty Folder|zero.log");
    // Ages.
    Expect(r, L"older:1y", L"film copy.mp4|film.MP4|ubuntu.iso");
    Expect(r, L"newer:7d", L"clip.mkv|notes.txt|small.ISO");
    Expect(r, L"newer:2w type:f", L"clip.mkv|notes.txt|small.ISO");
    Expect(r, L"newer:12h", L"");
    Expect(r, L"older:6m newer:2y", L"film copy.mp4|film.MP4");
    Expect(r, L"older:5 newer:20", L"Movies");
    // Types, extensions, empty.
    Expect(r, L"type:d", L"Empty Folder|Movies|iso");
    Expect(r, L"type:folders", L"Empty Folder|Movies|iso");
    Expect(r, L"ext:mp4,mkv", L"clip.mkv|film copy.mp4|film.MP4|trailer.mp4");
    Expect(r, L"ext:.GZ", L"archive.tar.gz");
    Expect(r, L"ext:iso type:d", L"");
    Expect(r, L"empty", L"Empty Folder|zero.log");
    Expect(r, L"empty type:d", L"Empty Folder");
    // Combinations.
    Expect(r, L"*.mp4 >500M older:1y", L"film copy.mp4|film.MP4");
    Expect(r, L"ext:mp4 newer:2m", L"trailer.mp4");
    Expect(r, L"  *.iso\t  <1G  ", L"small.ISO");

    SearchQuery q;
    std::wstring err;
    Check(ParseQuery(L"ext:MP4,.mkv,, >=1.5G", q, err) && q.exts == std::vector<std::wstring>{L"mp4", L"mkv"} &&
              q.minSize == 3 * GB / 2,
          L"parsed: extensions lowercased without dots, empty ones dropped; >=1.5G");
    int64_t now = util::NowFileTime();
    Check(ParseQuery(L"older:1y", q, err) && q.olderThan <= now - 365 * kDay && q.olderThan > now - 366 * kDay,
          L"parsed: older:1y is a cutoff 365 days ago");
    Check(ParseQuery(L"newer:36h", q, err) && q.newerThan <= now - kDay * 3 / 2 && q.newerThan > now - kDay * 2,
          L"parsed: newer:36h");
    Check(ParseQuery(L"\"Two Words\"", q, err) && q.names == std::vector<std::wstring>{L"two words"},
          L"parsed: a quoted phrase is one lowercase term");

    // Order: largest first (by size or by disk usage), then by name.
    SearchQuery files;
    ParseQuery(L"type:f", files, err);
    auto bySize = RunSearch(r, files, false);
    Check(bySize.size() == 12 && bySize[0]->name == L"ubuntu.iso" && bySize[1]->name == L"film copy.mp4" &&
              bySize[2]->name == L"film.MP4",
          L"results: largest first, ties by name");
    auto byDisk = RunSearch(r, files, true);
    Check(!byDisk.empty() && byDisk.back()->name == L"zero.log", L"results by disk usage: zero-byte file last");

    // Invalid input.
    ExpectError(L"", L"Empty");
    ExpectError(L"   ", L"Empty");
    ExpectError(L">abc", L">abc");
    ExpectError(L">", L"Bad size");
    ExpectError(L"<10Q", L"<10Q");
    ExpectError(L"size:>", L"Bad size");
    ExpectError(L"older:", L"Bad age");
    ExpectError(L"older:5x", L"older:5x");
    ExpectError(L"newer:d", L"Bad age");
    ExpectError(L"type:x", L"Bad type");
    ExpectError(L"*.iso type:", L"Bad type");
}

static void LargestAndTypesTests() {
    auto root = Fixture();
    Node* r = root.get();
    auto top = LargestFiles(r, 3, false);
    Check(top.size() == 3 && top[0]->name == L"ubuntu.iso" && top[1]->name == L"film.MP4" && top[2]->name == L"trailer.mp4",
          L"largest 3 files: hard link left out");
    Check(LargestFiles(r, 100, false).size() == 11, L"largest: asking for more than there are gives every file once");
    Check(LargestFiles(r, 0, false).empty(), L"largest: zero asked, none given");
    Check(LargestFiles(r->children[0].get(), 1, false)[0]->name == L"film.MP4", L"largest: limited to the scope");

    Check(FileExt(L"a.tar.GZ") == L"gz" && FileExt(L".bashrc").empty() && FileExt(L"name.").empty() &&
              FileExt(L"noext").empty() && FileExt(L"x.Mp4") == L"mp4",
          L"extension: last part, lowercase; dot-files and trailing dots have none");
    auto stats = ExtensionStats(r, false);
    auto find = [&](const std::wstring& e) -> const ExtStat* {
        for (auto& s : stats)
            if (s.ext == e) return &s;
        return nullptr;
    };
    Check(stats.size() == 8 && stats[0].ext == L"iso" && stats[1].ext == L"mp4", L"file types: largest type first");
    const ExtStat* mp4 = find(L"mp4");
    Check(mp4 && mp4->count == 2 && mp4->size == 3 * GB, L"file types: mp4 counts 2 files (hard link left out)");
    const ExtStat* none = find(L"");
    Check(none && none->count == 2 && none->size == 1 * KB + 50, L"file types: no extension (.bashrc, noext)");
    const ExtStat* iso = find(L"iso");
    Check(iso && iso->count == 2 && iso->disk == 5 * GB + 10 * MB, L"file types: case-insensitive, disk usage summed");
    auto mp4s = FilesWithExt(r, L"mp4", false);
    Check(mp4s.size() == 3 && mp4s[0]->name == L"film copy.mp4" && mp4s[2]->name == L"trailer.mp4",
          L"files of a type: largest first, ties by name");
    Check(FilesWithExt(r, L"exe", false).empty(), L"files of a type that isn't there: none");
}

// ---------------------------------------------------------------------------------------------
// Duplicates on real files

static void Put(const std::wstring& path, const std::string& data, int ageDays) {
    for (size_t i = path.find(L'\\', 3); i != std::wstring::npos; i = path.find(L'\\', i + 1)) CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    }
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    int64_t t = util::NowFileTime() - static_cast<int64_t>(ageDays) * kDay;
    FILETIME ft{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

static std::string Pattern(size_t n, unsigned seed) {
    std::string s(n, '\0');
    unsigned x = seed;
    for (auto& c : s) {
        x = x * 1103515245u + 12345u;
        c = static_cast<char>(x >> 16);
    }
    return s;
}

static std::unique_ptr<Node> Scan(const std::wstring& path, bool hardlinks) {
    auto root = std::make_unique<Node>();
    root->name = path;
    root->flags = NF_DIR;
    ReadNodeMeta(root.get(), path);
    ScanOptions opt;
    opt.hardlinks = hardlinks;
    ScanStats stats;
    ScanDirectory(root.get(), path, opt, stats);
    RecomputeTotals(root.get());
    return root;
}

static Node* Child(Node* dir, const std::wstring& name) {
    for (auto& c : dir->children)
        if (c->name == name) return c.get();
    return nullptr;
}

static std::wstring Groups(const std::vector<DupGroup>& groups) {
    std::wstring s;
    for (auto& g : groups) {
        s += L"[" + std::to_wstring(g.size) + L":";
        for (Node* n : g.files) s += L" " + n->name;
        s += L"] ";
    }
    return s;
}

static void DuplicateTests(const std::wstring& dir) {
    std::string big = Pattern(200000, 1);
    std::string bigOff = big;
    bigOff.back() ^= 1;  // same size, same first 64 KB, different end
    std::string mid = Pattern(65536, 2), midOff = mid;
    midOff.back() ^= 1;
    std::string edge = Pattern(65537, 3), edgeOff = edge;
    edgeOff.back() ^= 1;
    Put(dir + L"\\big1.bin", big, 30);
    Put(dir + L"\\sub\\big2.bin", big, 10);
    Put(dir + L"\\big3.bin", bigOff, 20);
    Check(CreateHardLinkW((dir + L"\\zlink.bin").c_str(), (dir + L"\\big1.bin").c_str(), nullptr) != 0,
          L"setup: hard link created");
    Put(dir + L"\\mid1.bin", mid, 6);
    Put(dir + L"\\sub\\deeper\\mid2.bin", mid, 5);
    Put(dir + L"\\mid3.bin", midOff, 7);
    Put(dir + L"\\edge1.bin", edge, 1);
    Put(dir + L"\\edge2.bin", edgeOff, 1);
    Put(dir + L"\\small1.txt", "hello duplicate", 3);
    Put(dir + L"\\small2.txt", "hello duplicate", 1);
    Put(dir + L"\\sub\\small3.txt", "hello duplicate", 2);
    Put(dir + L"\\other.txt", "hello different", 1);
    Put(dir + L"\\empty1.txt", "", 1);
    Put(dir + L"\\empty2.txt", "", 1);

    auto root = Scan(dir, true);
    DupProgress prog;
    auto groups = FindDuplicates(root.get(), 0, 4, prog);
    std::wstring got = Groups(groups);
    Check(groups.size() == 3, L"3 duplicate groups: " + got);
    Check(got == L"[200000: big1.bin big2.bin] [65536: mid1.bin mid2.bin] [15: small1.txt small3.txt small2.txt] ",
          L"groups by space wasted, files oldest first; hard link, different ends, empty files left out");
    Check(prog.stage == 2 && prog.filesDone == prog.filesTotal, L"progress: finished the full-hash stage");

    DupProgress p1;
    Check(Groups(FindDuplicates(root.get(), 0, 1, p1)) == got, L"same result with one thread");
    DupProgress p2;
    Check(Groups(FindDuplicates(root.get(), 16, 4, p2)) == L"[200000: big1.bin big2.bin] [65536: mid1.bin mid2.bin] ",
          L"minimum size leaves the small files out");
    DupProgress p3;
    Check(FindDuplicates(root.get(), 200001, 4, p3).empty(), L"minimum size above every file: nothing");
    DupProgress p4;
    p4.cancel = true;
    Check(FindDuplicates(root.get(), 0, 4, p4).empty(), L"cancelled: nothing");
    DupProgress p5;
    Check(FindDuplicates(Child(root.get(), L"sub"), 0, 4, p5).empty(), L"limited to the scope: no pairs inside sub");

    // Without hard-link detection the link looks like a third copy.
    auto plain = Scan(dir, false);
    DupProgress p6;
    auto pg = FindDuplicates(plain.get(), 100000, 4, p6);
    Check(pg.size() == 1 && pg[0].files.size() == 3, L"without link detection the hard link is listed: " + Groups(pg));

    // Cloud placeholders and links are never read; a file that vanished since the scan drops out.
    Child(root.get(), L"small2.txt")->flags |= NF_CLOUD;
    DeleteFileW((dir + L"\\sub\\deeper\\mid2.bin").c_str());
    DupProgress p7;
    Check(Groups(FindDuplicates(root.get(), 0, 4, p7)) == L"[200000: big1.bin big2.bin] [15: small1.txt small3.txt] ",
          L"cloud placeholder skipped, deleted file dropped");
}

int main() {
    SearchTests();
    LargestAndTypesTests();

    wchar_t tb[MAX_PATH];
    std::wstring tmp(tb, GetTempPathW(MAX_PATH, tb));
    std::wstring dir = tmp + L"winncdu-analysis-test-" + std::to_wstring(GetCurrentProcessId());
    DuplicateTests(dir);

    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> removed{0};
    std::wstring err;
    DeletePermanently(dir, cancel, removed, err);
    Check(GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES, L"test folder removed");
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
