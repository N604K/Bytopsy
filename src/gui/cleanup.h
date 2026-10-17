#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct CleanTarget {
    std::wstring path;         // may contain one or more '*' segments and %ENV% variables
    std::wstring filePattern;  // if set: only files in `path` matching this pattern
    int minAgeHours = 0;       // skip entries modified more recently
};

// One thing a category deletes: a top-level file or folder of a target, or one Recycle Bin item.
// The preview lists exactly these, and cleaning deletes exactly these (minus the kept ones).
struct CleanEntry {
    std::wstring path;              // Recycle Bin items: where the item was deleted from
    std::wstring binData, binInfo;  // Recycle Bin items: the $R and $I files
    uint64_t bytes = 0, files = 0;
    int64_t mtime = 0;              // Recycle Bin items: when it was deleted
    int target = -1;                // index into CleanCategory::targets
    bool dir = false;
    bool keep = false;  // unchecked in the preview: never deleted
};

struct CleanCategory {
    std::wstring id, name, desc;
    wchar_t icon = 0;
    std::vector<CleanTarget> targets;
    bool needsAdmin = false;
    bool recycleBin = false;
    bool external = false;  // handled by an external tool (Windows Disk Cleanup)
    bool safe = true;       // preselected

    // Measurement results.
    uint64_t bytes = 0, files = 0;
    bool measured = false, denied = false, selected = false;
    std::vector<CleanEntry> entries;  // largest first

    uint64_t KeptBytes() const;
    size_t KeptCount() const;
    uint64_t ToClean() const;  // what cleaning would free now
};

struct CleanResult {
    uint64_t freed = 0, files = 0, failed = 0;
    std::vector<std::wstring> lockedBy;  // apps that held skipped items open
};

std::vector<CleanCategory> BuildCleanupCategories();
void MeasureCategory(CleanCategory& c, std::atomic<bool>& cancel);
// Deletes the measured entries that aren't kept, permanently. Entries that changed since they
// were measured and no longer qualify (too new now) are left alone.
CleanResult CleanCategoryNow(const CleanCategory& c, std::atomic<bool>& cancel,
                             const std::function<void(const std::wstring&)>& onItem);
std::vector<std::wstring> ExpandedPaths(const CleanCategory& c);
bool IsElevated();
