// Apps view: installed programs (works without a scan), uninstalled through their own uninstaller,
// then a review of the folders they left behind.
#include "app.h"

#include <commctrl.h>
#include <shlwapi.h>

#include <algorithm>

#include "../util.h"

namespace {

constexpr wchar_t IC_APPS = 0xE71D, IC_DELETE = 0xE74D, IC_REFRESH = 0xE72C, IC_CLOCK = 0xE823, IC_SHIELD = 0xEA18,
                  IC_FOLDER = 0xE8B7, IC_UPDATE = 0xE896, IC_STORE = 0xE719,
                  IC_SETTINGS = 0xE713, IC_REPAIR = 0xE90F, IC_GLOBE = 0xE774;

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
    // Updates are asked for again only once they were asked for (winget contacts its sources).
    bool updates = appUpdChecked_;
    struct State {
        std::vector<InstalledApp> apps;
        std::wstring winget;
        WingetDiagnosis diag;
        WingetList upd;
        std::atomic<bool> cancel{false};
        std::atomic<int> phase{0};
    };
    auto st = std::make_shared<State>();
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Reading installed apps");
    t->lines = [st] {
        return std::vector<std::pair<std::wstring, std::wstring>>{
            {L"", st->phase ? Tr(L"Asking winget which apps have updates (winget contacts its sources)")
                            : Tr(L"Uninstall registry keys, and the size of apps that register none")}};
    };
    if (updates) t->cancel = [st] { st->cancel = true; };
    t->onDone = [this, st, updates] {
        apps_ = std::move(st->apps);
        appsLoaded_ = true;
        if (updates && !st->cancel) {
            winget_ = st->winget;
            wingetDiag_ = st->diag;
            appUpd_ = std::move(st->upd);
        }
        SortApps();
        if (!appUpdResults_.empty())
            ShowAppUpdateResults();
        else if (updates && !st->cancel && !winget_.empty() && !appUpd_.ok)
            Toast(TrF(L"winget couldn't list updates: {0}", Tr(appUpd_.error)), 1);
    };
    StartTask(std::move(t), [st, updates] {
        st->apps = FindInstalledApps();
        if (!updates || st->cancel) return;
        st->phase = 1;
        st->diag = DiagnoseWinget();
        st->winget = st->diag.winget;
        if (!st->winget.empty()) st->upd = ListWingetUpdates(st->winget, &st->cancel);
    });
}

// =============================================================================================
// Updates through winget

void App::CheckAppUpdates() {
    if (task_) return;
    if (!appsLoaded_) {  // read both
        appUpdChecked_ = true;
        return LoadApps();
    }
    struct State {
        std::wstring winget;
        WingetDiagnosis diag;
        WingetList upd;
        std::atomic<bool> cancel{false};
    };
    auto st = std::make_shared<State>();
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Asking winget for updates");
    t->lines = [] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{L"", Tr(L"winget upgrade: it only lists here, and contacts its sources")}};
    };
    t->cancel = [st] { st->cancel = true; };
    t->onDone = [this, st] {
        if (st->cancel) return;
        appUpdChecked_ = true;
        winget_ = st->winget;
        wingetDiag_ = st->diag;
        appUpd_ = std::move(st->upd);
        MatchAppUpdates();
        if (!winget_.empty() && !appUpd_.ok) Toast(TrF(L"winget couldn't list updates: {0}", Tr(appUpd_.error)), 1);
    };
    StartTask(std::move(t), [st] {
        st->diag = DiagnoseWinget();
        st->winget = st->diag.winget;
        if (!st->winget.empty()) st->upd = ListWingetUpdates(st->winget, &st->cancel);
    });
}

// Links updates and apps (after every sort: indices into apps_ change).
void App::MatchAppUpdates() {
    appUpdApp_ = MatchUpdatesToApps(appUpd_.updates, apps_);
    appUpdOf_.assign(apps_.size(), -1);
    for (size_t i = 0; i < appUpdApp_.size(); ++i)
        if (appUpdApp_[i] >= 0) appUpdOf_[appUpdApp_[i]] = static_cast<int>(i);
    // Checked updates that are gone (updated, or no longer offered) are unchecked.
    std::set<std::wstring> keep;
    for (auto& u : appUpd_.updates)
        if (appUpdSel_.count(u.id) && u.Updatable()) keep.insert(u.id);
    appUpdSel_ = std::move(keep);
    rowsDirty_ = true;
    Invalidate();
}

bool App::AppUpdVisible(size_t i) const {
    if (i >= appUpd_.updates.size()) return false;
    if (appFilter_.empty()) return true;
    std::wstring f = util::ToLower(appFilter_);
    const WingetUpdate& u = appUpd_.updates[i];
    int a = i < appUpdApp_.size() ? appUpdApp_[i] : -1;
    return util::ContainsI(u.name, f) || util::ContainsI(u.id, f) ||
           (a >= 0 && a < static_cast<int>(apps_.size()) && (util::ContainsI(apps_[a].e.displayName, f) || util::ContainsI(apps_[a].e.publisher, f)));
}

void App::OnAppUpdRow(size_t i) {
    if (i >= appUpd_.updates.size()) return;
    const WingetUpdate& u = appUpd_.updates[i];
    if (!u.Updatable()) return Toast(Tr(L"winget shortened its id: update it with winget in a terminal"), 2);
    if (!appUpdSel_.erase(u.id)) appUpdSel_.insert(u.id);
    Invalidate();
}

// One app after another, each through winget and its own installer; then the list is read again.
void App::StartAppUpdates(std::vector<size_t> which) {
    if (task_) return;
    if (appWait_) return Toast(TrF(L"Finish uninstalling {0} first", appWait_->e.displayName), 2);
    struct Item {
        WingetUpdate u;
        std::wstring name;
    };
    std::vector<Item> items;
    for (size_t i : which)
        if (i < appUpd_.updates.size() && appUpd_.updates[i].Updatable()) {
            int a = i < appUpdApp_.size() ? appUpdApp_[i] : -1;
            items.push_back({appUpd_.updates[i], a >= 0 && a < static_cast<int>(apps_.size()) ? apps_[a].e.displayName : appUpd_.updates[i].name});
        }
    if (items.empty()) return Toast(Tr(L"Choose an update first"), 2);
    if (winget_.empty()) return Toast(Tr(L"winget isn't installed"), 1);
    Dialog d;
    d.title = items.size() == 1 ? TrF(L"Update {0} to {1}?", Shorten(items[0].name, 50), items[0].u.available)
                                : TrN(items.size(), L"Update {0} app?", L"Update {0} apps?");
    d.message = items.size() == 1
                    ? Tr(L"winget downloads the update and runs the app's own installer. It may ask for administrator permission (its "
                         L"own prompt) or close the app while it updates. winget accepts the package's license agreements for you. "
                         L"Bytopsy deletes nothing.")
                    : Tr(L"winget downloads each update and runs the app's own installer, one after another. An installer may ask for "
                         L"administrator permission (its own prompt) or close the app while it updates. winget accepts the package's license "
                         L"agreements for you. Bytopsy deletes nothing.");
    for (size_t i = 0; i < items.size() && i < 8; ++i)
        d.lines.push_back(Shorten(items[i].name, 48) + L"\t" + TrF(L"{0} → {1}", items[i].u.version, items[i].u.available));
    if (items.size() > 8) d.lines.push_back(L"\t" + TrN(items.size() - 8, L"… and {0} more", L"… and {0} more"));
    if (items.size() == 1) d.lines.push_back(Tr(L"Runs") + L"\t" + L"winget upgrade --id " + items[0].u.id + L" --exact");
    d.icon = IC_UPDATE;
    d.tone = 0;
    d.buttons = {Tr(L"Cancel"), items.size() == 1 ? Tr(L"Update") : TrN(items.size(), L"Update {0} app", L"Update {0} apps")};
    d.primary = 1;
    d.onClose = [this, items](int b, bool) {
        if (b != 1 || task_) return;
        struct State {
            std::vector<Item> items;
            std::wstring winget;
            std::vector<AppUpdResult> results;
            std::atomic<size_t> index{0};
            std::atomic<bool> stop{false};
        };
        auto st = std::make_shared<State>();
        st->items = items;
        st->winget = winget_;
        auto t = std::make_unique<Task>();
        t->title = items.size() == 1 ? TrF(L"Updating {0}", Shorten(items[0].name, 40)) : Tr(L"Updating apps");
        t->lines = [st] {
            size_t i = std::min(st->index.load(), st->items.size() - 1);
            std::vector<std::pair<std::wstring, std::wstring>> l;
            if (st->items.size() > 1) l.push_back({L"", TrF(L"{0} of {1}", util::FormatCount(i + 1), util::FormatCount(st->items.size()))});
            l.push_back({L"", TrF(L"{0}: {1} → {2}", st->items[i].name, st->items[i].u.version, st->items[i].u.available)});
            l.push_back({L"", st->stop ? Tr(L"Stopping after this app") : Tr(L"Its installer may show a window or ask for permission")});
            return l;
        };
        t->progress = [st] { return static_cast<double>(st->index) / st->items.size(); };
        // A running installer is never interrupted: stopping waits for it.
        t->cancel = [st] { st->stop = true; };
        t->onDone = [this, st] {
            appUpdResults_ = std::move(st->results);
            LoadApps();  // reads apps and updates again, then shows the results
        };
        StartTask(std::move(t), [st] {
            for (size_t i = 0; i < st->items.size() && !st->stop; ++i) {
                st->index = i;
                const Item& it = st->items[i];
                WingetUpgradeResult r = RunWingetUpgrade(st->winget, it.u);
                st->results.push_back({it.name, it.u.id, it.u.version, it.u.available, r});
                i18n::EnglishScope english;  // the journal is kept in English
                JournalEntry je;
                je.action = JournalAction::Clean;
                je.path = L"App update: " + it.name;
                je.note = L"winget " + WingetUpgradeArgs(it.u) + L" (" + it.u.version + L" -> " + it.u.available + L"): " +
                          (r.ok ? (r.restart ? L"updated, restart to finish" : L"updated") : L"failed: " + r.message) +
                          (r.code > 0 || r.code < -2 ? L" (exit code " + std::to_wstring(r.code) + L")" : L"");
                JournalAppend(je);
            }
        });
    };
    ShowDialog(std::move(d));
}

void App::ShowAppUpdateResults() {
    std::vector<AppUpdResult> res = std::move(appUpdResults_);
    appUpdResults_.clear();
    if (res.empty()) return;
    size_t ok = 0, restart = 0;
    for (auto& r : res) {
        ok += r.r.ok;
        restart += r.r.restart && r.r.ok;
    }
    Dialog d;
    d.title = ok == res.size() ? TrN(ok, L"{0} app updated", L"{0} apps updated")
                               : TrF(L"{0} of {1} updated", util::FormatCount(ok), util::FormatCount(res.size()));
    d.message = restart ? Tr(L"Restart Windows to finish. The apps list was read again.") : Tr(L"The apps list was read again.");
    for (size_t i = 0; i < res.size() && i < 12; ++i) {
        const AppUpdResult& r = res[i];
        std::wstring what = r.r.ok ? (r.r.restart ? Tr(L"Updated: restart Windows to finish") : TrF(L"Updated to {0}", r.to))
                                   : L"⚠ " + Tr(r.r.message);
        d.lines.push_back(Shorten(r.name, 44) + L"\t" + Shorten(what, 90));
    }
    if (res.size() > 12) d.lines.push_back(L"\t" + TrN(res.size() - 12, L"… and {0} more", L"… and {0} more"));
    d.icon = ok == res.size() ? 0xE73E : 0xE7BA;  // check mark / warning
    d.tone = ok == res.size() ? 0 : 2;
    d.buttons = {Tr(L"OK")};
    d.primary = 0;
    ShowDialog(std::move(d));
}

// winget is missing but App Installer is there: register it again for this user (PowerShell's
// Add-AppxPackage, as this user, no admin, nothing downloaded), then look for winget again.
void App::StartWingetRepair() {
    if (task_ || !wingetDiag_.CanRepair() || !winget_.empty()) return;
    Dialog d;
    d.title = Tr(L"Repair winget?");
    d.message = Tr(L"Registers Microsoft's App Installer, which brings winget, again for your account. It runs as you, without "
                   L"administrator permission. Nothing is downloaded or installed, and nothing is deleted.");
    // "Microsoft.DesktopAppInstaller_1.29.379.0_x64__8wekyb3d8bbwe" -> "Microsoft.DesktopAppInstaller 1.29.379.0"
    const std::wstring& pkg = wingetDiag_.package;
    size_t v = pkg.find(L'_'), ve = v == std::wstring::npos ? v : pkg.find(L'_', v + 1);
    if (ve != std::wstring::npos) d.lines.push_back(Tr(L"Package") + L"\t" + pkg.substr(0, v) + L" " + pkg.substr(v + 1, ve - v - 1));
    // The command, cut where it reads well (WingetRepairCommand()).
    d.lines.push_back(Tr(L"Runs") + L"\tpowershell.exe -NoProfile -NonInteractive -Command");
    d.lines.push_back(L"\tAdd-AppxPackage -RegisterByFamilyName -MainPackage");
    d.lines.push_back(L"\t" + std::wstring(kAppInstallerFamily));
    d.icon = IC_REPAIR;
    d.tone = 0;
    d.buttons = {Tr(L"Cancel"), Tr(L"Repair winget")};
    d.primary = 1;
    d.onClose = [this](int b, bool) {
        if (b != 1 || task_) return;
        auto st = std::make_shared<WingetRepairResult>();
        auto t = std::make_unique<Task>();
        t->title = Tr(L"Repairing winget");
        t->lines = [] {
            return std::vector<std::pair<std::wstring, std::wstring>>{
                {L"", Tr(L"Registering App Installer for your account (PowerShell Add-AppxPackage)")}};
        };
        // Registering is never interrupted: no Cancel.
        t->onDone = [this, st] {
            appUpdChecked_ = true;
            wingetDiag_ = st->after;
            winget_ = st->after.winget;
            wingetRepairDetail_ = st->detail;
            if (st->outcome == WingetRepairOutcome::Fixed) {
                wingetRepair_ = -1;
                Toast(Tr(L"winget works again: asking it for updates"), 0);
                return CheckAppUpdates();
            }
            wingetRepair_ = static_cast<int>(st->outcome);
            Toast(st->outcome == WingetRepairOutcome::StillAlias ? Tr(L"App Installer is registered: turn on the winget.exe alias")
                                                                 : Tr(L"winget is still missing"),
                  st->outcome == WingetRepairOutcome::StillAlias ? 2 : 1);
            rowsDirty_ = true;
            Invalidate();
        };
        StartTask(std::move(t), [st] {
            *st = RepairWinget();
            i18n::EnglishScope english;  // the journal is kept in English
            JournalEntry je;
            je.action = JournalAction::Clean;
            je.path = L"Repair winget: App Installer";
            je.note = WingetRepairJournalNote(*st);
            JournalAppend(je);
        });
    };
    ShowDialog(std::move(d));
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
    MatchAppUpdates();
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
    } else if (r.kind == RK_APPUPD) {
        OnAppUpdRow(static_cast<size_t>(r.idx));
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
    bool updView = appShowUpdates_ && appsLoaded_;
    bool wingetOk = appUpdChecked_ && !winget_.empty() && appUpd_.ok;
    std::vector<size_t> checked;  // indices of the checked updates
    for (size_t i = 0; i < appUpd_.updates.size(); ++i)
        if (appUpdSel_.count(appUpd_.updates[i].id)) checked.push_back(i);
    std::wstring sub;
    if (!appsLoaded_)
        sub = Tr(L"Installed programs, uninstalled with their own uninstaller, then their leftovers reviewed");
    else if (updView) {
        if (!appUpdChecked_)
            sub = Tr(L"Asking winget …");
        else if (winget_.empty())
            sub = wingetDiag_.CanRepair() ? Tr(L"Updates come through winget, which can't be reached right now")
                                          : Tr(L"Updates come through winget, which isn't installed");
        else if (!appUpd_.ok)
            sub = Tr(L"winget couldn't list updates");
        else
            sub = TrN(appUpd_.updates.size(), L"{0} update from winget", L"{0} updates from winget") +
                  (checked.empty() ? L"" : L"  ·  " + TrN(checked.size(), L"{0} checked", L"{0} checked")) + L"  ·  " +
                  Tr(L"each through the app's own installer");
    } else if (!appFilter_.empty())
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
    if (updView) {
        if (!checked.empty())
            button(TrN(checked.size(), L"Update {0} app", L"Update {0} apps"), IC_UPDATE, CMD_APP_UPDATE_SEL, 1, top + 6, !appWait_);
        if (wingetOk && !appUpd_.updates.empty())
            button(checked.empty() ? Tr(L"Check all") : Tr(L"Check none"), 0, CMD_APP_UPD_SELALL, 0, top + 6);
    } else if (appChosen_ >= 0 && appChosen_ < static_cast<int>(apps_.size())) {
        button(TrF(L"Uninstall {0}", Shorten(apps_[appChosen_].e.displayName, 28)), IC_DELETE, CMD_APP_UNINSTALL, 2, top + 6, !appWait_);
        int u = appChosen_ < static_cast<int>(appUpdOf_.size()) ? appUpdOf_[appChosen_] : -1;
        if (u >= 0 && appUpd_.updates[u].Updatable())
            button(TrF(L"Update to {0}", Shorten(appUpd_.updates[u].available, 20)), IC_UPDATE, CMD_APP_UPDATE_ONE, 1, top + 6, !appWait_);
    }
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
    if (apps_.empty() && !updView) {
        DrawEmptyState(R(area.left, y, area.right, area.bottom), IC_APPS, Tr(L"No apps found"),
                       Tr(L"Nothing in the Uninstall registry keys looks like an app (system components, updates and patches are left out)."));
        return;
    }

    // The filter: every app, or the ones winget has an update for.
    {
        float x = area.left + 24;
        x += Chip(x, y - 3, Tr(L"All apps"), !appShowUpdates_, 41, 0) + 6;
        std::wstring u = wingetOk ? TrF(L"Updates ({0})", util::FormatCount(appUpd_.updates.size())) : Tr(L"Updates");
        x += Chip(x, y - 3, u, appShowUpdates_, 41, 1) + 16;
        std::wstring hint;
        if (!updView)
            hint = Tr(L"Click an app to choose it") + L"  ·  " + Tr(L"double-click to open its folder") + L"  ·  " + Tr(L"type in the search box to filter");
        else if (wingetOk) {
            hint = Tr(L"Click an update to check it") + L"  ·  " + Tr(L"marked + : not in Installed apps (a Store app)");
            if (appUpd_.unknownCount)
                hint += L"  ·  " + TrN(appUpd_.unknownCount, L"{0} app with a version winget can't tell isn't listed",
                                       L"{0} apps with versions winget can't tell aren't listed");
            if (appUpd_.pinnedCount) hint += L"  ·  " + TrN(appUpd_.pinnedCount, L"{0} pinned app isn't listed", L"{0} pinned apps aren't listed");
        }
        RectF hr = R(x, y, area.right - 24, y + 24);
        TextStyle hs = Small(12, DWRITE_FONT_WEIGHT_NORMAL, Align::Right);
        if (gfx_.Measure(hint, hs) > W(hr)) hs.align = Align::Left;  // long translations: cut at the right instead
        gfx_.Text(hint, hr, P_.textFaint, hs);
        y += 36;
    }
    if (updView) {
        RectF body = R(area.left, y, area.right, area.bottom);
        auto bodyButton = [&](const std::wstring& t, wchar_t icon, int cmd) {
            float w = ButtonWidth(t, icon, 140);
            float cx = (body.left + body.right) / 2, cy = (body.top + body.bottom) / 2 - 30;
            Button(RWH(cx - w / 2, cy + 136, w, 36), icon, t, cmd, 1);
        };
        if (!appUpdChecked_) return;
        if (winget_.empty()) return DrawWingetMissing(body);
        if (!appUpd_.ok) {
            DrawEmptyState(body, IC_UPDATE, Tr(L"winget couldn't list updates"), Tr(appUpd_.error));
            return bodyButton(Tr(L"Try again"), IC_REFRESH, CMD_APP_UPD_CHECK);
        }
        if (appUpd_.updates.empty()) {
            DrawEmptyState(body, IC_UPDATE, Tr(L"Everything is up to date"), Tr(L"winget knows of no newer version of any installed app."));
            return;
        }
    }

    // Columns: the Update column once winget was asked.
    bool updCol = wingetOk;
    std::vector<Column> cols = {{Tr(L"App"), -1.4f, Align::Left, SK_NAME},
                                {Tr(L"Publisher"), -0.8f, Align::Left, SK_FOLDER},
                                {Tr(L"Version"), 120, Align::Left, -1}};
    int cUpd = -1;
    if (updCol) {
        cUpd = static_cast<int>(cols.size());
        cols.push_back({Tr(L"Update"), 130, Align::Left, -1});
    }
    int cInst = static_cast<int>(cols.size());
    cols.push_back({Tr(L"Installed"), 96, Align::Left, SK_MTIME});
    cols.push_back({Tr(L"Size"), 92, Align::Right, SK_SIZE});
    TextStyle badge = TS(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto drawBadge = [&](const std::wstring& to, const RectF& cr, bool dim) {
        std::wstring t = L"↑ " + to;
        float w = std::min(gfx_.Measure(t, badge) + 16, W(cr));
        RectF b = RWH(cr.left, (cr.top + cr.bottom) / 2 - 11, w, 22);
        gfx_.FillRound(b, 11, WithAlpha(dim ? P_.textFaint : P_.accent, 0.16f));
        gfx_.Text(t, Inset(b, 8, 0), dim ? P_.textDim : P_.accent, badge);
    };
    DrawTable(
        R(area.left, y, area.right, area.bottom), cols, appSortKey_, appSortDesc_,
        [&](int i, const Column&, int ci, const RectF& cr, bool) {
            const Row& row = rows_[i];
            float cy = (cr.top + cr.bottom) / 2;
            if (row.kind == RK_APPLEFT) {
                if (row.idx >= static_cast<int>(appLeft_.size())) return;
                const AppLeftover& l = appLeft_[row.idx];
                size_t k = static_cast<size_t>(row.idx);
                if (ci == 0) {
                    if (l.Deletable())
                        Checkbox(RWH(cr.left, cy - 9, 18, 18), appLeftSel_.count(k) > 0, Hovered(HK_ROW, i));
                    else
                        gfx_.Icon(IC_SHIELD, RWH(cr.left, cr.top, 18, H(cr)), P_.warning, 13);
                    float right = cols[1].x + cols[1].w;
                    gfx_.Text(FitPath(l.path, right - cr.left - 30, 12.5f), R(cr.left + 30, cr.top, right, cr.bottom),
                              l.Deletable() ? P_.text : P_.textDim, Small(12.5f));
                } else if (ci == 2) {
                    RectF wide = R(cr.left, cr.top, cols[cInst].x + cols[cInst].w, cr.bottom);
                    std::wstring note = l.protectNote.empty() ? Tr(l.why) : l.protectNote;
                    gfx_.Text(FitPath(note, W(wide), 12), wide, l.level >= Protect::Caution ? P_.warning : P_.textFaint, Small(12));
                } else if (ci == cInst + 1) {
                    gfx_.Text(util::FormatSizeShort(l.bytes), cr, P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
                }
                return;
            }
            const InstalledApp* a = nullptr;
            const WingetUpdate* u = nullptr;
            if (row.kind == RK_APPUPD) {
                if (row.idx >= static_cast<int>(appUpd_.updates.size())) return;
                u = &appUpd_.updates[row.idx];
                int ai = row.idx < static_cast<int>(appUpdApp_.size()) ? appUpdApp_[row.idx] : -1;
                if (ai >= 0 && ai < static_cast<int>(apps_.size())) a = &apps_[ai];
            } else {
                if (row.idx >= static_cast<int>(apps_.size())) return;
                a = &apps_[row.idx];
                int ui = row.idx < static_cast<int>(appUpdOf_.size()) ? appUpdOf_[row.idx] : -1;
                if (ui >= 0) u = &appUpd_.updates[ui];
            }
            if (ci == 0) {
                if (row.kind == RK_APPUPD) {
                    if (u->Updatable())
                        Checkbox(RWH(cr.left, cy - 9, 18, 18), appUpdSel_.count(u->id) > 0, Hovered(HK_ROW, i));
                    else
                        gfx_.Icon(0xE946, RWH(cr.left, cr.top, 18, H(cr)), P_.textFaint, 13);  // info: can't be updated from here
                } else {
                    Checkbox(RWH(cr.left, cy - 9, 18, 18), appChosen_ == row.idx, Hovered(HK_ROW, i));
                }
                std::wstring name = a ? a->e.displayName : u->name + L"  +";
                gfx_.Text(name, R(cr.left + 30, cr.top, cr.right, cr.bottom), P_.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD));
            } else if (ci == 1) {
                // An app Installed apps doesn't list: winget's id says what it is.
                if (a)
                    gfx_.Text(a->e.publisher, cr, P_.textDim, Small(12.5f));
                else if (u)
                    gfx_.Text(u->id + (u->source.empty() ? L"" : L"  ·  " + u->source), cr, P_.textFaint, Small(12));
            } else if (ci == 2) {
                gfx_.Text(u && row.kind == RK_APPUPD ? u->version : a ? a->e.displayVersion : L"", cr, P_.textDim, Small(12.5f));
            } else if (ci == cUpd) {
                if (u) drawBadge(u->available, cr, !u->Updatable());
            } else if (ci == cInst) {
                if (a) gfx_.Text(a->e.installDate ? util::FormatDateLocal(a->e.installDate) : L"", cr, P_.textDim, Small(12.5f));
            } else if (a) {
                gfx_.Text(a->bytes ? util::FormatSizeShort(a->bytes) : L"—", cr, a->measured ? P_.textDim : P_.text,
                          TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
            }
        },
        [&](int i, const RectF& r) {
            int g = rows_[i].idx;
            bool left = g == 0;
            gfx_.Icon(left ? IC_DELETE : g == 2 ? IC_UPDATE : IC_APPS, RWH(r.left + 8, r.top, 22, H(r)), P_.accent2, 13);
            std::wstring title = left ? TrF(L"Left behind by {0}", Shorten(appLeftFor_, 40)) : g == 2 ? Tr(L"Updates from winget") : Tr(L"Installed apps");
            std::wstring note = g == 2   ? Tr(L"check the ones to update; each runs its own installer")
                                : !left ? Tr(L"choose one to uninstall it with its own uninstaller")
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

// Apps > Updates when winget can't be found: what is missing, and the buttons that fix it.
void App::DrawWingetMissing(const RectF& body) {
    struct Btn {
        std::wstring text;
        wchar_t icon;
        int cmd;
    };
    std::vector<Btn> btns;
    std::wstring title, text;
    const WingetDiagnosis& wd = wingetDiag_;
    const std::wstring aliases = Tr(L"Settings > Apps > Advanced app settings > App execution aliases");
    const Btn repair{Tr(L"Repair winget"), IC_REPAIR, CMD_APP_WINGET_REPAIR}, store{Tr(L"Get App Installer"), IC_STORE, CMD_APP_GET_WINGET},
        openAliases{Tr(L"Open App execution aliases"), IC_SETTINGS, CMD_APP_WINGET_ALIASES};
    int rep = wingetRepair_;
    if (wd.CanRepair() && rep == static_cast<int>(WingetRepairOutcome::StillAlias)) {
        title = Tr(L"Turn on the winget.exe alias");
        text = TrF(L"App Installer is registered for your account again, but winget.exe still can't be reached: its App execution alias "
                   L"is off. In {0}, turn on App Installer (winget.exe), then try again.",
                   aliases);
        btns = {openAliases, {Tr(L"Try again"), IC_REFRESH, CMD_APP_UPD_CHECK}};
    } else if (wd.CanRepair() && rep >= 0) {
        title = Tr(L"winget couldn't be repaired");
        auto o = static_cast<WingetRepairOutcome>(rep);
        std::wstring why = o == WingetRepairOutcome::Failed         ? TrF(L"PowerShell said: {0}", wingetRepairDetail_)
                           : o == WingetRepairOutcome::NoPowerShell ? Tr(L"PowerShell couldn't be started.")
                           : o == WingetRepairOutcome::TimedOut     ? Tr(L"PowerShell took longer than 5 minutes and was stopped.")
                           : o == WingetRepairOutcome::NotRun       ? Tr(L"A test setting stands in for winget, so nothing was run.")
                                                                    : Tr(L"PowerShell finished, but App Installer still isn't registered for your account.");
        text = TrF(L"{0} Get App Installer from the Microsoft Store to install it again, or try the repair once more.", why);
        btns = {store, repair};
    } else if (wd.state == WingetState::AliasOff) {
        title = Tr(L"winget's alias is turned off");
        text = TrF(L"App Installer is set up for your account, but the winget.exe App execution alias is off, so winget can't be "
                   L"started. Turn it on in {0}, or repair winget.",
                   aliases);
        btns = {openAliases, repair};
    } else if (wd.state == WingetState::NotReachable) {
        title = Tr(L"winget can't be reached");
        text = Tr(L"App Installer is registered for your account, but winget.exe isn't there yet: a new account before the Microsoft "
                  L"Store finishes setting it up, or its alias is turned off. Repair winget registers App Installer again for you, "
                  L"without administrator permission; nothing is downloaded.");
        btns = {repair, openAliases};
    } else if (wd.state == WingetState::NotRegistered) {
        title = Tr(L"App Installer isn't set up for your account");
        text = Tr(L"App Installer, which brings winget, is on this PC but isn't registered for your account. Repair winget registers it "
                  L"for you, without administrator permission; nothing is downloaded.");
        btns = {repair, store};
    } else {
        title = Tr(L"winget isn't installed");
        text = Tr(L"Updates come through winget, part of Microsoft's App Installer, which isn't on this PC. The Microsoft Store is the "
                  L"easy way to get it; Microsoft's winget release page is for PCs without the Store. Bytopsy doesn't install it "
                  L"for you: get it, then come back.");
        btns = {store, {Tr(L"Download from Microsoft"), IC_GLOBE, CMD_APP_WINGET_DOWNLOAD}};
    }

    float cx = (body.left + body.right) / 2, cy = (body.top + body.bottom) / 2 - 70;
    gfx_.FillCircle(cx, cy - 10, 34, WithAlpha(P_.accent, 0.12f));
    gfx_.Icon(IC_UPDATE, RWH(cx - 30, cy - 40, 60, 60), P_.accent, 26);
    gfx_.Text(title, R(body.left + 24, cy + 34, body.right - 24, cy + 60), P_.text, TS(16, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
    TextStyle s = TS(13, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
    s.wrap = true;
    s.center = false;
    float tw = std::max(200.0f, std::min(580.0f, W(body) - 48));
    float th = gfx_.MeasureHeight(text, tw, s);
    gfx_.Text(text, R(cx - tw / 2, cy + 64, cx + tw / 2, cy + 68 + th), P_.textDim, s);
    float by = cy + 64 + th + 18;
    std::vector<float> ws;
    float total = 0;
    for (auto& b : btns) {
        ws.push_back(ButtonWidth(b.text, b.icon, 140));
        total += ws.back();
    }
    total += 10 * (btns.size() - 1);
    bool stack = total > W(body) - 48;  // long translations: one under the other
    float x = cx - total / 2;
    for (size_t i = 0; i < btns.size(); ++i) {
        if (stack) {
            Button(RWH(cx - ws[i] / 2, by, ws[i], 36), btns[i].icon, btns[i].text, btns[i].cmd, i == 0 ? 1 : 0);
            by += 44;
        } else {
            Button(RWH(x, by, ws[i], 36), btns[i].icon, btns[i].text, btns[i].cmd, i == 0 ? 1 : 0);
            x += ws[i] + 10;
        }
    }
}
