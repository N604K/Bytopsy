#include "history.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>

#include "../util.h"

namespace {

const char kMagic[4] = {'W', 'N', 'C', 'H'};
const uint32_t kVersion = 1;

uint64_t Fnv(const std::wstring& s) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring HistoryDir() {
    std::wstring d = AppDataDir() + L"\\history";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

class Writer {
public:
    explicit Writer(FILE* f) : f_(f) {}
    ~Writer() { Flush(); }
    template <class T>
    void Put(const T& v) {
        const char* p = reinterpret_cast<const char*>(&v);
        buf_.append(p, sizeof(T));
        if (buf_.size() > (1 << 20)) Flush();
    }
    void Str(const std::wstring& s) {
        Put(static_cast<uint16_t>(std::min<size_t>(s.size(), 65535)));
        buf_.append(reinterpret_cast<const char*>(s.data()), std::min<size_t>(s.size(), 65535) * 2);
    }
    bool Flush() {
        if (!buf_.empty() && fwrite(buf_.data(), 1, buf_.size(), f_) != buf_.size()) ok_ = false;
        buf_.clear();
        return ok_;
    }

private:
    FILE* f_;
    std::string buf_;
    bool ok_ = true;
};

class Reader {
public:
    Reader(const std::string& d) : d_(d) {}
    template <class T>
    bool Get(T& v) {
        if (pos_ + sizeof(T) > d_.size()) return false;
        memcpy(&v, d_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }
    bool Str(std::wstring& s) {
        uint16_t n;
        if (!Get(n) || pos_ + n * 2u > d_.size()) return false;
        s.assign(reinterpret_cast<const wchar_t*>(d_.data() + pos_), n);
        pos_ += n * 2u;
        return true;
    }

private:
    const std::string& d_;
    size_t pos_ = 0;
};

bool ReadFileAll(const std::wstring& file, std::string& out, size_t limit = SIZE_MAX) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") != 0 || !f) return false;
    char buf[65536];
    size_t n;
    while (out.size() < limit && (n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool ReadHeader(Reader& r, SnapshotInfo& info) {
    char magic[4];
    uint32_t ver;
    for (char& c : magic)
        if (!r.Get(c)) return false;
    if (memcmp(magic, kMagic, 4) != 0 || !r.Get(ver) || ver != kVersion) return false;
    return r.Get(info.time) && r.Get(info.size) && r.Get(info.disk) && r.Get(info.items) && r.Get(info.dirs) &&
           r.Str(info.root);
}

}  // namespace

std::wstring AppDataDir() {
    std::wstring d = util::DataDir();
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

bool SaveSnapshot(const Node* root, std::wstring& err) {
    std::vector<std::pair<const Node*, uint16_t>> stack{{root, uint16_t{0}}};
    uint32_t dirs = 0;
    // Count directories first (header field).
    {
        std::vector<const Node*> st{root};
        while (!st.empty()) {
            const Node* n = st.back();
            st.pop_back();
            ++dirs;
            for (auto& c : n->children)
                if (c->CanEnter()) st.push_back(c.get());
        }
    }
    int64_t now = util::NowFileTime();
    wchar_t name[64];
    swprintf(name, 64, L"\\%016llx_%016llx.wsnap", static_cast<unsigned long long>(Fnv(util::ToLower(root->name))),
             static_cast<unsigned long long>(now));
    std::wstring file = HistoryDir() + name;
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") != 0 || !f) {
        err = L"Cannot write " + file;
        return false;
    }
    bool ok;
    {
        Writer w(f);
        for (char c : kMagic) w.Put(c);
        w.Put(kVersion);
        w.Put(now);
        w.Put(root->size);
        w.Put(root->disk);
        w.Put(root->items);
        w.Put(dirs);
        w.Str(root->name);
        while (!stack.empty()) {
            auto [n, depth] = stack.back();
            stack.pop_back();
            w.Put(depth);
            w.Str(depth == 0 ? std::wstring() : n->name);
            w.Put(n->size);
            w.Put(n->disk);
            w.Put(n->items);
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                if ((*it)->CanEnter()) stack.push_back({it->get(), static_cast<uint16_t>(depth + 1)});
        }
        ok = w.Flush();
    }
    fclose(f);
    if (!ok) {
        DeleteFileW(file.c_str());
        err = L"Write error";
        return false;
    }
    // Keep the 12 most recent snapshots per root.
    auto all = ListSnapshots(root->name);
    for (size_t i = 12; i < all.size(); ++i) DeleteFileW(all[i].file.c_str());
    return true;
}

std::vector<SnapshotInfo> ListSnapshots(const std::wstring& rootFilter) {
    std::vector<SnapshotInfo> out;
    std::wstring dir = HistoryDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.wsnap").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    std::wstring want = util::ToLower(rootFilter);
    do {
        std::string data;
        std::wstring file = dir + L"\\" + fd.cFileName;
        if (!ReadFileAll(file, data, 70000)) continue;
        Reader r(data);
        SnapshotInfo info;
        if (!ReadHeader(r, info)) continue;
        info.file = file;
        if (!want.empty() && util::ToLower(info.root) != want) continue;
        out.push_back(info);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const SnapshotInfo& a, const SnapshotInfo& b) { return a.time > b.time; });
    return out;
}

bool LoadSnapshot(const std::wstring& file, std::unordered_map<std::wstring, DirSize>& dirs, SnapshotInfo& info) {
    std::string data;
    if (!ReadFileAll(file, data)) return false;
    Reader r(data);
    if (!ReadHeader(r, info)) return false;
    info.file = file;
    std::vector<std::wstring> stack;
    dirs.reserve(info.dirs);
    for (uint32_t i = 0; i < info.dirs; ++i) {
        uint16_t depth;
        std::wstring name;
        DirSize d;
        if (!r.Get(depth) || !r.Str(name) || !r.Get(d.size) || !r.Get(d.disk) || !r.Get(d.items)) return false;
        stack.resize(depth);
        std::wstring path = depth == 0 ? info.root : JoinPath(stack.back(), name);
        stack.push_back(path);
        d.path = path;
        dirs[util::ToLower(path)] = d;
    }
    return true;
}

std::vector<DiffEntry> CompareWithSnapshot(Node* root, const std::unordered_map<std::wstring, DirSize>& old, bool useDisk) {
    struct Item {
        Node* node;
        std::wstring path;
        int64_t oldVal, newVal;
    };
    // Walk the current tree, recording deltas per folder.
    std::unordered_map<Node*, int64_t> delta;
    std::vector<Item> items;
    std::unordered_map<std::wstring, bool> seen;
    std::vector<std::pair<Node*, std::wstring>> st{{root, root->name}};
    while (!st.empty()) {
        auto [n, path] = st.back();
        st.pop_back();
        std::wstring key = util::ToLower(path);
        auto it = old.find(key);
        int64_t ov = it == old.end() ? 0 : static_cast<int64_t>(useDisk ? it->second.disk : it->second.size);
        int64_t nv = static_cast<int64_t>(useDisk ? n->disk : n->size);
        seen[key] = true;
        delta[n] = nv - ov;
        items.push_back({n, path, ov, nv});
        for (auto& c : n->children)
            if (c->CanEnter()) st.push_back({c.get(), JoinPath(path, c->name)});
    }
    int64_t total = static_cast<int64_t>(useDisk ? root->disk : root->size);
    int64_t threshold = std::max<int64_t>(1 << 20, total / 2000);
    std::vector<DiffEntry> out;
    for (auto& it : items) {
        int64_t d = it.newVal - it.oldVal;
        if (std::llabs(d) < threshold) continue;
        // Skip if one subfolder already explains most of this change.
        bool explained = false;
        for (auto& c : it.node->children) {
            if (!c->CanEnter()) continue;
            int64_t cd = delta[c.get()];
            if ((cd > 0) == (d > 0) && std::llabs(cd) >= std::llabs(d) * 8 / 10) {
                explained = true;
                break;
            }
        }
        if (explained) continue;
        out.push_back({it.path, it.node, it.oldVal, it.newVal});
    }
    // Folders that disappeared (only the topmost missing one).
    for (auto& kv : old) {
        if (seen.count(kv.first)) continue;
        size_t slash = kv.first.find_last_of(L'\\');
        if (slash == std::wstring::npos) continue;
        std::wstring parent = kv.first.substr(0, slash);
        if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
        if (!seen.count(parent)) continue;
        int64_t ov = static_cast<int64_t>(useDisk ? kv.second.disk : kv.second.size);
        if (ov < threshold) continue;
        out.push_back({kv.second.path, nullptr, ov, 0});
    }
    std::sort(out.begin(), out.end(), [](const DiffEntry& a, const DiffEntry& b) {
        return std::llabs(a.delta()) > std::llabs(b.delta());
    });
    if (out.size() > 2000) out.resize(2000);
    return out;
}

void DeleteSnapshot(const std::wstring& file) { DeleteFileW(file.c_str()); }
