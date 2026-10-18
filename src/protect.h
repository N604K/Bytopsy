#pragma once

// Protected locations: system folders that must never be deleted, and folders that hold data
// people can't get back (AI assistant sessions, keys, wallets, mail, browser profiles, ...).
// Every delete path (GUI, terminal app, Cleanup) asks this module first.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct Node;

enum class Protect : uint8_t {
    None = 0,
    Caution = 1,    // shown as a heads-up in the delete dialog
    Important = 2,  // extra confirmation step, Recycle Bin preselected
    Blocked = 3,    // never deleted by WinNCDU
};

enum class ProtectRel : uint8_t {
    Self,      // the item itself is protected
    Inside,    // the item is inside a protected folder
    Contains,  // the item contains a protected folder or file
};

struct ProtectHit {
    Protect level = Protect::None;
    ProtectRel rel = ProtectRel::Self;
    std::wstring group;   // short label: "System", "AI", "Keys", ...
    std::wstring name;    // "Claude Code (sessions, memory, settings)"
    std::wstring reason;  // one sentence: what is lost / what to do instead
    std::wstring path;    // the protected location (or the first match inside the item)
    uint64_t count = 1;   // number of matches (e.g. Git repositories inside a folder)
};

struct ProtectReport {
    Protect level = Protect::None;
    std::vector<ProtectHit> hits;  // most severe first
    bool truncated = false;        // the scanned subtree was too large to check completely

    bool Any() const { return level != Protect::None; }
};

// Reloads the user rule file and re-resolves which protected locations exist on this PC.
// Call it before checking a batch of delete targets (cheap: a few hundred attribute lookups).
void RefreshProtection();

// Checks one delete target. With `node` (an item of a scan tree), the scanned subtree is
// also searched for protected files and folders (Git repositories, password databases, ...).
ProtectReport CheckProtection(const std::wstring& path, const Node* node = nullptr);

// Shield badges: what the folder list and treemap show for an item of a scan tree, before anything
// is selected. CheckProtection(path, node) walks the whole subtree, too slow for every visible row;
// this cache checks each item once and summarizes each folder's name-rule matches (Git
// repositories, password databases, ...) bottom-up, so a subtree is walked at most once.
class ProtectBadges {
public:
    // The most severe protection of `n` (level None: nothing to show). Valid until Clear().
    const ProtectHit& For(const Node* n);
    // For a list of a folder's contents: null when `n` is only protected because the folder it is
    // listed in is (ForContents says that once, above the list), else the same as For().
    const ProtectHit* InList(const Node* n);
    // The folder's own protection when it covers the items listed in it, else null.
    const ProtectHit* ForContents(const Node* dir);
    // Call when the scan tree changed (new scan, delete, rescan). Reloaded rules clear it by themselves.
    void Clear();

private:
    struct Below {
        Protect level = Protect::None;
        size_t rule = 0;     // first name rule matched at `level`
        bool mixed = false;  // other rules matched at the same level too
        uint64_t count = 0;  // matches at `level`
        const Node* first = nullptr;
    };
    const Below& Summarize(const Node* dir);
    int MatchName(const Node* n) const;

    std::unordered_map<const Node*, ProtectHit> hits_;
    std::unordered_map<const Node*, Below> below_;  // folders only
    std::vector<size_t> nameRules_;
    unsigned gen_ = 0;
};

// The name rules (Git repositories, password databases, ...) for walking a folder on disk rather
// than a scan tree: a snapshot of the current rules, safe to use from a worker thread.
class ProtectNames {
public:
    ProtectNames();
    // How protected a folder is that contains an item with this name (None: not at all).
    Protect Contains(const std::wstring& name, bool isDir) const;

private:
    struct R {
        std::wstring pat;  // lowercase
        bool fileOnly;
        Protect contains;
    };
    std::vector<R> rules_;
};

// Every built-in or user protected location that exists on this PC (for listings).
std::vector<ProtectHit> ProtectedLocationsOnThisPC();

const wchar_t* ProtectLevelName(Protect p);
// "AI · Claude Code (sessions, memory, settings) · contains C:\Users\me\.claude"
std::wstring DescribeHit(const ProtectHit& h);
// protect.txt in util::DataDir() (created with a commented template on first use).
std::wstring ProtectRulesFile();
