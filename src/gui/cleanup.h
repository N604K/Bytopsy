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
    bool enabled = true;    // "enabled = false" in cleaners.ini turns a built-in category off
    bool user = false;      // from the user's cleaners.ini

    // Measurement results.
    uint64_t bytes = 0, files = 0;
    uint64_t guarded = 0;  // items left out because the protection rules cover them
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

// Categories come from res/cleaners.ini (built into the exe) merged with the user's
// %LOCALAPPDATA%\WinNCDU\cleaners.ini; problems in either go to `errors`.
std::vector<CleanCategory> BuildCleanupCategories(std::vector<std::wstring>* errors = nullptr);
std::vector<CleanCategory> ParseCleaners(const std::wstring& text, std::vector<std::wstring>& errors);
// The user's cleaners.ini; `create` writes a commented template if it doesn't exist yet.
std::wstring UserCleanersFile(bool create);
void MeasureCategory(CleanCategory& c, std::atomic<bool>& cancel);
// Deletes the measured entries that aren't kept, permanently. Entries that changed since they
// were measured and no longer qualify (too new now) are left alone.
CleanResult CleanCategoryNow(const CleanCategory& c, std::atomic<bool>& cancel,
                             const std::function<void(const std::wstring&)>& onItem);
std::vector<std::wstring> ExpandedPaths(const CleanCategory& c);
bool IsElevated();
// A System Restore point (needs administrator rights and System Protection on the system drive).
// Windows makes at most one per 24 hours by default; within that window it keeps the recent one.
bool CreateRestorePoint(const std::wstring& description, std::wstring& err);
