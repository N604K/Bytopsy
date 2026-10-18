// What the main window shows to UI Automation (screen readers): each frame, the click regions
// (AddHit), the text drawn, the checkboxes drawn and a few names given at the call sites
// (AccMark) are turned into an AccTree. Acting on an element runs the same code a click runs.

#include "app.h"

#include <oleacc.h>

#include <algorithm>
#include <unordered_map>

#include "../util.h"

#pragma comment(lib, "oleacc.lib")

namespace {

constexpr float kTopH = 56.0f;
constexpr float kSidebarW = 240.0f;
constexpr float kStatusH = 30.0f;
constexpr float kHeaderH = 32.0f;
constexpr int kChoiceSlot = 1000;  // dialog keyboard focus: 1000 + choice index (-1 checkbox, 0.. buttons)

// PROPID_ACC_NAME (oleacc.h declares it, but uiautomation.h includes that header before INITGUID could define it).
constexpr GUID kAccName = {0x608d3df8, 0x8128, 0x4aa7, {0xa4, 0x28, 0xf5, 0x5e, 0x49, 0x26, 0x72, 0x91}};

const wchar_t* ViewName(View v) {
    switch (v) {
        case View::Home: return L"Home";
        case View::Browse: return L"Folders";
        case View::Largest: return L"Largest files";
        case View::Types: return L"File types";
        case View::TypeFiles: return L"Files of one type";
        case View::Duplicates: return L"Duplicates";
        case View::Old: return L"Old files";
        case View::Empty: return L"Empty folders";
        case View::DevJunk: return L"Developer junk";
        case View::Search: return L"Search results";
        case View::Cleanup: return L"Cleanup";
        case View::History: return L"History";
        case View::Compare: return L"Compare scans";
        case View::Deleted: return L"Recently deleted";
        case View::AiModels: return L"AI models";
        case View::Wsl: return L"WSL & Docker";
        case View::WinTools: return L"Windows";
        case View::Games: return L"Games";
        case View::Startup: return L"Startup";
        case View::Apps: return L"Apps";
        case View::CleanReview: return L"Review";
        case View::DiskHealth: return L"Disk health";
        default: return L"WinNCDU";
    }
}

std::wstring Join(const std::vector<std::wstring>& v, size_t from) {
    std::wstring s;
    for (size_t i = from; i < v.size(); ++i) s += (s.empty() ? L"" : L", ") + v[i];
    return s;
}

}  // namespace

// =============================================================================================
// Recording during painting

void App::AccMark(const std::wstring& name, int toggle) {
    if (!gfx_.Recording() || hits_.empty()) return;
    accMarks_.push_back({hits_.size() - 1, hits_.back(), name, toggle, hits_.size() - 1});
}

void App::AccDisabled(const RectF& r, int kind, int a, const std::wstring& name) {
    if (!gfx_.Recording()) return;
    Hit h;
    h.r = r;
    h.kind = kind;
    h.a = a;
    accMarks_.push_back({SIZE_MAX, h, name, -1, hits_.size()});
}

void App::AccFrameBegin() {
    gfx_.SetRecording(acc_ && acc_->Active());
    accMarks_.clear();
    accChecks_.clear();
    accTipFrom_ = accTipTo_ = accModalText_ = SIZE_MAX;
    accModalRect_ = RectF{};
}

// =============================================================================================
// Building the tree

void App::AccBuild() {
    if (!gfx_.Recording()) return;
    accTree_.Clear();
    accActs_.clear();
    D2D1_SIZE_F sz = gfx_.Size();
    bool modal = dialog_.active || task_;
    size_t lo = modal ? std::min(modalHitStart_, hits_.size()) : 0;
    const auto& runs = gfx_.Runs();
    size_t textLo = modal ? std::min(accModalText_, runs.size()) : 0;

    // Candidates: the click regions (and disabled controls, which have none) in drawing order.
    struct Cand {
        Hit h;
        std::wstring name;
        int toggle = -1;
        bool disabled = false;
        int region = 0;  // 0 top bar, 1 sidebar, 2 content, 3 status bar, 4 dialog
        int check = -1;  // a checkbox drawn inside it: 0 off, 1 on, 2 partly
        std::vector<std::wstring> texts;
        int node = -1;
    };
    std::vector<Cand> cands;
    std::unordered_map<size_t, size_t> markOf;
    for (size_t m = 0; m < accMarks_.size(); ++m)
        if (accMarks_[m].hit != SIZE_MAX) markOf[accMarks_[m].hit] = m;
    // Which part of the window drew it (by drawing order: content, sidebar, top bar, status bar,
    // overlays), and only the part of it that is visible there: scrolled-away items are left out.
    const RectF areas[4] = {R(0, 0, sz.width, kTopH), R(0, kTopH, kSidebarW, sz.height - kStatusH),
                            R(kSidebarW, kTopH, sz.width, sz.height - kStatusH), R(0, sz.height - kStatusH, sz.width, sz.height)};
    auto segment = [&](const size_t* seg, size_t i) {
        if (modal) return 4;
        if (i < seg[0]) return 2;
        if (i < seg[1]) return 1;
        if (i < seg[2]) return 0;
        if (i < seg[3]) return 3;
        return 2;  // toasts float over the content
    };
    auto place = [&](Cand& c, size_t hitIndex) {
        c.region = segment(accHitSeg_, hitIndex);
        if (c.region == 4) return true;
        const RectF& a = areas[c.region];
        const RectF& r = c.h.r;
        RectF v = R(std::max(r.left, a.left), std::max(r.top, a.top), std::min(r.right, a.right), std::min(r.bottom, a.bottom));
        if (W(v) < 2 || H(v) < std::min(12.0f, H(r) * 0.5f)) return false;
        c.h.r = v;
        return true;
    };
    size_t mi = 0;
    auto addDisabledBefore = [&](size_t hitIndex) {
        for (; mi < accMarks_.size(); ++mi) {
            const AccMarkInfo& m = accMarks_[mi];
            if (m.at > hitIndex) break;
            if (m.hit != SIZE_MAX || m.at < lo) continue;
            Cand c;
            c.h = m.h;
            c.name = m.name;
            c.disabled = true;
            if (place(c, m.at)) cands.push_back(std::move(c));
        }
    };
    for (size_t i = 0; i < hits_.size(); ++i) {
        addDisabledBefore(i);
        const Hit& h = hits_[i];
        if (i < lo) continue;
        if (h.kind == HK_NONE || h.kind == HK_MODAL_BG || h.kind == HK_SCROLL || h.kind == HK_SPLITTER || h.kind == HK_EXPANDER ||
            h.kind == HK_BADGE)
            continue;
        Cand c;
        c.h = h;
        if (!place(c, i)) continue;
        if (auto it = markOf.find(i); it != markOf.end()) {
            c.name = accMarks_[it->second].name;
            c.toggle = accMarks_[it->second].toggle;
        }
        cands.push_back(std::move(c));
    }
    addDisabledBefore(SIZE_MAX - 1);

    std::vector<RectF> rects;
    for (auto& c : cands) rects.push_back(c.h.r);
    std::vector<int> nest = AccNest(rects);
    for (size_t i = 0; i < cands.size(); ++i)
        if (nest[i] >= 0 && cands[static_cast<size_t>(nest[i])].region != cands[i].region) nest[i] = -1;

    // Checkboxes drawn inside a region say it toggles, and whether it is on.
    {
        std::vector<RectF> boxes;
        for (auto& b : accChecks_) boxes.push_back(b.r);
        std::vector<int> at = AccAssign(rects, boxes);
        for (size_t i = 0; i < at.size(); ++i)
            if (at[i] >= 0) cands[static_cast<size_t>(at[i])].check = accChecks_[i].mixed ? 2 : (accChecks_[i].on ? 1 : 0);
    }

    // Text: what belongs to an element names it; the rest becomes text elements.
    std::vector<size_t> textIdx;
    std::vector<RectF> textRects;
    for (size_t i = textLo; i < runs.size(); ++i) {
        if (accTipFrom_ != SIZE_MAX && i >= accTipFrom_ && i < accTipTo_) continue;  // tooltips repeat a name
        if (!modal && accModalText_ != SIZE_MAX && i >= accModalText_) break;
        textIdx.push_back(i);
        textRects.push_back(runs[i].r);
    }
    std::vector<int> owner = AccAssign(rects, textRects);
    std::vector<size_t> loose;
    for (size_t k = 0; k < textIdx.size(); ++k) {
        std::wstring t = AccTidy(runs[textIdx[k]].s);
        if (t.empty()) continue;
        if (owner[k] >= 0)
            cands[static_cast<size_t>(owner[k])].texts.push_back(t);
        else
            loose.push_back(k);
    }

    // Containers.
    auto add = [&](AccRole role, const std::wstring& name, const RectF& r, uint32_t flags, uint64_t key, int parent) {
        AccNode n;
        n.role = role;
        n.name = name;
        n.r = r;
        n.flags = flags;
        n.key = key;
        n.parent = parent;
        return accTree_.Add(n);
    };
    int toolbar = -1, nav = -1, main = -1, status = -1, win = -1, list = -1, header = -1;
    RectF listR{}, headerR{};
    bool hasRows = std::any_of(cands.begin(), cands.end(), [](const Cand& c) { return c.h.kind == HK_ROW; });
    bool multi = false, single = false;
    if (hasRows && !modal)
        for (auto& c : cands)
            if (c.h.kind == HK_ROW && c.check < 0 && c.h.a >= 0 && c.h.a < static_cast<int>(rows_.size())) {
                if (rows_[c.h.a].node && rows_[c.h.a].kind != RK_GROUP) multi = true;
                if (view_ == View::Startup) single = true;
            }
    if (modal) {
        bool busy = task_ != nullptr;
        std::wstring title = busy ? task_->title : dialog_.title;
        AccNode w;
        w.role = AccRole::Window;
        w.name = title;
        w.help = busy ? L"" : AccTidy(dialog_.message);
        w.r = accModalRect_;
        w.flags = AF_MODAL;
        w.key = busy ? AccKey({0xB5, reinterpret_cast<uint64_t>(task_.get())}) : AccKey({0xD1A, dialogGen_});
        win = accTree_.Add(w);
    } else {
        toolbar = add(AccRole::Toolbar, L"Toolbar", R(0, 0, sz.width, kTopH), 0, AccKey({0xA11, 1}), -1);
        nav = add(AccRole::Navigation, L"Navigation", R(0, kTopH, kSidebarW, sz.height - kStatusH), AF_NAV | AF_SELECTION, AccKey({0xA11, 2}), -1);
        main = add(AccRole::Pane, ViewName(view_), R(kSidebarW, kTopH, sz.width, sz.height - kStatusH), AF_MAIN,
                   AccKey({0xA11, 3, static_cast<uint64_t>(view_)}), -1);
        if (hasRows) {
            listR = R(listArea_.left, listArea_.top - kHeaderH, listArea_.right, listArea_.bottom);
            headerR = R(listR.left, listR.top, listR.right, listArea_.top);
            uint32_t f = (multi || single) ? AF_SELECTION : 0;
            if (multi) f |= AF_MULTI;
            list = add(AccRole::List, std::wstring(ViewName(view_)) + L" list", listR, f, AccKey({0xA11, 5, static_cast<uint64_t>(view_)}), main);
            header = add(AccRole::Header, L"Columns", headerR, 0, AccKey({0xA11, 6, static_cast<uint64_t>(view_)}), list);
        }
        status = add(AccRole::StatusBar, L"Status", R(0, sz.height - kStatusH, sz.width, sz.height), 0, AccKey({0xA11, 4}), -1);
    }
    auto containerFor = [&](int reg, const RectF& r, bool rowOrHeader, int kind) {
        if (reg == 4) return win;
        if (reg == 0) return toolbar;
        if (reg == 1) return nav;
        if (reg == 3) return status;
        if (list >= 0) {
            if (kind == HK_HEADER) return header;
            if (kind == HK_ROW) return list;
            if (!rowOrHeader) {
                float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
                if (Inside(headerR, cx, cy)) return header;
                if (Inside(listR, cx, cy)) return list;
            }
        }
        return main;
    };

    // Dialog keyboard focus, as an element.
    int dlgFocus = modal && dialog_.active && !task_ ? DialogFocus() : -1000;
    ListState& ls = lists_[static_cast<int>(view_)];
    bool windowFocused = GetFocus() == hwnd_;
    uint64_t dlgSeed = AccKey({0xD1A, dialogGen_});

    for (size_t ci = 0; ci < cands.size(); ++ci) {
        Cand& c = cands[ci];
        const Hit& h = c.h;
        AccNode n;
        n.r = h.r;
        n.flags = c.disabled ? AF_DISABLED : 0;
        n.action = static_cast<int>(accActs_.size());
        n.key = AccKey({static_cast<uint64_t>(h.kind), static_cast<uint64_t>(static_cast<int64_t>(h.a)), static_cast<uint64_t>(h.b)});
        std::wstring fallback = L"Button";
        bool rowName = false;
        n.role = AccRole::Button;
        switch (h.kind) {
            case HK_BUTTON:
                n.id = L"cmd." + std::to_wstring(h.a);
                if (h.a == CMD_SEARCH_CLEAR) fallback = L"Clear search";
                if (h.a == CMD_CANCEL_TASK) fallback = L"Cancel";
                if (c.toggle >= 0)
                    n.flags |= AF_TOGGLE | (c.toggle ? AF_ON : 0);
                else
                    n.flags |= AF_INVOKE;
                if (task_ && h.a == CMD_CANCEL_TASK) n.flags |= AF_FOCUSED | AF_FOCUSABLE;
                break;
            case HK_NAV:
                n.id = L"nav." + std::to_wstring(h.a);
                if (c.region == 1) {
                    View v = static_cast<View>(h.a);
                    bool active = view_ == v || (v == View::Types && view_ == View::TypeFiles) || (v == View::History && view_ == View::Compare) ||
                                  (v == View::Cleanup && view_ == View::CleanReview);
                    n.role = AccRole::ListItem;
                    n.flags |= AF_SELECTABLE | AF_INVOKE | (active ? AF_SELECTED : 0);
                    fallback = ViewName(v);
                } else {
                    n.flags |= AF_INVOKE;
                    fallback = L"Back";
                    n.key = AccKey({n.key, 0xBAC});
                }
                break;
            case HK_ROW: {
                n.role = AccRole::ListItem;
                n.id = L"row." + std::to_wstring(h.a);
                rowName = true;
                n.flags |= AF_INVOKE | AF_SCROLLITEM | AF_FOCUSABLE;
                if (h.a >= 0 && h.a < static_cast<int>(rows_.size())) {
                    const Row& row = rows_[h.a];
                    n.key = AccKey({static_cast<uint64_t>(HK_ROW), static_cast<uint64_t>(view_), reinterpret_cast<uint64_t>(row.node),
                                    static_cast<uint64_t>(row.kind), static_cast<uint64_t>(static_cast<int64_t>(row.idx))});
                    if (c.check >= 0) {
                        n.flags |= AF_TOGGLE | (c.check == 1 ? AF_ON : 0) | (c.check == 2 ? AF_MIXED : 0);
                    } else if (row.node && row.kind != RK_GROUP) {
                        n.flags |= AF_SELECTABLE | (sel_.count(row.node) ? AF_SELECTED : 0);
                    } else if (view_ == View::Startup && row.kind == RK_STARTUP) {
                        n.flags |= AF_SELECTABLE | (ls.focus == h.a ? AF_SELECTED : 0);
                    }
                    if (view_ == View::Browse || view_ == View::AiModels) n.level = row.depth + 1;  // tree-like lists
                    if (view_ == View::Browse && row.node && row.node->CanEnter() && !row.node->children.empty())
                        n.flags |= AF_EXPANDABLE | (expanded_.count(row.node) ? AF_EXPANDED : 0);
                    if (windowFocused && ls.focus == h.a) n.flags |= AF_FOCUSED;
                }
                break;
            }
            case HK_HEADER: {
                n.role = AccRole::HeaderItem;
                n.flags |= AF_INVOKE;
                n.id = L"sort." + std::to_wstring(h.a);
                int key = view_ == View::Apps ? appSortKey_ : view_ == View::Browse ? sortKey_ : resSortKey_;
                bool desc = view_ == View::Apps ? appSortDesc_ : view_ == View::Browse ? sortDesc_ : resSortDesc_;
                if (key == h.a) c.texts.push_back(desc ? L"sorted descending" : L"sorted ascending");
                break;
            }
            case HK_TREEMAP:
                n.role = AccRole::Image;
                c.name = L"Treemap";
                c.texts = {L"the open folder's contents as rectangles sized by the space they use; the list shows the same items"};
                break;
            case HK_CHIP:
                n.flags |= AF_TOGGLE | (c.toggle == 1 ? AF_ON : 0);
                n.id = L"chip." + std::to_wstring(h.a) + L"." + std::to_wstring(h.b);
                break;
            case HK_CHECK:
                n.role = AccRole::CheckBox;
                n.flags |= AF_TOGGLE;
                if (h.a >= 0 && h.a < static_cast<int>(cleanup_.size()) && cleanup_[h.a].selected) n.flags |= AF_ON;
                n.id = L"check." + std::to_wstring(h.a);
                break;
            case HK_SWITCH:
                n.flags |= AF_TOGGLE;
                c.name = L"Start with Windows";
                if (h.a >= 0 && h.a < static_cast<int>(startup_.entries.size())) {
                    const StartupEntry& e = startup_.entries[h.a];
                    if (e.enabled) n.flags |= AF_ON;
                    c.texts.push_back(e.display);
                    if (!e.canToggle) {
                        n.flags |= AF_DISABLED;
                        c.texts.push_back(e.cantToggle);
                    }
                }
                break;
            case HK_SNAPSHOT:
                n.flags |= AF_INVOKE;
                if (h.b == 1) fallback = L"Delete this scan from history";
                break;
            case HK_DIALOG:
                n.key = AccKey({n.key, dlgSeed});
                n.id = L"dialog." + std::to_wstring(h.a);
                if (h.a == -1) {
                    n.role = AccRole::CheckBox;
                    n.flags |= AF_TOGGLE | (dialog_.checked ? AF_ON : 0) | AF_FOCUSABLE;
                    if (dlgFocus == -1) n.flags |= AF_FOCUSED;
                } else {
                    n.flags |= AF_INVOKE | AF_FOCUSABLE;
                    if (dlgFocus == h.a) n.flags |= AF_FOCUSED;
                }
                break;
            case HK_DIALOG_CHOICE:
                n.key = AccKey({n.key, dlgSeed});
                if (h.a >= 0 && h.a < static_cast<int>(dialog_.choices.size())) {
                    const DialogChoice& ch = dialog_.choices[h.a];
                    std::wstring now = ch.value >= 0 && ch.value < static_cast<int>(ch.options.size()) ? ch.options[ch.value] : L"";
                    if (ch.stepper) {
                        bool next = h.b > ch.value;
                        c.name = (next ? L"Next " : L"Previous ") + util::ToLower(ch.label);
                        c.texts = {L"now " + now};
                        n.flags |= AF_INVOKE | AF_FOCUSABLE;
                        n.key = AccKey({static_cast<uint64_t>(HK_DIALOG_CHOICE), static_cast<uint64_t>(h.a), next ? 1u : 0u, dlgSeed});
                        if (next && dlgFocus == kChoiceSlot + h.a) n.flags |= AF_FOCUSED;
                    } else {
                        n.role = AccRole::RadioButton;
                        n.flags |= AF_SELECTABLE | AF_FOCUSABLE | (h.b == ch.value ? AF_SELECTED : 0);
                        c.texts.push_back(ch.label);
                        if (h.b == ch.value && dlgFocus == kChoiceSlot + h.a) n.flags |= AF_FOCUSED;
                    }
                }
                break;
            default:  // cards, recent scans, drives, crumbs, restore, review links
                n.flags |= AF_INVOKE;
                break;
        }
        if (c.check >= 0 && h.kind != HK_ROW && !(n.flags & AF_TOGGLE)) {
            n.role = AccRole::CheckBox;
            n.flags = (n.flags & ~AF_INVOKE) | AF_TOGGLE | (c.check == 1 ? AF_ON : 0) | (c.check == 2 ? AF_MIXED : 0);
        }
        // Names: the row's cells read in order; elsewhere the first text names it and the rest describes it.
        if (!c.name.empty()) {
            n.name = c.name;
            n.help = Join(c.texts, 0);
        } else if (rowName) {
            n.name = Join(c.texts, 0);
        } else if (!c.texts.empty()) {
            n.name = c.texts[0];
            n.help = Join(c.texts, 1);
        } else {
            n.name = fallback;
        }
        n.name = AccTidy(n.name);
        int parent = nest[ci] >= 0 ? cands[static_cast<size_t>(nest[ci])].node : -1;
        if (parent < 0) parent = containerFor(c.region, h.r, h.kind == HK_ROW || h.kind == HK_HEADER, h.kind);
        n.parent = parent;
        c.node = accTree_.Add(n);
        accActs_.push_back(h);
    }

    // Text elements are known by their place among their parent's texts, not by what they say, so a
    // changing status line is the same element with a new name rather than a new element.
    std::unordered_map<int, uint64_t> textCount;
    for (size_t k : loose) {
        const RectF& r = textRects[k];
        AccNode n;
        n.role = AccRole::Text;
        n.name = AccTidy(runs[textIdx[k]].s);
        n.r = r;
        n.parent = containerFor(segment(accRunSeg_, textIdx[k]), r, false, HK_NONE);
        if (n.parent == header) n.role = AccRole::HeaderItem;
        n.key = AccKey({0x7E47, n.parent >= 0 ? accTree_[n.parent].key : 0, textCount[n.parent]++});
        accTree_.Add(n);
    }
    accTree_.Finish();
    if (acc_->Listening() && !accEventPosted_) {
        accEventPosted_ = true;
        PostMessageW(hwnd_, WM_APP_ACC, 0, 0);
    }
}

// =============================================================================================
// AccHost

const AccTree* App::AccCurrent() {
    if (!hwnd_ || !acc_) return nullptr;
    if (!inPaint_) {
        // A change not drawn yet (or the first request): draw now, so what a client reads is current.
        if (!gfx_.Recording() || accTree_.Size() == 0)
            RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        else if (GetUpdateRect(hwnd_, nullptr, FALSE))
            UpdateWindow(hwnd_);
    }
    return &accTree_;
}

RECT App::AccScreenRect(const RectF& r) const {
    POINT a{Dip2Px(r.left), Dip2Px(r.top)}, b{Dip2Px(r.right), Dip2Px(r.bottom)};
    ClientToScreen(hwnd_, &a);
    ClientToScreen(hwnd_, &b);
    return RECT{a.x, a.y, b.x, b.y};
}

bool App::AccClientPoint(double sx, double sy, float* x, float* y) const {
    POINT p{static_cast<LONG>(sx), static_cast<LONG>(sy)};
    ScreenToClient(hwnd_, &p);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (!PtInRect(&rc, p)) return false;
    *x = Px2Dip(p.x);
    *y = Px2Dip(p.y);
    return true;
}

HRESULT App::AccDo(uint64_t key, AccAct act) {
    int i = accTree_.Find(key);
    if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
    uint32_t f = accTree_[i].flags;
    bool ok = false;
    switch (act) {
        case AccAct::Invoke: ok = (f & AF_INVOKE) != 0; break;
        case AccAct::Toggle: ok = (f & AF_TOGGLE) != 0; break;
        case AccAct::Select:
        case AccAct::AddToSelection:
        case AccAct::RemoveFromSelection: ok = (f & AF_SELECTABLE) != 0; break;
        case AccAct::Expand:
        case AccAct::Collapse: ok = (f & AF_EXPANDABLE) != 0; break;
        case AccAct::ScrollIntoView: ok = (f & AF_SCROLLITEM) != 0; break;
        case AccAct::Focus: ok = true; break;
    }
    if (!ok) return UIA_E_INVALIDOPERATION;
    if (act == AccAct::RemoveFromSelection && !(f & AF_MULTI) && accTree_[i].role != AccRole::ListItem) return UIA_E_INVALIDOPERATION;
    // Invoke returns at once (it may open a dialog or a file picker); the click runs from the message loop.
    if (act == AccAct::Invoke) {
        PostMessageW(hwnd_, WM_APP_ACCDO, static_cast<WPARAM>(act), static_cast<LPARAM>(key));
        return S_OK;
    }
    AccRun(key, act);
    return S_OK;
}

void App::AccRun(uint64_t key, AccAct act) {
    AccCurrent();
    int i = accTree_.Find(key);
    if (i < 0) return;
    const AccNode& n = accTree_[i];
    if (n.action < 0 || n.action >= static_cast<int>(accActs_.size()) || (n.flags & AF_DISABLED)) return;
    Hit h = accActs_[static_cast<size_t>(n.action)];
    // Only what is on screen now: the page behind a dialog isn't in the tree, so it can't be reached.
    bool isRow = h.kind == HK_ROW && h.a >= 0 && h.a < static_cast<int>(rows_.size());
    switch (act) {
        case AccAct::Invoke:
            if (isRow)
                OpenRow(h.a);
            else if (h.kind != HK_ROW)
                ActivateHit(h, false, false, false);
            break;
        case AccAct::Toggle: ActivateHit(h, false, false, false); break;
        case AccAct::Select:
            if (h.kind == HK_ROW || !(n.flags & AF_SELECTED)) ActivateHit(h, false, false, false);
            break;
        case AccAct::AddToSelection:
            if (!(n.flags & AF_SELECTED)) {
                if (isRow && rows_[h.a].node)
                    SelectRow(h.a, true, false);
                else
                    ActivateHit(h, false, false, false);
            }
            break;
        case AccAct::RemoveFromSelection:
            if ((n.flags & AF_SELECTED) && isRow && rows_[h.a].node) SelectRow(h.a, true, false);
            break;
        case AccAct::Expand:
        case AccAct::Collapse:
            if (isRow && ((n.flags & AF_EXPANDED) != 0) != (act == AccAct::Expand)) {
                Hit e = h;
                e.kind = HK_EXPANDER;
                ActivateHit(e, false, false, false);
            }
            break;
        case AccAct::ScrollIntoView:
            if (isRow) EnsureVisible(h.a);
            break;
        case AccAct::Focus:
            if (isRow) lists_[static_cast<int>(view_)].focus = h.a;
            if (dialog_.active && h.kind == HK_DIALOG) dialog_.focus = h.a;
            if (dialog_.active && h.kind == HK_DIALOG_CHOICE) dialog_.focus = kChoiceSlot + h.a;
            break;
    }
    Invalidate();
}

// =============================================================================================
// Events, raised after a frame (never while painting: a client reading back would repaint)

void App::AccEvents() {
    accEventPosted_ = false;
    if (!acc_ || !acc_->Listening()) return;
    uint64_t sig = accTree_.Signature();
    if (sig != accSig_) {
        accSig_ = sig;
        acc_->StructureChanged();
    }
    uint64_t modalKey = 0;
    for (int t : accTree_.Top())
        if (accTree_[t].flags & AF_MODAL) modalKey = accTree_[t].key;
    bool modalClosed = !modalKey && accModalKey_;
    if (modalKey && modalKey != accModalKey_) acc_->WindowOpened(modalKey);
    accModalKey_ = modalKey;

    int f = accTree_.Focused();
    uint64_t fk = f >= 0 ? accTree_[f].key : 0;
    if (GetFocus() != hwnd_) {  // the search box (or another window) has it
        accFocusKey_ = 0;
        return;
    }
    if (fk != accFocusKey_) {
        if (fk || modalClosed) acc_->FocusChanged(fk);
    } else if (f >= 0) {
        const AccNode& n = accTree_[f];
        uint32_t diff = n.flags ^ accFocusFlags_;
        auto i4 = [](long v) {
            VARIANT x;
            VariantInit(&x);
            x.vt = VT_I4;
            x.lVal = v;
            return x;
        };
        auto toggle = [](uint32_t fl) { return (fl & AF_MIXED) ? ToggleState_Indeterminate : (fl & AF_ON) ? ToggleState_On : ToggleState_Off; };
        if (diff & (AF_ON | AF_MIXED)) acc_->PropertyChanged(fk, UIA_ToggleToggleStatePropertyId, i4(toggle(accFocusFlags_)), i4(toggle(n.flags)));
        if (diff & AF_SELECTED) {
            VARIANT o, v;
            VariantInit(&o);
            VariantInit(&v);
            o.vt = v.vt = VT_BOOL;
            o.boolVal = (accFocusFlags_ & AF_SELECTED) ? VARIANT_TRUE : VARIANT_FALSE;
            v.boolVal = (n.flags & AF_SELECTED) ? VARIANT_TRUE : VARIANT_FALSE;
            acc_->PropertyChanged(fk, UIA_SelectionItemIsSelectedPropertyId, o, v);
        }
        if (diff & AF_EXPANDED)
            acc_->PropertyChanged(fk, UIA_ExpandCollapseExpandCollapseStatePropertyId,
                                  i4((accFocusFlags_ & AF_EXPANDED) ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed),
                                  i4((n.flags & AF_EXPANDED) ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed));
        if (n.name != accFocusName_) {
            VARIANT o, v;
            VariantInit(&o);
            VariantInit(&v);
            o.vt = v.vt = VT_BSTR;
            o.bstrVal = SysAllocString(accFocusName_.c_str());
            v.bstrVal = SysAllocString(n.name.c_str());
            acc_->PropertyChanged(fk, UIA_NamePropertyId, o, v);
            VariantClear(&o);
            VariantClear(&v);
        }
    }
    accFocusKey_ = fk;
    accFocusFlags_ = f >= 0 ? accTree_[f].flags : 0;
    accFocusName_ = f >= 0 ? accTree_[f].name : L"";
}

// The search box is a real edit control (accessible on its own); give it a name to be read by.
void App::AccNameSearchBox(bool set) {
    if (!search_) return;
    IAccPropServices* ps = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CAccPropServices), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ps))) || !ps) return;
    if (set) {
        ps->SetHwndPropStr(search_, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, kAccName, L"Search files and folders");
    } else {
        MSAAPROPID props[] = {kAccName};
        ps->ClearHwndProps(search_, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, props, 1);
    }
    ps->Release();
}

// =============================================================================================
// Dialog keyboard focus: Tab / Shift+Tab / arrows move between the checkbox, the choices and the
// buttons; Enter presses the focused button (the primary one until Tab moves on).

std::vector<int> App::DialogSlots() const {
    std::vector<int> s;
    if (dialog_.hasCheck) s.push_back(-1);
    for (int i = 0; i < static_cast<int>(dialog_.choices.size()); ++i)
        if (!dialog_.choices[i].hidden && !dialog_.choices[i].options.empty()) s.push_back(kChoiceSlot + i);
    for (int i = 0; i < static_cast<int>(dialog_.buttons.size()); ++i) s.push_back(i);
    return s;
}

int App::DialogFocus() const {
    std::vector<int> s = DialogSlots();
    if (std::find(s.begin(), s.end(), dialog_.focus) != s.end()) return dialog_.focus;
    if (dialog_.primary >= 0 && dialog_.primary < static_cast<int>(dialog_.buttons.size())) return dialog_.primary;
    return dialog_.buttons.empty() ? (s.empty() ? -1000 : s.front()) : 0;
}

bool App::DialogKey(WPARAM vk, bool shift) {
    std::vector<int> slots = DialogSlots();
    if (slots.empty()) return false;
    int cur = DialogFocus();
    int n = static_cast<int>(slots.size());
    int pos = static_cast<int>(std::find(slots.begin(), slots.end(), cur) - slots.begin());
    if (pos >= n) pos = 0;
    auto moveTo = [&](int p) {
        dialog_.focus = slots[static_cast<size_t>(((p % n) + n) % n)];
        dialog_.kbd = true;
        Invalidate();
    };
    switch (vk) {
        case VK_TAB: moveTo(pos + (shift ? -1 : 1)); return true;
        case VK_UP: moveTo(pos - 1); return true;
        case VK_DOWN: moveTo(pos + 1); return true;
        case VK_LEFT:
        case VK_RIGHT: {
            int d = vk == VK_RIGHT ? 1 : -1;
            dialog_.kbd = true;
            if (cur >= kChoiceSlot) {
                DialogChoice& ch = dialog_.choices[static_cast<size_t>(cur - kChoiceSlot)];
                int count = static_cast<int>(ch.options.size());
                ch.value = ch.stepper ? ((ch.value + d) % count + count) % count : std::clamp(ch.value + d, 0, count - 1);
                if (dialog_.onChoice) dialog_.onChoice(dialog_);
            } else if (cur >= 0) {
                dialog_.focus = std::clamp(cur + d, 0, static_cast<int>(dialog_.buttons.size()) - 1);
            }
            Invalidate();
            return true;
        }
        default: return false;
    }
}
