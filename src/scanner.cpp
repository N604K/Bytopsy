#include "scanner.h"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <thread>
#include <unordered_set>

#include "util.h"

#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif

void ScanStats::SetCurrent(const std::wstring& p) {
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (lk.owns_lock()) current_ = p;
}

std::wstring ScanStats::GetCurrent() {
    std::lock_guard<std::mutex> lk(mutex_);
    return current_;
}

uint64_t GetClusterSize(const std::wstring& path) {
    wchar_t vol[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(path.c_str(), vol, MAX_PATH)) return 0;
    DWORD spc = 0, bps = 0, fc = 0, tc = 0;
    if (GetDiskFreeSpaceW(vol, &spc, &bps, &fc, &tc)) return static_cast<uint64_t>(spc) * bps;
    return 0;
}

bool ReadNodeMeta(Node* n, const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(ToLongPath(path).c_str(), GetFileExInfoStandard, &d)) return false;
    n->attrs = d.dwFileAttributes;
    n->mtime = (static_cast<int64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        n->size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return true;
}

namespace {

struct Task {
    Node* dir;
    std::wstring path;
};

struct HlKey {
    uint32_t vol;
    uint64_t idx;
    bool operator==(const HlKey&) const = default;
};
struct HlHash {
    size_t operator()(const HlKey& k) const { return std::hash<uint64_t>()(k.idx * 31 + k.vol); }
};

struct Context {
    const ScanOptions& opt;
    ScanStats& stats;
    uint64_t cluster = 0;

    std::mutex m;
    std::condition_variable cv;
    std::vector<Task> stack;
    size_t pending = 0;
    bool finished = false;

    std::mutex hlMutex;
    std::unordered_set<HlKey, HlHash> hardlinks;
};

bool IsExcluded(const ScanOptions& opt, const std::wstring& name, const std::wstring& fullPath) {
    for (const auto& p : opt.excludes) {
        bool pathPattern = p.find(L'\\') != std::wstring::npos;
        if (util::GlobMatchI(p, pathPattern ? fullPath : name)) return true;
    }
    return false;
}

uint64_t RoundToCluster(uint64_t size, uint64_t cluster) {
    if (!cluster) return size;
    return (size + cluster - 1) / cluster * cluster;
}

void ProcessDir(Context& ctx, Task& t, std::vector<Task>& out) {
    ScanStats& stats = ctx.stats;
    stats.SetCurrent(t.path);
    std::wstring pattern = ToLongPath(t.path);
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_NO_MORE_FILES) {
            t.dir->flags |= NF_ERROR;
            ++stats.errors;
        }
        return;
    }
    const bool haveExcludes = !ctx.opt.excludes.empty();
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        auto c = std::make_unique<Node>();
        c->name = fd.cFileName;
        c->parent = t.dir;
        c->attrs = fd.dwFileAttributes;
        c->mtime = (static_cast<int64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        const DWORD a = fd.dwFileAttributes;
        const bool isDir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool isReparse = (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        const bool isLink = isReparse && IsReparseTagNameSurrogate(fd.dwReserved0);
        if (a & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE))
            c->flags |= NF_CLOUD;

        std::wstring childPath;
        if (isDir || haveExcludes || isReparse || ctx.opt.hardlinks ||
            (a & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) || c->Has(NF_CLOUD))
            childPath = JoinPath(t.path, c->name);

        if (haveExcludes && IsExcluded(ctx.opt, c->name, childPath)) {
            c->flags |= NF_EXCLUDED | (isDir ? NF_DIR : 0);
        } else if (isDir) {
            c->flags |= NF_DIR;
            ++stats.dirs;
            if (isLink)
                c->flags |= NF_LINK;
            else
                out.push_back(Task{c.get(), childPath});
        } else {
            ++stats.files;
            if (isLink) {
                c->flags |= NF_LINK;
            } else {
                c->size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                bool queried = false;
                if (isReparse || c->Has(NF_CLOUD) || (a & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE))) {
                    // Compressed (NTFS/WOF), sparse or cloud files: ask for the real allocation.
                    DWORD hi = 0;
                    DWORD lo = GetCompressedFileSizeW(ToLongPath(childPath).c_str(), &hi);
                    if (lo != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) {
                        c->disk = RoundToCluster((static_cast<uint64_t>(hi) << 32) | lo, ctx.cluster);
                        queried = true;
                    }
                }
                if (!queried) c->disk = RoundToCluster(c->size, ctx.cluster);

                if (ctx.opt.hardlinks) {
                    HANDLE fh = CreateFileW(ToLongPath(childPath).c_str(), FILE_READ_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                            nullptr);
                    if (fh != INVALID_HANDLE_VALUE) {
                        BY_HANDLE_FILE_INFORMATION bi;
                        if (GetFileInformationByHandle(fh, &bi) && bi.nNumberOfLinks > 1) {
                            HlKey key{bi.dwVolumeSerialNumber,
                                      (static_cast<uint64_t>(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow};
                            std::lock_guard<std::mutex> lk(ctx.hlMutex);
                            if (!ctx.hardlinks.insert(key).second) c->flags |= NF_HARDLINK;
                        }
                        CloseHandle(fh);
                    }
                }
                if (!c->Has(NF_HARDLINK)) stats.bytes += c->size;
            }
        }
        t.dir->children.push_back(std::move(c));
    } while (!stats.cancel.load(std::memory_order_relaxed) && FindNextFileW(h, &fd));

    DWORD e = GetLastError();
    if (!stats.cancel && e != ERROR_NO_MORE_FILES) {
        t.dir->flags |= NF_ERROR;
        ++stats.errors;
    }
    if (stats.cancel) t.dir->flags |= NF_ERROR;
    FindClose(h);
}

void Worker(Context* ctx) {
    std::vector<Task> local;
    for (;;) {
        Task t;
        {
            std::unique_lock<std::mutex> lk(ctx->m);
            ctx->cv.wait(lk, [&] { return !ctx->stack.empty() || ctx->finished; });
            if (ctx->stack.empty()) return;
            t = std::move(ctx->stack.back());
            ctx->stack.pop_back();
        }
        if (ctx->stats.cancel.load(std::memory_order_relaxed))
            t.dir->flags |= NF_ERROR;  // not scanned
        else
            ProcessDir(*ctx, t, local);
        {
            std::lock_guard<std::mutex> lk(ctx->m);
            for (auto& nt : local) ctx->stack.push_back(std::move(nt));
            ctx->pending += local.size();
            --ctx->pending;
            if (ctx->pending == 0) {
                ctx->finished = true;
                ctx->cv.notify_all();
            } else if (!local.empty()) {
                ctx->cv.notify_all();
            }
        }
        local.clear();
    }
}

}  // namespace

void ScanDirectory(Node* dir, const std::wstring& path, const ScanOptions& opt, ScanStats& stats) {
    Context ctx{opt, stats};
    ctx.cluster = GetClusterSize(path);
    ctx.stack.push_back(Task{dir, path});
    ctx.pending = 1;
    int n = std::clamp(opt.threads, 1, 64);
    std::vector<std::thread> threads;
    threads.reserve(n);
    for (int i = 0; i < n; ++i) threads.emplace_back(Worker, &ctx);
    for (auto& t : threads) t.join();
}
