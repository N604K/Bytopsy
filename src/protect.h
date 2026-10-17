#pragma once

// Protected locations: system folders that must never be deleted, and folders that hold data
// people can't get back (AI assistant sessions, keys, wallets, mail, browser profiles, ...).
// Every delete path (GUI, terminal app, Cleanup) asks this module first.

#include <cstdint>
#include <string>
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

// Every built-in or user protected location that exists on this PC (for listings).
std::vector<ProtectHit> ProtectedLocationsOnThisPC();

const wchar_t* ProtectLevelName(Protect p);
// "AI · Claude Code (sessions, memory, settings) · contains C:\Users\me\.claude"
std::wstring DescribeHit(const ProtectHit& h);
// %LOCALAPPDATA%\WinNCDU\protect.txt (created with a commented template on first use).
std::wstring ProtectRulesFile();
