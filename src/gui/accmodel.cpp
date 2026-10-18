#include "accmodel.h"

#include <uiautomation.h>

#include <algorithm>

void AccTree::Clear() {
    nodes_.clear();
    top_.clear();
    byKey_.clear();
}

int AccTree::Add(const AccNode& n) {
    int i = static_cast<int>(nodes_.size());
    nodes_.push_back(n);
    nodes_.back().kids.clear();
    if (n.parent >= 0 && n.parent < i)
        nodes_[static_cast<size_t>(n.parent)].kids.push_back(i);
    else {
        nodes_.back().parent = -1;
        top_.push_back(i);
    }
    return i;
}

void AccTree::Finish() {
    std::vector<RectF> rects;
    rects.reserve(nodes_.size());
    for (auto& n : nodes_) rects.push_back(n.r);
    for (auto& n : nodes_) AccReadingOrder(n.kids, rects);
    byKey_.clear();
    // Keys are made unique in tree order, so the same layout gives the same keys every frame.
    std::vector<int> stack(top_.rbegin(), top_.rend());
    std::unordered_map<uint64_t, uint64_t> seen;
    while (!stack.empty()) {
        int i = stack.back();
        stack.pop_back();
        AccNode& n = nodes_[static_cast<size_t>(i)];
        if (!n.key) n.key = 1;
        uint64_t& count = seen[n.key];
        if (count++) n.key = AccKey({n.key, count});
        while (byKey_.count(n.key)) n.key = AccKey({n.key, 0x9E3779B97F4A7C15ull});
        byKey_[n.key] = i;
        for (auto it = n.kids.rbegin(); it != n.kids.rend(); ++it) stack.push_back(*it);
    }
}

int AccTree::Find(uint64_t key) const {
    auto it = byKey_.find(key);
    return it == byKey_.end() ? -1 : it->second;
}

int AccTree::Parent(int i) const { return i < 0 || i >= static_cast<int>(nodes_.size()) ? -1 : nodes_[static_cast<size_t>(i)].parent; }

int AccTree::FirstChild(int i) const {
    if (i >= static_cast<int>(nodes_.size())) return -1;
    const auto& k = Kids(i);
    return k.empty() ? -1 : k.front();
}

int AccTree::LastChild(int i) const {
    if (i >= static_cast<int>(nodes_.size())) return -1;
    const auto& k = Kids(i);
    return k.empty() ? -1 : k.back();
}

int AccTree::Next(int i) const {
    if (i < 0 || i >= static_cast<int>(nodes_.size())) return -1;
    const auto& k = Kids(nodes_[static_cast<size_t>(i)].parent);
    auto it = std::find(k.begin(), k.end(), i);
    return it == k.end() || it + 1 == k.end() ? -1 : *(it + 1);
}

int AccTree::Prev(int i) const {
    if (i < 0 || i >= static_cast<int>(nodes_.size())) return -1;
    const auto& k = Kids(nodes_[static_cast<size_t>(i)].parent);
    auto it = std::find(k.begin(), k.end(), i);
    return it == k.end() || it == k.begin() ? -1 : *(it - 1);
}

int AccTree::Deepest(const std::vector<int>& kids, float x, float y) const {
    // Later siblings are drawn on top (the dialog after the page); reading order doesn't change that
    // much for siblings, which rarely overlap.
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        const AccNode& n = nodes_[static_cast<size_t>(*it)];
        if (!Inside(n.r, x, y)) continue;
        int d = Deepest(n.kids, x, y);
        return d >= 0 ? d : *it;
    }
    return -1;
}

int AccTree::FromPoint(float x, float y) const { return Deepest(top_, x, y); }

int AccTree::Focused() const {
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].flags & AF_FOCUSED) return static_cast<int>(i);
    return -1;
}

uint64_t AccTree::Signature() const {
    uint64_t h = AccKey({nodes_.size()});
    for (auto& n : nodes_) h = AccKey({h, n.key, n.parent < 0 ? 0 : nodes_[static_cast<size_t>(n.parent)].key});
    return h;
}

uint64_t AccKey(std::initializer_list<uint64_t> parts) {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t p : parts)
        for (int b = 0; b < 8; ++b) {
            h ^= (p >> (b * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    return h;
}

uint64_t AccKey(const std::wstring& s, uint64_t seed) {
    uint64_t h = AccKey({seed});
    for (wchar_t c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring AccTidy(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    bool sep = false;  // a " · " separator is pending
    for (wchar_t c : s) {
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
        if (c == L'·') {
            sep = true;
            continue;
        }
        if (c == L' ') {
            if (!out.empty() && out.back() != L' ' && !sep) out += c;
            continue;
        }
        if (sep) {
            while (!out.empty() && out.back() == L' ') out.pop_back();
            if (!out.empty()) out += L", ";
            sep = false;
        }
        out += c;
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'›' || out.back() == L',')) out.pop_back();
    size_t b = 0;
    while (b < out.size() && (out[b] == L' ' || out[b] == L'‹')) ++b;
    return out.substr(b);
}

std::vector<int> AccAssign(const std::vector<RectF>& targets, const std::vector<RectF>& runs) {
    std::vector<int> out(runs.size(), -1);
    for (size_t i = 0; i < runs.size(); ++i) {
        float cx = (runs[i].left + runs[i].right) / 2, cy = (runs[i].top + runs[i].bottom) / 2;
        for (size_t t = targets.size(); t-- > 0;)
            if (Inside(targets[t], cx, cy)) {
                out[i] = static_cast<int>(t);
                break;
            }
    }
    return out;
}

std::vector<int> AccNest(const std::vector<RectF>& targets) {
    std::vector<int> out(targets.size(), -1);
    auto contains = [](const RectF& a, const RectF& b) {
        return b.left >= a.left - 0.5f && b.top >= a.top - 0.5f && b.right <= a.right + 0.5f && b.bottom <= a.bottom + 0.5f &&
               (W(b) < W(a) || H(b) < H(a));
    };
    for (size_t i = 0; i < targets.size(); ++i)
        for (size_t p = i; p-- > 0;)
            if (contains(targets[p], targets[i])) {
                out[i] = static_cast<int>(p);
                break;
            }
    return out;
}

void AccReadingOrder(std::vector<int>& ids, const std::vector<RectF>& rects) {
    if (ids.size() < 2) return;
    auto rc = [&](int i) -> const RectF& { return rects[static_cast<size_t>(i)]; };
    std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) { return rc(a).top < rc(b).top; });
    // Lines: an element joins the current line when its middle is within the line's first element.
    std::vector<int> out;
    out.reserve(ids.size());
    size_t start = 0;
    while (start < ids.size()) {
        const RectF& first = rc(ids[start]);
        size_t end = start + 1;
        while (end < ids.size()) {
            float mid = (rc(ids[end]).top + rc(ids[end]).bottom) / 2;
            if (mid > first.bottom || mid < first.top) break;
            ++end;
        }
        std::vector<int> line(ids.begin() + static_cast<std::ptrdiff_t>(start), ids.begin() + static_cast<std::ptrdiff_t>(end));
        std::stable_sort(line.begin(), line.end(), [&](int a, int b) { return rc(a).left < rc(b).left; });
        out.insert(out.end(), line.begin(), line.end());
        start = end;
    }
    ids.swap(out);
}

long AccControlType(AccRole role) {
    switch (role) {
        case AccRole::Pane: return UIA_PaneControlTypeId;
        case AccRole::Toolbar: return UIA_ToolBarControlTypeId;
        case AccRole::Navigation: return UIA_ListControlTypeId;
        case AccRole::List: return UIA_ListControlTypeId;
        case AccRole::Header: return UIA_HeaderControlTypeId;
        case AccRole::HeaderItem: return UIA_HeaderItemControlTypeId;
        case AccRole::ListItem: return UIA_ListItemControlTypeId;
        case AccRole::Button: return UIA_ButtonControlTypeId;
        case AccRole::CheckBox: return UIA_CheckBoxControlTypeId;
        case AccRole::RadioButton: return UIA_RadioButtonControlTypeId;
        case AccRole::Text: return UIA_TextControlTypeId;
        case AccRole::Image: return UIA_ImageControlTypeId;
        case AccRole::Window: return UIA_WindowControlTypeId;
        case AccRole::StatusBar: return UIA_StatusBarControlTypeId;
    }
    return UIA_PaneControlTypeId;
}

bool AccSupports(const AccNode& n, int patternId) {
    switch (patternId) {
        case UIA_InvokePatternId: return (n.flags & AF_INVOKE) != 0;
        case UIA_TogglePatternId: return (n.flags & AF_TOGGLE) != 0;
        case UIA_SelectionItemPatternId: return (n.flags & AF_SELECTABLE) != 0;
        case UIA_SelectionPatternId: return (n.flags & AF_SELECTION) != 0;
        case UIA_ExpandCollapsePatternId: return (n.flags & AF_EXPANDABLE) != 0;
        case UIA_ScrollItemPatternId: return (n.flags & AF_SCROLLITEM) != 0;
        default: return false;
    }
}
