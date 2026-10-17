#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum NodeFlags : uint16_t {
    NF_DIR = 1 << 0,
    NF_ERROR = 1 << 1,     // could not read this directory (fully)
    NF_SUBERR = 1 << 2,    // an error occurred somewhere below
    NF_LINK = 1 << 3,      // symlink / junction / mount point, not followed
    NF_EXCLUDED = 1 << 4,  // matched an exclude pattern
    NF_DELETED = 1 << 5,   // detached from the tree (pending free)
    NF_CLOUD = 1 << 6,     // cloud placeholder (OneDrive etc.), not fully on disk
    NF_HARDLINK = 1 << 7,  // another link to an already counted file
};

struct Node {
    std::wstring name;  // root: the full path
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
    uint64_t size = 0;   // apparent size (directories: aggregate)
    uint64_t disk = 0;   // allocated size (directories: aggregate)
    uint64_t items = 0;  // number of descendants
    int64_t mtime = 0;   // FILETIME ticks
    uint32_t attrs = 0;  // FILE_ATTRIBUTE_*
    uint16_t flags = 0;

    bool IsDir() const { return (flags & NF_DIR) != 0; }
    bool Has(uint16_t f) const { return (flags & f) != 0; }
    bool CanEnter() const { return IsDir() && !Has(NF_LINK | NF_EXCLUDED); }
};

std::wstring NodePath(const Node* n);
std::wstring ToLongPath(const std::wstring& path);
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);

// Recomputes aggregate sizes for a whole subtree.
void RecomputeTotals(Node* n);
// Recomputes aggregates of `n` and each ancestor from their direct children.
void RecomputeUp(Node* n);
// False if this node or an ancestor has been detached.
bool IsAlive(const Node* n);
bool IsAncestor(const Node* ancestor, const Node* n);
// Removes `n` from its parent, fixes ancestor totals, and returns ownership.
std::unique_ptr<Node> DetachNode(Node* n);
