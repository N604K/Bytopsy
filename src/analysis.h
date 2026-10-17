#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "tree.h"

// Query syntax (space separated, all terms must match):
//   word / "two words"   name contains text (case-insensitive)
//   *.iso, foo?.txt      wildcard match on the name
//   >100M  <1G           size limits (also size:>100M)
//   older:30d newer:2w   modification age (h, d, w, m, y)
//   type:f | type:d      only files / only folders
//   ext:mp4,mkv          by extension
//   empty                empty folders / zero byte files
struct SearchQuery {
    std::vector<std::wstring> names;  // lowercase
    std::vector<std::wstring> exts;   // lowercase, without dot
    uint64_t minSize = 0;
    uint64_t maxSize = UINT64_MAX;
    int64_t olderThan = 0;  // FILETIME cutoffs (0 = unused)
    int64_t newerThan = 0;
    int type = 0;  // 0 any, 1 files, 2 dirs
    bool empty = false;
};

bool ParseQuery(const std::wstring& text, SearchQuery& q, std::wstring& err);
std::vector<Node*> RunSearch(Node* scope, const SearchQuery& q, bool useDisk);
std::vector<Node*> LargestFiles(Node* scope, size_t n, bool useDisk);

struct ExtStat {
    std::wstring ext;  // lowercase, "" for none
    uint64_t size = 0, disk = 0, count = 0;
};
std::wstring FileExt(const std::wstring& name);
std::vector<ExtStat> ExtensionStats(Node* scope, bool useDisk);
std::vector<Node*> FilesWithExt(Node* scope, const std::wstring& ext, bool useDisk);

struct DupGroup {
    uint64_t size = 0;
    std::vector<Node*> files;  // oldest first
};

struct DupProgress {
    std::atomic<int> stage{0};  // 0 collect, 1 quick hash, 2 full hash
    std::atomic<uint64_t> filesDone{0}, filesTotal{0}, bytesDone{0}, bytesTotal{0};
    std::atomic<bool> cancel{false};
};

// Groups files with identical content (size, then partial SHA-256, then full SHA-256).
// Cloud placeholders, links and hard-link duplicates are skipped.
std::vector<DupGroup> FindDuplicates(Node* scope, uint64_t minSize, int threads, DupProgress& prog);
