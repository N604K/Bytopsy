#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "tree.h"

struct ScanOptions {
    std::vector<std::wstring> excludes;  // lowercase glob patterns; patterns with '\' match full paths
    int threads = 4;
    bool hardlinks = false;  // detect NTFS hard links (opens every file; slower)
};

struct ScanStats {
    std::atomic<uint64_t> files{0}, dirs{0}, bytes{0}, errors{0};
    std::atomic<bool> cancel{false};

    void SetCurrent(const std::wstring& p);
    std::wstring GetCurrent();

private:
    std::mutex mutex_;
    std::wstring current_;
};

// Fills dir->children by scanning `path` recursively with a pool of worker threads.
// Blocks until finished or cancelled. Does NOT compute aggregate totals.
void ScanDirectory(Node* dir, const std::wstring& path, const ScanOptions& opt, ScanStats& stats);

// Reads attributes and modification time of a single path into `n`.
bool ReadNodeMeta(Node* n, const std::wstring& path);
uint64_t GetClusterSize(const std::wstring& path);
