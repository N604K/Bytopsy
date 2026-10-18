// Apps view: installed programs (works without a scan), uninstalled through their own uninstaller,
// then a review of the folders they left behind.
#include "app.h"

#include <commctrl.h>
#include <shlwapi.h>

#include <algorithm>

#include "../util.h"

namespace {

constexpr wchar_t IC_APPS = 0xE71D, IC_DELETE = 0xE74D, IC_REFRESH = 0xE72C, IC_CLOCK = 0xE823, IC_SHIELD = 0xEA18,
                  IC_FOLDER = 0xE8B7;

TextStyle Small(float size = 12.0f, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, Align a = Align::Left) { return TS(size, w, a); }

std::wstring Shorten(std::wstring s, size_t max) {
    if (s.size() > max) s = s.substr(0, max - 1) + L"…";
    return s;
}

}  // namespace

// =============================================================================================
// Model

void App::LoadApps() {
    if (task_) return;
    auto st = std::make_shared<std::vector<InstalledApp>>();
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Reading installed apps");
    t->lines = [] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{L"", Tr(L"Uninstall registry keys, and the size of apps that register none")}};
    };
    t->onDone = [this, st] {
        apps_ = std::move(*st);
        appsLoaded_ = true;
        SortApps();
    };
    StartTask(std::move(t), [st] { *st = FindInstalledApps(); });
}

void App::SortApps() {
    appChosen_ = -1;
    int key = appSortKey_;
    bool desc = appSortDesc_;
    std::stable_sort(apps_.begin(), apps_.end(), [key, desc](const InstalledApp& a, const InstalledApp& b) {
        int r = 0;
        auto cmp3 = [](auto x, auto y) { return x < y ? -1 : (x > y ? 1 : 0); };
        switch (key) {
            case SK_SIZE: r = cmp3(a.bytes, b.bytes); break;
            case SK_MTIME: r = cmp3(a.e.installDate, b.e.installDate); break;
            case SK_FOLDER: r = StrCmpLogicalW(a.e.publisher.c_str(), b.e.publisher.c_str()); break;
            default: r = StrCmpLogicalW(a.e.displayName.c_str(), b.e.displayName.c_str()); break;
        }
        if (r == 0) return StrCmpLogicalW(a.e.displayName.c_str(), b.e.displayName.c_str()) < 0;
        return desc ? r > 0 : r < 0;
    });
    rowsDirty_ = true;
    Invalidate();
}

bool App::AppVisible(const InstalledApp& a) const {
    if (appFilter_.empty()) return true;
    std::wstring f = util::ToLower(appFilter_);
    return util::ContainsI(a.e.displayName, f) || util::ContainsI(a.e.publisher, f);
}

// An app is chosen for Uninstall (one at a time: its own uninstaller asks); leftovers are checked.
void App::OnAppRow(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const Row& r = rows_[row];
    lists_[static_cast<int>(view_)].focus = row;
    if (r.kind == RK_APP && r.idx < static_cast<int>(apps_.size())) {
        appChosen_ = appChosen_ == r.idx ? -1 : r.idx;
    } else if (r.kind == RK_APPLEFT && r.idx < static_cast<int>(appLeft_.size())) {
        const AppLeftover& l = appLeft_[r.idx];
        if (!l.Deletable()) return Toast(TrF(L"Protected, so Bytopsy leaves it alone: {0}", l.protectNote), 2);
        size_t i = static_cast<size_t>(r.idx);
        if (!appLeftSel_.erase(i)) appLeftSel_.insert(i);
    }
    Invalidate();
}

// The search box filters the list while the Apps view is shown (and searches files elsewhere).
void App::SyncSearchBox() {
    bool want = view_ == View::Apps;
    if (!search_ || want == searchForApps_) return;
    searchForApps_ = want;
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(SearchCue().c_str()));
    SetWindowTextW(search_, want ? appFilter_.c_str() : searchQuery_.c_str());
}

void App::OnSearchChanged() {
    if (!searchForApps_) return;
    int n = GetWindowTextLengthW(search_);
    std::wstring text(n, L'\0');
    if (n) GetWindowTextW(search_, text.data(), n + 1);
    while (!text.empty() && text.back() == L' ') text.pop_back();
    if (text == appFilter_) return;
    appFilter_ = text;
    lists_[static_cast<int>(View::Apps)].scroll = 0;
    lists_[static_cast<int>(View::Apps)].focus = -1;
    rowsDirty_ = true;
    Invalidate();
}

// =============================================================================================
// Uninstall: the app's own uninstaller, then wait for its entry to disappear

void App::StartAppUninstall() {
    if (appChosen_ < 0 || appChosen_ >= static_cast<int>(apps_.size())) return Toast(Tr(L"Choose an app first"), 2);
    if (appWait_) return Toast(TrF(L"Finish uninstalling {0} first", appWait_->e.displayName), 2);
    InstalledApp a = apps_[appChosen_];
    if (a.e.noRemove) return Toast(TrF(L"{0} says it can't be uninstalled (NoRemove)", a.e.displayName), 2);
    Dialog d;
    d.title = a.bytes ? TrF(L"Uninstall {0} ({1})?", Shorten(a.e.displayName, 60), util::FormatSizeShort(a.bytes))
                      : TrF(L"Uninstall {0}?", Shorten(a.e.displayName, 60));
    d.message = Tr(L"Its own uninstaller opens and asks you what to remove; Bytopsy deletes nothing itself here. When it is done, "
                   L"Bytopsy looks for folders it left behind and lets you review them. Registry entries are never touched.");
    if (!a.e.publisher.empty()) d.lines.push_back(Tr(L"Publisher") + L"\t" + a.e.publisher);
    if (!a.e.displayVersion.empty()) d.lines.push_back(Tr(L"Version") + L"\t" + a.e.displayVersion);
    if (!a.folder.empty()) d.lines.push_back(Tr(L"Folder") + L"\t" + a.folder);
    d.lines.push_back(Tr(L"Runs") + L"\t" + Shorten(a.e.uninstallString, 110));
    d.icon = IC_DELETE;
    d.tone = 1;
    d.buttons = {Tr(L"Cancel"), Tr(L"Uninstall")};
    d.primary = 1;
    d.onClose = [this, a](int b, bool) {
        if (b != 1) return;
        auto w = std::make_shared<AppWait>();
        w->e = a.e;
        if (!UninstallEntryExists(a.e)) {  // already gone (uninstalled elsewhere since the list was read)
            appWait_ = w;
            w->phase = 2;
            return LookForAppLeftovers(false);
        }
        HANDLE proc = nullptr;
        std::wstring err;
        if (!LaunchCommand(a.e.uninstallString, &proc, err)) return Toast(Tr(err), err == L"Cancelled" ? 2 : 1);
        appWait_ = w;
        appLeftFor_.clear();
        appLeft_.clear();
        appLeftSel_.clear();
        rowsDirty_ = true;
        HWND hwnd = hwnd_;
        HANDLE th = util::StartThread([w, proc, hwnd] {
            if (proc) {
                while (WaitForSingleObject(proc, 300) == WAIT_TIMEOUT && !w->stop) {
                }
                CloseHandle(proc);
            }
            if (w->stop) return;
            w->phase = 1;
            PostMessageW(hwnd, WM_APP_UNINSTALL, 0, 0);
            // Many uninstallers hand over to a copy of themselves and exit at once: the entry going
            // away is what says the app is gone.
            ULONGLONG start = GetTickCount64();
            while (!w->stop) {
                if (!UninstallEntryExists(w->e)) {
                    w->phase = 2;
                    PostMessageW(hwnd, WM_APP_UNINSTALL, 0, 0);
                    return;
                }
                if (GetTickCount64() - start > 15 * 60 * 1000ull) {
                    w->phase = 3;
                    PostMessageW(hwnd, WM_APP_UNINSTALL, 0, 0);
                    return;
                }
                for (int i = 0; i < 6 && !w->stop; ++i) Sleep(250);
            }
        }, 1 << 20);
        if (th) CloseHandle(th);
        Toast(TrF(L"Finish in {0}'s uninstaller; Bytopsy then looks for leftovers", Shorten(a.e.displayName, 40)), 2);
    };
    ShowDialog(std::move(d));
}

void App::OnAppUninstallProgress() {
    if (!appWait_) return;
    // Gone: look for leftovers right away, unless something else is on screen (then the banner's
    // button does it).
    if (appWait_->phase == 2 && !task_ && !dialog_.active) return LookForAppLeftovers(false);
    Invalidate();
}

void App::LookForAppLeftovers(bool askIfInstalled) {
    if (!appWait_ || task_) return;
    UninstallEntry e = appWait_->e;
    if (UninstallEntryExists(e)) {
        if (!askIfInstalled) return;
        Dialog d;
        d.title = TrF(L"{0} is still installed", Shorten(e.displayName, 60));
        d.message = Tr(L"Its entry in Installed apps is still there, so its folders still belong to it. Finish (or restart) its "
                       L"uninstaller first: Bytopsy only offers the folders of apps that are gone.");
        d.icon = 0xE946;
        d.buttons = {Tr(L"OK")};
        d.primary = 0;
        ShowDialog(std::move(d));
        return;
    }
    appWait_->stop = true;
    appWait_.reset();
    struct State {
        std::vector<AppLeftover> left;
        std::vector<InstalledApp> apps;
    };
    auto st = std::make_shared<State>();
    auto t = std::make_unique<Task>();
    t->title = TrF(L"Looking for what {0} left behind", Shorten(e.displayName, 40));
    t->lines = [] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{L"", L"Program Files, ProgramData, AppData (Roaming, Local, LocalLow)"}};
    };
    t->onDone = [this, st, e] {
        apps_ = std::move(st->apps);
        appsLoaded_ = true;
        SortApps();
        appLeft_ = std::move(st->left);
        appLeftFor_ = e.displayName;
        appLeftEntry_ = e;
        appLeftSel_.clear();
        for (size_t i = 0; i < appLeft_.size(); ++i)
            if (appLeft_[i].preselect && appLeft_[i].Deletable()) appLeftSel_.insert(i);
        lists_[static_cast<int>(View::Apps)] = ListState{};
        rowsDirty_ = true;
        if (view_ != View::Apps) ShowView(View::Apps);
        Toast(appLeft_.empty() ? TrF(L"{0} is uninstalled and left no folders behind that Bytopsy can tell", e.displayName)
                               : TrF(L"{0} is uninstalled: review the folders it left behind", e.displayName),
              0);
    };
    StartTask(std::move(t), [st, e] {
        RefreshProtection();
        st->left = FindAppLeftovers(e, ReadUninstallEntries(), LeftoverRoots::ThisPC());
        st->apps = FindInstalledApps();
    });
}

void App::StartAppLeftoversDelete() {
    std::vector<AppLeftover> chosen;
    uint64_t bytes = 0;
    for (size_t i : appLeftSel_)
        if (i < appLeft_.size() && appLeft_[i].Deletable()) {
            chosen.push_back(appLeft_[i]);
            bytes += appLeft_[i].bytes;
        }
    if (chosen.empty()) return;
    std::wstring name = appLeftFor_;
    UninstallEntry entry = appLeftEntry_;
    Dialog d;
    d.title = TrN(chosen.size(), L"Move {0} folder ({1}) to the Recycle Bin?", L"Move {0} folders ({1}) to the Recycle Bin?", util::FormatSizeShort(bytes));
    d.message = chosen.size() == 1 ? TrF(L"{0} is uninstalled; this folder held it or is named after it. If it still holds something you want, "
                                         L"restore it from Recently deleted. The registry is not touched.",
                                         Shorten(name, 60))
                                   : TrF(L"{0} is uninstalled; these folders held it or are named after it. If one still holds something you "
                                         L"want, restore it from Recently deleted. The registry is not touched.",
                                         Shorten(name, 60));
    for (size_t i = 0; i < chosen.size() && i < 8; ++i) {
        d.lines.push_back(util::FormatSizeShort(chosen[i].bytes) + L"\t" + chosen[i].path);
        if (!chosen[i].protectNote.empty()) d.lines.push_back(L"\t⚠ " + chosen[i].protectNote);
    }
    if (chosen.size() > 8) d.lines.push_back(L"\t" + TrN(chosen.size() - 8, L"… and {0} more", L"… and {0} more"));
    d.icon = IC_DELETE;
    d.tone = 1;
    d.buttons = {Tr(L"Cancel"), Tr(L"Move to Recycle Bin")};
    d.primary = 1;
    d.onClose = [this, chosen, name, entry](int b, bool) {
        if (b != 1) return;
        if (!entry.key.empty() && UninstallEntryExists(entry)) return Toast(TrF(L"{0} is installed again: its folders are left alone", name), 1);
        struct State {
            std::vector<AppLeftover> items;
            std::vector<std::wstring> done;
            uint64_t freed = 0;
            std::vector<std::wstring> errors;
            std::atomic<size_t> index{0};
        };
        auto st = std::make_shared<State>();
        st->items = chosen;
        auto t = std::make_unique<Task>();
        t->title = Tr(L"Moving leftovers to the Recycle Bin");
        t->lines = [st] {
            size_t i = std::min(st->index.load(), st->items.size() - 1);
            return std::vector<std::pair<std::wstring, std::wstring>>{{L"", st->items[i].path}};
        };
        t->progress = [st] { return static_cast<double>(st->index) / st->items.size(); };
        t->onDone = [this, st] {
            Toast(st->errors.empty() ? TrF(L"Moved {0} of leftovers to the Recycle Bin", util::FormatSizeShort(st->freed)) : st->errors[0],
                  st->errors.empty() ? 0 : 1);
            for (auto& p : st->done)
                appLeft_.erase(std::remove_if(appLeft_.begin(), appLeft_.end(), [&](const AppLeftover& l) { return l.path == p; }),
                               appLeft_.end());
            appLeftSel_.clear();
            rowsDirty_ = true;
            Invalidate();
        };
        StartTask(std::move(t), [st, name] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            RefreshProtection();
            for (size_t i = 0; i < st->items.size(); ++i) {
                st->index = i;
                const AppLeftover& l = st->items[i];
                if (!PathExists(l.path)) continue;
                Protect p = LeftoverProtection(l.path);
                if (p == Protect::Important || p == Protect::Blocked) {
                    st->errors.push_back(TrF(L"{0} is protected and was left alone", l.path));
                    continue;
                }
                std::wstring err;
                if (!DeleteToRecycleBin(l.path, err)) {
                    st->errors.push_back(l.path + L": " + err);
                    continue;
                }
                st->freed += l.bytes;
                st->done.push_back(l.path);
                JournalEntry je;
                je.action = JournalAction::Recycle;
                je.bytes = l.bytes;
                je.items = l.items;
                je.path = l.path;
                je.note = L"leftover of uninstalled app " + name;
                JournalAppend(je);
            }
            CoUninitialize();
        });
    };
    ShowDialog(std::move(d));
}

// =============================================================================================
// Drawing

void App::DrawApps(const RectF& area) {
    float top = area.top + 18;
    uint64_t total = 0, chosenLeft = 0;
    size_t shown = 0;
    for (auto& a : apps_)
        if (AppVisible(a)) {
            total += a.bytes;
            ++shown;
        }
    for (size_t i : appLeftSel_)
        if (i < appLeft_.size()) chosenLeft += appLeft_[i].bytes;
    std::wstring sub;
    if (!appsLoaded_)
        sub = Tr(L"Installed programs, uninstalled with their own uninstaller, then their leftovers reviewed");
    else if (!appFilter_.empty())
        sub = TrN(apps_.size(), L"{1} of {0} app match “{2}”", L"{1} of {0} apps match “{2}”", util::FormatCount(shown), appFilter_) + L"  ·  " +
              util::FormatSizeShort(total);
    else
        sub = TrN(apps_.size(), L"{0} app", L"{0} apps") + L"  ·  " + util::FormatSizeShort(total) + L"  ·  " +
              Tr(L"uninstalled with their own uninstaller, then leftovers reviewed");
    float bx = area.right - 24;
    auto button = [&](const std::wstring& t, wchar_t icon, int cmd, int style, float y, bool enabled = true) {
        float w = gfx_.Measure(t, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + (icon ? 54 : 36);
        bx -= w;
        Button(RWH(bx, y, w, 34), icon, t, cmd, style, enabled);
        bx -= 8;
    };
    if (!appLeftSel_.empty()) button(TrF(L"Delete leftovers {0}", util::FormatSizeShort(chosenLeft)), IC_DELETE, CMD_APPS_DELETE, 2, top + 6);
    if (appChosen_ >= 0 && appChosen_ < static_cast<int>(apps_.size()))
        button(TrF(L"Uninstall {0}", Shorten(apps_[appChosen_].e.displayName, 28)), IC_DELETE, CMD_APP_UNINSTALL, 2, top + 6, !appWait_);
    button(Tr(L"Refresh"), IC_REFRESH, CMD_APPS_REFRESH, 0, top + 6);
    bx -= 8;
    DrawViewHeader(R(area.left + 24, top, bx, top + 46), Tr(L"Apps"), sub, IC_APPS);
    float y = top + 58;

    // An uninstaller Bytopsy is waiting for.
    if (appWait_) {
        RectF b = R(area.left + 24, y, area.right - 24, y + 52);
        gfx_.FillRound(b, 10, WithAlpha(P_.accent, 0.12f));
        gfx_.Icon(IC_CLOCK, RWH(b.left + 10, b.top, 30, H(b)), P_.accent, 16);
        int ph = appWait_->phase;
        std::wstring n = Shorten(appWait_->e.displayName, 48);
        std::wstring text = ph == 0   ? TrF(L"Waiting for {0}’s uninstaller … finish it in its own window.", n)
                            : ph == 1 ? TrF(L"The uninstaller closed. Waiting for {0} to leave the installed apps …", n)
                            : ph == 2 ? TrF(L"{0} is uninstalled.", n)
                                      : TrF(L"{0} is still installed: its uninstaller was cancelled or didn’t finish. Nothing was removed.", n);
        float save = bx;
        bx = b.right - 9;
        button(ph == 3 ? Tr(L"Dismiss") : Tr(L"Stop waiting"), 0, CMD_APP_STOPWAIT, 0, b.top + 9);
        button(Tr(L"Look for leftovers"), IC_FOLDER, CMD_APP_LEFTOVERS, 1, b.top + 9);
        gfx_.Text(text, R(b.left + 46, b.top, bx - 8, b.bottom), P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        bx = save;
        y += 64;
    }
    if (!appsLoaded_) return;
    if (apps_.empty()) {
        DrawEmptyState(R(area.left, y, area.right, area.bottom), IC_APPS, Tr(L"No apps found"),
                       Tr(L"Nothing in the Uninstall registry keys looks like an app (system components, updates and patches are left out)."));
        return;
    }
    gfx_.Text(Tr(L"Click an app to choose it") + L"  ·  " + Tr(L"double-click to open its folder") + L"  ·  " + Tr(L"type in the search box to filter"),
              R(area.left + 24, y, area.right - 24, y + 24), P_.textFaint, Small(12, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
    y += 28;

    std::vector<Column> cols = {{Tr(L"App"), -1.4f, Align::Left, SK_NAME},
                                {Tr(L"Publisher"), -0.8f, Align::Left, SK_FOLDER},
                                {Tr(L"Version"), 120, Align::Left, -1},
                                {Tr(L"Installed"), 96, Align::Left, SK_MTIME},
                                {Tr(L"Size"), 92, Align::Right, SK_SIZE}};
    DrawTable(
        R(area.left, y, area.right, area.bottom), cols, appSortKey_, appSortDesc_,
        [&](int i, const Column&, int ci, const RectF& cr, bool) {
            const Row& row = rows_[i];
            float cy = (cr.top + cr.bottom) / 2;
            if (row.kind == RK_APPLEFT) {
                if (row.idx >= static_cast<int>(appLeft_.size())) return;
                const AppLeftover& l = appLeft_[row.idx];
                size_t k = static_cast<size_t>(row.idx);
                switch (ci) {
                    case 0: {
                        if (l.Deletable())
                            Checkbox(RWH(cr.left, cy - 9, 18, 18), appLeftSel_.count(k) > 0, Hovered(HK_ROW, i));
                        else
                            gfx_.Icon(IC_SHIELD, RWH(cr.left, cr.top, 18, H(cr)), P_.warning, 13);
                        float right = cols[1].x + cols[1].w;
                        gfx_.Text(FitPath(l.path, right - cr.left - 30, 12.5f), R(cr.left + 30, cr.top, right, cr.bottom),
                                  l.Deletable() ? P_.text : P_.textDim, Small(12.5f));
                        break;
                    }
                    case 2: {
                        RectF wide = R(cr.left, cr.top, cols[3].x + cols[3].w, cr.bottom);
                        std::wstring note = l.protectNote.empty() ? Tr(l.why) : l.protectNote;
                        gfx_.Text(FitPath(note, W(wide), 12), wide, l.level >= Protect::Caution ? P_.warning : P_.textFaint, Small(12));
                        break;
                    }
                    case 4:
                        gfx_.Text(util::FormatSizeShort(l.bytes), cr, P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
                        break;
                    default: break;
                }
                return;
            }
            if (row.idx >= static_cast<int>(apps_.size())) return;
            const InstalledApp& a = apps_[row.idx];
            switch (ci) {
                case 0:
                    Checkbox(RWH(cr.left, cy - 9, 18, 18), appChosen_ == row.idx, Hovered(HK_ROW, i));
                    gfx_.Text(a.e.displayName, R(cr.left + 30, cr.top, cr.right, cr.bottom), P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD));
                    break;
                case 1: gfx_.Text(a.e.publisher, cr, P_.textDim, Small(12.5f)); break;
                case 2: gfx_.Text(a.e.displayVersion, cr, P_.textDim, Small(12.5f)); break;
                case 3:
                    gfx_.Text(a.e.installDate ? util::FormatDateLocal(a.e.installDate) : L"", cr, P_.textDim, Small(12.5f));
                    break;
                default:
                    gfx_.Text(a.bytes ? util::FormatSizeShort(a.bytes) : L"—", cr, a.measured ? P_.textDim : P_.text,
                              TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
            }
        },
        [&](int i, const RectF& r) {
            bool left = rows_[i].idx == 0;
            gfx_.Icon(left ? IC_DELETE : IC_APPS, RWH(r.left + 8, r.top, 22, H(r)), P_.accent2, 13);
            std::wstring title = left ? TrF(L"Left behind by {0}", Shorten(appLeftFor_, 40)) : Tr(L"Installed apps");
            std::wstring note = !left ? Tr(L"choose one to uninstall it with its own uninstaller")
                                : appLeft_.empty()
                                    ? Tr(L"no folders found in Program Files, ProgramData or AppData")
                                    : Tr(L"check what to move to the Recycle Bin") + L"  ·  " + Tr(L"shielded folders hold protected data and stay");
            TextStyle ts = TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            float tw = gfx_.Measure(title, ts) + 4;
            float right = r.right - 12;
            if (left) {
                std::wstring done = Tr(L"Done");
                float dw = ButtonWidth(done, 0, 70);
                right = r.right - 14 - dw;
                Button(R(r.right - 6 - dw, r.top + 3, r.right - 6, r.bottom - 3), 0, done, CMD_APP_LEFT_DONE, 0);
            }
            gfx_.Text(title, R(r.left + 36, r.top, r.left + 36 + tw, r.bottom), P_.text, ts);
            gfx_.Text(note, R(r.left + 48 + tw, r.top, right, r.bottom), P_.textFaint, Small(12));
        });
}
