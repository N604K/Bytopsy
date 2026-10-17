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
};

std::vector<CleanCategory> BuildCleanupCategories();
void MeasureCategory(CleanCategory& c, std::atomic<bool>& cancel);
// Deletes the category's content permanently. Returns freed bytes (measured before/after).
uint64_t CleanCategoryNow(CleanCategory& c, std::atomic<bool>& cancel, uint64_t& failedItems,
                          const std::function<void(const std::wstring&)>& onItem);
std::vector<std::wstring> ExpandedPaths(const CleanCategory& c);
bool IsElevated();
