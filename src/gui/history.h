#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../tree.h"

// Each completed scan stores a compact snapshot of its folder sizes so later scans can be compared.
struct SnapshotInfo {
    std::wstring file;
    std::wstring root;
    int64_t time = 0;  // FILETIME
    uint64_t size = 0, disk = 0, items = 0;
    uint32_t dirs = 0;
};

struct DirSize {
    uint64_t size = 0, disk = 0, items = 0;
    std::wstring path;  // original spelling
};

struct DiffEntry {
    std::wstring path;
    Node* node = nullptr;  // null if the folder no longer exists
    int64_t oldVal = 0, newVal = 0;
    int64_t delta() const { return newVal - oldVal; }
};

std::wstring AppDataDir();
bool SaveSnapshot(const Node* root, std::wstring& err);
std::vector<SnapshotInfo> ListSnapshots(const std::wstring& rootFilter = L"");
bool LoadSnapshot(const std::wstring& file, std::unordered_map<std::wstring, DirSize>& dirs, SnapshotInfo& info);
// Folders that explain the change best (the most specific folder carrying most of a change).
std::vector<DiffEntry> CompareWithSnapshot(Node* root, const std::unordered_map<std::wstring, DirSize>& old, bool useDisk);
void DeleteSnapshot(const std::wstring& file);
