// Fuzz test of the ncdu JSON importer: bytopsy-importfuzz-test.exe
// Mutates valid exports with a fixed-seed generator (truncation, byte flips, lost or extra brackets,
// huge numbers, deep nesting, invalid UTF-8, bad escapes) and checks the importer never crashes or
// hangs, and either fails with a message or returns a sound tree that exports and imports again
// to the same file. Works on two files in %TEMP%.
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

static std::wstring g_in, g_out;

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

// splitmix64: small, fast and the same sequence everywhere.
struct Rng {
    uint64_t s;
    uint64_t Next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    size_t Below(size_t n) { return n ? static_cast<size_t>(Next() % n) : 0; }
    template <size_t N>
    const char* Pick(const char* const (&list)[N]) { return list[Below(N)]; }
};

static Node* Add(Node* parent, const std::wstring& name, uint64_t size, uint16_t flags = 0) {
    auto c = std::make_unique<Node>();
    c->name = name;
    c->parent = parent;
    c->size = size;
    c->disk = (size + 4095) / 4096 * 4096;
    c->mtime = util::UnixToFileTime(1600000000 + static_cast<int64_t>(size));
    c->flags = flags;
    Node* raw = c.get();
    parent->children.push_back(std::move(c));
    return raw;
}

static std::vector<std::string> Seeds() {
    std::vector<std::string> seeds;
    auto root = std::make_unique<Node>();
    root->name = L"C:\\Fuzz \"root\"";
    root->flags = NF_DIR;
    Add(root.get(), L"plain.txt", 100);
    Add(root.get(), L"quote\"back\\slash\ttab\x01", 200);
    Add(root.get(), L"\U0001F600 \u65E5\u672C \u00E9", 300);
    Node* d = Add(root.get(), L"dir", 0, NF_DIR | NF_ERROR);
    Add(d, L"inner", 1ull << 40);
    Add(d, L"hard", 50, NF_HARDLINK);
    Add(d, L"empty", 0, NF_DIR);
    Add(root.get(), L"link", 0, NF_DIR | NF_LINK);
    Add(root.get(), L"skip", 0, NF_EXCLUDED);
    RecomputeTotals(root.get());
    std::atomic<uint64_t> count{0};
    std::wstring err;
    ExportNcduJson(root.get(), g_in, count, err);
    seeds.push_back(ReadAll(g_in));
    seeds.push_back(R"J([1,2,{"progname":"ncdu","progver":"2.3","timestamp":1700000000},
[{"name":"/home/u","asize":4096,"dsize":4096,"dev":2049,"ino":1,"mtime":1700000000},
{"name":"a.txt","asize":10,"dsize":4096,"ino":2,"mtime":1600000000},
{"name":"h1","asize":100,"dsize":4096,"ino":5,"nlink":2,"hlnkc":true},
[{"name":"d","ino":3},{"name":"h2","asize":100,"dsize":4096,"ino":5,"nlink":2,"hlnkc":true},
{"name":"\u00e9\ud83d\ude00\/x","asize":1.5e3}],
{"name":"sock","notreg":true},{"name":"skip","excluded":"pattern"},
[{"name":"err","read_error":true}],
{"name":"extra","unknown":{"nested":[1,{"a":null},"s"],"t":false}}
]]
)J");
    seeds.push_back(R"([1,0,{},[{"name":"/"}]])");
    return seeds;
}

static std::string Mutate(const std::string& in, const std::vector<std::string>& seeds, Rng& rng) {
    static const char* const kChars[] = {"[", "]", "{", "}", "\"", ",", ":", "\\", "0", "9", "-", "+", ".", "e", "t",
                                         "f", "n", " ", "\n"};
    static const char* const kNumbers[] = {"18446744073709551615", "18446744073709551616", "99999999999999999999999999999999999999",
                                           "1e308", "1e309", "-1", "-9223372036854775808", "0.0000001", "1.5e-300",
                                           "00000000000000000000001", "1e", "-", ".", "1e+", "--1", "1.2.3", "4e4e4"};
    static const char* const kUtf8[] = {"\xC3", "\xFF", "\xFE", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xC0\x80", "\xE2\x82",
                                        "\x80\x80", "\xF0\x9F\x98", ""};  // "" stands for a NUL byte
    static const char* const kEscapes[] = {"\\", "\\u", "\\u12", "\\uZZZZ", "\\ud800", "\\udc00\\ud800", "\\ud83d\\u0041",
                                           "\\u-001", "\\u+fff", "\\x41", "\\u0000", "\\uffff", "\\\""};
    static const char* const kPieces[] = {",\"asize\":", ",\"hlnkc\":true,\"ino\":5", ",\"excluded\":\"x\"", ",\"notreg\":true",
                                          ",\"read_error\":true", ",\"mtime\":99999999999999999", ",\"mtime\":910692730086",
                                          "null", "true", ",{}", ",[]", ",[{}]", "{\"name\":\"a\"}", ",[{\"name\":\"x\"}]",
                                          ",\"name\":", ",\"dev\":1,\"ino\":5,\"hlnkc\":true", "[1,2,{},"};
    std::string s = in;
    int rounds = 1 + static_cast<int>(rng.Below(4));
    for (int r = 0; r < rounds; ++r) {
        size_t pos = rng.Below(s.size() + 1);
        size_t op = rng.Below(13);
        switch (op) {
            case 0: s.resize(rng.Below(s.size() + 1)); break;  // truncate
            case 1:
                if (!s.empty()) s[rng.Below(s.size())] ^= static_cast<char>(1 << rng.Below(8));
                break;
            case 2:
                if (!s.empty()) s[rng.Below(s.size())] = rng.Pick(kChars)[0];
                break;
            case 3:
            case 4: {  // drop or double a bracket
                std::vector<size_t> at;
                for (size_t i = 0; i < s.size(); ++i)
                    if (s[i] == '[' || s[i] == ']' || s[i] == '{' || s[i] == '}') at.push_back(i);
                if (at.empty()) break;
                size_t i = at[rng.Below(at.size())];
                if (op == 3) s.erase(i, 1);
                else s.insert(i, 1, s[i]);
                break;
            }
            case 5: {  // replace a number with an extreme one
                std::vector<size_t> at;
                for (size_t i = 0; i < s.size(); ++i)
                    if (isdigit(static_cast<unsigned char>(s[i])) && (i == 0 || !isdigit(static_cast<unsigned char>(s[i - 1]))))
                        at.push_back(i);
                if (at.empty()) break;
                size_t i = at[rng.Below(at.size())], j = i;
                while (j < s.size() && isdigit(static_cast<unsigned char>(s[j]))) ++j;
                s.replace(i, j - i, rng.Pick(kNumbers));
                break;
            }
            case 6: {  // deep nesting, sometimes closed again
                size_t n = (size_t{1} << rng.Below(15)) + rng.Below(64);
                std::string open, close;
                bool dirs = rng.Below(2) == 0;
                for (size_t i = 0; i < n; ++i) open += dirs ? "[{\"name\":\"d\"}," : "[";
                if (rng.Below(2)) {
                    if (dirs) open.pop_back();  // no trailing comma before the first close
                    close.assign(n, ']');
                }
                s.insert(pos, open + close);
                break;
            }
            case 7: {  // invalid UTF-8 inside a string
                size_t q = s.find('"', pos);
                const char* bytes = rng.Pick(kUtf8);
                std::string ins = *bytes ? std::string(bytes) : std::string(1, '\0');
                s.insert(q == std::string::npos ? pos : q + 1, ins);
                break;
            }
            case 8: s.erase(pos, rng.Below(64)); break;
            case 9: {
                std::string piece = s.substr(rng.Below(s.size() + 1), rng.Below(256));
                s.insert(pos, piece);
                break;
            }
            case 10: {
                size_t q = s.find('"', pos);
                s.insert(q == std::string::npos ? pos : q + 1, rng.Pick(kEscapes));
                break;
            }
            case 11: {  // splice in part of another seed
                const std::string& o = seeds[rng.Below(seeds.size())];
                std::string piece = o.substr(rng.Below(o.size() + 1), rng.Below(512));
                s.replace(pos, rng.Below(128), piece);
                break;
            }
            case 12: {
                size_t q = s.find_first_of("{}[],", pos);
                s.insert(q == std::string::npos ? pos : q + rng.Below(2), rng.Pick(kPieces));
                break;
            }
        }
        if (s.size() > (1u << 20)) s.resize(1u << 20);
    }
    return s;
}

// Parent links, folder totals and item counts agree; only folders have children.
static bool Sound(const Node* root) {
    std::vector<const Node*> stack{root};
    while (!stack.empty()) {
        const Node* n = stack.back();
        stack.pop_back();
        if (!n->IsDir() && !n->children.empty()) return false;
        if (!n->IsDir()) continue;
        uint64_t size = 0, disk = 0, items = 0;
        for (auto& c : n->children) {
            if (c->parent != n) return false;
            if (!c->Has(NF_HARDLINK)) {
                size += c->size;
                disk += c->disk;
            }
            items += 1 + c->items;
            stack.push_back(c.get());
        }
        if (n->size != size || n->disk != disk || n->items != items) return false;
    }
    return true;
}

static std::string Body(const std::string& s) {
    size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(nl);
}

struct Stats {
    int accepted = 0, rejected = 0, bad = 0;
};

// Imports `data`; returns an empty string if all is well, else what went wrong.
static std::string Probe(const std::string& data, Stats& st) {
    WriteAll(g_in, data);
    std::atomic<uint64_t> count{0};
    std::wstring err;
    auto root = ImportNcduJson(g_in, count, err);
    if (!root) {
        ++st.rejected;
        return err.empty() ? "rejected without a message" : "";
    }
    ++st.accepted;
    if (!err.empty()) return "accepted with an error message";
    if (!root->IsDir()) return "root is not a folder";
    if (!Sound(root.get())) return "tree not sound";
    if (!ExportNcduJson(root.get(), g_out, count, err)) return "export failed";
    std::string first = ReadAll(g_out);
    auto again = ImportNcduJson(g_out, count, err);
    if (!again) return "own export not importable: " + util::WideToUtf8(err);
    if (!ExportNcduJson(again.get(), g_out, count, err)) return "second export failed";
    if (Body(ReadAll(g_out)) != Body(first)) return "export -> import -> export differs";
    return "";
}

static std::atomic<int> g_iteration{-1};

static void Run() {
    const int kIterations = 5000;
    auto seeds = Seeds();
    Stats st;
    int reported = 0;
    auto note = [&](const std::string& what, const std::string& input, const wchar_t* label) {
        if (what.empty()) return;
        ++st.bad;
        if (++reported > 5) return;
        Check(false, label + util::Utf8ToWide(": " + what + " (" + std::to_string(input.size()) + " bytes)"));
    };
    for (size_t i = 0; i < seeds.size(); ++i) {
        std::string what = Probe(seeds[i], st);
        Check(what.empty() && st.accepted == static_cast<int>(i) + 1, L"seed " + std::to_wstring(i) + L" imports cleanly");
    }

    // Hand-made worst cases.
    std::vector<std::string> fixed = {
        "",
        "\xEF\xBB\xBF",
        "[",
        "[1",
        "[1,",
        "[1,2,{},",
        "[1,2,{},[",
        "[1,2,{},[{",
        "[1,2,{},[{\"name\":\"\\",
        "[1,2,{},[{\"name\":\"\\u",
        "[1,2,{},[{\"name\":\"/\",\"asize\":",
        "[1,2,{},[{\"name\":\"/\"},",
        std::string(200000, '['),
        "[1,2," + std::string(200000, '[') + std::string(200000, ']') + ",[{\"name\":\"/\"}]]",
        std::string(100000, '"'),
        std::string(100000, '\\'),
        std::string(100000, '\0'),
        "[1,2,{},[{\"name\":\"/\",\"asize\":" + std::string(100000, '9') + "}]]",
        "[1,2,{},[{\"name\":\"/\",\"asize\":0." + std::string(100000, '0') + "1}]]",
        "[1,2,{},[{\"name\":\"" + std::string(100000, '\xFF') + "\"}]]",
    };
    std::string chain = "[1,2,{},";
    for (int i = 0; i < 16384; ++i) chain += "[{\"name\":\"d\"},";
    chain += "{\"name\":\"f\",\"asize\":1}" + std::string(16384, ']') + "]";
    fixed.push_back(chain);
    std::string flat = "[1,2,{},[{\"name\":\"/\"}";
    for (int i = 0; i < 50000; ++i) flat += ",{\"name\":\"f" + std::to_string(i) + "\",\"asize\":1}";
    fixed.push_back(flat + "]]");
    Stats fs;
    for (size_t i = 0; i < fixed.size(); ++i) note(Probe(fixed[i], fs), fixed[i], (L"worst case " + std::to_wstring(i)).c_str());
    Check(fs.accepted == 5 && fs.rejected == static_cast<int>(fixed.size()) - 5,
          L"worst cases: huge numbers, invalid UTF-8 names, 16384 levels and 50000 items import; the rest fail with a message");

    Rng rng{0x5EED5EEDull};
    ULONGLONG start = GetTickCount64();
    Stats ms;
    for (int i = 0; i < kIterations; ++i) {
        g_iteration = i;
        const std::string& seed = seeds[rng.Below(seeds.size())];
        std::string input = Mutate(seed, seeds, rng);
        note(Probe(input, ms), input, (L"mutation " + std::to_wstring(i)).c_str());
    }
    g_iteration = -2;
    double secs = static_cast<double>(GetTickCount64() - start) / 1000.0;
    Check(ms.bad == 0, L"fuzzing: " + std::to_wstring(kIterations) + L" mutated files, " + std::to_wstring(ms.accepted) +
                           L" imported, " + std::to_wstring(ms.rejected) + L" rejected, " + std::to_wstring(ms.bad) +
                           L" wrong (" + util::FormatDuration(secs) + L")");
    Check(ms.accepted > 50 && ms.rejected > 50, L"fuzzing reaches both outcomes");
}

int main() {
    wchar_t tb[MAX_PATH];
    std::wstring tmp(tb, GetTempPathW(MAX_PATH, tb));
    std::wstring pid = std::to_wstring(GetCurrentProcessId());
    g_in = tmp + L"bytopsy-fuzz-" + pid + L".json";
    g_out = tmp + L"bytopsy-fuzz-" + pid + L".out.json";

    // Imports run as tasks on a 16 MB stack in the app; a hang shows up as a timeout here.
    HANDLE h = util::StartThread(Run);
    DWORD wait = h ? WaitForSingleObject(h, 120000) : WAIT_OBJECT_0;
    if (wait != WAIT_OBJECT_0) {
        Check(false, L"importer hung at mutation " + std::to_wstring(g_iteration.load()));
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    if (h) CloseHandle(h);
    DeleteFileW(g_in.c_str());
    DeleteFileW(g_out.c_str());
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
