#include "analysis.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <map>
#include <thread>
#include <unordered_map>

#include "util.h"

namespace {

template <class F>
void Walk(Node* root, F&& f) {
    std::vector<Node*> stack{root};
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        for (auto& c : n->children) {
            f(c.get());
            if (!c->children.empty()) stack.push_back(c.get());
        }
    }
}

std::vector<std::wstring> Tokenize(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool quoted = false, have = false;
    for (wchar_t c : s) {
        if (c == L'"') {
            quoted = !quoted;
            have = true;
        } else if (!quoted && (c == L' ' || c == L'\t')) {
            if (have) out.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur += c;
            have = true;
        }
    }
    if (have) out.push_back(cur);
    return out;
}

bool ParseAge(std::wstring_view s, int64_t& ticks) {
    size_t i = 0;
    while (i < s.size() && (iswdigit(s[i]) || s[i] == L'.')) ++i;
    if (i == 0) return false;
    double v = _wtof(std::wstring(s.substr(0, i)).c_str());
    std::wstring_view unit = s.substr(i);
    double hours;
    if (unit.empty() || unit == L"d") hours = 24;
    else if (unit == L"h") hours = 1;
    else if (unit == L"w") hours = 24 * 7;
    else if (unit == L"m") hours = 24 * 30;
    else if (unit == L"y") hours = 24 * 365;
    else return false;
    ticks = static_cast<int64_t>(v * hours * 3600.0 * 1e7);
    return true;
}

uint64_t Val(const Node* n, bool useDisk) { return useDisk ? n->disk : n->size; }

void SortBySize(std::vector<Node*>& v, bool useDisk) {
    std::sort(v.begin(), v.end(), [useDisk](Node* a, Node* b) {
        uint64_t va = Val(a, useDisk), vb = Val(b, useDisk);
        if (va != vb) return va > vb;
        return a->name < b->name;
    });
}

}  // namespace

bool ParseQuery(const std::wstring& text, SearchQuery& q, std::wstring& err) {
    q = SearchQuery{};
    auto tokens = Tokenize(text);
    if (tokens.empty()) {
        err = L"Empty query";
        return false;
    }
    int64_t now = util::NowFileTime();
    for (auto tok : tokens) {
        std::wstring t = util::ToLower(tok);
        if (t.rfind(L"size:", 0) == 0) t = t.substr(5);
        if (!t.empty() && (t[0] == L'>' || t[0] == L'<')) {
            bool greater = t[0] == L'>';
            size_t start = (t.size() > 1 && t[1] == L'=') ? 2 : 1;
            uint64_t v;
            if (!util::ParseSize(t.substr(start), v)) {
                err = L"Bad size: " + tok;
                return false;
            }
            if (greater)
                q.minSize = std::max(q.minSize, v);
            else
                q.maxSize = std::min(q.maxSize, v);
        } else if (t.rfind(L"older:", 0) == 0 || t.rfind(L"newer:", 0) == 0) {
            int64_t ticks;
            if (!ParseAge(t.substr(6), ticks)) {
                err = L"Bad age (use e.g. 30d, 2w, 6m, 1y): " + tok;
                return false;
            }
            if (t[0] == L'o')
                q.olderThan = now - ticks;
            else
                q.newerThan = now - ticks;
        } else if (t.rfind(L"type:", 0) == 0) {
            std::wstring v = t.substr(5);
            if (v == L"f" || v == L"file" || v == L"files")
                q.type = 1;
            else if (v == L"d" || v == L"dir" || v == L"folder" || v == L"dirs" || v == L"folders")
                q.type = 2;
            else {
                err = L"Bad type (use type:f or type:d): " + tok;
                return false;
            }
        } else if (t.rfind(L"ext:", 0) == 0) {
            std::wstring v = t.substr(4);
            size_t pos = 0;
            while (pos <= v.size()) {
                size_t comma = v.find(L',', pos);
                std::wstring e = v.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
                if (!e.empty() && e[0] == L'.') e.erase(e.begin());
                if (!e.empty()) q.exts.push_back(e);
                if (comma == std::wstring::npos) break;
                pos = comma + 1;
            }
        } else if (t == L"empty") {
            q.empty = true;
        } else {
            q.names.push_back(t);
        }
    }
    return true;
}

std::vector<Node*> RunSearch(Node* scope, const SearchQuery& q, bool useDisk) {
    std::vector<Node*> out;
    Walk(scope, [&](Node* n) {
        bool dir = n->IsDir();
        if (q.type == 1 && dir) return;
        if (q.type == 2 && !dir) return;
        uint64_t v = Val(n, useDisk);
        if (v < q.minSize || v > q.maxSize) return;
        if (q.olderThan && (n->mtime <= 0 || n->mtime >= q.olderThan)) return;
        if (q.newerThan && n->mtime < q.newerThan) return;
        if (q.empty && (dir ? !n->children.empty() : n->size != 0)) return;
        if (!q.exts.empty()) {
            if (dir) return;
            std::wstring e = FileExt(n->name);
            if (std::find(q.exts.begin(), q.exts.end(), e) == q.exts.end()) return;
        }
        for (const auto& p : q.names) {
            bool ok = util::HasWildcards(p) ? util::GlobMatchI(p, n->name) : util::ContainsI(n->name, p);
            if (!ok) return;
        }
        out.push_back(n);
    });
    SortBySize(out, useDisk);
    return out;
}

std::vector<Node*> LargestFiles(Node* scope, size_t n, bool useDisk) {
    std::vector<Node*> files;
    Walk(scope, [&](Node* c) {
        if (!c->IsDir() && !c->Has(NF_HARDLINK)) files.push_back(c);
    });
    size_t k = std::min(n, files.size());
    std::partial_sort(files.begin(), files.begin() + static_cast<ptrdiff_t>(k), files.end(), [useDisk](Node* a, Node* b) {
        return Val(a, useDisk) > Val(b, useDisk);
    });
    files.resize(k);
    return files;
}

std::wstring FileExt(const std::wstring& name) {
    size_t pos = name.rfind(L'.');
    if (pos == std::wstring::npos || pos == 0 || pos + 1 == name.size()) return L"";
    return util::ToLower(std::wstring_view(name).substr(pos + 1));
}

std::vector<ExtStat> ExtensionStats(Node* scope, bool useDisk) {
    std::unordered_map<std::wstring, ExtStat> map;
    Walk(scope, [&](Node* n) {
        if (n->IsDir() || n->Has(NF_HARDLINK)) return;
        std::wstring e = FileExt(n->name);
        ExtStat& s = map[e];
        s.ext = e;
        s.size += n->size;
        s.disk += n->disk;
        ++s.count;
    });
    std::vector<ExtStat> out;
    out.reserve(map.size());
    for (auto& kv : map) out.push_back(std::move(kv.second));
    std::sort(out.begin(), out.end(), [useDisk](const ExtStat& a, const ExtStat& b) {
        uint64_t va = useDisk ? a.disk : a.size, vb = useDisk ? b.disk : b.size;
        if (va != vb) return va > vb;
        return a.ext < b.ext;
    });
    return out;
}

std::vector<Node*> FilesWithExt(Node* scope, const std::wstring& ext, bool useDisk) {
    std::vector<Node*> out;
    Walk(scope, [&](Node* n) {
        if (!n->IsDir() && FileExt(n->name) == ext) out.push_back(n);
    });
    SortBySize(out, useDisk);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Duplicate detection

namespace {

using Digest = std::array<uint8_t, 32>;
constexpr uint64_t kPartialBytes = 64 * 1024;

// Hashes up to `limit` bytes (0 = whole file) of each file in parallel.
void HashFiles(const std::vector<Node*>& files, uint64_t limit, int threads, DupProgress& prog,
               std::vector<Digest>& digests, std::vector<char>& ok) {
    digests.assign(files.size(), Digest{});
    ok.assign(files.size(), 0);
    std::vector<std::wstring> paths(files.size());
    for (size_t i = 0; i < files.size(); ++i) paths[i] = ToLongPath(NodePath(files[i]));

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return;
    std::atomic<size_t> next{0};
    auto worker = [&] {
        std::vector<uint8_t> buf(1 << 20);
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= files.size() || prog.cancel.load()) return;
            uint64_t want = limit ? std::min(limit, files[i]->size) : files[i]->size;
            HANDLE h = CreateFileW(paths[i].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            bool good = false;
            if (h != INVALID_HANDLE_VALUE) {
                BCRYPT_HASH_HANDLE hh = nullptr;
                if (BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) >= 0) {
                    uint64_t got = 0;
                    good = true;
                    while (got < want) {
                        if (prog.cancel.load()) {
                            good = false;
                            break;
                        }
                        DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(buf.size(), want - got));
                        DWORD read = 0;
                        if (!ReadFile(h, buf.data(), chunk, &read, nullptr) || read == 0) {
                            good = false;
                            break;
                        }
                        BCryptHashData(hh, buf.data(), read, 0);
                        got += read;
                        prog.bytesDone += read;
                    }
                    if (good) good = BCryptFinishHash(hh, digests[i].data(), 32, 0) >= 0;
                    BCryptDestroyHash(hh);
                }
                CloseHandle(h);
            }
            ok[i] = good ? 1 : 0;
            ++prog.filesDone;
        }
    };
    std::vector<std::thread> pool;
    int n = std::clamp(threads, 1, 16);
    for (int t = 0; t < n; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    BCryptCloseAlgorithmProvider(alg, 0);
}

// Splits each group by digest, keeping only buckets with 2+ members.
std::vector<std::vector<Node*>> Regroup(const std::vector<std::vector<Node*>>& groups, uint64_t limit, int threads,
                                        DupProgress& prog) {
    std::vector<Node*> flat;
    for (auto& g : groups) flat.insert(flat.end(), g.begin(), g.end());
    prog.filesDone = 0;
    prog.filesTotal = flat.size();
    prog.bytesDone = 0;
    uint64_t total = 0;
    for (Node* n : flat) total += limit ? std::min(limit, n->size) : n->size;
    prog.bytesTotal = total;

    std::vector<Digest> digests;
    std::vector<char> ok;
    HashFiles(flat, limit, threads, prog, digests, ok);

    std::vector<std::vector<Node*>> out;
    size_t idx = 0;
    for (auto& g : groups) {
        std::map<Digest, std::vector<Node*>> buckets;
        for (size_t k = 0; k < g.size(); ++k, ++idx)
            if (ok[idx]) buckets[digests[idx]].push_back(g[k]);
        for (auto& kv : buckets)
            if (kv.second.size() >= 2) out.push_back(std::move(kv.second));
    }
    return out;
}

}  // namespace

std::vector<DupGroup> FindDuplicates(Node* scope, uint64_t minSize, int threads, DupProgress& prog) {
    prog.stage = 0;
    std::unordered_map<uint64_t, std::vector<Node*>> bySize;
    Walk(scope, [&](Node* n) {
        if (n->IsDir() || n->Has(NF_LINK | NF_HARDLINK | NF_EXCLUDED | NF_CLOUD)) return;
        if (n->size == 0 || n->size < minSize) return;
        bySize[n->size].push_back(n);
    });
    std::vector<std::vector<Node*>> groups;
    for (auto& kv : bySize)
        if (kv.second.size() >= 2) groups.push_back(std::move(kv.second));
    if (prog.cancel) return {};

    prog.stage = 1;
    groups = Regroup(groups, kPartialBytes, threads, prog);
    if (prog.cancel) return {};

    // Files larger than the partial block need a full comparison.
    std::vector<std::vector<Node*>> big, done;
    for (auto& g : groups) (g[0]->size > kPartialBytes ? big : done).push_back(std::move(g));
    prog.stage = 2;
    if (!big.empty()) {
        auto confirmed = Regroup(big, 0, threads, prog);
        if (prog.cancel) return {};
        for (auto& g : confirmed) done.push_back(std::move(g));
    }

    std::vector<DupGroup> out;
    for (auto& g : done) {
        DupGroup d;
        d.size = g[0]->size;
        d.files = std::move(g);
        std::sort(d.files.begin(), d.files.end(), [](Node* a, Node* b) {
            if (a->mtime != b->mtime) return a->mtime < b->mtime;
            return a->name < b->name;
        });
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const DupGroup& a, const DupGroup& b) {
        return a.size * (a.files.size() - 1) > b.size * (b.files.size() - 1);
    });
    return out;
}
