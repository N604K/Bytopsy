#include "app.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>

#include "../util.h"
#include "../version.h"

namespace {

constexpr float kTopH = 56.0f;
constexpr float kSidebarW = 240.0f;
constexpr float kStatusH = 30.0f;
constexpr float kRowH = 32.0f;
constexpr float kHeaderH = 32.0f;

// Segoe Fluent Icons / MDL2 glyphs
constexpr wchar_t IC_SEARCH = 0xE721, IC_CLEAR = 0xE711, IC_MORE = 0xE712, IC_REFRESH = 0xE72C, IC_SUN = 0xE706,
                  IC_MOON = 0xE708, IC_SHIELD = 0xEA18, IC_HOME = 0xE80F, IC_FOLDER = 0xE8B7, IC_LARGEST = 0xE8CB,
                  IC_TYPES = 0xE8FD, IC_COPY = 0xE8C8, IC_CLOCK = 0xE823, IC_EMPTY = 0xE8F4, IC_CLEAN = 0xE74D,
                  IC_HISTORY = 0xE81C, IC_BACK = 0xE72B, IC_FWD = 0xE72A, IC_UP = 0xE74A, IC_CHEV_R = 0xE76C,
                  IC_CHEV_D = 0xE70D, IC_CHEV_U = 0xE70E, IC_CHECK = 0xE73E, IC_INFO = 0xE946, IC_WARN = 0xE7BA,
                  IC_OPEN = 0xE8E5, IC_DELETE = 0xE74D, IC_COMPARE = 0xE8AB, IC_DRIVE = 0xEDA2, IC_GRID = 0xF0E2,
                  IC_UNDO = 0xE7A7, IC_CODE = 0xE943, IC_AI = 0xE99A, IC_TERMINAL = 0xE756, IC_SYSTEM = 0xE770;

std::wstring Pct(double v) {
    wchar_t b[32];
    if (v >= 99.95)
        swprintf(b, 32, L"100%%");
    else if (v >= 10)
        swprintf(b, 32, L"%.0f%%", v);
    else
        swprintf(b, 32, L"%.1f%%", v);
    return b;
}

std::wstring SignedSize(int64_t d) {
    std::wstring s = util::FormatSizeShort(static_cast<uint64_t>(std::llabs(d)));
    return (d > 0 ? L"+" : d < 0 ? L"−" : L"") + s;
}

std::wstring RelativeTime(int64_t ft) {
    int64_t diff = (util::NowFileTime() - ft) / 10000000LL;
    if (diff < 60) return L"just now";
    if (diff < 3600) return std::to_wstring(diff / 60) + L" min ago";
    if (diff < 86400) return std::to_wstring(diff / 3600) + L" h ago";
    if (diff < 86400 * 2) return L"yesterday";
    if (diff < 86400 * 30) return std::to_wstring(diff / 86400) + L" days ago";
    return util::FormatTime(ft).substr(0, 10);
}

// "today", "3 days ago", "8 months ago", "2 years ago": how long a project has been left alone.
std::wstring AgeText(int64_t ft) {
    if (!ft) return L"never";
    int64_t days = (util::NowFileTime() - ft) / 864000000000LL;
    if (days < 1) return L"today";
    if (days < 2) return L"yesterday";
    if (days < 60) return std::to_wstring(days) + L" days ago";
    if (days < 730) return std::to_wstring(days / 30) + L" months ago";
    return std::to_wstring(days / 365) + L" years ago";
}

TextStyle Small(float size = 12.0f, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, Align a = Align::Left) {
    return TS(size, w, a);
}

}  // namespace

const std::wstring& App::TypeName(const std::wstring& ext) {
    auto it = typeNames_.find(ext);
    if (it != typeNames_.end()) return it->second;
    std::wstring name;
    if (ext.empty()) {
        name = L"Files without extension";
    } else {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW((L"x." + ext).c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME))
            name = sfi.szTypeName;
        if (name.empty()) name = L"." + ext + L" file";
    }
    return typeNames_[ext] = name;
}

std::wstring App::FitPath(const std::wstring& s, float width, float fontSize, bool bold) {
    // Keep the end of a path (the most specific part) and elide the beginning.
    TextStyle st = TS(fontSize, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    std::wstring key = s + L'|' + std::to_wstring(static_cast<int>(width)) + L'|' + std::to_wstring(static_cast<int>(fontSize * 10) + bold);
    if (auto it = fitCache_.find(key); it != fitCache_.end()) return it->second;
    if (fitCache_.size() > 4000) fitCache_.clear();
    std::wstring& out = fitCache_[key];
    if (s.empty() || gfx_.Measure(s, st) <= width) return out = s;
    size_t lo = 0, hi = s.size();  // number of trailing characters kept
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (gfx_.Measure(L"…" + s.substr(s.size() - mid), st) <= width)
            lo = mid;
        else
            hi = mid - 1;
    }
    return out = L"…" + s.substr(s.size() - lo);
}

std::wstring App::RelFolder(Node* n) {
    if (!n || !n->parent) return root_ ? root_->name : L"";
    if (n->parent == root_.get()) return root_->name;
    std::wstring rel;
    for (Node* p = n->parent; p && p->parent; p = p->parent) rel = rel.empty() ? p->name : p->name + L"\\" + rel;
    return rel;
}

bool App::Hovered(int kind, int a, intptr_t b) const { return hover_.kind == kind && hover_.a == a && hover_.b == b; }

// =============================================================================================
// Widgets

float App::Button(const RectF& r, wchar_t icon, const std::wstring& text, int cmd, int style, bool enabled) {
    bool hover = enabled && Hovered(HK_BUTTON, cmd);
    D2D1_COLOR_F bg, fg;
    switch (style) {
        case 1: bg = P_.accent; fg = P_.accentText; break;
        case 2: bg = P_.danger; fg = P_.accentText; break;
        case 3: bg = WithAlpha(P_.text, 0); fg = P_.text; break;
        default: bg = P_.surface2; fg = P_.text; break;
    }
    if (hover) bg = style == 3 ? P_.hover : Mix(bg, P_.dark ? Hex(0xFFFFFF) : Hex(0x000000), 0.08f);
    if (style == 3 && hover) bg = P_.surface3;
    if (!enabled) {
        bg = WithAlpha(bg, bg.a * 0.5f);
        fg = WithAlpha(fg, 0.45f);
    }
    gfx_.FillRound(r, 7, bg);
    if (style == 0) gfx_.StrokeRound(r, 7, P_.border);
    float tw = gfx_.Measure(text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD));
    float iw = icon ? 22.0f : 0.0f;
    float x = r.left + (W(r) - tw - iw) / 2;
    if (icon) gfx_.Icon(icon, RWH(x, r.top, 16, H(r)), fg, 14);
    gfx_.Text(text, R(x + iw, r.top, r.right, r.bottom), fg, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD));
    if (enabled) AddHit(r, HK_BUTTON, cmd);
    return W(r);
}

float App::IconButton(float x, float y, wchar_t icon, int cmd, const std::wstring& tip, bool active) {
    RectF r = RWH(x, y, 36, 36);
    bool hover = Hovered(HK_BUTTON, cmd);
    if (active)
        gfx_.FillRound(r, 8, P_.selected);
    else if (hover)
        gfx_.FillRound(r, 8, P_.surface3);
    gfx_.Icon(icon, r, active ? P_.accent : (hover ? P_.text : P_.textDim), 16);
    AddHit(r, HK_BUTTON, cmd);
    if (hover && !tip.empty()) {
        tipText_ = tip;
        tipAnchor_ = r;
    }
    return 36;
}

float App::Chip(float x, float y, const std::wstring& text, bool active, int group, intptr_t value) {
    float w = gfx_.Measure(text, TS(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 26;
    RectF r = RWH(x, y, w, 30);
    bool hover = Hovered(HK_CHIP, group, value);
    gfx_.FillRound(r, 15, active ? P_.accent : (hover ? P_.surface3 : P_.surface2));
    if (!active) gfx_.StrokeRound(r, 15, P_.border);
    gfx_.Text(text, r, active ? P_.accentText : P_.text, TS(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
    AddHit(r, HK_CHIP, group, value);
    return w;
}

void App::Checkbox(const RectF& r, bool checked, bool hover) {
    if (checked) {
        gfx_.FillRound(r, 5, P_.accent);
        gfx_.Icon(IC_CHECK, r, P_.accentText, 12);
    } else {
        gfx_.FillRound(r, 5, hover ? P_.surface3 : P_.surface);
        gfx_.StrokeRound(r, 5, hover ? P_.accent : P_.textFaint, 1.2f);
    }
}

void App::DrawUsageBar(const RectF& r, double frac, bool pct, double pctValue) {
    float barW = W(r) - (pct ? 52 : 0);
    float cy = (r.top + r.bottom) / 2;
    RectF track = R(r.left, cy - 3.5f, r.left + barW, cy + 3.5f);
    gfx_.FillRound(track, 3.5f, P_.track);
    float fw = static_cast<float>(std::clamp(frac, 0.0, 1.0)) * barW;
    if (fw > 0.5f) {
        RectF fill = R(track.left, track.top, track.left + std::max(fw, 4.0f), track.bottom);
        gfx_.GradientH(fill, 3.5f, P_.accent, Mix(P_.accent, P_.accent2, static_cast<float>(std::clamp(frac, 0.0, 1.0))));
    }
    if (pct) gfx_.Text(Pct(pctValue), R(r.left + barW, r.top, r.right, r.bottom), P_.textDim, Small(12, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
}

void App::DrawViewHeader(const RectF& r, const std::wstring& title, const std::wstring& subtitle, wchar_t icon) {
    RectF ic = RWH(r.left, r.top + 2, 40, 40);
    gfx_.FillRound(ic, 10, WithAlpha(P_.accent, 0.16f));
    gfx_.Icon(icon, ic, P_.accent, 18);
    TextStyle t = TS(21, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    t.font = FontKind::Display;
    gfx_.Text(title, R(r.left + 54, r.top - 1, r.right, r.top + 26), P_.text, t);
    gfx_.Text(subtitle, R(r.left + 54, r.top + 25, r.right, r.top + 44), P_.textDim, Small(12.5f));
}

void App::DrawEmptyState(const RectF& area, wchar_t icon, const std::wstring& title, const std::wstring& text) {
    float cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2 - 30;
    gfx_.FillCircle(cx, cy - 10, 34, WithAlpha(P_.accent, 0.12f));
    gfx_.Icon(icon, RWH(cx - 30, cy - 40, 60, 60), P_.accent, 26);
    gfx_.Text(title, R(area.left, cy + 34, area.right, cy + 60), P_.text, TS(16, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
    TextStyle s = TS(13, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
    s.wrap = true;
    s.center = false;
    gfx_.Text(text, R(cx - 260, cy + 64, cx + 260, cy + 130), P_.textDim, s);
}

// =============================================================================================
// Frame

void App::Paint() {
    if (!gfx_.Begin()) return;
    if (rowsDirty_) BuildRows();
    hits_.clear();
    modalHitStart_ = SIZE_MAX;
    tipText_.clear();
    // The search box is a native child window that the overlay scrim can't cover: hide it while modal.
    bool modal = dialog_.active || task_;
    if (search_ && modal == (IsWindowVisible(search_) != FALSE)) {
        if (modal && GetFocus() == search_) SetFocus(hwnd_);
        ShowWindow(search_, modal ? SW_HIDE : SW_SHOWNA);
    }
    D2D1_SIZE_F sz = gfx_.Size();
    gfx_.Clear(P_.bg);

    contentArea_ = R(kSidebarW, kTopH, sz.width, sz.height - kStatusH);
    switch (view_) {
        case View::Home: DrawHome(contentArea_); break;
        case View::Browse: DrawBrowse(contentArea_); break;
        case View::Largest:
        case View::Search:
        case View::Old:
        case View::Empty:
        case View::TypeFiles: DrawResults(contentArea_); break;
        case View::Types: DrawTypes(contentArea_); break;
        case View::Duplicates: DrawDuplicates(contentArea_); break;
        case View::DevJunk: DrawDevJunk(contentArea_); break;
        case View::Cleanup: DrawCleanup(contentArea_); break;
        case View::History: DrawHistory(contentArea_); break;
        case View::Compare: DrawCompare(contentArea_); break;
        case View::Deleted: DrawDeleted(contentArea_); break;
        case View::AiModels: DrawAiModels(contentArea_); break;
        case View::Wsl: DrawWsl(contentArea_); break;
        case View::WinTools: DrawWinTools(contentArea_); break;
        case View::CleanReview: DrawCleanReview(contentArea_); break;
        default: break;
    }
    DrawSidebar();
    DrawTopBar();
    DrawStatusBar();
    DrawToast();
    DrawTooltip();
    if (dialog_.active) DrawDialog();
    if (task_) DrawBusy();
    if (!gfx_.End()) {
        SafeRelease(tmBitmap_);
        tmDirty_ = true;
        Invalidate();
    }
}

void App::DrawTopBar() {
    D2D1_SIZE_F sz = gfx_.Size();
    gfx_.Fill(R(0, 0, sz.width, kTopH), P_.surface);
    gfx_.Fill(R(0, kTopH - 1, sz.width, kTopH), P_.border);
    // Logo: a tiny treemap.
    float lx = 20, ly = 14;
    gfx_.FillRound(RWH(lx, ly, 15, 28), 4, P_.accent);
    gfx_.FillRound(RWH(lx + 17, ly, 11, 15), 3.5f, P_.accent2);
    gfx_.FillRound(RWH(lx + 17, ly + 17, 11, 11), 3.5f, Hex(CategoryRgb(CAT_IMAGE)));
    TextStyle logo = TS(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    logo.font = FontKind::Display;
    gfx_.Text(L"WinNCDU", R(lx + 40, 0, kSidebarW, kTopH), P_.text, logo);

    // Search box (the native edit control lives inside it).
    float left = kSidebarW + 20;
    float boxW = std::clamp(sz.width - left - 230, 200.0f, 620.0f);
    RectF box = RWH(left, 10, boxW, 36);
    bool focused = GetFocus() == search_;
    gfx_.FillRound(box, 9, P_.dark ? Hex(0x272933) : Hex(0xEEF0F6));
    gfx_.StrokeRound(box, 9, focused ? P_.accent : P_.border, focused ? 1.5f : 1.0f);
    gfx_.Icon(IC_SEARCH, RWH(left + 6, 10, 30, 36), focused ? P_.accent : P_.textFaint, 14);
    if (search_ && !IsWindowVisible(search_)) {
        int n = GetWindowTextLengthW(search_);
        std::wstring text(n, L' ');
        if (n) GetWindowTextW(search_, text.data(), n + 1);
        gfx_.Text(n ? text : L"Search files & folders", R(left + 40, 10, box.right - 40, 46), n ? P_.text : P_.textFaint, TS(14));
    }
    if (search_ && GetWindowTextLengthW(search_) > 0) {
        RectF cr = RWH(box.right - 32, 14, 28, 28);
        if (Hovered(HK_BUTTON, CMD_SEARCH_CLEAR)) gfx_.FillRound(cr, 6, P_.surface3);
        gfx_.Icon(IC_CLEAR, cr, P_.textDim, 10);
        AddHit(cr, HK_BUTTON, CMD_SEARCH_CLEAR);
    }

    float x = sz.width - 12 - 36;
    IconButton(x, 10, IC_MORE, CMD_MORE, L"Menu");
    x -= 40;
    IconButton(x, 10, P_.dark ? IC_SUN : IC_MOON, CMD_THEME_TOGGLE, P_.dark ? L"Light theme" : L"Dark theme");
    if (root_ && !readOnly_) {
        x -= 40;
        IconButton(x, 10, IC_REFRESH, CMD_RESCAN, L"Rescan (F5)");
    }
    if (!elevated_) {
        x -= 40;
        IconButton(x, 10, IC_SHIELD, CMD_ADMIN, L"Restart as administrator");
    }
}

void App::DrawSidebar() {
    D2D1_SIZE_F sz = gfx_.Size();
    RectF sb = R(0, kTopH, kSidebarW, sz.height - kStatusH);
    gfx_.Fill(sb, P_.surface);
    gfx_.Fill(R(kSidebarW - 1, kTopH, kSidebarW, sb.bottom), P_.border);
    float y = kTopH + 14;

    if (root_) {
        RectF card = R(12, y, kSidebarW - 12, y + 76);
        gfx_.FillRound(card, 10, P_.surface2);
        std::wstring name = root_->name;
        gfx_.Text(readOnly_ ? L"SAVED SCAN" : L"CURRENT SCAN", R(card.left + 14, card.top + 8, card.right - 10, card.top + 24),
                  readOnly_ ? P_.warning : P_.textFaint, Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        gfx_.Text(FitPath(name, W(card) - 28, 12.5f, true), R(card.left + 14, card.top + 24, card.right - 10, card.top + 42), P_.text,
                  Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        TextStyle big = TS(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        big.font = FontKind::Display;
        gfx_.Text(util::FormatSizeShort(Val(root_.get())), R(card.left + 14, card.top + 44, card.right - 10, card.top + 68),
                  P_.accent, big);
        gfx_.Text(util::FormatCount(root_->items) + L" items", R(card.left + 14, card.top + 44, card.right - 12, card.top + 68),
                  P_.textDim, Small(11.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
        y = card.bottom + 14;
    }

    struct NavItem {
        View v;
        wchar_t icon;
        const wchar_t* label;
        bool needsScan;
        std::wstring badge;
    };
    uint64_t reclaim = 0;
    for (auto& c : cleanup_)
        if (!c.external) reclaim += c.bytes;
    uint64_t dupWaste = 0;
    for (auto& g : dups_) dupWaste += g.size * (g.files.size() - 1);
    uint64_t devJunkTotal = 0;
    for (auto& p : devJunk_) devJunkTotal += DevProjectSize(p, !settings_.apparent);
    std::vector<NavItem> items = {
        {View::Home, IC_HOME, L"Home", false, L""},
        {View::Browse, IC_FOLDER, L"Folders", true, L""},
        {View::Largest, IC_LARGEST, L"Largest files", true, L""},
        {View::Types, IC_TYPES, L"File types", true, L""},
        {View::Duplicates, IC_COPY, L"Duplicates", true, dupsDone_ && dupWaste ? util::FormatSizeShort(dupWaste) : L""},
        {View::Old, IC_CLOCK, L"Old files", true, L""},
        {View::Empty, IC_EMPTY, L"Empty folders", true, L""},
        {View::DevJunk, IC_CODE, L"Developer junk", true, devJunkTotal ? util::FormatSizeShort(devJunkTotal) : L""},
    };
    if (!searchQuery_.empty()) items.push_back({View::Search, IC_SEARCH, L"Search results", true, util::FormatCount(searchRes_.size())});
    auto drawItem = [&](const NavItem& it) {
        RectF r = R(10, y, kSidebarW - 10, y + 36);
        bool active = view_ == it.v || (it.v == View::Types && view_ == View::TypeFiles) ||
                      (it.v == View::History && view_ == View::Compare) || (it.v == View::Cleanup && view_ == View::CleanReview);
        bool enabled = !it.needsScan || root_;
        bool hover = enabled && Hovered(HK_NAV, static_cast<int>(it.v));
        if (active) {
            gfx_.FillRound(r, 8, P_.selected);
            gfx_.FillRound(RWH(r.left, r.top + 9, 3, 18), 1.5f, P_.accent);
        } else if (hover) {
            gfx_.FillRound(r, 8, P_.hover);
        }
        D2D1_COLOR_F c = !enabled ? P_.textFaint : (active ? P_.text : P_.textDim);
        gfx_.Icon(it.icon, RWH(r.left + 10, r.top, 24, 36), active ? P_.accent : c, 15);
        gfx_.Text(it.label, R(r.left + 42, r.top, r.right - 8, r.bottom), active ? P_.text : (enabled ? P_.text : P_.textFaint),
                  TS(13.5f, active ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL));
        if (!it.badge.empty())
            gfx_.Text(it.badge, R(r.left, r.top, r.right - 10, r.bottom), P_.textFaint, Small(11.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
        if (enabled) AddHit(r, HK_NAV, static_cast<int>(it.v));
        y += 38;
    };
    for (auto& it : items) drawItem(it);
    y += 10;
    gfx_.Text(L"MAINTENANCE", R(22, y, kSidebarW, y + 20), P_.textFaint, Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
    y += 24;
    drawItem({View::Cleanup, IC_CLEAN, L"Cleanup", false, reclaim ? util::FormatSizeShort(reclaim) : L""});
    uint64_t aiTotal = 0;
    for (auto& st : ai_) aiTotal += st.bytes;
    drawItem({View::AiModels, IC_AI, L"AI models", false, aiTotal ? util::FormatSizeShort(aiTotal) : L""});
    uint64_t wslTotal = 0;
    for (auto& d : vdisks_) wslTotal += d.Reclaimable();
    drawItem({View::Wsl, IC_TERMINAL, L"WSL & Docker", false, wslTotal ? util::FormatSizeShort(wslTotal) : L""});
    uint64_t oldDrivers = 0;
    for (auto& p : win_.drivers)
        if (p.old) oldDrivers += p.bytes;
    drawItem({View::WinTools, IC_SYSTEM, L"Windows", false, oldDrivers ? util::FormatSizeShort(oldDrivers) : L""});
    drawItem({View::History, IC_HISTORY, L"History", false, L""});
    drawItem({View::Deleted, IC_UNDO, L"Recently deleted", false, L""});

    // Drives
    y += 10;
    if (y + 24 + 46 < sb.bottom && !drives_.empty()) {
        gfx_.Text(L"DRIVES", R(22, y, kSidebarW, y + 20), P_.textFaint, Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        y += 24;
        for (size_t i = 0; i < drives_.size() && y + 46 < sb.bottom; ++i) {
            const DriveInfo& d = drives_[i];
            RectF r = R(10, y, kSidebarW - 10, y + 46);
            if (Hovered(HK_DRIVE, static_cast<int>(i))) gfx_.FillRound(r, 8, P_.hover);
            gfx_.Bitmap(icons_.ForPath(gfx_, d.root), RWH(r.left + 10, r.top + 11, 20, 20));
            std::wstring label = d.root.substr(0, 2) + L"  " + (d.label.empty() ? std::wstring(DriveTypeName(d.type)) + L" disk" : d.label);
            gfx_.Text(label, R(r.left + 40, r.top + 4, r.right - 8, r.top + 24), P_.text, TS(12.5f));
            if (d.ready && d.total) {
                double used = 1.0 - static_cast<double>(d.free) / d.total;
                D2D1_COLOR_F col = used > 0.9 ? P_.danger : (used > 0.75 ? P_.warning : P_.success);
                RectF bar = R(r.left + 40, r.top + 29, r.right - 62, r.top + 33);
                gfx_.FillRound(bar, 2, P_.track);
                gfx_.FillRound(R(bar.left, bar.top, bar.left + W(bar) * static_cast<float>(used), bar.bottom), 2, col);
                gfx_.Text(util::FormatSizeShort(d.free), R(r.left, r.top + 22, r.right - 8, r.top + 40), P_.textFaint,
                          Small(11, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
            }
            AddHit(r, HK_DRIVE, static_cast<int>(i));
            y += 48;
        }
    }
}

void App::DrawStatusBar() {
    D2D1_SIZE_F sz = gfx_.Size();
    RectF r = R(0, sz.height - kStatusH, sz.width, sz.height);
    gfx_.Fill(r, P_.surface);
    gfx_.Fill(R(0, r.top, sz.width, r.top + 1), P_.border);
    std::wstring left;
    if (root_) {
        left = util::FormatCount(root_->items) + L" items  ·  " + util::FormatSizeShort(root_->disk) + L" on disk  ·  " +
               util::FormatSizeShort(root_->size) + L" apparent";
        if (scanSeconds_ > 0 && !readOnly_) left += L"  ·  scanned in " + util::FormatDuration(scanSeconds_);
        if (incomplete_) left += L"  ·  incomplete";
        if (scanErrors_) left += L"  ·  " + util::FormatCount(scanErrors_) + L" folders unreadable";
    } else {
        left = L"Ready";
    }
    gfx_.Text(left, R(14, r.top, sz.width * 0.55f, r.bottom), P_.textDim, Small(12));
    float x = sz.width - 14;
    if (elevated_) {
        float w = gfx_.Measure(L"Administrator", Small(11, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 16;
        RectF b = R(x - w, r.top + 6, x, r.bottom - 6);
        gfx_.FillRound(b, 9, WithAlpha(P_.warning, 0.18f));
        gfx_.Text(L"Administrator", b, P_.warning, Small(11, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
        x -= w + 12;
    }
    if (haveVol_ && volTotal_) {
        std::wstring t = util::FormatSizeShort(volFree_) + L" free of " + util::FormatSizeShort(volTotal_);
        float tw = gfx_.Measure(t, Small(12)) + 4;
        gfx_.Text(t, R(x - tw, r.top, x, r.bottom), P_.textDim, Small(12, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
        x -= tw + 8;
        double used = 1.0 - static_cast<double>(volFree_) / volTotal_;
        RectF bar = R(x - 80, r.top + 12, x, r.top + 18);
        gfx_.FillRound(bar, 3, P_.track);
        gfx_.FillRound(R(bar.left, bar.top, bar.left + 80 * static_cast<float>(used), bar.bottom), 3,
                       used > 0.9 ? P_.danger : (used > 0.75 ? P_.warning : P_.success));
        x -= 92;
    }
    if (!sel_.empty()) {
        uint64_t total = 0;
        auto targets = NormalizeTargets(SelectedNodes());
        for (Node* n : targets) total += Val(n);
        std::wstring t = util::FormatCount(sel_.size()) + L" selected  ·  " + util::FormatSizeShort(total);
        float tw = gfx_.Measure(t, Small(12, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 4;
        gfx_.Text(t, R(x - tw, r.top, x, r.bottom), P_.accent, Small(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
    }
}

// =============================================================================================
// Table

void App::DrawTable(const RectF& area, std::vector<Column>& cols, int sortKey, bool sortDesc,
                    const std::function<void(int, const Column&, int, const RectF&, bool)>& cell,
                    const std::function<void(int, const RectF&)>& fullRow) {
    float left = area.left + 16, right = area.right - 18;
    float fixed = 0, flex = 0;
    for (auto& c : cols) (c.width > 0 ? fixed : flex) += std::fabs(c.width);
    float avail = std::max(0.0f, right - left - fixed - 12.0f * (cols.size() - 1));
    float x = left;
    for (auto& c : cols) {
        c.x = x;
        c.w = c.width > 0 ? c.width : avail * (-c.width) / std::max(0.001f, flex);
        x += c.w + 12;
    }
    // Header
    RectF hdr = R(area.left, area.top, area.right, area.top + kHeaderH);
    for (auto& c : cols) {
        RectF hr = RWH(c.x, hdr.top, c.w, kHeaderH);
        bool active = c.sortKey >= 0 && c.sortKey == sortKey;
        bool hover = c.sortKey >= 0 && Hovered(HK_HEADER, c.sortKey);
        D2D1_COLOR_F col = active ? P_.accent : (hover ? P_.text : P_.textFaint);
        float tw = gfx_.Measure(c.title, Small(12, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        float tx = c.align == Align::Right ? hr.right - tw : hr.left;
        if (c.align == Align::Right && active) tx -= 14;
        gfx_.Text(c.title, R(tx, hr.top, tx + tw + 2, hr.bottom), col, Small(12, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        if (active) gfx_.Icon(sortDesc ? IC_CHEV_D : IC_CHEV_U, RWH(tx + tw + 2, hr.top, 14, kHeaderH), col, 9);
        if (c.sortKey >= 0) AddHit(hr, HK_HEADER, c.sortKey);
    }
    gfx_.Fill(R(area.left + 12, hdr.bottom - 1, area.right - 12, hdr.bottom), P_.border);

    RectF body = R(area.left, hdr.bottom, area.right, area.bottom);
    listArea_ = body;
    ListState& ls = lists_[static_cast<int>(view_)];
    float contentH = rows_.size() * kRowH;
    listContentH_ = contentH;
    ls.scroll = std::clamp(ls.scroll, 0.0f, std::max(0.0f, contentH - H(body)));
    gfx_.PushClip(body);
    int first = static_cast<int>(ls.scroll / kRowH);
    for (int i = std::max(0, first); i < static_cast<int>(rows_.size()); ++i) {
        float y = body.top + i * kRowH - ls.scroll;
        if (y > body.bottom) break;
        RectF rr = R(area.left + 8, y + 1, area.right - 12, y + kRowH - 1);
        const Row& row = rows_[i];
        bool sel = row.node && sel_.count(row.node);
        bool hover = Hovered(HK_ROW, i) || Hovered(HK_BADGE, i);
        AddHit(rr, HK_ROW, i);
        if (sel)
            gfx_.FillRound(rr, 7, i == ls.focus ? P_.selectedFocus : P_.selected);
        else if (hover)
            gfx_.FillRound(rr, 7, P_.hover);
        else if (i == ls.focus && GetFocus() == hwnd_)
            gfx_.StrokeRound(rr, 7, WithAlpha(P_.accent, 0.5f));
        if (fullRow && row.kind == RK_GROUP) {
            fullRow(i, rr);
            continue;
        }
        for (size_t ci = 0; ci < cols.size(); ++ci) {
            RectF cr = R(cols[ci].x, rr.top, cols[ci].x + cols[ci].w, rr.bottom);
            cell(i, cols[ci], static_cast<int>(ci), cr, sel);
        }
    }
    gfx_.PopClip();
    // Scrollbar
    if (contentH > H(body)) {
        RectF track = R(area.right - 10, body.top + 2, area.right - 4, body.bottom - 2);
        float thumbH = std::max(28.0f, H(track) * H(body) / contentH);
        float t = ls.scroll / std::max(1.0f, contentH - H(body));
        RectF thumb = R(track.left, track.top + (H(track) - thumbH) * t, track.right, track.top + (H(track) - thumbH) * t + thumbH);
        bool hv = Hovered(HK_SCROLL) || dragKind_ == HK_SCROLL;
        gfx_.FillRound(thumb, 3, WithAlpha(P_.textFaint, hv ? 0.8f : 0.45f));
        AddHit(R(track.left - 4, track.top, track.right + 4, track.bottom), HK_SCROLL);
    }
}

void App::DrawNameCell(const RectF& cell, Node* n, int depth, bool expander, bool expanded, int row, bool sel) {
    float x = cell.left + depth * 18.0f;
    if (expander) {
        if (n->CanEnter() && !n->children.empty()) {
            RectF er = RWH(x - 2, cell.top, 20, H(cell));
            bool hv = Hovered(HK_EXPANDER, row);
            if (hv) gfx_.FillRound(Inset(er, 1, 6), 4, P_.surface3);
            gfx_.Icon(expanded ? IC_CHEV_D : IC_CHEV_R, er, hv ? P_.text : P_.textFaint, 9);
            AddHit(er, HK_EXPANDER, row);
        }
        x += 20;
    }
    float cy = (cell.top + cell.bottom) / 2;
    gfx_.Bitmap(icons_.ForNode(gfx_, n), RWH(x, cy - 9, 18, 18), n->Has(NF_LINK | NF_EXCLUDED) ? 0.5f : 1.0f);
    x += 26;
    bool hidden = (n->attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
    D2D1_COLOR_F col = n->Has(NF_LINK | NF_EXCLUDED | NF_HARDLINK) ? P_.textFaint : (hidden ? P_.textDim : P_.text);
    TextStyle ts = TS(13, n->IsDir() ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    // Status badges after the name; the shield (protected data) comes first.
    struct Badge {
        std::wstring text;
        D2D1_COLOR_F color;
        wchar_t icon = 0;
        bool outline = false;  // the item contains protected data rather than being protected
    };
    std::vector<Badge> badges;
    const ProtectHit* shield = Shield(n);
    if (shield)
        badges.push_back({shield->rel == ProtectRel::Contains ? shield->group + L" inside" : shield->group,
                          ShieldColor(shield->level), IC_SHIELD, shield->rel == ProtectRel::Contains});
    if (n->Has(NF_ERROR)) badges.push_back({L"No access", P_.danger});
    else if (n->Has(NF_SUBERR)) badges.push_back({L"Partial", P_.warning});
    if (n->Has(NF_LINK)) badges.push_back({L"Link", P_.accent2});
    if (n->Has(NF_HARDLINK)) badges.push_back({L"Hard link", P_.accent2});
    if (n->Has(NF_CLOUD)) badges.push_back({L"Cloud", P_.accent});
    if (n->Has(NF_EXCLUDED)) badges.push_back({L"Excluded", P_.textFaint});
    float bw = 0;
    for (auto& b : badges) bw += gfx_.Measure(b.text, Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + (b.icon ? 32 : 18);
    float nameW = std::min(gfx_.Measure(n->name, ts) + 2, cell.right - x - bw);
    gfx_.Text(n->name, R(x, cell.top, x + std::max(0.0f, nameW), cell.bottom), sel ? P_.text : col, ts);
    x += std::max(0.0f, nameW) + 8;
    for (auto& b : badges) {
        float iw = b.icon ? 14.0f : 0.0f;
        float w = gfx_.Measure(b.text, Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 12 + iw;
        if (x + w > cell.right) break;
        RectF br = R(x, cy - 9, x + w, cy + 9);
        if (b.outline)
            gfx_.StrokeRound(Inset(br, 0.5f, 0.5f), 8.5f, WithAlpha(b.color, 0.7f));
        else
            gfx_.FillRound(br, 9, WithAlpha(b.color, 0.16f));
        if (b.icon) gfx_.Icon(b.icon, RWH(br.left + 5, br.top, 12, H(br)), b.color, 10);
        gfx_.Text(b.text, R(br.left + iw, br.top, br.right, br.bottom), b.color, Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
        if (b.icon) {
            AddHit(br, HK_BADGE, row);
            if (Hovered(HK_BADGE, row)) {
                tipText_ = std::wstring(ProtectLevelName(shield->level)) + L": " + DescribeHit(*shield);
                tipAnchor_ = br;
            }
        }
        x += w + 6;
    }
}

D2D1_COLOR_F App::ShieldColor(Protect p) const {
    switch (p) {
        case Protect::Blocked: return P_.danger;
        case Protect::Important: return P_.warning;
        default: return P_.textDim;
    }
}

// In the folder view, items covered by the same protected location as the folder they are listed
// in stay unmarked: the open folder's notice (or the expanded parent row) already says it once.
const ProtectHit* App::Shield(Node* n) {
    if (view_ == View::Browse) return badges_.InList(n);
    const ProtectHit& h = badges_.For(n);
    return h.level == Protect::None ? nullptr : &h;
}

// =============================================================================================
// Browse

void App::DrawBrowse(const RectF& area) {
    if (!root_ || !browseDir_) return;
    float x = area.left + 16, y = area.top + 12;
    IconButton(x, y, IC_BACK, CMD_BACK, L"Back (Alt+←)");
    x += 38;
    IconButton(x, y, IC_FWD, CMD_FORWARD, L"Forward (Alt+→)");
    x += 38;
    IconButton(x, y, IC_UP, CMD_UP, L"Up one level (Backspace)");
    x += 46;

    // Right-side controls: size mode segmented control + treemap toggle.
    float rx = area.right - 16;
    {
        rx -= 36;
        IconButton(rx, y, IC_GRID, CMD_TOGGLE_TREEMAP, settings_.treemap ? L"Hide treemap" : L"Show treemap", settings_.treemap);
        rx -= 10;
        const wchar_t* labels[2] = {L"Disk usage", L"Size"};
        int cmds[2] = {CMD_MODE_DISK, CMD_MODE_APPARENT};
        float w0 = gfx_.Measure(labels[0], Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 24;
        float w1 = gfx_.Measure(labels[1], Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 24;
        RectF seg = R(rx - w0 - w1 - 8, y + 2, rx, y + 34);
        gfx_.FillRound(seg, 8, P_.surface2);
        float sx = seg.left + 4;
        for (int i = 0; i < 2; ++i) {
            float w = i == 0 ? w0 : w1;
            RectF r = R(sx, seg.top + 3, sx + w, seg.bottom - 3);
            bool active = (i == 1) == settings_.apparent;
            if (active) {
                gfx_.FillRound(r, 6, P_.dark ? P_.surface3 : P_.surface);
                if (!P_.dark) gfx_.StrokeRound(r, 6, P_.border);
            } else if (Hovered(HK_BUTTON, cmds[i])) {
                gfx_.FillRound(r, 6, P_.hover);
            }
            gfx_.Text(labels[i], r, active ? P_.text : P_.textDim, Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
            AddHit(r, HK_BUTTON, cmds[i]);
            sx += w;
        }
        rx = seg.left - 12;
    }

    // Breadcrumb
    std::vector<Node*> chain;
    for (Node* p = browseDir_; p; p = p->parent) chain.push_back(p);
    std::reverse(chain.begin(), chain.end());
    auto crumbText = [](Node* n) { return n->parent ? n->name : n->name; };
    float availW = rx - x;
    size_t from = 0;
    auto widthFrom = [&](size_t f) {
        float w = f > 0 ? 30.0f : 0.0f;
        for (size_t i = f; i < chain.size(); ++i) w += gfx_.Measure(crumbText(chain[i]), TS(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 30;
        return w;
    };
    while (from + 1 < chain.size() && widthFrom(from) > availW) ++from;
    if (from > 0) {
        gfx_.Text(L"…", RWH(x, y, 20, 36), P_.textFaint, TS(13.5f));
        x += 24;
    }
    for (size_t i = from; i < chain.size(); ++i) {
        bool last = i + 1 == chain.size();
        std::wstring t = crumbText(chain[i]);
        float tw = std::min(gfx_.Measure(t, TS(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 16, std::max(40.0f, rx - x));
        if (tw < gfx_.Measure(t, TS(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 16) t = FitPath(t, tw - 18, 13.5f, last);
        RectF cr = RWH(x, y + 2, tw, 32);
        bool hv = !last && Hovered(HK_CRUMB, 0, reinterpret_cast<intptr_t>(chain[i]));
        if (hv) gfx_.FillRound(cr, 6, P_.hover);
        gfx_.Text(t, Inset(cr, 8, 0), last ? P_.text : (hv ? P_.text : P_.textDim),
                  TS(13.5f, last ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL));
        if (!last) AddHit(cr, HK_CRUMB, 0, reinterpret_cast<intptr_t>(chain[i]));
        x += tw;
        if (!last) {
            gfx_.Icon(IC_CHEV_R, RWH(x, y, 14, 36), P_.textFaint, 8);
            x += 14;
        }
    }

    // Folder summary line
    float sy = area.top + 56;
    uint64_t files = 0, dirs = 0;
    for (auto& c : browseDir_->children) (c->IsDir() ? dirs : files)++;
    std::wstring summary = util::FormatSizeShort(Val(browseDir_)) + L"   ·   " + util::FormatCount(browseDir_->items) +
                           L" items inside   ·   " + util::FormatCount(dirs) + (dirs == 1 ? L" folder, " : L" folders, ") +
                           util::FormatCount(files) + (files == 1 ? L" file here" : L" files here");
    if (browseDir_->parent && Val(browseDir_->parent))
        summary += L"   ·   " + Pct(100.0 * Val(browseDir_) / Val(browseDir_->parent)) + L" of parent";
    // The open folder's protection, when it covers the items listed here (their rows stay unmarked).
    float summaryRight = area.right - 20;
    if (const ProtectHit* here = badges_.ForContents(browseDir_)) {
        std::wstring t = std::wstring(ProtectLevelName(here->level)) + L": " +
                         (here->group.empty() ? here->name : here->group + L" · " + here->name);
        TextStyle ts = Small(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float w = std::min(gfx_.Measure(t, ts) + 34, std::max(120.0f, (area.right - area.left) * 0.5f));
        RectF pr = R(summaryRight - w, sy, summaryRight, sy + 22);
        D2D1_COLOR_F col = ShieldColor(here->level);
        gfx_.FillRound(pr, 11, WithAlpha(col, 0.14f));
        gfx_.Icon(IC_SHIELD, RWH(pr.left + 8, pr.top, 14, H(pr)), col, 11);
        gfx_.Text(t, R(pr.left + 26, pr.top, pr.right - 8, pr.bottom), col, ts);
        AddHit(pr, HK_BADGE, -1);
        if (Hovered(HK_BADGE, -1)) {
            tipText_ = here->reason.empty() ? DescribeHit(*here) : here->reason;
            tipAnchor_ = pr;
        }
        summaryRight = pr.left - 12;
    }
    gfx_.Text(summary, R(area.left + 24, sy, summaryRight, sy + 22), P_.textDim, Small(12.5f));

    // List + treemap split
    RectF body = R(area.left, area.top + 84, area.right, area.bottom);
    contentArea_ = body;
    RectF listR = body, tmR{};
    if (settings_.treemap && H(body) > 260) {
        float splitY = body.top + H(body) * settings_.split;
        listR = R(body.left, body.top, body.right, splitY - 4);
        RectF spl = R(body.left, splitY - 4, body.right, splitY + 4);
        bool hv = Hovered(HK_SPLITTER) || dragKind_ == HK_SPLITTER;
        float cx = (spl.left + spl.right) / 2;
        gfx_.FillRound(R(cx - 22, splitY - 2, cx + 22, splitY + 2), 2, hv ? P_.accent : P_.border);
        AddHit(spl, HK_SPLITTER);
        tmR = R(body.left + 16, splitY + 6, body.right - 16, body.bottom - 12);
    }

    std::vector<Column> cols = {{L"Name", -1.0f, Align::Left, SK_NAME},
                                {settings_.apparent ? L"Size" : L"Disk usage", 96, Align::Right, SK_SIZE},
                                {L"Share of folder", 170, Align::Left, -1},
                                {L"Items", 80, Align::Right, SK_ITEMS},
                                {L"Modified", 128, Align::Left, SK_MTIME}};
    if (W(listR) < 760) {
        cols.erase(cols.begin() + 4);
        if (W(listR) < 640) cols.erase(cols.begin() + 3);
    }
    if (rows_.empty()) {
        DrawEmptyState(listR, IC_FOLDER, L"This folder is empty", L"There's nothing inside it, or everything is hidden.");
    } else {
        DrawTable(listR, cols, sortKey_, sortDesc_, [&](int i, const Column& c, int, const RectF& cr, bool sel) {
            const Row& row = rows_[i];
            Node* n = row.node;
            switch (c.sortKey) {
                case SK_NAME: DrawNameCell(cr, n, row.depth, true, expanded_.count(n) > 0, i, sel); break;
                case SK_SIZE:
                    gfx_.Text(util::FormatSizeShort(Val(n)), cr, n->Has(NF_HARDLINK) ? P_.textFaint : P_.text,
                              TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
                    break;
                case SK_ITEMS:
                    gfx_.Text(n->IsDir() ? util::FormatCount(n->items) : L"—", cr, P_.textDim, Small(12.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
                    break;
                case SK_MTIME: gfx_.Text(util::FormatTime(n->mtime), cr, P_.textDim, Small(12.5f)); break;
                default: {
                    uint64_t pv = n->parent ? Val(n->parent) : 0;
                    double frac = pv ? static_cast<double>(Val(n)) / pv : 0;
                    DrawUsageBar(cr, frac, true, frac * 100);
                }
            }
        });
    }
    if (W(tmR) > 0) DrawTreemap(tmR);
}

// =============================================================================================
// Treemap

void App::DrawTreemap(const RectF& area) {
    gfx_.FillRound(area, 12, P_.surface);
    gfx_.StrokeRound(area, 12, P_.border);
    // Legend
    float lx = area.left + 16;
    gfx_.Text(L"Treemap", R(lx, area.top + 8, lx + 80, area.top + 30), P_.text, Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
    lx += 74;
    const wchar_t* hint = L"click to select · double-click to zoom";
    float hintW = gfx_.Measure(hint, Small(11)) + 24;
    float legendRight = area.right - 16;
    float legendW = 0;
    for (int c = 0; c < CAT_COUNT; ++c) legendW += gfx_.Measure(CategoryName(c), Small(11.5f)) + 28;
    if (lx + legendW + hintW < legendRight) {
        gfx_.Text(hint, R(area.left, area.top + 8, area.right - 16, area.top + 30), P_.textFaint, Small(11, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
        legendRight -= hintW;
    }
    for (int c = 0; c < CAT_COUNT; ++c) {
        float w = gfx_.Measure(CategoryName(c), Small(11.5f));
        if (lx + w + 14 > legendRight) break;
        gfx_.FillRound(RWH(lx, area.top + 15, 9, 9), 2.5f, Hex(CategoryRgb(c)));
        gfx_.Text(CategoryName(c), R(lx + 14, area.top + 8, lx + 14 + w + 2, area.top + 30), P_.textDim, Small(11.5f));
        lx += w + 28;
    }
    RectF map = R(area.left + 10, area.top + 36, area.right - 10, area.bottom - 10);
    if (W(map) < 20 || H(map) < 20) return;

    bool relayout = tmDirty_ || tmRoot_ != browseDir_ || std::fabs(W(map) - W(tmArea_)) > 0.5f ||
                    std::fabs(H(map) - H(tmArea_)) > 0.5f || std::fabs(map.left - tmArea_.left) > 0.5f ||
                    std::fabs(map.top - tmArea_.top) > 0.5f || tmGen_ != gfx_.Generation() || !tmBitmap_;
    if (relayout && dragKind_ != HK_SPLITTER) {
        tm_ = LayoutTreemap(browseDir_, map, !settings_.apparent);
        tmRoot_ = browseDir_;
        tmArea_ = map;
        tmDirty_ = false;
        tmGen_ = gfx_.Generation();
        tmIndex_.clear();
        for (size_t i = 0; i < tm_.size(); ++i) tmIndex_[tm_[i].node] = static_cast<int>(i);
        // Shields on protected rectangles big enough to carry one. Items inside a protected folder
        // aren't marked: the folder's own rectangle (or the open folder's notice) is.
        tmShields_.clear();
        std::vector<char> shielded(tm_.size());
        for (size_t i = 0; i < tm_.size(); ++i) {
            const TmRect& t = tm_[i];
            if (t.depth < 1 || W(t.r) < 22 || H(t.r) < 18) continue;
            const ProtectHit& h = badges_.For(t.node);
            if (h.level == Protect::None || h.rel == ProtectRel::Inside) continue;
            tmShields_.push_back(static_cast<int>(i));
            shielded[i] = 1;
        }
        SafeRelease(tmBitmap_);
        ID2D1BitmapRenderTarget* brt = nullptr;
        if (SUCCEEDED(gfx_.RT()->CreateCompatibleRenderTarget(D2D1::SizeF(W(map), H(map)), &brt))) {
            ID2D1SolidColorBrush* br = nullptr;
            brt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
            D2D1_GRADIENT_STOP stops[2] = {{0.0f, D2D1::ColorF(1, 1, 1, 0.22f)}, {1.0f, D2D1::ColorF(0, 0, 0, 0.18f)}};
            ID2D1GradientStopCollection* coll = nullptr;
            brt->CreateGradientStopCollection(stops, 2, &coll);
            ID2D1LinearGradientBrush* gloss = nullptr;
            if (coll) brt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 1)), coll, &gloss);
            brt->BeginDraw();
            brt->Clear(P_.surface);
            brt->SetTransform(D2D1::Matrix3x2F::Translation(-map.left, -map.top));
            D2D1_COLOR_F dirBg = P_.dark ? Hex(0x2B2E3A) : Hex(0xD5DAE5);
            for (auto& t : tm_) {
                if (!t.leaf) {
                    br->SetColor(t.depth == 0 ? P_.surface : Mix(dirBg, P_.surface, std::min(0.6f, t.depth * 0.08f)));
                    brt->FillRectangle(t.r, br);
                    continue;
                }
                RectF r = W(t.r) > 3 && H(t.r) > 3 ? Inset(t.r, 0.5f, 0.5f) : t.r;
                D2D1_COLOR_F c = t.category >= 0 ? Hex(CategoryRgb(t.category)) : (P_.dark ? Hex(0x4A5163) : Hex(0xAEB6C6));
                br->SetColor(P_.dark ? Mix(c, Hex(0x000000), 0.12f) : c);
                brt->FillRectangle(r, br);
                if (gloss && W(r) > 4 && H(r) > 4) {
                    gloss->SetStartPoint(D2D1::Point2F(r.left, r.top));
                    gloss->SetEndPoint(D2D1::Point2F(r.right, r.bottom));
                    brt->FillRectangle(r, gloss);
                }
            }
            // Labels for the big top-level items.
            IDWriteTextFormat* fmt = nullptr;
            IDWriteFactory* dw = nullptr;
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dw));
            if (dw) dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"", &fmt);
            if (fmt) {
                fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                IDWriteInlineObject* ell = nullptr;
                dw->CreateEllipsisTrimmingSign(fmt, &ell);
                fmt->SetTrimming(&trim, ell);
                for (size_t i = 0; i < tm_.size(); ++i) {
                    const TmRect& t = tm_[i];
                    if (t.depth != 1 || W(t.r) < 70 || H(t.r) < 34) continue;
                    std::wstring label = t.node->name + L"  " + util::FormatSizeShort(Val(t.node));
                    RectF lr = R(t.r.left + 6, t.r.top + 4, t.r.right - (shielded[i] ? 24 : 4), t.r.top + 20);
                    br->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
                    brt->DrawText(label.c_str(), static_cast<UINT32>(label.size()), fmt, R(lr.left + 1, lr.top + 1, lr.right + 1, lr.bottom + 1), br,
                                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
                    brt->DrawText(label.c_str(), static_cast<UINT32>(label.size()), fmt, lr, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                SafeRelease(ell);
                fmt->Release();
            }
            SafeRelease(dw);
            brt->EndDraw();
            brt->GetBitmap(&tmBitmap_);
            SafeRelease(gloss);
            SafeRelease(coll);
            SafeRelease(br);
            brt->Release();
        }
        tmHover_ = mouseX_ >= 0 && Inside(map, mouseX_, mouseY_) ? HitTreemap(tm_, mouseX_, mouseY_) : -1;
    }
    if (tmBitmap_) {
        gfx_.PushClip(map);
        gfx_.Bitmap(tmBitmap_, RWH(tmArea_.left, tmArea_.top, W(tmArea_), H(tmArea_)));
        gfx_.PopClip();
    }
    for (int i : tmShields_) {
        const TmRect& t = tm_[i];
        const ProtectHit& h = badges_.For(t.node);
        D2D1_COLOR_F col = ShieldColor(h.level);
        if (h.rel == ProtectRel::Self) gfx_.Stroke(Inset(t.r, 0.75f, 0.75f), WithAlpha(col, 0.85f), 1.5f);
        RectF chip = RWH(t.r.right - 20, t.r.top + 3, 17, 15);
        gfx_.FillRound(chip, 4, P_.dark ? Hex(0x000000, 0.6f) : Hex(0xFFFFFF, 0.9f));
        gfx_.Icon(IC_SHIELD, chip, col, 10);
    }
    // Selection and hover outlines.
    for (Node* s : sel_) {
        auto it = tmIndex_.find(s);
        if (it != tmIndex_.end()) {
            gfx_.Stroke(tm_[it->second].r, P_.dark ? Hex(0xFFFFFF) : Hex(0x111111), 2.0f);
        }
    }
    if (tmHover_ >= 0 && tmHover_ < static_cast<int>(tm_.size())) {
        const TmRect& h = tm_[tmHover_];
        for (int i = tmHover_; i >= 0; --i)
            if (tm_[i].depth == 1 && IsAncestor(tm_[i].node, h.node)) {
                gfx_.Stroke(tm_[i].r, P_.accent, 2.0f);
                break;
            }
        gfx_.Stroke(h.r, Hex(0xFFFFFF, 0.9f), 1.5f);
    }
    AddHit(map, HK_TREEMAP);
}

// =============================================================================================
// Result lists

void App::DrawResults(const RectF& area) {
    std::wstring title, sub;
    wchar_t icon = IC_LARGEST;
    uint64_t total = 0;
    for (auto& r : rows_)
        if (r.node) total += Val(r.node);
    std::wstring countText = util::FormatCount(rows_.size()) + (rows_.size() == 1 ? L" item" : L" items") + L"  ·  " +
                             util::FormatSizeShort(total);
    float top = area.top + 18;
    switch (view_) {
        case View::Largest:
            title = L"Largest files";
            sub = L"The 1,000 biggest files of the scan  ·  " + countText;
            break;
        case View::Search:
            title = L"Search results";
            sub = L"“" + searchQuery_ + L"”  ·  " + countText;
            icon = IC_SEARCH;
            break;
        case View::Old:
            title = L"Old files";
            sub = L"Files not modified for a long time  ·  " + countText;
            icon = IC_CLOCK;
            break;
        case View::Empty:
            title = L"Empty folders";
            sub = L"Folders without a single file inside  ·  " + util::FormatCount(rows_.size()) + L" folders";
            icon = IC_EMPTY;
            break;
        case View::TypeFiles:
            title = TypeName(typeFilesExt_);
            sub = (typeFilesExt_.empty() ? L"" : L"*." + typeFilesExt_ + L"  ·  ") + countText;
            icon = IC_TYPES;
            break;
        default: break;
    }
    float hx = area.left + 24;
    if (view_ == View::TypeFiles) {
        IconButton(hx, top + 4, IC_BACK, CMD_BACK_TO_TYPES, L"Back to file types");
        hx += 46;
    }
    DrawViewHeader(R(hx, top, area.right - 24, top + 46), title, sub, icon);
    float y = top + 58;
    if (view_ == View::Old) {
        float x = area.left + 24;
        gfx_.Text(L"Not modified for", R(x, y, x + 120, y + 30), P_.textDim, Small(12.5f));
        x += 112;
        std::pair<const wchar_t*, int> ages[] = {{L"3 months", 90}, {L"6 months", 182}, {L"1 year", 365}, {L"2 years", 730}, {L"5 years", 1826}};
        for (auto& a : ages) x += Chip(x, y, a.first, oldAgeDays_ == a.second, 2, a.second) + 6;
        x += 18;
        if (x + 82 + 4 * 80 > area.right - 24) {
            x = area.left + 24;
            y += 40;
        }
        gfx_.Text(L"Larger than", R(x, y, x + 90, y + 30), P_.textDim, Small(12.5f));
        x += 82;
        std::pair<const wchar_t*, uint64_t> sizes[] = {{L"Any", 0}, {L"1 MB", 1ull << 20}, {L"10 MB", 10ull << 20}, {L"100 MB", 100ull << 20}};
        for (auto& s : sizes) x += Chip(x, y, s.first, oldMinSize_ == s.second, 3, static_cast<intptr_t>(s.second)) + 6;
        y += 44;
    }
    if (view_ == View::Search) {
        gfx_.Text(L"Tip: combine terms like   *.mp4 >500M older:1y   or   type:d node_modules", R(area.left + 24, y, area.right - 24, y + 22),
                  P_.textFaint, Small(12));
        y += 28;
    }
    RectF tr = R(area.left, y, area.right, area.bottom);
    if (rows_.empty()) {
        DrawEmptyState(tr, icon, L"Nothing found", view_ == View::Search ? L"No files or folders match your search." : L"There are no items for this view.");
        return;
    }
    std::vector<Column> cols = {{L"Name", -1.0f, Align::Left, SK_NAME},
                                {L"Folder", -1.25f, Align::Left, SK_FOLDER},
                                {settings_.apparent ? L"Size" : L"Disk usage", 100, Align::Right, SK_SIZE},
                                {L"Modified", 128, Align::Left, SK_MTIME}};
    DrawTable(tr, cols, resSortKey_, resSortDesc_, [&](int i, const Column& c, int, const RectF& cr, bool sel) {
        Node* n = rows_[i].node;
        if (!n) return;
        switch (c.sortKey) {
            case SK_NAME: DrawNameCell(cr, n, 0, false, false, i, sel); break;
            case SK_FOLDER: gfx_.Text(FitPath(RelFolder(n), c.w, 12.5f), cr, P_.textDim, Small(12.5f)); break;
            case SK_SIZE: gfx_.Text(util::FormatSizeShort(Val(n)), cr, P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right)); break;
            case SK_MTIME: gfx_.Text(util::FormatTime(n->mtime), cr, P_.textDim, Small(12.5f)); break;
        }
    });
}

void App::DrawTypes(const RectF& area) {
    float top = area.top + 18;
    uint64_t total = 0;
    for (int c = 0; c < CAT_COUNT; ++c) total += catTotals_[c];
    DrawViewHeader(R(area.left + 24, top, area.right - 24, top + 46), L"File types",
                   util::FormatCount(types_.size()) + L" kinds of files  ·  " + util::FormatSizeShort(total) + L"  ·  double-click a type to list its files",
                   IC_TYPES);
    // Category distribution bar + legend.
    float y = top + 62;
    RectF bar = R(area.left + 24, y, area.right - 24, y + 14);
    gfx_.FillRound(bar, 7, P_.track);
    if (total) {
        gfx_.PushClip(bar);
        float x = bar.left;
        for (int c = 0; c < CAT_COUNT; ++c) {
            float w = W(bar) * static_cast<float>(static_cast<double>(catTotals_[c]) / total);
            if (w <= 0) continue;
            gfx_.Fill(R(x, bar.top, x + w + 0.5f, bar.bottom), Hex(CategoryRgb(c)));
            x += w;
        }
        gfx_.PopClip();
        // Round the ends by painting the background corners.
    }
    y += 24;
    float lx = area.left + 24;
    for (int c = 0; c < CAT_COUNT; ++c) {
        if (!catTotals_[c]) continue;
        std::wstring t = std::wstring(CategoryName(c)) + L"  " + util::FormatSizeShort(catTotals_[c]);
        float w = gfx_.Measure(t, Small(12)) + 30;
        if (lx + w > area.right - 24) {
            lx = area.left + 24;
            y += 22;
        }
        gfx_.FillRound(RWH(lx, y + 6, 10, 10), 3, Hex(CategoryRgb(c)));
        gfx_.Text(t, R(lx + 16, y, lx + w, y + 22), P_.textDim, Small(12));
        lx += w;
    }
    y += 34;
    std::vector<Column> cols = {{L"Type", -1.0f, Align::Left, -1},
                                {L"Files", 90, Align::Right, -1},
                                {settings_.apparent ? L"Size" : L"Disk usage", 100, Align::Right, -1},
                                {L"Share", 210, Align::Left, -1}};
    uint64_t maxv = 0;
    for (auto& t : types_) maxv = std::max(maxv, settings_.apparent ? t.size : t.disk);
    DrawTable(R(area.left, y, area.right, area.bottom), cols, -1, true, [&](int i, const Column& c, int ci, const RectF& cr, bool) {
        const ExtStat& t = types_[rows_[i].idx];
        uint64_t v = settings_.apparent ? t.size : t.disk;
        switch (ci) {
            case 0: {
                float cy = (cr.top + cr.bottom) / 2;
                gfx_.Bitmap(t.ext.empty() ? nullptr : icons_.ForExt(gfx_, t.ext), RWH(cr.left, cy - 9, 18, 18));
                gfx_.FillRound(RWH(cr.left + 26, cy - 4, 8, 8), 2.5f, Hex(CategoryRgb(CategoryOfExt(t.ext))));
                std::wstring ext = t.ext.empty() ? L"(none)" : L"." + t.ext;
                float ew = gfx_.Measure(ext, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 12;
                gfx_.Text(ext, R(cr.left + 42, cr.top, cr.left + 42 + ew, cr.bottom), P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD));
                gfx_.Text(TypeName(t.ext), R(cr.left + 42 + ew, cr.top, cr.right, cr.bottom), P_.textDim, Small(12.5f));
                break;
            }
            case 1: gfx_.Text(util::FormatCount(t.count), cr, P_.textDim, Small(12.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right)); break;
            case 2: gfx_.Text(util::FormatSizeShort(v), cr, P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right)); break;
            default: DrawUsageBar(cr, maxv ? static_cast<double>(v) / maxv : 0, true, total ? 100.0 * v / total : 0);
        }
        (void)c;
    });
}

void App::DrawDuplicates(const RectF& area) {
    float top = area.top + 18;
    uint64_t waste = 0, files = 0;
    for (auto& g : dups_) {
        waste += g.size * (g.files.size() - 1);
        files += g.files.size();
    }
    std::wstring sub = dupsDone_ ? util::FormatCount(dups_.size()) + (dups_.size() == 1 ? L" set of identical files  ·  " : L" sets of identical files  ·  ") + util::FormatCount(files) +
                                       L" files  ·  " + util::FormatSizeShort(waste) + L" can be reclaimed"
                                 : L"Find files with identical content (compared by size, then SHA-256)";
    float bx = area.right - 24;
    if (dupsDone_ && !dups_.empty()) {
        if (!sel_.empty()) {
            bx -= 150;
            Button(RWH(bx, top + 6, 150, 34), IC_DELETE, L"Delete selected", CMD_DELETE, 2);
            bx -= 8;
        }
        bx -= 196;
        Button(RWH(bx, top + 6, 196, 34), IC_CHECK, L"Select all but oldest", CMD_DUP_MARK, 0);
        bx -= 16;
    }
    DrawViewHeader(R(area.left + 24, top, bx, top + 46), L"Duplicate files", sub, IC_COPY);
    float y = top + 58;
    float x = area.left + 24;
    gfx_.Text(L"Ignore files smaller than", R(x, y, x + 170, y + 30), P_.textDim, Small(12.5f));
    x += 160;
    std::pair<const wchar_t*, uint64_t> sizes[] = {{L"100 KB", 100ull << 10}, {L"1 MB", 1ull << 20}, {L"10 MB", 10ull << 20}, {L"100 MB", 100ull << 20}};
    for (auto& s : sizes) x += Chip(x, y, s.first, dupMin_ == s.second, 1, static_cast<intptr_t>(s.second)) + 6;
    x += 12;
    Button(RWH(x, y - 1, 150, 32), IC_SEARCH, dupsDone_ ? L"Search again" : L"Find duplicates", CMD_DUP_FIND, 1, !readOnly_);
    y += 46;
    RectF tr = R(area.left, y, area.right, area.bottom);
    if (!dupsDone_) {
        DrawEmptyState(tr, IC_COPY, readOnly_ ? L"Not available for saved scans" : L"Ready when you are",
                       readOnly_ ? L"Duplicate detection needs to read the real files."
                                 : L"Pick a minimum size and press “Find duplicates”. Large drives can take a while; you can cancel any time.");
        return;
    }
    if (dups_.empty()) {
        DrawEmptyState(tr, IC_CHECK, L"No duplicates found", L"Every file larger than the minimum size is unique.");
        return;
    }
    std::vector<Column> cols = {{L"Name", -1.0f, Align::Left, -1},
                                {L"Folder", -1.3f, Align::Left, -1},
                                {L"Size", 100, Align::Right, -1},
                                {L"Modified", 128, Align::Left, -1}};
    DrawTable(
        tr, cols, -1, true,
        [&](int i, const Column&, int ci, const RectF& cr, bool sel) {
            Node* n = rows_[i].node;
            if (!n) return;
            switch (ci) {
                case 0: {
                    RectF c2 = cr;
                    c2.left += 18;
                    DrawNameCell(c2, n, 0, false, false, i, sel);
                    const DupGroup& g = dups_[rows_[i].idx];
                    if (!g.files.empty() && g.files[0] == n)
                        gfx_.Icon(0xE735 /*FavoriteStarFill*/, RWH(cr.left, cr.top, 14, H(cr)), P_.warning, 10);
                    break;
                }
                case 1: gfx_.Text(FitPath(RelFolder(n), cr.right - cr.left, 12.5f), cr, P_.textDim, Small(12.5f)); break;
                case 2: gfx_.Text(util::FormatSizeShort(n->size), cr, P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right)); break;
                default: gfx_.Text(util::FormatTime(n->mtime), cr, P_.textDim, Small(12.5f));
            }
        },
        [&](int i, const RectF& r) {
            const DupGroup& g = dups_[rows_[i].idx];
            gfx_.Icon(IC_COPY, RWH(r.left + 8, r.top, 22, H(r)), P_.accent2, 13);
            std::wstring t = util::FormatCount(g.files.size()) + L" identical copies of " + util::FormatSizeShort(g.size);
            float tw = gfx_.Measure(t, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 4;
            gfx_.Text(t, R(r.left + 36, r.top, r.left + 36 + tw, r.bottom), P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD));
            gfx_.Text(util::FormatSizeShort(g.size * (g.files.size() - 1)) + L" reclaimable", R(r.left + 48 + tw, r.top, r.right, r.bottom),
                      P_.accent2, Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        });
}

void App::DrawDevJunk(const RectF& area) {
    float top = area.top + 18;
    bool disk = !settings_.apparent;
    uint64_t total = 0;
    size_t folders = 0;
    for (auto& p : devJunk_) {
        total += DevProjectSize(p, disk);
        folders += p.artifacts.size();
    }
    std::wstring sub = devJunk_.empty() ? L"Build output and dependencies your projects recreate on the next build or install"
                                        : util::FormatCount(devJunk_.size()) + (devJunk_.size() == 1 ? L" project  ·  " : L" projects  ·  ") +
                                              util::FormatCount(folders) + L" folders  ·  " + util::FormatSizeShort(total) +
                                              L" comes back with the next build or install";
    float bx = area.right - 24;
    if (!devJunk_.empty()) {
        if (!sel_.empty()) {
            bx -= 150;
            Button(RWH(bx, top + 6, 150, 34), IC_DELETE, L"Delete selected", CMD_DELETE, 2, !readOnly_);
            bx -= 8;
        }
        bx -= 222;
        Button(RWH(bx, top + 6, 222, 34), IC_CLOCK, L"Select inactive 3+ months", CMD_DEV_MARK_OLD, 0);
        bx -= 16;
    }
    DrawViewHeader(R(area.left + 24, top, bx, top + 46), L"Developer junk", sub, IC_CODE);
    float y = top + 58;
    if (!devJunk_.empty()) {
        float x = area.left + 24;
        gfx_.Text(L"Sort projects by", R(x, y, x + 120, y + 30), P_.textDim, Small(12.5f));
        x += 112;
        x += Chip(x, y, L"Size", !devJunkByAge_, 4, 0) + 6;
        Chip(x, y, L"Least recently changed", devJunkByAge_, 4, 1);
        gfx_.Text(L"Click a project to select all its folders", R(area.left + 24, y, area.right - 24, y + 30), P_.textFaint,
                  Small(12, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
        y += 44;
    }
    RectF tr = R(area.left, y, area.right, area.bottom);
    if (devJunk_.empty()) {
        DrawEmptyState(tr, IC_CODE, L"No developer junk found",
                       L"No node_modules, target, bin/obj, build, .gradle, Unity Library, __pycache__ or .venv folders next to their projects "
                       L"in this scan. App folders (AppData, Program Files) are not searched.");
        return;
    }
    std::vector<Column> cols = {{L"Folder", -1.0f, Align::Left, -1},
                                {L"Comes back with", -1.0f, Align::Left, -1},
                                {settings_.apparent ? L"Size" : L"Disk usage", 100, Align::Right, -1},
                                {L"Modified", 128, Align::Left, -1}};
    DrawTable(
        tr, cols, -1, true,
        [&](int i, const Column&, int ci, const RectF& cr, bool sel) {
            Node* n = rows_[i].node;
            if (!n) return;
            const DevProject& p = devJunk_[rows_[i].idx];
            const DevArtifact* a = nullptr;
            for (auto& x : p.artifacts)
                if (x.node == n) a = &x;
            switch (ci) {
                case 0: {
                    RectF c2 = cr;
                    c2.left += 18;
                    DrawNameCell(c2, n, 0, false, false, i, sel);
                    break;
                }
                case 1:
                    if (a) gfx_.Text(std::wstring(a->kind) + L"  ·  " + a->how, cr, P_.textDim, Small(12.5f));
                    break;
                case 2: gfx_.Text(util::FormatSizeShort(Val(n)), cr, P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right)); break;
                default: gfx_.Text(util::FormatTime(n->mtime), cr, P_.textDim, Small(12.5f));
            }
        },
        [&](int i, const RectF& r) {
            const DevProject& p = devJunk_[rows_[i].idx];
            bool all = std::all_of(p.artifacts.begin(), p.artifacts.end(), [&](const DevArtifact& a) { return sel_.count(a.node) > 0; });
            bool some = !all && std::any_of(p.artifacts.begin(), p.artifacts.end(), [&](const DevArtifact& a) { return sel_.count(a.node) > 0; });
            RectF cb = RWH(r.left + 10, (r.top + r.bottom) / 2 - 8, 16, 16);
            Checkbox(cb, all, Hovered(HK_ROW, i));
            if (some) gfx_.FillRound(Inset(cb, 4, 7), 1, P_.accent);
            std::wstring size = util::FormatSizeShort(DevProjectSize(p, disk));
            float sw = gfx_.Measure(size, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 8;
            std::wstring age = L"changed " + AgeText(p.lastChange);
            float aw = gfx_.Measure(age, Small(12.5f)) + 24;
            int64_t days = p.lastChange ? (util::NowFileTime() - p.lastChange) / 864000000000LL : 0;
            float x = r.left + 36;
            float right = r.right - 12 - sw - aw;
            std::wstring kinds = p.kinds;
            float kw = std::min(gfx_.Measure(kinds, Small(12, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 18, 220.0f);
            std::wstring path = p.dir == root_.get() ? root_->name : RelFolder(p.dir) + L"\\" + p.dir->name;
            if (p.dir->parent == root_.get()) path = p.dir->name;
            float pw = std::max(40.0f, right - x - kw - 12);
            gfx_.Text(FitPath(path, pw, 13, true), R(x, r.top, x + pw, r.bottom), P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD));
            float px = x + std::min(pw, gfx_.Measure(FitPath(path, pw, 13, true), TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 10);
            RectF kr = RWH(px, (r.top + r.bottom) / 2 - 9, kw, 18);
            gfx_.FillRound(kr, 9, WithAlpha(P_.accent2, 0.16f));
            gfx_.Text(kinds, kr, P_.accent2, Small(11, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
            gfx_.Text(age, R(r.right - 12 - sw - aw, r.top, r.right - 12 - sw, r.bottom), days >= 90 ? P_.warning : P_.textDim,
                      Small(12.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
            gfx_.Text(size, R(r.right - 12 - sw, r.top, r.right - 12, r.bottom), P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
        });
}

void App::DrawAiModels(const RectF& area) {
    float top = area.top + 18;
    uint64_t total = 0;
    bool anyUnused = false;
    for (auto& s : ai_) {
        total += s.bytes;
        for (auto& it : s.items) anyUnused = anyUnused || it.unused;
    }
    std::wstring sub = ai_.empty() ? L"Hugging Face, Ollama and LM Studio downloads, one model at a time"
                                   : util::FormatSizeShort(total) + L" in " + util::FormatCount(ai_.size()) +
                                         (ai_.size() == 1 ? L" model folder" : L" model folders") +
                                         L"  ·  “Frees” is what deleting that model alone frees  ·  chats and settings are never touched";
    float bx = area.right - 24;
    if (!aiSel_.empty()) {
        std::wstring t = L"Delete " + util::FormatSizeShort(AiSelectedBytes());
        float w = gfx_.Measure(t, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 54;
        bx -= w;
        Button(RWH(bx, top + 6, w, 34), IC_DELETE, t, CMD_AI_DELETE, 2);
        bx -= 8;
    }
    if (anyUnused) {
        bx -= 204;
        Button(RWH(bx, top + 6, 204, 34), IC_CHECK, L"Select unused revisions", CMD_AI_UNUSED, 0);
        bx -= 8;
    }
    bx -= 108;
    Button(RWH(bx, top + 6, 108, 34), IC_REFRESH, L"Refresh", CMD_AI_REFRESH, 0);
    bx -= 16;
    DrawViewHeader(R(area.left + 24, top, bx, top + 46), L"AI models", sub, IC_AI);
    float y = top + 58;
    if (!ai_.empty()) {
        std::wstring hint = L"Click a model to select it  ·  double-click to show its files";
        if (!aiAtime_) hint += L"  ·  “Last used” is unknown: Windows doesn't record file access on this PC";
        gfx_.Text(hint, R(area.left + 24, y, area.right - 24, y + 24), P_.textFaint, Small(12));
        y += 30;
    }
    RectF tr = R(area.left, y, area.right, area.bottom);
    if (ai_.empty()) {
        DrawEmptyState(tr, IC_AI, L"No local AI models found",
                       L"Looked for the Hugging Face cache (HF_HOME, HF_HUB_CACHE, ~\\.cache\\huggingface), Ollama (OLLAMA_MODELS, "
                       L"~\\.ollama\\models) and LM Studio (its models and downloads folders).");
        return;
    }
    std::vector<Column> cols = {{L"Model", -1.3f, Align::Left, -1},
                                {L"Details", -1.0f, Align::Left, -1},
                                {L"Frees", 96, Align::Right, -1},
                                {L"Downloaded", 100, Align::Left, -1}};
    if (aiAtime_) cols.push_back({L"Last used", 100, Align::Left, -1});
    auto item = [&](int row) -> std::pair<size_t, size_t> {
        return {static_cast<size_t>(rows_[row].idx) >> 20, static_cast<size_t>(rows_[row].idx) & 0xFFFFF};
    };
    DrawTable(
        tr, cols, -1, true,
        [&](int i, const Column&, int ci, const RectF& cr, bool) {
            auto [s, k] = item(i);
            if (s >= ai_.size() || k >= ai_[s].items.size()) return;
            const AiItem& it = ai_[s].items[k];
            bool checked = aiSel_.count({s, k}) || (it.parent >= 0 && aiSel_.count({s, static_cast<size_t>(it.parent)}));
            float cy = (cr.top + cr.bottom) / 2;
            switch (ci) {
                case 0: {
                    float x = cr.left + rows_[i].depth * 24.0f;
                    Checkbox(RWH(x, cy - 9, 18, 18), checked, Hovered(HK_ROW, i));
                    x += 30;
                    TextStyle ts = TS(13, it.parent >= 0 ? DWRITE_FONT_WEIGHT_NORMAL : DWRITE_FONT_WEIGHT_SEMI_BOLD);
                    float nw = std::min(gfx_.Measure(it.name, ts) + 4, std::max(0.0f, cr.right - x));
                    gfx_.Text(it.name, R(x, cr.top, x + nw, cr.bottom), P_.text, ts);
                    x += nw + 8;
                    if (it.unused) {
                        float uw = gfx_.Measure(L"Unused", Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 14;
                        if (x + uw < cr.right) {
                            RectF br = R(x, cy - 9, x + uw, cy + 9);
                            gfx_.FillRound(br, 9, WithAlpha(P_.warning, 0.16f));
                            gfx_.Text(L"Unused", br, P_.warning, Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
                        }
                    }
                    break;
                }
                case 1: gfx_.Text(it.detail, cr, P_.textDim, Small(12.5f)); break;
                case 2: gfx_.Text(util::FormatSizeShort(it.bytes), cr, P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right)); break;
                case 3: gfx_.Text(it.added ? util::FormatTime(it.added).substr(0, 10) : L"", cr, P_.textDim, Small(12.5f)); break;
                default: gfx_.Text(it.used ? AgeText(it.used) : L"", cr, P_.textDim, Small(12.5f));
            }
        },
        [&](int i, const RectF& r) {
            size_t s = static_cast<size_t>(rows_[i].idx);
            if (s >= ai_.size()) return;
            const AiStore& st = ai_[s];
            gfx_.Icon(IC_AI, RWH(r.left + 8, r.top, 22, H(r)), P_.accent2, 13);
            TextStyle ts = TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            float tw = gfx_.Measure(st.name, ts) + 4;
            gfx_.Text(st.name, R(r.left + 36, r.top, r.left + 36 + tw, r.bottom), P_.text, ts);
            std::wstring size = util::FormatSizeShort(st.bytes);
            float sw = gfx_.Measure(size, ts) + 8;
            float fx = r.left + 48 + tw;
            gfx_.Text(FitPath(st.folder, std::max(0.0f, r.right - 24 - sw - fx), 12), R(fx, r.top, r.right - 24 - sw, r.bottom), P_.textFaint,
                      Small(12));
            gfx_.Text(size, R(r.right - 12 - sw, r.top, r.right - 12, r.bottom), P_.accent2, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
        });
}

// =============================================================================================
// Cleanup

void App::DrawWsl(const RectF& area) {
    float top = area.top + 18;
    uint64_t total = 0, selTotal = 0;
    for (auto& d : vdisks_) total += d.Reclaimable();
    for (int id : wslSel_) selTotal += WslRowBytes(id);
    std::wstring sub = vdisks_.empty() ? L"Virtual disks of WSL distros and Docker Desktop"
                                       : L"Linux disks grow but never shrink by themselves  ·  " + util::FormatSizeShort(total) +
                                             L" could go back to Windows";
    float bx = area.right - 24;
    if (!wslSel_.empty()) {
        std::wstring t = L"Free up " + util::FormatSizeShort(selTotal);
        float w = gfx_.Measure(t, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 54;
        bx -= w;
        Button(RWH(bx, top + 6, w, 34), IC_CLEAN, t, CMD_WSL_FREE, 1);
        bx -= 8;
    }
    bx -= 108;
    Button(RWH(bx, top + 6, 108, 34), IC_REFRESH, L"Refresh", CMD_WSL_REFRESH, 0);
    bx -= 16;
    DrawViewHeader(R(area.left + 24, top, bx, top + 46), L"WSL & Docker", sub, IC_TERMINAL);
    float y = top + 58;
    if (!wslLoaded_) return;
    if (vdisks_.empty() && !docker_.cli) {
        DrawEmptyState(R(area.left, y, area.right, area.bottom), IC_TERMINAL, L"No WSL or Docker disks found",
                       L"WSL 2 distros and Docker Desktop keep Linux files in .vhdx virtual disks. None are registered for this user.");
        return;
    }
    gfx_.Text(L"Click to select  ·  double-click a disk to show its file  ·  running distros report their own usage, stopped "
              L"ones are read from the disk file without starting them",
              R(area.left + 24, y, area.right - 24, y + 24), P_.textFaint, Small(12));
    y += 30;
    std::vector<Column> cols = {{L"Disk", -1.1f, Align::Left, -1},
                                {L"Details", -1.3f, Align::Left, -1},
                                {L"Size", 92, Align::Right, -1},
                                {L"Used inside", 92, Align::Right, -1},
                                {L"Could free", 92, Align::Right, -1}};
    static const wchar_t* kDockerName[] = {L"Build cache", L"Unused images", L"Stopped containers", L"Unused volumes"};
    static const wchar_t* kDockerType[] = {L"Build Cache", L"Images", L"Containers", L"Local Volumes"};
    static const wchar_t* kDockerHow[] = {L"rebuilt by the next build  ·  docker builder prune -a",
                                          L"pulled again when needed  ·  docker image prune -a",
                                          L"can hold data: remove them in Docker Desktop if you're sure",
                                          L"can hold data: remove them in Docker Desktop if you're sure"};
    DrawTable(
        R(area.left, y, area.right, area.bottom), cols, -1, true,
        [&](int i, const Column&, int ci, const RectF& cr, bool) {
            int id = rows_[i].idx;
            bool disk = id < kWslDocker;
            if (disk && id >= static_cast<int>(vdisks_.size())) return;
            int k = id - kWslDocker;
            const DockerRow* dr = disk ? nullptr : docker_.Find(kDockerType[k]);
            bool selectable = disk ? vdisks_[id].usedKnown : k < 2;
            float cy = (cr.top + cr.bottom) / 2;
            switch (ci) {
                case 0: {
                    float x = cr.left;
                    if (selectable)
                        Checkbox(RWH(x, cy - 9, 18, 18), wslSel_.count(id) > 0, Hovered(HK_ROW, i));
                    else
                        gfx_.Icon(IC_INFO, RWH(x, cr.top, 18, H(cr)), P_.textFaint, 12);
                    x += 30;
                    std::wstring name = disk ? vdisks_[id].name : kDockerName[k];
                    TextStyle ts = TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                    float nw = std::min(gfx_.Measure(name, ts) + 4, std::max(0.0f, cr.right - x));
                    gfx_.Text(name, R(x, cr.top, x + nw, cr.bottom), selectable ? P_.text : P_.textDim, ts);
                    x += nw + 8;
                    if (disk && vdisks_[id].running) {
                        float rw = gfx_.Measure(L"Running", Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 14;
                        if (x + rw < cr.right) {
                            RectF br = R(x, cy - 9, x + rw, cy + 9);
                            gfx_.FillRound(br, 9, WithAlpha(P_.success, 0.16f));
                            gfx_.Text(L"Running", br, P_.success, Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
                        }
                    }
                    break;
                }
                case 1:
                    if (disk)
                        gfx_.Text(FitPath(vdisks_[id].path, W(cr), 12), cr, P_.textFaint, Small(12));
                    else
                        gfx_.Text(kDockerHow[k], cr, P_.textDim, Small(12.5f));
                    break;
                case 2: {
                    uint64_t v = disk ? vdisks_[id].fileBytes : (dr ? dr->size : 0);
                    gfx_.Text(util::FormatSizeShort(v), cr, P_.text, TS(13, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
                    break;
                }
                case 3:
                    if (disk)
                        gfx_.Text(vdisks_[id].usedKnown ? (vdisks_[id].usedFrom == L"superblock" ? L"≈ " : L"") + util::FormatSizeShort(vdisks_[id].used)
                                                        : L"unknown",
                                  cr, P_.textDim, Small(12.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
                    break;
                default: {
                    uint64_t v = WslRowBytes(id);
                    gfx_.Text(disk && !vdisks_[id].usedKnown ? L"" : util::FormatSizeShort(v), cr, selectable ? P_.accent : P_.textDim,
                              TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
                }
            }
        },
        [&](int i, const RectF& r) {
            bool dockerGroup = rows_[i].idx == 1;
            gfx_.Icon(dockerGroup ? IC_DRIVE : IC_TERMINAL, RWH(r.left + 8, r.top, 22, H(r)), P_.accent2, 13);
            std::wstring title = dockerGroup ? L"Docker" : L"Virtual disks";
            std::wstring note = !dockerGroup ? L"space each takes on the drive, and what Linux uses inside"
                                : docker_.running ? L"data inside Docker's disk: compact the disk afterwards to give the space to Windows"
                                                  : L"isn't running: start Docker Desktop to see what it could remove" +
                                                        (docker_.error.empty() ? L"" : L" (" + docker_.error + L")");
            TextStyle ts = TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            float tw = gfx_.Measure(title, ts) + 4;
            gfx_.Text(title, R(r.left + 36, r.top, r.left + 36 + tw, r.bottom), P_.text, ts);
            gfx_.Text(note, R(r.left + 48 + tw, r.top, r.right - 12, r.bottom), P_.textFaint, Small(12));
        });
}

void App::DrawWinTools(const RectF& area) {
    float top = area.top + 18;
    Button(RWH(area.right - 24 - 108, top + 6, 108, 34), IC_REFRESH, L"Refresh", CMD_WIN_REFRESH, 0);
    DrawViewHeader(R(area.left + 24, top, area.right - 150, top + 46), L"Windows",
                   L"Space Windows keeps for itself, changed with Windows' own tools  ·  works without a scan", IC_SYSTEM);
    if (!winLoaded_) return;
    struct Act {
        const wchar_t* text;
        int action;
        int style;
    };
    struct Card {
        wchar_t icon;
        std::wstring title, desc, status, size;
        std::vector<Act> acts;
    };
    std::vector<Card> cards;
    {
        Card c{0xE8F1, L"Component store (WinSxS)",
               L"Older versions of Windows components kept after updates. DISM removes the ones newer updates replaced.", L"", L"", {}};
        const ComponentStore& s = win_.store;
        if (!win_.storeAnalyzed) {
            c.status = L"Not analyzed yet: DISM takes a few minutes";
            c.acts.push_back({L"Analyze", WA_ANALYZE, 0});
        } else {
            c.size = util::FormatSizeShort(s.actual);
            c.status = util::FormatCount(s.reclaimablePackages < 0 ? 0 : s.reclaimablePackages) + L" components replaced by newer updates  ·  " +
                       (s.recommended ? L"cleanup recommended" : L"no cleanup needed") +
                       (s.lastCleanup.empty() ? L"" : L"  ·  last cleanup " + s.lastCleanup);
            c.acts.push_back({L"Analyze", WA_ANALYZE, 0});
            if (s.recommended || s.reclaimablePackages > 0) c.acts.push_back({L"Clean up", WA_CLEAN_STORE, 1});
        }
        cards.push_back(c);
    }
    {
        uint64_t bytes = 0;
        size_t n = 0;
        for (auto& p : win_.drivers)
            if (p.old) {
                bytes += p.bytes;
                ++n;
            }
        Card c{0xE964, L"Old driver packages",
               L"Older versions of drivers that a newer version replaced. Drivers a device uses, and printer drivers, are kept.",
               util::FormatCount(n) + L" old of " + util::FormatCount(win_.drivers.size()) + L" third-party driver packages",
               n ? util::FormatSizeShort(bytes) : L"", {}};
        if (n) c.acts.push_back({L"Remove old", WA_DRIVERS, 1});
        cards.push_back(c);
    }
    {
        Card c{0xE777, L"System Restore", L"Space for restore points. A smaller limit deletes the oldest restore points first.", L"", L"", {}};
        if (win_.shadows.empty()) {
            c.status = win_.shadowErr.empty() ? L"No restore point storage on this PC"
                       : elevated_          ? win_.shadowErr
                                            : L"Restart as administrator to see how much it uses";
        } else {
            uint64_t used = 0;
            for (auto& s : win_.shadows) {
                used += s.used;
                std::wstring max = s.max == UINT64_MAX ? L"no limit"
                                   : s.volumeSize    ? util::FormatSizeShort(s.max) + L" (" +
                                                        std::to_wstring(static_cast<int>(s.max * 100.0 / s.volumeSize + 0.5)) + L"% of " +
                                                        s.onDrive + L")"
                                                     : util::FormatSizeShort(s.max);
                c.status += (c.status.empty() ? L"" : L"  ·  ") + s.forDrive + L" uses " + util::FormatSizeShort(s.used) + L", limit " + max;
            }
            c.size = util::FormatSizeShort(used);
            c.acts.push_back({L"Set limit", WA_SHADOW, 0});
        }
        cards.push_back(c);
    }
    {
        const Hibernation& h = win_.hiber;
        Card c{0xE708, L"Hibernation file",
               L"hiberfil.sys holds memory while the PC hibernates. Off also turns off Fast Startup; reduced keeps Fast Startup.",
               !h.on ? L"Off" : std::wstring(h.reduced ? L"On, reduced (Fast Startup only)" : L"On") +
                                    (h.fastStartup ? L"  ·  Fast Startup on" : L"  ·  Fast Startup off"),
               h.bytes ? util::FormatSizeShort(h.bytes) : L"", {}};
        if (h.on) {
            if (!h.reduced) c.acts.push_back({L"Reduce", WA_HIBER_REDUCED, 0});
            c.acts.push_back({L"Turn off", WA_HIBER_OFF, 1});
        } else {
            c.acts.push_back({L"Turn on", WA_HIBER_ON, 0});
        }
        cards.push_back(c);
    }
    {
        Card c{0xE7B8, L"Compact OS", L"Stores Windows' own files compressed, usually saving 2 GB or more. Can be turned off again.",
               win_.compactOs ? L"On: Windows files are compressed" : L"Off", L"", {}};
        c.acts.push_back(win_.compactOs ? Act{L"Turn off", WA_COMPACT_OFF, 0} : Act{L"Turn on", WA_COMPACT_ON, 1});
        cards.push_back(c);
    }
    {
        const EventLogs& e = win_.logs;
        Card c{0xE7C3, L"Event logs", L"Windows' diagnostic history. Clearing loses it for troubleshooting; the Security log is kept.",
               e.known ? util::FormatCount(e.files) + L" logs" : L"Couldn't read the log folder",
               e.known ? util::FormatSizeShort(e.bytes - e.security) : L"", {}};
        if (e.known) c.acts.push_back({L"Clear", WA_LOGS, 0});
        cards.push_back(c);
    }

    float y = top + 62;
    RectF body = R(area.left, y, area.right, area.bottom);
    ListState& ls = lists_[static_cast<int>(View::WinTools)];
    float contentH = (elevated_ ? 0 : 64) + cards.size() * 102.0f + 20;
    ls.scroll = std::clamp(ls.scroll, 0.0f, std::max(0.0f, contentH - H(body)));
    gfx_.PushClip(body);
    y -= ls.scroll;
    if (!elevated_) {
        RectF b = R(area.left + 24, y, area.right - 24, y + 52);
        gfx_.FillRound(b, 10, WithAlpha(P_.warning, 0.12f));
        gfx_.Icon(IC_SHIELD, RWH(b.left + 10, b.top, 30, H(b)), P_.warning, 16);
        gfx_.Text(L"Changes here need administrator rights: Windows asks each time, or restart as administrator once.",
                  R(b.left + 46, b.top, b.right - 200, b.bottom), P_.text, TS(13));
        Button(R(b.right - 190, b.top + 10, b.right - 12, b.bottom - 10), IC_SHIELD, L"Restart as admin", CMD_ADMIN, 0);
        y += 64;
    }
    for (auto& c : cards) {
        RectF card = R(area.left + 24, y, area.right - 24, y + 92);
        gfx_.FillRound(card, 12, P_.surface);
        gfx_.StrokeRound(card, 12, P_.border);
        float x = card.left + 18;
        gfx_.FillRound(RWH(x, card.top + 18, 40, 40), 10, WithAlpha(P_.accent, 0.14f));
        gfx_.Icon(c.icon, RWH(x, card.top + 18, 40, 40), P_.accent, 16);
        x += 54;
        float textRight = card.right - 330;
        gfx_.Text(c.title, R(x, card.top + 12, textRight, card.top + 34), P_.text, TS(14, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        gfx_.Text(c.desc, R(x, card.top + 35, textRight, card.top + 55), P_.textDim, Small(12.5f));
        gfx_.Text(c.status, R(x, card.top + 58, textRight, card.top + 80), P_.textFaint, Small(12));
        if (!c.size.empty())
            gfx_.Text(c.size, R(card.right - 310, card.top + 10, card.right - 20, card.top + 38), P_.text,
                      TS(15, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
        float bx = card.right - 20;
        for (auto it = c.acts.rbegin(); it != c.acts.rend(); ++it) {
            float w = gfx_.Measure(it->text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 36;
            bx -= w;
            Button(RWH(bx, card.top + 46, w, 32), 0, it->text, CMD_WIN_ACT + it->action, it->style);
            bx -= 8;
        }
        y += 102;
    }
    gfx_.PopClip();
}

void App::DrawCleanup(const RectF& area) {
    float top = area.top + 18;
    uint64_t selTotal = 0, all = 0;
    for (auto& c : cleanup_) {
        if (c.external) continue;
        all += c.bytes;
        if (c.selected) selTotal += c.ToClean();
    }
    DrawViewHeader(R(area.left + 24, top, area.right - 360, top + 46), L"Cleanup",
                   cleanupMeasuring_ ? L"Measuring caches and leftovers…"
                                     : util::FormatSizeShort(all) + L" of caches and leftovers found  ·  works without a scan",
                   IC_CLEAN);
    float bx = area.right - 24;
    float w = 190;
    bx -= w;
    Button(RWH(bx, top + 6, w, 34), IC_CLEAN, selTotal ? L"Clean up " + util::FormatSizeShort(selTotal) : L"Clean up", CMD_CLEAN_SELECTED,
           selTotal ? 2 : 0, selTotal > 0 && !cleanupMeasuring_);
    bx -= 8 + 130;
    Button(RWH(bx, top + 6, 130, 34), IC_REFRESH, L"Measure", CMD_CLEAN_MEASURE, 0, !cleanupMeasuring_);

    // Built-in and user categories in their order, then imported ones (winapp2.ini): only those
    // that found something on this PC, largest first, under a header (kHeader in the list).
    const size_t kHeader = static_cast<size_t>(-1);
    std::vector<size_t> show, imported;
    size_t importedAll = 0;
    for (size_t i = 0; i < cleanup_.size(); ++i) {
        const CleanCategory& c = cleanup_[i];
        if (!c.imported) {
            show.push_back(i);
            continue;
        }
        ++importedAll;
        if (c.measured && c.present && c.bytes > 0) imported.push_back(i);
    }
    std::stable_sort(imported.begin(), imported.end(), [&](size_t a, size_t b) { return cleanup_[a].bytes > cleanup_[b].bytes; });
    if (importedAll && (!imported.empty() || cleanupMeasuring_)) show.push_back(kHeader);
    show.insert(show.end(), imported.begin(), imported.end());

    float y = top + 62;
    RectF body = R(area.left, y, area.right, area.bottom);
    ListState& ls = lists_[static_cast<int>(View::Cleanup)];
    float contentH = (elevated_ ? 0 : 64) + show.size() * 82.0f + 20;
    ls.scroll = std::clamp(ls.scroll, 0.0f, std::max(0.0f, contentH - H(body)));
    gfx_.PushClip(body);
    y -= ls.scroll;
    if (!elevated_) {
        RectF b = R(area.left + 24, y, area.right - 24, y + 52);
        gfx_.FillRound(b, 10, WithAlpha(P_.warning, 0.12f));
        gfx_.Icon(IC_SHIELD, RWH(b.left + 10, b.top, 30, H(b)), P_.warning, 16);
        gfx_.Text(L"Some locations (Windows Update, system temp) need administrator rights.", R(b.left + 46, b.top, b.right - 200, b.bottom),
                  P_.text, TS(13));
        Button(R(b.right - 190, b.top + 10, b.right - 12, b.bottom - 10), IC_SHIELD, L"Restart as admin", CMD_ADMIN, 0);
        y += 64;
    }
    for (size_t i : show) {
        if (i == kHeader) {
            RectF hr = R(area.left + 28, y + 44, area.right - 28, y + 66);
            float lw = gfx_.Measure(L"FROM WINAPP2.INI", Small(11, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 14;
            gfx_.Text(L"FROM WINAPP2.INI", R(hr.left, hr.top, hr.left + lw, hr.bottom), P_.textFaint, Small(11, DWRITE_FONT_WEIGHT_SEMI_BOLD));
            std::wstring note = cleanupMeasuring_ && imported.empty()
                                    ? L"Checking " + util::FormatCount(importedAll) + L" community app cleaners…"
                                    : util::FormatCount(imported.size()) + L" apps on this PC have something to clean  ·  community rules, "
                                                                           L"never preselected: review before cleaning";
            gfx_.Text(note, R(hr.left + lw, hr.top, hr.right, hr.bottom), P_.textDim, Small(12));
            y += 82;
            continue;
        }
        CleanCategory& c = cleanup_[i];
        RectF card = R(area.left + 24, y, area.right - 24, y + 72);
        bool locked = c.needsAdmin && !elevated_;
        bool hover = Hovered(HK_CHECK, static_cast<int>(i));
        gfx_.FillRound(card, 12, hover ? P_.surface2 : P_.surface);
        gfx_.StrokeRound(card, 12, c.selected ? WithAlpha(P_.accent, 0.7f) : P_.border);
        float x = card.left + 18;
        if (!c.external) {
            Checkbox(RWH(x, card.top + 27, 18, 18), c.selected, hover);
            x += 34;
        }
        gfx_.FillRound(RWH(x, card.top + 16, 40, 40), 10, WithAlpha(P_.accent, 0.14f));
        gfx_.Icon(c.icon, RWH(x, card.top + 16, 40, 40), P_.accent, 16);
        x += 54;
        float textRight = card.right - 200;
        float nw = gfx_.Measure(c.name, TS(14, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 10;
        gfx_.Text(c.name, R(x, card.top + 14, std::min(textRight, x + nw), card.top + 36), P_.text, TS(14, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        float bxx = x + nw;
        auto badge = [&](const wchar_t* t, D2D1_COLOR_F col) {
            float bw2 = gfx_.Measure(t, Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 14;
            if (bxx + bw2 > textRight) return;
            RectF br = RWH(bxx, card.top + 16, bw2, 18);
            gfx_.FillRound(br, 9, WithAlpha(col, 0.16f));
            gfx_.Text(t, br, col, Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
            bxx += bw2 + 6;
        };
        if (locked) badge(L"Needs admin", P_.warning);
        if (!c.safe && !c.external && !c.imported) badge(L"Review first", P_.accent2);
        if (c.denied && !locked) badge(L"Partly inaccessible", P_.textFaint);
        if (c.user) badge(L"Your rule", P_.accent);
        if (c.guarded) badge(L"Protected items left out", P_.success);
        std::wstring tool = c.command.substr(0, c.command.find(L' '));
        if (c.toolMissing && c.bytes) badge((tool + L" not found on PATH").c_str(), P_.warning);
        gfx_.Text(c.desc, R(x, card.top + 37, textRight, card.top + 58), P_.textDim, Small(12.5f));
        RectF sr = R(card.right - 190, card.top, card.right - 20, card.bottom);
        RectF review{};
        if (c.external) {
            if (c.bytes) {
                gfx_.Text(util::FormatSizeShort(c.bytes), R(sr.left, card.top + 8, sr.right, card.top + 36), P_.text,
                          TS(15, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
                gfx_.Text(L"use Windows Disk Cleanup", R(sr.left - 40, card.top + 36, sr.right, card.top + 58), P_.textFaint,
                          Small(11.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
            } else {
                gfx_.Text(c.measured ? L"Not present" : L"…", sr, P_.textFaint, Small(12.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
            }
        } else if (!c.measured || cleanupMeasuring_) {
            float a = (GetTickCount64() % 1200) / 1200.0f * 360.0f;
            gfx_.Arc(sr.right - 10, (card.top + card.bottom) / 2, 8, a, 270, P_.accent, 2.2f);
        } else {
            gfx_.Text(util::FormatSizeShort(c.ToClean()), R(sr.left, card.top + 8, sr.right, card.top + 36), c.bytes ? P_.text : P_.textFaint,
                      TS(15, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
            std::wstring count = util::FormatCount(c.files) + (c.recycleBin ? (c.files == 1 ? L" item" : L" items") : (c.files == 1 ? L" file" : L" files"));
            if (size_t kept = c.KeptCount()) count += L" · " + util::FormatCount(kept) + L" kept";
            // A tool cleans its own cache: there is no file list to pick from, so say what runs.
            if (!c.command.empty()) count = L"runs " + c.command;
            if (!c.entries.empty() && c.command.empty()) {
                // "Review" opens the exact list of what would be deleted (a dry run with opt-outs).
                std::wstring link = L"Review " + count + L"  ›";
                float lw = gfx_.Measure(link, Small(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 12;
                RectF lr = R(sr.right - lw + 6, card.top + 37, sr.right + 6, card.top + 57);
                bool hv = Hovered(HK_REVIEW, static_cast<int>(i));
                if (hv) gfx_.FillRound(lr, 6, P_.hover);
                gfx_.Text(link, R(lr.left, lr.top, lr.right - 6, lr.bottom), hv ? P_.text : P_.accent,
                          Small(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
                review = lr;
            } else {
                gfx_.Text(count, R(sr.left, card.top + 36, sr.right, card.top + 58), P_.textFaint,
                          Small(11.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
            }
        }
        if (!c.external && !c.toolMissing) AddHit(card, HK_CHECK, static_cast<int>(i));
        if (W(review) > 0) AddHit(review, HK_REVIEW, static_cast<int>(i));
        y += 82;
    }
    gfx_.PopClip();
}

void App::DrawCleanReview(const RectF& area) {
    if (reviewCat_ < 0 || reviewCat_ >= static_cast<int>(cleanup_.size())) return;
    CleanCategory& c = cleanup_[reviewCat_];
    float top = area.top + 18;
    IconButton(area.left + 24, top + 4, IC_BACK, CMD_NONE, L"");
    hits_.back().kind = HK_NAV;
    hits_.back().a = static_cast<int>(View::Cleanup);
    size_t kept = c.KeptCount();
    std::wstring sub = util::FormatCount(c.entries.size() - kept) + L" of " + util::FormatCount(c.entries.size()) +
                       L" selected  ·  " + util::FormatSizeShort(c.ToClean()) +
                       L" would be deleted permanently  ·  uncheck what you want to keep" +
                       (c.guarded ? L"  ·  " + util::FormatCount(c.guarded) + L" protected items left out" : L"");
    DrawViewHeader(R(area.left + 70, top, area.right - 290, top + 46), L"Review: " + c.name, sub, c.icon);
    float bx = area.right - 24 - 130;
    Button(RWH(bx, top + 6, 130, 34), IC_CHECK, L"Select all", CMD_REVIEW_ALL, 0, kept > 0);
    bx -= 8 + 130;
    Button(RWH(bx, top + 6, 130, 34), 0, L"Keep all", CMD_REVIEW_NONE, 0, kept < c.entries.size());
    float y = top + 62;
    if (cleanupMeasuring_) {
        gfx_.Text(L"Measuring again…", R(area.left + 24, y, area.right - 24, y + 22), P_.textDim, Small(12.5f));
        y += 26;
    }
    RectF body = R(area.left, y, area.right, area.bottom);
    if (c.entries.empty()) {
        DrawEmptyState(body, c.icon, L"Nothing to clean here", L"This category is empty right now.");
        return;
    }
    std::vector<Column> cols = {{L"Item", -1.0f, Align::Left, -1},
                                {L"Size", 100, Align::Right, -1},
                                {c.recycleBin ? L"Deleted" : L"Modified", 140, Align::Left, -1}};
    DrawTable(body, cols, -1, true, [&](int i, const Column&, int ci, const RectF& cr, bool) {
        const CleanEntry& e = c.entries[rows_[i].idx];
        D2D1_COLOR_F textCol = e.keep ? P_.textFaint : P_.text;
        switch (ci) {
            case 0: {
                float cy = (cr.top + cr.bottom) / 2;
                Checkbox(RWH(cr.left, cy - 9, 18, 18), !e.keep, Hovered(HK_ROW, i));
                size_t slash = e.path.find_last_of(L'\\');
                std::wstring name = slash == std::wstring::npos ? e.path : e.path.substr(slash + 1);
                std::wstring folder = slash == std::wstring::npos ? L"" : e.path.substr(0, slash);
                std::wstring ext;
                size_t dot = name.find_last_of(L'.');
                if (!e.dir && dot != std::wstring::npos) ext = util::ToLower(name.substr(dot + 1));
                gfx_.Bitmap(e.dir ? icons_.Folder(gfx_) : icons_.ForExt(gfx_, ext), RWH(cr.left + 30, cy - 9, 18, 18), e.keep ? 0.5f : 1.0f);
                float x = cr.left + 56;
                TextStyle ts = TS(13, e.dir ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
                float nw = std::min(gfx_.Measure(name, ts) + 4, std::max(0.0f, cr.right - x));
                gfx_.Text(name, R(x, cr.top, x + nw, cr.bottom), textCol, ts);
                x += nw + 10;
                if (e.keep) {
                    float kw = gfx_.Measure(L"Kept", Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 14;
                    if (x + kw < cr.right) {
                        RectF br = R(x, cy - 9, x + kw, cy + 9);
                        gfx_.FillRound(br, 9, WithAlpha(P_.success, 0.16f));
                        gfx_.Text(L"Kept", br, P_.success, Small(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
                        x += kw + 10;
                    }
                }
                if (x + 40 < cr.right)
                    gfx_.Text(FitPath(folder, cr.right - x, 12), R(x, cr.top, cr.right, cr.bottom), P_.textFaint, Small(12));
                break;
            }
            case 1:
                gfx_.Text(util::FormatSizeShort(e.bytes), cr, textCol, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
                break;
            default: gfx_.Text(e.mtime ? util::FormatTime(e.mtime) : L"", cr, P_.textDim, Small(12.5f)); break;
        }
    });
}

// =============================================================================================
// History & compare

void App::DrawHistory(const RectF& area) {
    float top = area.top + 18;
    bool scoped = root_ && !readOnly_;
    DrawViewHeader(R(area.left + 24, top, area.right - 24, top + 46), L"History",
                   scoped ? L"Earlier scans of " + root_->name + L"  ·  compare to see what grew"
                          : L"Every completed scan is saved automatically (folder sizes only)",
                   IC_HISTORY);
    float y = top + 64;
    RectF body = R(area.left, y, area.right, area.bottom);
    if (snapshots_.empty()) {
        DrawEmptyState(body, IC_HISTORY, L"No history yet", L"Scan a drive or folder; next time you scan it you can compare the two and see exactly what changed.");
        return;
    }
    ListState& ls = lists_[static_cast<int>(View::History)];
    float contentH = snapshots_.size() * 70.0f;
    ls.scroll = std::clamp(ls.scroll, 0.0f, std::max(0.0f, contentH - H(body)));
    gfx_.PushClip(body);
    y -= ls.scroll;
    for (size_t i = 0; i < snapshots_.size(); ++i) {
        const SnapshotInfo& s = snapshots_[i];
        RectF card = R(area.left + 24, y, area.right - 24, y + 60);
        gfx_.FillRound(card, 12, P_.surface);
        gfx_.StrokeRound(card, 12, P_.border);
        gfx_.FillRound(RWH(card.left + 14, card.top + 12, 36, 36), 9, WithAlpha(P_.accent, 0.14f));
        gfx_.Icon(IC_HISTORY, RWH(card.left + 14, card.top + 12, 36, 36), P_.accent, 15);
        gfx_.Text(util::FormatTime(s.time) + L"   ·   " + RelativeTime(s.time), R(card.left + 64, card.top + 10, card.right - 360, card.top + 32), P_.text,
                  TS(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        gfx_.Text(FitPath(s.root, 420, 12.5f) + L"   ·   " + util::FormatCount(s.items) + L" items", R(card.left + 64, card.top + 31, card.right - 360, card.top + 52),
                  P_.textDim, Small(12.5f));
        float x = card.right - 14;
        bool sameRoot = scoped && _wcsicmp(s.root.c_str(), root_->name.c_str()) == 0;
        // Delete snapshot
        RectF del = RWH(x - 34, card.top + 13, 34, 34);
        if (Hovered(HK_SNAPSHOT, static_cast<int>(i), 1)) gfx_.FillRound(del, 8, P_.surface3);
        gfx_.Icon(IC_DELETE, del, P_.textDim, 14);
        AddHit(del, HK_SNAPSHOT, static_cast<int>(i), 1);
        x -= 42;
        if (sameRoot) {
            RectF cmp = RWH(x - 100, card.top + 13, 100, 34);
            bool hv = Hovered(HK_SNAPSHOT, static_cast<int>(i), 0);
            gfx_.FillRound(cmp, 8, hv ? P_.accent : WithAlpha(P_.accent, 0.16f));
            gfx_.Text(L"Compare", cmp, hv ? P_.accentText : P_.accent, Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
            AddHit(cmp, HK_SNAPSHOT, static_cast<int>(i), 0);
            x -= 112;
            int64_t d = static_cast<int64_t>(root_->disk) - static_cast<int64_t>(s.disk);
            gfx_.Text(SignedSize(d) + L" since", R(x - 160, card.top + 10, x, card.top + 32),
                      d > 0 ? P_.danger : (d < 0 ? P_.success : P_.textDim), TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
        } else if (!scoped) {
            RectF scan = RWH(x - 100, card.top + 13, 100, 34);
            bool hv = Hovered(HK_RECENT, static_cast<int>(i));
            gfx_.FillRound(scan, 8, hv ? P_.accent : WithAlpha(P_.accent, 0.16f));
            gfx_.Text(L"Scan again", scan, hv ? P_.accentText : P_.accent, Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
            AddHit(scan, HK_RECENT, static_cast<int>(i));
            x -= 112;
        }
        gfx_.Text(util::FormatSizeShort(s.disk) + L" on disk", R(x - 200, card.top + 31, x, card.top + 52), P_.textDim,
                  Small(12.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
        y += 70;
    }
    gfx_.PopClip();
}

void App::DrawDeleted(const RectF& area) {
    float top = area.top + 18;
    DrawViewHeader(R(area.left + 24, top, area.right - 200, top + 46), L"Recently deleted",
                   L"Everything WinNCDU deleted or cleaned up, newest first  ·  restore what is still in the Recycle Bin", IC_UNDO);
    Button(RWH(area.right - 24 - 150, top + 6, 150, 34), IC_OPEN, L"Open journal", CMD_OPEN_JOURNAL, 0);
    float y = top + 64;
    RectF body = R(area.left, y, area.right, area.bottom);
    if (deleted_.empty()) {
        DrawEmptyState(body, IC_UNDO, L"Nothing deleted yet",
                       L"Everything you delete or clean up with WinNCDU is listed here. Items still in the Recycle Bin can be restored with one click.");
        return;
    }
    ListState& ls = lists_[static_cast<int>(View::Deleted)];
    float contentH = deleted_.size() * 70.0f;
    ls.scroll = std::clamp(ls.scroll, 0.0f, std::max(0.0f, contentH - H(body)));
    gfx_.PushClip(body);
    y -= ls.scroll;
    for (size_t i = 0; i < deleted_.size(); ++i, y += 70) {
        if (y + 60 < body.top) continue;
        if (y > body.bottom) break;
        const DeletedRow& d = deleted_[i];
        const JournalEntry& e = d.e;
        RectF card = R(area.left + 24, y, area.right - 24, y + 60);
        gfx_.FillRound(card, 12, P_.surface);
        gfx_.StrokeRound(card, 12, P_.border);
        bool clean = e.action == JournalAction::Clean, perm = e.action == JournalAction::Delete;
        D2D1_COLOR_F tone = clean ? P_.success : (perm ? P_.danger : P_.accent);
        gfx_.FillRound(RWH(card.left + 14, card.top + 12, 36, 36), 9, WithAlpha(tone, 0.14f));
        gfx_.Icon(clean ? IC_CLEAN : IC_DELETE, RWH(card.left + 14, card.top + 12, 36, 36), tone, 15);

        // Right side: what can be done now, in a fixed-width column so the sizes line up.
        float x = card.right - 14;
        const float statusW = 210;
        std::wstring status;
        D2D1_COLOR_F statusCol = P_.textFaint;
        if (d.bin >= 0) {
            RectF btn = RWH(x - 100, card.top + 13, 100, 34);
            bool hv = Hovered(HK_RESTORE, static_cast<int>(i));
            gfx_.FillRound(btn, 8, hv ? P_.accent : WithAlpha(P_.accent, 0.16f));
            gfx_.Text(L"Restore", btn, hv ? P_.accentText : P_.accent, Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
            AddHit(btn, HK_RESTORE, static_cast<int>(i));
        } else if (e.action == JournalAction::Recycle) {
            status = d.back ? L"Restored" : L"No longer in the Recycle Bin";
            if (d.back) statusCol = P_.success;
        } else {
            status = clean ? L"Cleaned up" : L"Deleted permanently";
        }
        if (!status.empty())
            gfx_.Text(status, R(x - statusW, card.top, x, card.bottom), statusCol, Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
        x -= statusW + 16;
        std::wstring size = util::FormatSizeShort(e.bytes);
        float szw = gfx_.Measure(size, TS(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 4;
        gfx_.Text(size, R(x - szw, card.top + 10, x, card.top + 32), P_.text, TS(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
        gfx_.Text(RelativeTime(e.time), R(x - 160, card.top + 31, x, card.top + 52), P_.textFaint,
                  Small(12, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
        x -= std::max(szw, 110.0f) + 16;

        // Left: what it was.
        size_t slash = e.path.find_last_of(L'\\');
        std::wstring name = clean || slash == std::wstring::npos || slash + 1 >= e.path.size() ? e.path : e.path.substr(slash + 1);
        std::wstring where = clean ? L"Cleanup" : (slash == std::wstring::npos ? L"" : e.path.substr(0, slash));
        if (where.size() == 2 && where[1] == L':') where += L'\\';
        std::wstring detail = where;
        if (e.items > 1) detail += L"   ·   " + util::FormatCount(e.items) + (clean ? L" files" : L" items");
        if (!e.note.empty()) detail += L"   ·   " + e.note;
        float textW = std::max(40.0f, x - (card.left + 64));
        gfx_.Text(name, R(card.left + 64, card.top + 10, card.left + 64 + textW, card.top + 32), P_.text, TS(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        gfx_.Text(FitPath(detail, textW, 12.5f), R(card.left + 64, card.top + 31, card.left + 64 + textW, card.top + 52), P_.textDim,
                  Small(12.5f));
    }
    gfx_.PopClip();
}

void App::DrawCompare(const RectF& area) {
    float top = area.top + 18;
    int64_t oldTotal = static_cast<int64_t>(settings_.apparent ? diffInfo_.size : diffInfo_.disk);
    int64_t nowTotal = root_ ? static_cast<int64_t>(Val(root_.get())) : 0;
    IconButton(area.left + 24, top + 4, IC_BACK, CMD_NONE, L"");
    hits_.back().kind = HK_NAV;
    hits_.back().a = static_cast<int>(View::History);
    DrawViewHeader(R(area.left + 70, top, area.right - 24, top + 46), L"Changes since " + util::FormatTime(diffInfo_.time),
                   util::FormatSizeShort(static_cast<uint64_t>(oldTotal)) + L"  →  " + util::FormatSizeShort(static_cast<uint64_t>(nowTotal)) +
                       L"   (" + SignedSize(nowTotal - oldTotal) + L")  ·  showing the folders that explain the change",
                   IC_COMPARE);
    RectF tr = R(area.left, top + 62, area.right, area.bottom);
    if (rows_.empty()) {
        DrawEmptyState(tr, IC_CHECK, L"No significant changes", L"Folder sizes are about the same as in this snapshot.");
        return;
    }
    int64_t maxAbs = 1;
    for (auto& d : diff_) maxAbs = std::max(maxAbs, std::llabs(d.delta()));
    std::vector<Column> cols = {{L"Folder", -1.0f, Align::Left, -1},
                                {L"Before", 100, Align::Right, -1},
                                {L"Now", 100, Align::Right, -1},
                                {L"Change", 230, Align::Left, -1}};
    DrawTable(tr, cols, -1, true, [&](int i, const Column&, int ci, const RectF& cr, bool) {
        const DiffEntry& d = diff_[rows_[i].idx];
        switch (ci) {
            case 0: {
                float cy = (cr.top + cr.bottom) / 2;
                gfx_.Bitmap(icons_.Folder(gfx_), RWH(cr.left, cy - 9, 18, 18), d.node ? 1.0f : 0.4f);
                std::wstring p = d.path;
                if (root_ && p.size() > root_->name.size() && _wcsnicmp(p.c_str(), root_->name.c_str(), root_->name.size()) == 0) {
                    p = p.substr(root_->name.size());
                    if (!p.empty() && p[0] == L'\\') p.erase(0, 1);
                }
                if (p.empty()) p = root_ ? root_->name : d.path;
                gfx_.Text(FitPath(p, cr.right - cr.left - 30, 13) + (d.node ? L"" : L"   (deleted)"), R(cr.left + 26, cr.top, cr.right, cr.bottom),
                          d.node ? P_.text : P_.textFaint, TS(13));
                break;
            }
            case 1: gfx_.Text(util::FormatSizeShort(static_cast<uint64_t>(d.oldVal)), cr, P_.textDim, Small(12.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right)); break;
            case 2: gfx_.Text(util::FormatSizeShort(static_cast<uint64_t>(d.newVal)), cr, P_.text, Small(12.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right)); break;
            default: {
                int64_t dv = d.delta();
                D2D1_COLOR_F col = dv > 0 ? P_.danger : P_.success;
                float bw = (W(cr) - 100) * static_cast<float>(static_cast<double>(std::llabs(dv)) / maxAbs);
                float cy = (cr.top + cr.bottom) / 2;
                gfx_.FillRound(R(cr.left, cy - 3.5f, cr.left + W(cr) - 100, cy + 3.5f), 3.5f, P_.track);
                gfx_.FillRound(R(cr.left, cy - 3.5f, cr.left + std::max(4.0f, bw), cy + 3.5f), 3.5f, col);
                gfx_.Text(SignedSize(dv), R(cr.right - 96, cr.top, cr.right, cr.bottom), col, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
            }
        }
    });
}

// =============================================================================================
// Home

void App::DrawHome(const RectF& area) {
    ListState& ls = lists_[static_cast<int>(View::Home)];
    gfx_.PushClip(area);
    float x0 = area.left + 40, maxW = std::min(W(area) - 80, 1100.0f);
    float y = area.top + 36 - ls.scroll;
    float startY = y;
    TextStyle h1 = TS(28, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    h1.font = FontKind::Display;
    gfx_.Text(L"What would you like to analyze?", R(x0, y, x0 + maxW, y + 40), P_.text, h1);
    y += 42;
    gfx_.Text(L"Pick a drive or folder to see what's using your disk space — then clean it up.", R(x0, y, x0 + maxW, y + 24),
              P_.textDim, TS(14));
    y += 46;

    gfx_.Text(L"DRIVES", R(x0, y, x0 + 200, y + 20), P_.textFaint, Small(11, DWRITE_FONT_WEIGHT_SEMI_BOLD));
    y += 28;
    float cardW = 300, cardH = 118, gap = 16;
    int perRow = std::max(1, static_cast<int>((maxW + gap) / (cardW + gap)));
    for (size_t i = 0; i < drives_.size(); ++i) {
        const DriveInfo& d = drives_[i];
        float cx = x0 + (i % perRow) * (cardW + gap), cy = y + (i / perRow) * (cardH + gap);
        RectF card = RWH(cx, cy, cardW, cardH);
        bool hover = Hovered(HK_CARD, static_cast<int>(i));
        if (hover) gfx_.Shadow(card, 14, 10);
        gfx_.FillRound(card, 14, P_.surface);
        gfx_.StrokeRound(card, 14, hover ? P_.accent : P_.border, hover ? 1.5f : 1.0f);
        gfx_.Bitmap(icons_.ForPath(gfx_, d.root), RWH(card.left + 18, card.top + 18, 36, 36));
        std::wstring name = (d.label.empty() ? std::wstring(DriveTypeName(d.type)) + L" disk" : d.label) + L" (" + d.root.substr(0, 2) + L")";
        gfx_.Text(name, R(card.left + 66, card.top + 16, card.right - 14, card.top + 38), P_.text, TS(14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        gfx_.Text((d.fs.empty() ? L"" : d.fs + L"  ·  ") + DriveTypeName(d.type), R(card.left + 66, card.top + 37, card.right - 14, card.top + 56),
                  P_.textFaint, Small(12));
        if (d.ready && d.total) {
            double used = 1.0 - static_cast<double>(d.free) / d.total;
            D2D1_COLOR_F col = used > 0.9 ? P_.danger : (used > 0.75 ? P_.warning : P_.success);
            RectF bar = R(card.left + 18, card.top + 70, card.right - 18, card.top + 78);
            gfx_.FillRound(bar, 4, P_.track);
            gfx_.FillRound(R(bar.left, bar.top, bar.left + W(bar) * static_cast<float>(used), bar.bottom), 4, col);
            gfx_.Text(util::FormatSizeShort(d.free) + L" free of " + util::FormatSizeShort(d.total), R(card.left + 18, card.top + 84, card.right - 18, card.top + 104),
                      P_.textDim, Small(12));
            gfx_.Text(Pct(used * 100) + L" used", R(card.left + 18, card.top + 84, card.right - 18, card.top + 104), col,
                      Small(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
        } else {
            gfx_.Text(d.label == L"(not responding)" ? L"Not responding" : L"Not ready", R(card.left + 18, card.top + 70, card.right - 18, card.top + 100),
                      P_.textFaint, Small(12.5f));
        }
        AddHit(card, HK_CARD, static_cast<int>(i));
    }
    y += ((drives_.size() + perRow - 1) / perRow) * (cardH + gap) + 18;

    gfx_.Text(L"MORE WAYS TO START", R(x0, y, x0 + 300, y + 20), P_.textFaint, Small(11, DWRITE_FONT_WEIGHT_SEMI_BOLD));
    y += 28;
    struct Card {
        int id;
        wchar_t icon;
        const wchar_t* title;
        const wchar_t* text;
    };
    std::vector<Card> cards = {{1000, IC_FOLDER, L"Scan a folder", L"Choose any folder or network share"},
                               {1001, IC_OPEN, L"Open a saved scan", L"Browse a .json export (also from Linux ncdu)"},
                               {1002, IC_CLEAN, L"Quick cleanup", L"Caches, temp files and the Recycle Bin"}};
    if (!elevated_) cards.push_back({1003, IC_SHIELD, L"Run as administrator", L"Read protected system folders too"});
    for (size_t i = 0; i < cards.size(); ++i) {
        float cx = x0 + (i % perRow) * (cardW + gap), cy = y + (i / perRow) * (84 + gap);
        RectF card = RWH(cx, cy, cardW, 84);
        bool hover = Hovered(HK_CARD, cards[i].id);
        if (hover) gfx_.Shadow(card, 14, 10);
        gfx_.FillRound(card, 14, P_.surface);
        gfx_.StrokeRound(card, 14, hover ? P_.accent : P_.border, hover ? 1.5f : 1.0f);
        gfx_.FillRound(RWH(card.left + 18, card.top + 22, 40, 40), 10, WithAlpha(P_.accent, 0.14f));
        gfx_.Icon(cards[i].icon, RWH(card.left + 18, card.top + 22, 40, 40), P_.accent, 17);
        gfx_.Text(cards[i].title, R(card.left + 72, card.top + 20, card.right - 14, card.top + 42), P_.text, TS(14, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        gfx_.Text(cards[i].text, R(card.left + 72, card.top + 42, card.right - 14, card.top + 62), P_.textDim, Small(12));
        AddHit(card, HK_CARD, cards[i].id);
    }
    y += ((cards.size() + perRow - 1) / perRow) * (84 + gap) + 18;

    if (!snapshots_.empty()) {
        gfx_.Text(L"RECENT SCANS", R(x0, y, x0 + 300, y + 20), P_.textFaint, Small(11, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        y += 28;
        for (size_t i = 0; i < snapshots_.size() && i < 8; ++i) {
            const SnapshotInfo& s = snapshots_[i];
            RectF r = RWH(x0, y, std::min(maxW, perRow * (cardW + gap) - gap), 52);
            bool hover = Hovered(HK_RECENT, static_cast<int>(i));
            gfx_.FillRound(r, 10, hover ? P_.surface2 : P_.surface);
            gfx_.StrokeRound(r, 10, hover ? P_.accent : P_.border);
            gfx_.Bitmap(icons_.Folder(gfx_), RWH(r.left + 16, r.top + 16, 20, 20));
            gfx_.Text(s.root, R(r.left + 50, r.top, r.right - 300, r.bottom), P_.text, TS(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
            gfx_.Text(util::FormatSizeShort(s.disk) + L"   ·   " + util::FormatCount(s.items) + L" items   ·   " + RelativeTime(s.time),
                      R(r.left, r.top, r.right - 18, r.bottom), P_.textDim, Small(12.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
            AddHit(r, HK_RECENT, static_cast<int>(i));
            y += 60;
        }
        y += 12;
    }
    gfx_.Text(L"Tip: drop a folder onto this window to scan it  ·  right-click any item for more actions  ·  F1 shows all shortcuts",
              R(x0, y, x0 + maxW, y + 22), P_.textFaint, Small(12));
    y += 40;
    float contentH = y - startY + 36;
    gfx_.PopClip();
    ls.scroll = std::clamp(ls.scroll, 0.0f, std::max(0.0f, contentH - H(area)));
}

// =============================================================================================
// Overlays

void App::DrawDialog() {
    D2D1_SIZE_F sz = gfx_.Size();
    gfx_.Fill(R(0, 0, sz.width, sz.height), P_.scrim);
    modalHitStart_ = hits_.size();
    AddHit(R(0, 0, sz.width, sz.height), HK_MODAL_BG);
    float w = std::min(560.0f, sz.width - 40);
    TextStyle msgStyle = TS(13.5f);
    msgStyle.wrap = true;
    msgStyle.center = false;
    float msgH = dialog_.message.empty() ? 0 : gfx_.MeasureHeight(dialog_.message, w - 100, msgStyle) + 6;
    float listH = dialog_.lines.empty() ? 0 : dialog_.lines.size() * 24.0f + 20;
    float h = 24 + 30 + msgH + (listH ? listH + 14 : 0) + (dialog_.hasCheck ? 40 : 0) + 24 + 40 + 24;
    RectF card = RWH((sz.width - w) / 2, std::max(20.0f, (sz.height - h) / 2 - 20), w, h);
    gfx_.Shadow(card, 16, 24);
    gfx_.FillRound(card, 16, P_.surface);
    gfx_.StrokeRound(card, 16, P_.border);
    D2D1_COLOR_F tone = dialog_.tone == 1 ? P_.danger : (dialog_.tone == 2 ? P_.warning : P_.accent);
    float x = card.left + 24, y = card.top + 24;
    if (dialog_.icon) {
        gfx_.FillRound(RWH(x, y, 40, 40), 12, WithAlpha(tone, 0.16f));
        gfx_.Icon(dialog_.icon, RWH(x, y, 40, 40), tone, 18);
    }
    float tx = x + (dialog_.icon ? 56 : 0);
    TextStyle ts = TS(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ts.font = FontKind::Display;
    gfx_.Text(dialog_.title, R(tx, y, card.right - 24, y + 28), P_.text, ts);
    y += 32;
    if (msgH) {
        gfx_.Text(dialog_.message, R(tx, y, card.right - 24, y + msgH), P_.textDim, msgStyle);
        y += msgH;
    }
    if (listH) {
        y += 10;
        RectF lb = R(tx, y, card.right - 24, y + listH);
        gfx_.FillRound(lb, 10, P_.surface2);
        float ly = lb.top + 10;
        for (auto& l : dialog_.lines) {
            size_t tab = l.find(L'\t');
            if (tab != std::wstring::npos) {
                gfx_.Text(l.substr(0, tab), R(lb.left + 12, ly, lb.left + 86, ly + 24), P_.text, Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
                gfx_.Text(FitPath(l.substr(tab + 1), W(lb) - 120, 12.5f), R(lb.left + 100, ly, lb.right - 10, ly + 24), P_.textDim, Small(12.5f));
            } else {
                gfx_.Text(FitPath(l, W(lb) - 24, 12.5f), R(lb.left + 12, ly, lb.right - 10, ly + 24), P_.textDim, Small(12.5f));
            }
            ly += 24;
        }
        y += listH + 4;
    }
    if (dialog_.hasCheck) {
        y += 12;
        RectF cr = R(tx, y, card.right - 24, y + 26);
        bool hv = Hovered(HK_DIALOG, -1);
        Checkbox(RWH(tx, y + 4, 18, 18), dialog_.checked, hv);
        gfx_.Text(dialog_.checkText, R(tx + 28, y, cr.right, cr.bottom), dialog_.checked && dialog_.checkDanger ? P_.danger : P_.text, TS(13));
        AddHit(cr, HK_DIALOG, -1);
        y += 28;
    }
    // Buttons, right aligned.
    float bx = card.right - 24, by = card.bottom - 24 - 38;
    for (int i = static_cast<int>(dialog_.buttons.size()) - 1; i >= 0; --i) {
        const std::wstring& t = dialog_.buttons[i];
        float bw = std::max(96.0f, gfx_.Measure(t, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 36);
        RectF br = R(bx - bw, by, bx, by + 38);
        bool hv = Hovered(HK_DIALOG, i);
        bool primary = i == dialog_.primary, danger = i == dialog_.danger;
        D2D1_COLOR_F bg = danger ? P_.danger : (primary ? P_.accent : P_.surface2);
        if (hv) bg = Mix(bg, P_.dark ? Hex(0xFFFFFF) : Hex(0x000000), 0.1f);
        gfx_.FillRound(br, 9, bg);
        if (!primary && !danger) gfx_.StrokeRound(br, 9, P_.border);
        gfx_.Text(t, br, primary || danger ? P_.accentText : P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
        AddHit(br, HK_DIALOG, i);
        bx -= bw + 10;
    }
}

void App::DrawBusy() {
    D2D1_SIZE_F sz = gfx_.Size();
    gfx_.Fill(R(0, 0, sz.width, sz.height), P_.scrim);
    modalHitStart_ = hits_.size();
    AddHit(R(0, 0, sz.width, sz.height), HK_MODAL_BG);
    auto lines = task_->lines ? task_->lines() : std::vector<std::pair<std::wstring, std::wstring>>{};
    double prog = task_->progress ? task_->progress() : -1.0;
    float w = std::min(560.0f, sz.width - 40);
    float h = 24 + 34 + lines.size() * 26.0f + 18 + 10 + 38 + 24;
    RectF card = RWH((sz.width - w) / 2, (sz.height - h) / 2 - 20, w, h);
    gfx_.Shadow(card, 16, 24);
    gfx_.FillRound(card, 16, P_.surface);
    gfx_.StrokeRound(card, 16, P_.border);
    float x = card.left + 26, y = card.top + 24;
    ULONGLONG ms = GetTickCount64() - task_->start;
    float angle = static_cast<float>((ms % 1100) / 1100.0 * 360.0);
    gfx_.Arc(x + 16, y + 16, 14, 0, 360, P_.track, 3.5f);
    gfx_.Arc(x + 16, y + 16, 14, angle, 100, P_.accent, 3.5f);
    TextStyle ts = TS(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ts.font = FontKind::Display;
    gfx_.Text(task_->title, R(x + 48, y, card.right - 24, y + 32), P_.text, ts);
    y += 46;
    for (auto& [k, v] : lines) {
        if (k.empty()) {
            gfx_.Text(FitPath(v, w - 60, 12), R(x, y, card.right - 24, y + 24), P_.textFaint, Small(12));
        } else {
            gfx_.Text(k, R(x, y, x + 100, y + 24), P_.textDim, TS(13));
            gfx_.Text(v, R(x + 100, y, card.right - 24, y + 24), P_.text, TS(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        }
        y += 26;
    }
    y += 8;
    RectF bar = R(x, y, card.right - 26, y + 6);
    gfx_.FillRound(bar, 3, P_.track);
    if (prog >= 0) {
        gfx_.GradientH(R(bar.left, bar.top, bar.left + std::max(6.0f, W(bar) * static_cast<float>(std::min(prog, 1.0))), bar.bottom), 3,
                       P_.accent, P_.accent2);
    } else {
        float t = static_cast<float>((ms % 1600) / 1600.0);
        float segW = W(bar) * 0.3f;
        float sx = bar.left - segW + (W(bar) + segW) * t;
        gfx_.PushClip(bar);
        gfx_.GradientH(R(sx, bar.top, sx + segW, bar.bottom), 3, WithAlpha(P_.accent, 0), P_.accent2);
        gfx_.PopClip();
    }
    if (task_->cancel) {
        RectF br = R(card.right - 26 - 110, card.bottom - 24 - 36, card.right - 26, card.bottom - 24);
        bool hv = Hovered(HK_BUTTON, CMD_CANCEL_TASK);
        gfx_.FillRound(br, 9, hv ? P_.surface3 : P_.surface2);
        gfx_.StrokeRound(br, 9, P_.border);
        gfx_.Text(task_->cancelRequested ? L"Stopping…" : L"Cancel", br, P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
        AddHit(br, HK_BUTTON, CMD_CANCEL_TASK);
    }
}

void App::DrawToast() {
    if (toast_.text.empty()) return;
    ULONGLONG now = GetTickCount64();
    float alpha = now > toast_.until ? std::max(0.0f, 1.0f - (now - toast_.until) / 400.0f) : 1.0f;
    if (alpha <= 0) return;
    D2D1_SIZE_F sz = gfx_.Size();
    float tw = std::min(gfx_.Measure(toast_.text, TS(13)) + 4, sz.width * 0.5f);
    float w = tw + 64, h = 46;
    RectF r = RWH(sz.width - w - 24, sz.height - kStatusH - h - 18, w, h);
    D2D1_COLOR_F tone = toast_.tone == 1 ? P_.danger : (toast_.tone == 2 ? P_.accent : P_.success);
    gfx_.Shadow(r, 12, 14 * alpha);
    gfx_.FillRound(r, 12, WithAlpha(P_.dark ? P_.surface3 : Hex(0x1F2230), alpha));
    gfx_.FillCircle(r.left + 24, (r.top + r.bottom) / 2, 11, WithAlpha(tone, alpha));
    gfx_.Icon(toast_.tone == 1 ? IC_CLEAR : (toast_.tone == 2 ? IC_INFO : IC_CHECK), RWH(r.left + 13, r.top, 22, h),
              WithAlpha(Hex(0xFFFFFF), alpha), toast_.tone == 2 ? 13.0f : 10.0f);
    gfx_.Text(toast_.text, R(r.left + 46, r.top, r.right - 14, r.bottom), WithAlpha(Hex(0xF2F4F8), alpha), TS(13));
}

void App::DrawTooltip() {
    D2D1_SIZE_F sz = gfx_.Size();
    if (!tipText_.empty() && !dialog_.active && !task_) {
        float tw = gfx_.Measure(tipText_, Small(12)) + 20;
        float x = std::clamp((tipAnchor_.left + tipAnchor_.right) / 2 - tw / 2, 6.0f, sz.width - tw - 6);
        RectF r = RWH(x, tipAnchor_.bottom + 6, tw, 28);
        gfx_.FillRound(r, 7, P_.dark ? Hex(0x3A3D4A) : Hex(0x1F2230));
        gfx_.Text(tipText_, r, Hex(0xF2F4F8), Small(12, DWRITE_FONT_WEIGHT_NORMAL, Align::Center));
    }
    if (tmHover_ >= 0 && tmHover_ < static_cast<int>(tm_.size()) && mouseX_ >= 0 && !dialog_.active && !task_ && view_ == View::Browse) {
        Node* n = tm_[tmHover_].node;
        std::wstring title = n->name;
        std::wstring size = util::FormatSizeShort(Val(n));
        if (tmRoot_ && Val(tmRoot_)) size += L"  ·  " + Pct(100.0 * Val(n) / Val(tmRoot_)) + L" of this folder";
        std::wstring path = NodePath(n->parent ? n->parent : n);
        const ProtectHit& prot = badges_.For(n);
        std::wstring protText = prot.level == Protect::None ? L"" : std::wstring(ProtectLevelName(prot.level)) + L": " + DescribeHit(prot);
        float w = std::max({gfx_.Measure(title, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)), gfx_.Measure(size, Small(12)),
                            protText.empty() ? 0.0f : gfx_.Measure(protText, Small(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 18, 160.0f}) + 28;
        w = std::min(w, 420.0f);
        float h = protText.empty() ? 76.0f : 96.0f;
        float x = mouseX_ + 16, y = mouseY_ + 18;
        if (x + w > sz.width - 8) x = mouseX_ - w - 12;
        if (y + h > sz.height - 8) y = mouseY_ - h - 12;
        RectF r = RWH(x, y, w, h);
        gfx_.Shadow(r, 10, 10);
        gfx_.FillRound(r, 10, P_.dark ? Hex(0x2C2F3B) : Hex(0xFFFFFF));
        gfx_.StrokeRound(r, 10, P_.border);
        int cat = n->IsDir() ? -1 : CategoryOfNode(n);
        gfx_.FillRound(RWH(r.left + 12, r.top + 16, 8, 8), 2.5f, cat >= 0 ? Hex(CategoryRgb(cat)) : P_.textFaint);
        gfx_.Text(title, R(r.left + 26, r.top + 8, r.right - 10, r.top + 30), P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        gfx_.Text(size, R(r.left + 12, r.top + 30, r.right - 10, r.top + 50), P_.accent, Small(12, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        gfx_.Text(FitPath(path, w - 24, 11.5f), R(r.left + 12, r.top + 50, r.right - 10, r.top + 68), P_.textFaint, Small(11.5f));
        if (!protText.empty()) {
            D2D1_COLOR_F col = ShieldColor(prot.level);
            gfx_.Icon(IC_SHIELD, RWH(r.left + 10, r.top + 70, 14, 18), col, 11);
            gfx_.Text(protText, R(r.left + 28, r.top + 70, r.right - 10, r.top + 88), col, Small(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        }
    }
}
