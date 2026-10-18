#pragma once

// The accessible element tree behind the UI Automation provider (uia.cpp). The window is drawn
// with Direct2D, so there are no controls to ask: every frame records what was drawn (text runs,
// clickable regions, checkboxes) and this tree is rebuilt from that. No COM in here, so the pure
// parts (identity, reading order, which text belongs to which element, patterns) are self-tested.

#include <cstdint>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"  // RectF

enum class AccRole { Pane, Toolbar, Navigation, List, Header, HeaderItem, ListItem, Button, CheckBox, RadioButton, Text, Image, Window, StatusBar };

enum AccFlag : uint32_t {
    AF_DISABLED = 1u << 0,
    AF_INVOKE = 1u << 1,      // Invoke pattern
    AF_TOGGLE = 1u << 2,      // Toggle pattern
    AF_ON = 1u << 3,          // toggled on
    AF_MIXED = 1u << 4,       // partly on
    AF_SELECTABLE = 1u << 5,  // SelectionItem pattern
    AF_SELECTED = 1u << 6,
    AF_EXPANDABLE = 1u << 7,  // ExpandCollapse pattern
    AF_EXPANDED = 1u << 8,
    AF_SCROLLITEM = 1u << 9,  // ScrollItem pattern
    AF_FOCUSABLE = 1u << 10,
    AF_FOCUSED = 1u << 11,
    AF_SELECTION = 1u << 12,  // a container with the Selection pattern
    AF_MULTI = 1u << 13,      // ... where several items can be selected
    AF_MODAL = 1u << 14,      // a dialog
    AF_MAIN = 1u << 15,       // landmark: the main content
    AF_NAV = 1u << 16,        // landmark: navigation
};

struct AccNode {
    AccRole role = AccRole::Pane;
    std::wstring name, help, id;  // id: AutomationId
    RectF r{};                    // client DIPs
    uint32_t flags = 0;
    uint64_t key = 0;  // identity across frames (the runtime id); made unique by Finish()
    int parent = -1;   // -1: the window itself
    int action = -1;   // the owner's index of what a click on it does (-1: nothing)
    int level = 0;     // depth in a tree-like list (1 = top level), 0: not in one
    std::vector<int> kids;
};

class AccTree {
public:
    void Clear();
    int Add(const AccNode& n);  // n.parent must already be in the tree (or -1); returns its index
    // Children in reading order (top to bottom, then left to right; the window's own children stay
    // in the order they were added), duplicate keys made unique, the key index built.
    void Finish();

    const std::vector<AccNode>& Nodes() const { return nodes_; }
    const std::vector<int>& Top() const { return top_; }
    size_t Size() const { return nodes_.size(); }
    const AccNode& operator[](int i) const { return nodes_[static_cast<size_t>(i)]; }
    int Find(uint64_t key) const;  // -1: not on screen any more
    // Navigation; -1 stands for the window itself (as a parent) or "none".
    int Parent(int i) const;
    int FirstChild(int i) const;
    int LastChild(int i) const;
    int Next(int i) const;
    int Prev(int i) const;
    int FromPoint(float x, float y) const;  // the deepest element at a client point
    int Focused() const;                    // the element with keyboard focus, -1: none
    uint64_t Signature() const;             // changes when elements come, go or move between parents

private:
    const std::vector<int>& Kids(int i) const { return i < 0 ? top_ : nodes_[static_cast<size_t>(i)].kids; }
    int Deepest(const std::vector<int>& kids, float x, float y) const;
    std::vector<AccNode> nodes_;
    std::vector<int> top_;
    std::unordered_map<uint64_t, int> byKey_;
};

// FNV-1a over the parts: stable identities for elements rebuilt every frame.
uint64_t AccKey(std::initializer_list<uint64_t> parts);
uint64_t AccKey(const std::wstring& s, uint64_t seed);
// Drops decoration from drawn text for a spoken name: trailing "›", leading "‹", runs of spaces,
// and the " · " separators become ", ".
std::wstring AccTidy(const std::wstring& s);
// For each run, the last target whose rectangle contains the run's center (targets are in drawing
// order, so the last one is on top), or -1.
std::vector<int> AccAssign(const std::vector<RectF>& targets, const std::vector<RectF>& runs);
// For each target, the last earlier target that fully contains it (its parent element), or -1.
std::vector<int> AccNest(const std::vector<RectF>& targets);
// Indices sorted into reading order by their rectangles: lines top to bottom, left to right within a line.
void AccReadingOrder(std::vector<int>& ids, const std::vector<RectF>& rects);
// UI Automation control type and pattern ids for an element.
long AccControlType(AccRole role);
bool AccSupports(const AccNode& n, int patternId);
