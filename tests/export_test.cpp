// Self-check of export and import: bytopsy-export-test.exe
// Exports in-memory trees with odd names and every flag to ncdu JSON in %TEMP%, imports them back
// and compares; reads an export as Linux ncdu writes it; checks the CSV; rejects broken files.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "export.h"
#include "tree.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static std::wstring g_dir;

static std::wstring TempFile(const wchar_t* name) { return g_dir + L"\\" + name; }

static std::string ReadAll(const std::wstring& path) {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        fclose(f);
    }
    return s;
}

static void WriteAll(const std::wstring& path, const std::string& data) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    }
}

static int64_t Unix(int64_t secs) { return util::UnixToFileTime(secs); }

static Node* Add(Node* parent, const std::wstring& name, uint64_t size, int64_t mtime, uint16_t flags = 0) {
    auto c = std::make_unique<Node>();
    c->name = name;
    c->parent = parent;
    c->size = size;
    c->disk = (size + 4095) / 4096 * 4096;
    c->mtime = mtime;
    c->flags = flags;
    Node* raw = c.get();
    parent->children.push_back(std::move(c));
    return raw;
}

static const std::wstring kNul(L"nul\0inside", 10);

static std::unique_ptr<Node> Fixture() {
    auto root = std::make_unique<Node>();
    root->name = L"C:\\Users\\tester\\Data";
    root->flags = NF_DIR;
    root->mtime = Unix(1700000000);
    Add(root.get(), L"quote\"d \"name\"", 10, Unix(1600000000));
    Add(root.get(), L"back\\slash\\", 11, Unix(1600000001));
    Add(root.get(), L"ctl\x01\x08\x0c\x1f\x7f", 12, Unix(1));
    Add(root.get(), L"tab\tnew\nline\rcr", 13, Unix(1600000003));
    Add(root.get(), L"emoji \U0001F600 \u65E5\u672C\u8A9E \u00E9 \u0410", 14, Unix(1600000004));
    Add(root.get(), kNul, 15, Unix(1600000005));
    Add(root.get(), L"{\"json\":[1,2]},\\u0041", 16, Unix(1600000006));
    Add(root.get(), L"comma,semi;\x2028", 17, Unix(1600000007));
    std::wstring longName(4000, L'L');
    longName += L"\u00E9\U0001F4BE";
    Add(root.get(), longName, 18, Unix(1600000008));
    Add(root.get(), L"no time", 19, 0);
    Add(root.get(), L"zero bytes", 0, Unix(1600000009));
    Add(root.get(), L"huge", (1ull << 62) + 1, Unix(4102444800));  // 2100-01-01
    Node* sub = Add(root.get(), L"sub", 0, Unix(1500000000), NF_DIR | NF_ERROR);
    Add(sub, L"x", 123, Unix(1500000001));
    Add(sub, L"hard", 1000, Unix(1500000002), NF_HARDLINK);  // counted elsewhere
    Node* deeper = Add(sub, L"deeper", 0, Unix(1500000003), NF_DIR);
    Add(deeper, L"bad", 7, Unix(1500000004), NF_ERROR);
    Add(root.get(), L"junction", 0, Unix(1400000000), NF_DIR | NF_LINK);
    Add(root.get(), L"excluded dir", 0, Unix(1400000001), NF_DIR | NF_EXCLUDED);
    Add(root.get(), L"excl.tmp", 44, Unix(1400000002), NF_EXCLUDED);
    Add(root.get(), L"symlink.txt", 0, Unix(1400000003), NF_LINK);
    Add(root.get(), L"empty dir", 0, Unix(1400000004), NF_DIR);
    Node* chain = root.get();
    for (int i = 0; i < 200; ++i) chain = Add(chain, L"level" + std::to_wstring(i), 0, Unix(1300000000 + i), NF_DIR);
    Add(chain, L"leaf", 5, Unix(1300000999));
    RecomputeTotals(root.get());
    return root;
}

// Compares an imported tree with the one that was exported, by what ncdu JSON keeps: hard-link
// copies come back with no size, links and excluded folders come back as plain entries.
static bool Same(const Node* a, const Node* b, bool top, std::wstring& why) {
    std::wstring where = top ? L"root" : a->name.substr(0, 40);
    bool dir = a->IsDir() && (top || !a->Has(NF_LINK | NF_EXCLUDED));
    uint64_t size = a->Has(NF_HARDLINK) ? 0 : a->size, disk = a->Has(NF_HARDLINK) ? 0 : a->disk;
    const uint16_t kept = NF_ERROR | NF_SUBERR | NF_LINK | NF_EXCLUDED;
    if ((top ? NodePath(a) : a->name) != b->name) why = L"name of " + where;
    else if (dir != b->IsDir()) why = L"type of " + where;
    else if (size != b->size || disk != b->disk) why = L"size of " + where;
    else if (a->mtime != b->mtime) why = L"mtime of " + where;
    else if ((a->flags & kept) != (b->flags & kept)) why = L"flags of " + where;
    else if (dir && (a->items != b->items || a->children.size() != b->children.size())) why = L"items of " + where;
    if (!why.empty()) return false;
    if (!dir) return b->children.empty() || (why = L"children of " + where, false);
    for (size_t i = 0; i < a->children.size(); ++i)
        if (!Same(a->children[i].get(), b->children[i].get(), false, why)) return false;
    return true;
}

static std::unique_ptr<Node> Import(const std::wstring& file, std::wstring& err, uint64_t* items = nullptr) {
    std::atomic<uint64_t> count{0};
    err.clear();
    auto r = ImportNcduJson(file, count, err);
    if (items) *items = count;
    return r;
}

static bool Export(const Node* n, const std::wstring& file, uint64_t* items = nullptr) {
    std::atomic<uint64_t> count{0};
    std::wstring err;
    bool ok = ExportNcduJson(n, file, count, err);
    if (items) *items = count;
    return ok && err.empty();
}

// Everything after the first line (which carries the export time).
static std::string Body(const std::string& s) {
    size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(nl);
}

static void RoundTripTests() {
    auto root = Fixture();
    std::wstring file = TempFile(L"tree.json"), again = TempFile(L"again.json"), err;
    uint64_t written = 0, read = 0;
    Check(Export(root.get(), file, &written), L"export written");
    Check(written == root->items + 1, L"export counts every item: " + std::to_wstring(written));
    std::string json = ReadAll(file);
    Check(json.rfind("[1,2,{\"progname\":\"bytopsy\"", 0) == 0 && json.back() == '\n', L"ncdu format header");
    Check(json.find("\"ctl\\u0001\\b") == std::string::npos && json.find("\"ctl\\u0001\\u0008\\u000c\\u001f\x7f\"") != std::string::npos,
          L"control characters escaped as \\u00XX, DEL kept as is");
    Check(json.find("\"quote\\\"d \\\"name\\\"\"") != std::string::npos && json.find("\"back\\\\slash\\\\\"") != std::string::npos,
          L"quotes and backslashes escaped");
    Check(json.find("\"name\":\"C:\\\\Users\\\\tester\\\\Data\"") != std::string::npos, L"root named by its full path");
    Check(json.find("\"read_error\":true") != std::string::npos && json.find("\"notreg\":true") != std::string::npos &&
              json.find("\"excluded\":\"pattern\"") != std::string::npos,
          L"flags written");

    auto back = Import(file, err, &read);
    Check(back != nullptr && err.empty(), L"import: " + (err.empty() ? L"no error" : err));
    std::wstring why;
    Check(back && Same(root.get(), back.get(), true, why), L"import gives the same tree back " + why);
    Check(read == written, L"import counts every item: " + std::to_wstring(read));
    Check(back && back->size == root->size && back->size > (1ull << 62), L"totals: 64-bit sizes survive");
    Check(back && back->children[5]->name == kNul, L"a NUL inside a name survives (\\u0000)");

    Check(back && Export(back.get(), again), L"imported tree exported again");
    Check(Body(ReadAll(again)) == Body(json), L"export -> import -> export gives the same file");

    // A sub-folder is exported under its full path.
    Node* sub = nullptr;
    for (auto& c : root->children)
        if (c->name == L"sub") sub = c.get();
    std::wstring subFile = TempFile(L"sub.json");
    Check(sub && Export(sub, subFile), L"sub-folder exported");
    auto subBack = Import(subFile, err);
    Check(subBack && subBack->name == L"C:\\Users\\tester\\Data\\sub" && subBack->children.size() == 3 &&
              subBack->Has(NF_ERROR) && subBack->Has(NF_SUBERR) && subBack->size == 130,
          L"sub-folder import: full path, flags, hard link not counted");

    // A top folder that is itself a link or excluded is still written as a folder.
    auto linkRoot = std::make_unique<Node>();
    linkRoot->name = L"D:\\junction";
    linkRoot->flags = NF_DIR | NF_LINK;
    Add(linkRoot.get(), L"inside", 9, Unix(1600000000));
    RecomputeTotals(linkRoot.get());
    std::wstring linkFile = TempFile(L"linkroot.json");
    Check(Export(linkRoot.get(), linkFile), L"linked top folder exported");
    auto lb = Import(linkFile, err);
    Check(lb && lb->IsDir() && lb->Has(NF_LINK) && lb->children.size() == 1 && lb->size == 9,
          L"linked top folder imports as a folder: " + err);

    // A lone surrogate can't be written as UTF-8: it's replaced, never a broken file.
    auto lone = std::make_unique<Node>();
    lone->name = L"E:\\";
    lone->flags = NF_DIR;
    Add(lone.get(), std::wstring(L"a") + static_cast<wchar_t>(0xD800) + L"b", 1, 0);
    std::wstring loneFile = TempFile(L"lone.json");
    Check(Export(lone.get(), loneFile), L"name with a lone surrogate exported");
    auto lob = Import(loneFile, err);
    Check(lob && lob->children.size() == 1 && lob->children[0]->name == L"a\uFFFDb", L"lone surrogate comes back as U+FFFD");
}

static void NcduInputTests() {
    // As Linux ncdu writes it: inode numbers, hard links, other keys, escapes, BOM and CRLF.
    std::string ncdu = "\xEF\xBB\xBF" R"J([1,2,{"progname":"ncdu","progver":"2.3","timestamp":1700000000},
[{"name":"/home/u","asize":4096,"dsize":4096,"dev":2049,"ino":1,"mtime":1700000000},
{"name":"a.txt","asize":10,"dsize":4096,"ino":2,"mtime":1600000000},
{"name":"h1","asize":100,"dsize":4096,"ino":5,"nlink":2,"hlnkc":true},
[{"name":"d","ino":3,"read_error":false},
 {"name":"h2","asize":100,"dsize":4096,"ino":5,"nlink":2,"hlnkc":true},
 {"name":"\u00e9\ud83d\ude00\/x\"y","asize":1}],
{"name":"sock","notreg":true},
{"name":"skip","excluded":"pattern"},
{"name":"other fs","excluded":"othfs"},
{"name":"kernfs","excluded":true},
[{"name":"err","read_error":true}],
{"name":"extra","asize":1.5e3,"unknown":{"nested":[1,{"a":null},"s"],"t":true},"mode":33188,"uid":1000},
{"name":"neg","asize":-5,"mtime":-1},
{"name":"big","asize":99999999999999999999999}
]]
)J";
    std::string crlf;
    for (char c : ncdu) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    std::wstring file = TempFile(L"ncdu.json"), err;
    WriteAll(file, crlf);
    uint64_t items = 0;
    auto r = Import(file, err, &items);
    Check(r != nullptr, L"Linux ncdu export imported: " + err);
    if (!r) return;
    auto kid = [&](Node* d, size_t i) { return i < d->children.size() ? d->children[i].get() : nullptr; };
    Check(r->name == L"/home/u" && r->children.size() == 11 && items == 14, L"structure: 11 entries, 14 items");
    Node* d = kid(r.get(), 2);
    Check(d && d->IsDir() && kid(d, 0) && kid(d, 0)->Has(NF_HARDLINK) && !kid(r.get(), 1)->Has(NF_HARDLINK),
          L"same inode with hlnkc: the second link is marked");
    Check(d && kid(d, 1) && kid(d, 1)->name == L"\u00E9\U0001F600/x\"y", L"\\u escapes, surrogate pairs, \\/ and \\\"");
    Check(kid(r.get(), 3)->Has(NF_LINK) && kid(r.get(), 4)->Has(NF_EXCLUDED) && kid(r.get(), 5)->Has(NF_EXCLUDED) &&
              kid(r.get(), 6)->Has(NF_EXCLUDED),
          L"notreg and excluded (string or true)");
    Node* e = kid(r.get(), 7);
    Check(e && e->IsDir() && e->Has(NF_ERROR) && r->Has(NF_SUBERR), L"read error on a folder, noted above it");
    Check(kid(r.get(), 8)->size == 1500, L"unknown keys skipped, 1.5e3 read as 1500");
    Check(kid(r.get(), 9)->size == 0 && kid(r.get(), 9)->mtime == util::UnixToFileTime(0), L"negative numbers read as 0");
    Check(kid(r.get(), 10)->size == UINT64_MAX, L"a size past 64 bits is capped");
    Check(kid(r.get(), 0)->mtime == util::UnixToFileTime(1600000000), L"mtime");
    Check(r->size == 10 + 100 + 1 + 1500 + UINT64_MAX && r->items == 13, L"totals count the hard link once");
}

static void ErrorTests() {
    std::wstring err;
    auto expectFail = [&](const char* label, const std::string& data, const wchar_t* mention) {
        std::wstring file = TempFile(L"bad.json");
        WriteAll(file, data);
        auto r = Import(file, err);
        Check(!r && err.find(mention) != std::wstring::npos, util::Utf8ToWide(label) + L": " + err);
    };
    bool missing = !Import(TempFile(L"missing.json"), err);
    Check(missing && err.find(L"Cannot open") != std::wstring::npos, L"missing file: " + err);
    expectFail("empty file", "", L"Expected '['");
    expectFail("gzip", "\x1f\x8b\x08\x00", L"Compressed");
    expectFail("version 2", R"([2,0,{},[{"name":"/"}]])", L"major version");
    expectFail("no root folder", R"([1,2,{},{"name":"/"}])", L"root directory");
    expectFail("truncated", R"([1,2,{},[{"name":"/"},{"name":"a","asi)", L"(at byte");
    expectFail("unterminated string", R"([1,2,{},[{"name":"/)", L"Unterminated");
    expectFail("bad escape", "[1,2,{},[{\"name\":\"\\u12", L"Bad escape");
    expectFail("missing comma", R"([1,2,{},[{"name":"/"}{"name":"a"}]])", L"Expected ','");
    expectFail("not JSON", "hello", L"Expected '['");
    expectFail("bad literal", R"([1,2,{},[{"name":"/","read_error":yes}]])", L"literal");

    std::atomic<uint64_t> count{0};
    err.clear();
    auto root = Fixture();
    bool failed = !ExportNcduJson(root.get(), g_dir + L"\\no such folder\\x.json", count, err);
    Check(failed && err.find(L"Cannot create") != std::wstring::npos, L"export to a missing folder fails: " + err);
    err.clear();
    Check(!ExportCsv(root.get(), g_dir + L"\\no such folder\\x.csv", count, err) && !err.empty(), L"CSV export to a missing folder fails");
}

// Splits CSV (RFC 4180: quoted fields, doubled quotes, line breaks inside quotes) into rows.
static std::vector<std::vector<std::string>> ParseCsv(const std::string& s) {
    std::vector<std::vector<std::string>> rows(1);
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quoted) {
            if (c == '"' && i + 1 < s.size() && s[i + 1] == '"') field += s[++i];
            else if (c == '"') quoted = false;
            else field += c;
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            rows.back().push_back(field);
            field.clear();
        } else if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n') {
            rows.back().push_back(field);
            field.clear();
            rows.emplace_back();
            ++i;
        } else {
            field += c;
        }
    }
    if (!field.empty() || !rows.back().empty()) rows.back().push_back(field);
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

static void CsvTests() {
    auto root = Fixture();
    std::wstring file = TempFile(L"tree.csv"), err;
    std::atomic<uint64_t> count{0};
    Check(ExportCsv(root.get(), file, count, err), L"CSV written");
    std::string csv = ReadAll(file);
    Check(csv.rfind("\xEF\xBB\xBFPath,Type,Size,DiskUsage,Items,Modified,Flags\r\n", 0) == 0, L"CSV: BOM and header");
    auto rows = ParseCsv(csv.substr(3));
    bool shape = rows.size() == root->items + 2;
    for (auto& r : rows) shape = shape && r.size() == 7;
    Check(shape, L"CSV: one row per item, 7 columns each (" + std::to_wstring(rows.size()) + L" rows)");
    Check(count == root->items + 1, L"CSV: counts every item");
    if (rows.size() < 4) return;
    std::string rootPath = "C:\\Users\\tester\\Data";
    Check(rows[1][0] == rootPath && rows[1][1] == "dir" && rows[1][2] == std::to_string(root->size) &&
              rows[1][4] == std::to_string(root->items),
          L"CSV: root row with totals");
    Check(rows[2][0] == rootPath + "\\quote\"d \"name\"" && rows[2][1] == "file" && rows[2][2] == "10" && rows[2][3] == "4096",
          L"CSV: quotes doubled and read back");
    auto row = [&](const std::string& path) -> const std::vector<std::string>* {
        for (auto& r : rows)
            if (r[0] == rootPath + "\\" + path) return &r;
        return nullptr;
    };
    auto* tab = row("tab\tnew\nline\rcr");
    Check(tab && (*tab)[2] == "13", L"CSV: a line break inside a name stays inside its field");
    auto* sub = row("sub");
    Check(sub && (*sub)[1] == "dir" && (*sub)[6] == "read_error", L"CSV: read_error flag");
    auto* hard = row("sub\\hard");
    Check(hard && (*hard)[6] == "hardlink", L"CSV: hardlink flag, path joined with backslashes");
    auto* junction = row("junction");
    Check(junction && (*junction)[6] == "link", L"CSV: link flag");
    auto* excl = row("excl.tmp");
    Check(excl && (*excl)[6] == "excluded", L"CSV: excluded flag");
    auto* none = row("no time");
    Check(none && (*none)[5].empty() && !(*row("zero bytes"))[5].empty(), L"CSV: no time is an empty cell");
    auto* emoji = row(util::WideToUtf8(L"emoji \U0001F600 \u65E5\u672C\u8A9E \u00E9 \u0410"));
    Check(emoji != nullptr, L"CSV: names are UTF-8");
}

static void DeepTests() {
    // Folders nested as deep as a Windows path allows: export and import on a task-sized stack.
    auto root = std::make_unique<Node>();
    root->name = L"C:\\";
    root->flags = NF_DIR;
    Node* cur = root.get();
    for (int i = 0; i < 16000; ++i) cur = Add(cur, L"d", 0, 0, NF_DIR);
    Add(cur, L"f", 1, 0);
    RecomputeTotals(root.get());
    std::wstring file = TempFile(L"deep.json"), err;
    Check(Export(root.get(), file), L"16000 nested folders exported");
    auto back = Import(file, err);
    int depth = 0;
    for (Node* n = back.get(); n && !n->children.empty(); n = n->children[0].get()) ++depth;
    Check(back && depth == 16001 && back->size == 1, L"16000 nested folders imported: " + std::to_wstring(depth) + L" levels " + err);
    back.reset();

    // Deeper than any real path: rejected, not a stack overflow.
    for (int i = 0; i < 4000; ++i) cur = Add(cur, L"e", 0, 0, NF_DIR);
    Check(Export(root.get(), file), L"20000 nested folders exported");
    back = Import(file, err);
    Check(!back && err.find(L"nested too deeply") != std::wstring::npos, L"20000 nested folders rejected: " + err);
    std::string deepMeta = "[1,2," + std::string(1000000, '[');
    WriteAll(file, deepMeta);
    back = Import(file, err);
    Check(!back && err.find(L"Nested too deeply") != std::wstring::npos, L"a million nested arrays in the metadata rejected: " + err);
}

int main() {
    wchar_t tb[MAX_PATH];
    std::wstring tmp(tb, GetTempPathW(MAX_PATH, tb));
    g_dir = tmp + L"bytopsy-export-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);

    // Like the app, run on a worker thread with a 16 MB stack (imports and exports are tasks).
    HANDLE h = util::StartThread([] {
        RoundTripTests();
        NcduInputTests();
        CsvTests();
        ErrorTests();
        DeepTests();
    });
    if (h) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }

    for (const wchar_t* f : {L"tree.json", L"again.json", L"sub.json", L"linkroot.json", L"lone.json", L"ncdu.json",
                             L"bad.json", L"tree.csv", L"deep.json"})
        DeleteFileW(TempFile(f).c_str());
    Check(RemoveDirectoryW(g_dir.c_str()) != 0, L"test folder removed");
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
