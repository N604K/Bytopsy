#include "ui.h"

#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <unordered_set>

#include "analysis.h"
#include "console.h"
#include "export.h"
#include "fsops.h"
#include "protect.h"
#include "util.h"
#include "version.h"

namespace {

// ---------------------------------------------------------------------------------------------
// Theme (a calm dark palette; everything is painted so it looks the same in any terminal)

struct Theme {
    uint32_t bg = 0x1A1B26;
    uint32_t bgAlt = 0x1E2030;
    uint32_t panel = 0x24283B;
    uint32_t panel2 = 0x2F354D;
    uint32_t border = 0x414868;
    uint32_t fg = 0xC0CAF5;
    uint32_t bright = 0xFFFFFF;
    uint32_t dim = 0x8089B3;
    uint32_t faint = 0x565F89;
    uint32_t accent = 0x7AA2F7;
    uint32_t accent2 = 0xBB9AF7;
    uint32_t dir = 0x7DCFFF;
    uint32_t sel = 0x33467C;
    uint32_t track = 0x2A2F45;
    uint32_t green = 0x9ECE6A;
    uint32_t yellow = 0xE0AF68;
    uint32_t orange = 0xFF9E64;
    uint32_t red = 0xF7768E;
    uint32_t barA = 0x7DCFFF;
    uint32_t barB = 0xBB9AF7;
};
const Theme T;

const wchar_t kSpinner[] = L"\u280B\u2819\u2839\u2838\u283C\u2834\u2826\u2827\u2807\u280F";

enum SortCol { SORT_SIZE, SORT_NAME, SORT_ITEMS, SORT_MTIME };
enum class Screen { Drives, Browser, Results, Help };
enum class RKind { Files, Types, Dups };

struct REntry {
    Node* node = nullptr;
    std::wstring label;
    uint64_t size = 0, disk = 0, count = 0;
    int group = -1;
};

struct ResultsView {
    RKind kind = RKind::Files;
    std::wstring title;
    Node* scope = nullptr;
    std::vector<REntry> entries;
    std::vector<DupGroup> groups;
    int sel = 0, top = 0;
};

struct KV {
    std::wstring k, v;
};

struct Task {
    std::wstring title;
    std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    std::function<std::vector<KV>()> lines;
    std::function<double()> progress;  // 0..1, or <0 when unknown
    std::function<void()> cancel;
    std::function<void()> onDone;
    HANDLE thread = nullptr;
    ULONGLONG start = 0;
    bool cancelRequested = false;
};

struct Prompt {
    bool active = false;
    bool inlineFilter = false;
    std::wstring title, text;
    std::vector<std::wstring> hints;
    size_t cursor = 0;
    std::function<void(const std::wstring&)> onAccept, onChange;
    std::function<void()> onCancel;
};

struct Menu {
    bool active = false;
    std::wstring title;
    std::vector<std::pair<std::wstring, std::wstring>> items;  // label, description
    int sel = 0;
    std::function<void(int)> onPick;
};

struct Confirm {
    bool active = false;
    bool warn = false;  // protected data: wider box in the warning color
    std::wstring title;
    std::vector<std::wstring> lines;
    int button = 0;  // 0 yes, 1 no
    std::function<void()> onYes;
};

struct DeleteDialog {
    bool active = false;
    std::vector<Node*> targets;
    std::vector<std::wstring> notes;  // protection warnings and skipped items
    Protect worst = Protect::None;
    uint64_t total = 0;
    int button = 0;  // 0 recycle, 1 permanent, 2 cancel
};

struct Message {
    bool active = false;
    bool error = false;
    std::wstring title;
    std::vector<std::wstring> lines;
};

struct PlaceRow {
    std::wstring icon, title, sub, path;
    DriveInfo drive;
    bool isDrive = false;
    int action = 0;  // 0 scan path, 1 type a path, 2 load export
};

struct HelpLine {
    std::wstring key, text;  // key empty + text = section title
};

class App {
public:
    explicit App(const AppConfig& cfg) : cfg_(cfg) { useDisk_ = !cfg.apparent; }
    int Run();

private:
    // --- tasks
    void StartTask(std::unique_ptr<Task> t, std::function<void()> work);
    void PollTask();
    void StartScan(const std::wstring& path);
    void StartRescan(std::vector<Node*> dirs, std::vector<std::wstring> afterMsg = {}, bool afterErr = false);
    void StartDelete(std::vector<Node*> targets, bool permanent);
    void StartImport(const std::wstring& file);
    void StartExport(int kind, const std::wstring& file, Node* scope);
    void StartDuplicates(Node* scope, uint64_t minSize);

    // --- model helpers
    uint64_t Val(const Node* n) const { return useDisk_ ? n->disk : n->size; }
    void SetRoot(std::unique_ptr<Node> r, bool readOnly);
    void PostMutation();
    void AdoptChildren(Node* target, Node* fresh);
    void EnsureView();
    void SortNodes(std::vector<Node*>& v) const;
    Node* SelectedNode();
    std::vector<Node*> DeleteTargets();
    void UpdateFree();
    void PruneResults(ResultsView& rv);
    void RebuildDupEntries(ResultsView& rv);
    void SortResults(ResultsView& rv);
    void PushResults(std::unique_ptr<ResultsView> rv);
    void JumpTo(Node* n);
    void LoadPlaces();

    // --- actions
    void Activate();
    void GoUp();
    void SetSort(int col);
    void ToggleMark(Node* n);
    void MarkAll(const std::vector<Node*>& nodes);
    void RequestDelete(bool permanent);
    void RequestRescan();
    void Reveal();
    void OpenSelected();
    void CopyPath();
    void OpenTerminal();
    void BeginFilter();
    void BeginFind();
    void ShowLargest();
    void ShowTypes();
    void BeginDuplicates();
    void BeginExport();
    void BeginTypePath();
    void BeginLoad();
    void ShowDrives();
    void OpenHelp();
    void RequestQuit();
    void Toast(const std::wstring& msg, bool error = false);
    void ShowMessage(const std::wstring& title, std::vector<std::wstring> lines, bool error = false);
    void OpenPrompt(const std::wstring& title, const std::wstring& initial, std::vector<std::wstring> hints,
                    std::function<void(const std::wstring&)> onAccept);

    // --- input
    void HandleEvent(const Event& e);
    void HandleMouse(const Event& e);
    void KeyBrowser(const Event& e);
    void KeyResults(const Event& e);
    void KeyDrives(const Event& e);
    void KeyHelp(const Event& e);
    void KeyPrompt(const Event& e);
    void KeyMenu(const Event& e);
    void KeyConfirm(const Event& e);
    void KeyDelete(const Event& e);
    bool KeyCommon(const Event& e);  // shared actions on the selected item
    bool NavKey(const Event& e, int& sel, int count);

    // --- drawing
    void Render();
    void DrawHeader(const std::wstring& subtitle, const std::wstring& right);
    void DrawHints(const std::vector<std::pair<std::wstring, std::wstring>>& hints);
    void DrawBox(int x, int y, int w, int h, const std::wstring& title, uint32_t border, uint32_t titleColor);
    void DrawBar(int x, int y, int w, double frac, bool gradient = true, uint32_t solid = 0);
    void DrawPathBar(int y, const std::wstring& path, const std::wstring& right);
    void DrawColumns(int y, bool results);
    void DrawNodeRow(int y, Node* n, bool selected, bool zebra, uint64_t total, uint64_t maxv, bool showPath,
                     Node* relTo = nullptr);
    void DrawSummary(int y, Node* scope);
    void DrawInfo(int y, int h, Node* n);
    void DrawBrowser();
    void DrawResults();
    void DrawDrives();
    void DrawHelp();
    void DrawPrompt();
    void DrawMenu();
    void DrawConfirm();
    void DrawDelete();
    void DrawMessage();
    void DrawBusy();
    void DrawEmpty(int y, int h, const std::wstring& text);
    int DrawButton(int x, int y, const std::wstring& label, bool active, uint32_t color);

    struct Cols {
        int size = 4, bar = -1, barW = 0, pct = -1, items = -1, mtime = -1, name = 15;
    };
    Cols ComputeCols(bool graphAllowed) const;
    std::wstring FlagChar(const Node* n, uint32_t& color) const;
    void ScrollFix(int& sel, int& top, int n, int h);

    AppConfig cfg_;
    Terminal term_;
    bool running_ = true;
    Screen screen_ = Screen::Drives;
    Screen helpReturn_ = Screen::Browser;

    std::unique_ptr<Node> root_;
    std::vector<std::unique_ptr<Node>> graveyard_;
    bool readOnly_ = false;
    bool incomplete_ = false;
    uint64_t scanErrors_ = 0;
    double scanSeconds_ = 0;
    uint64_t volTotal_ = 0, volFree_ = 0;
    bool haveVol_ = false;

    Node* cur_ = nullptr;
    std::vector<Node*> view_;
    bool viewDirty_ = true;
    Node* pendingSel_ = nullptr;
    int sel_ = 0, top_ = 0;
    std::wstring filter_;

    bool useDisk_ = true;
    int sortCol_ = SORT_SIZE;
    bool sortDesc_ = true;
    bool dirsFirst_ = false;
    int graphMode_ = 0;  // 0 bar+percent, 1 percent, 2 bar, 3 none
    bool showItems_ = false, showMtime_ = false, hideHidden_ = false, showInfo_ = false;

    std::unordered_set<Node*> marks_;
    std::vector<std::unique_ptr<ResultsView>> rstack_;

    std::vector<PlaceRow> places_;
    int placeSel_ = 0;
    int helpTop_ = 0;

    std::unique_ptr<Task> task_;
    Prompt prompt_;
    Menu menu_;
    Confirm confirm_;
    DeleteDialog del_;
    Message msg_;
    std::wstring toast_;
    bool toastErr_ = false;
    ULONGLONG toastUntil_ = 0;

    std::vector<int> rowMap_;  // screen row -> list index (-1 = none)
    int listH_ = 10;
};

// =============================================================================================
// Run loop

int App::Run() {
    std::wstring err;
    if (!term_.Init(cfg_.mouse, err)) {
        fwprintf(stderr, L"winncdu-cli: %s\n", err.c_str());
        return 1;
    }
    if (!cfg_.importFile.empty())
        StartImport(cfg_.importFile);
    else if (!cfg_.path.empty())
        StartScan(cfg_.path);
    else
        ShowDrives();

    while (running_) {
        PollTask();
        if (!running_) break;
        Render();
        DWORD timeout = INFINITE;
        if (task_) timeout = 80;
        if (!toast_.empty()) {
            ULONGLONG now = GetTickCount64();
            if (now >= toastUntil_) {
                toast_.clear();
                continue;
            }
            timeout = std::min<DWORD>(timeout, static_cast<DWORD>(toastUntil_ - now));
        }
        Event ev;
        if (term_.ReadEvent(ev, timeout)) HandleEvent(ev);
    }
    bool busy = task_ != nullptr;
    if (busy) {
        if (task_->cancel) task_->cancel();
        WaitForSingleObject(task_->thread, 3000);
    }
    term_.Shutdown();
    if (busy) ExitProcess(0);  // never free the tree under a running worker
    return 0;
}

// =============================================================================================
// Tasks

void App::StartTask(std::unique_ptr<Task> t, std::function<void()> work) {
    auto done = t->done;
    t->start = GetTickCount64();
    t->thread = util::StartThread([work = std::move(work), done] {
        work();
        done->store(true);
    });
    task_ = std::move(t);
    if (!task_->thread) PollTask();
}

void App::PollTask() {
    if (!task_ || !task_->done->load()) return;
    if (task_->thread) {
        WaitForSingleObject(task_->thread, INFINITE);
        CloseHandle(task_->thread);
    }
    std::unique_ptr<Task> t = std::move(task_);
    if (t->onDone) t->onDone();
}

namespace {
std::vector<KV> ScanLines(ScanStats& s, ULONGLONG start) {
    double secs = (GetTickCount64() - start) / 1000.0;
    std::vector<KV> l;
    l.push_back({L"Files", util::FormatCount(s.files)});
    l.push_back({L"Folders", util::FormatCount(s.dirs)});
    l.push_back({L"Size", util::FormatSizeShort(s.bytes)});
    if (s.errors) l.push_back({L"Errors", util::FormatCount(s.errors) + L"  (access denied / unreadable)"});
    wchar_t rate[64];
    swprintf(rate, 64, L"  \u00B7  %.0f items/s", secs > 0.2 ? (s.files + s.dirs) / secs : 0.0);
    l.push_back({L"Elapsed", util::FormatDuration(secs) + rate});
    l.push_back({L"", s.GetCurrent()});
    return l;
}
}  // namespace

void App::StartScan(const std::wstring& input) {
    std::wstring err;
    std::wstring path = NormalizeDirPath(input, err);
    if (path.empty()) {
        ShowMessage(L"Cannot scan", {input, err}, true);
        if (!root_) ShowDrives();
        return;
    }
    auto holder = std::make_shared<std::unique_ptr<Node>>(std::make_unique<Node>());
    Node* n = holder->get();
    n->name = path;
    n->flags = NF_DIR;
    ReadNodeMeta(n, path);
    auto stats = std::make_shared<ScanStats>();
    auto t = std::make_unique<Task>();
    t->title = L"Scanning " + path;
    ULONGLONG start = GetTickCount64();
    t->lines = [stats, start] { return ScanLines(*stats, start); };
    t->cancel = [stats] { stats->cancel = true; };
    t->onDone = [this, holder, stats, start] {
        SetRoot(std::move(*holder), false);
        incomplete_ = stats->cancel.load();
        scanErrors_ = stats->errors.load();
        scanSeconds_ = (GetTickCount64() - start) / 1000.0;
        Toast(incomplete_ ? L"Scan aborted \u2014 showing partial results"
                          : L"Scanned " + util::FormatCount(root_->items) + L" items in " +
                                util::FormatDuration(scanSeconds_),
              incomplete_);
    };
    ScanOptions opt = cfg_.scan;
    StartTask(std::move(t), [n, path, opt, stats] {
        ScanDirectory(n, path, opt, *stats);
        RecomputeTotals(n);
    });
}

void App::StartRescan(std::vector<Node*> dirs, std::vector<std::wstring> afterMsg, bool afterErr) {
    struct Job {
        Node* target;
        std::wstring path;
        bool exists = true;
        std::unique_ptr<Node> fresh;
    };
    auto jobs = std::make_shared<std::vector<Job>>();
    for (Node* d : dirs) {
        if (!IsAlive(d)) continue;
        jobs->push_back(Job{d, NodePath(d)});
    }
    auto stats = std::make_shared<ScanStats>();
    auto t = std::make_unique<Task>();
    t->title = dirs.size() == 1 ? L"Rescanning " + NodePath(dirs[0]) : L"Rescanning " + std::to_wstring(dirs.size()) + L" items";
    ULONGLONG start = GetTickCount64();
    t->lines = [stats, start] { return ScanLines(*stats, start); };
    t->cancel = [stats] { stats->cancel = true; };
    t->onDone = [this, jobs, stats, afterMsg, afterErr] {
        bool cancelled = stats->cancel.load();
        for (auto& j : *jobs) {
            if (!IsAlive(j.target)) continue;
            if (!j.exists) {
                if (j.target->parent) graveyard_.push_back(DetachNode(j.target));
                continue;
            }
            if (cancelled) continue;
            if (j.fresh) {
                AdoptChildren(j.target, j.fresh.get());
            } else {
                ReadNodeMeta(j.target, j.path);
                j.target->disk = j.target->size;
                RecomputeUp(j.target->parent);
            }
        }
        PostMutation();
        if (!afterMsg.empty())
            ShowMessage(afterErr ? L"Some items could not be deleted" : L"Done", afterMsg, afterErr);
        else
            Toast(cancelled ? L"Rescan cancelled" : L"Rescan complete", cancelled);
    };
    ScanOptions opt = cfg_.scan;
    StartTask(std::move(t), [jobs, stats, opt] {
        for (auto& j : *jobs) {
            if (stats->cancel) break;
            j.exists = PathExists(j.path);
            if (!j.exists || !j.target->IsDir() || j.target->Has(NF_LINK)) continue;
            j.fresh = std::make_unique<Node>();
            j.fresh->flags = NF_DIR;
            ReadNodeMeta(j.fresh.get(), j.path);
            ScanDirectory(j.fresh.get(), j.path, opt, *stats);
            RecomputeTotals(j.fresh.get());
        }
    });
}

void App::AdoptChildren(Node* t, Node* fresh) {
    for (auto& c : t->children) {
        c->flags |= NF_DELETED;
        graveyard_.push_back(std::move(c));
    }
    t->children = std::move(fresh->children);
    for (auto& c : t->children) c->parent = t;
    t->flags = static_cast<uint16_t>((t->flags & ~(NF_ERROR | NF_SUBERR)) | (fresh->flags & (NF_ERROR | NF_SUBERR)));
    t->attrs = fresh->attrs;
    t->mtime = fresh->mtime;
    RecomputeTotals(t);
    RecomputeUp(t->parent);
}

void App::StartDelete(std::vector<Node*> targets, bool permanent) {
    // Last line of defence: blocked locations are never touched, whatever led here.
    targets.erase(std::remove_if(targets.begin(), targets.end(),
                                 [](Node* n) { return CheckProtection(NodePath(n)).level == Protect::Blocked; }),
                  targets.end());
    if (targets.empty()) return Toast(L"Protected locations are never deleted", true);
    struct State {
        std::vector<std::wstring> paths;
        std::vector<std::wstring> errors;
        std::atomic<size_t> index{0};
        std::atomic<size_t> attempted{0};
        std::atomic<uint64_t> removed{0};
        std::atomic<bool> cancel{false};
    };
    auto st = std::make_shared<State>();
    for (Node* n : targets) st->paths.push_back(NodePath(n));
    st->errors.resize(targets.size());
    auto t = std::make_unique<Task>();
    t->title = permanent ? L"Deleting permanently" : L"Moving to Recycle Bin";
    t->lines = [st, permanent] {
        std::vector<KV> l;
        size_t i = std::min(st->index.load(), st->paths.size() - 1);
        l.push_back({L"Item", std::to_wstring(i + 1) + L" of " + std::to_wstring(st->paths.size())});
        if (permanent) l.push_back({L"Removed", util::FormatCount(st->removed) + L" files and folders"});
        l.push_back({L"", st->paths[i]});
        return l;
    };
    t->progress = [st] { return st->paths.empty() ? 0.0 : static_cast<double>(st->index) / st->paths.size(); };
    t->cancel = [st] { st->cancel = true; };
    t->onDone = [this, st, targets] {
        std::vector<Node*> partial;
        std::vector<std::wstring> errLines;
        uint64_t freed = 0;
        size_t deleted = 0;
        for (size_t i = 0; i < targets.size(); ++i) {
            Node* n = targets[i];
            marks_.erase(n);
            if (!IsAlive(n)) continue;
            if (!PathExists(st->paths[i])) {
                freed += Val(n);
                ++deleted;
                graveyard_.push_back(DetachNode(n));
            } else if (i < st->attempted.load()) {
                partial.push_back(n);
            }
            if (!st->errors[i].empty() && errLines.size() < 12) errLines.push_back(st->errors[i]);
        }
        PostMutation();
        Toast(L"Deleted " + util::FormatCount(deleted) + L" item" + (deleted == 1 ? L"" : L"s") + L" \u00B7 freed " +
                  util::FormatSizeShort(freed),
              deleted == 0);
        if (!partial.empty()) {
            if (errLines.empty()) errLines.push_back(L"Some items are still present.");
            StartRescan(partial, errLines, true);
        } else if (!errLines.empty()) {
            ShowMessage(L"Some items could not be deleted", errLines, true);
        }
    };
    StartTask(std::move(t), [st, permanent] {
        if (!permanent) CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        for (size_t i = 0; i < st->paths.size(); ++i) {
            if (st->cancel) break;
            st->index = i;
            st->attempted = i + 1;
            std::wstring err;
            bool ok = permanent ? DeletePermanently(st->paths[i], st->cancel, st->removed, err)
                                : DeleteToRecycleBin(st->paths[i], err);
            if (!ok) st->errors[i] = err.empty() ? st->paths[i] + L": failed" : err;
        }
        if (!permanent) CoUninitialize();
    });
}

void App::StartImport(const std::wstring& file) {
    auto count = std::make_shared<std::atomic<uint64_t>>(0);
    auto result = std::make_shared<std::unique_ptr<Node>>();
    auto err = std::make_shared<std::wstring>();
    auto t = std::make_unique<Task>();
    t->title = L"Loading export";
    t->lines = [count, file] { return std::vector<KV>{{L"Items", util::FormatCount(*count)}, {L"", file}}; };
    t->onDone = [this, result, err, file] {
        if (!*result) {
            ShowMessage(L"Import failed", {*err}, true);
            if (!root_) ShowDrives();
            return;
        }
        SetRoot(std::move(*result), true);
        incomplete_ = false;
        scanErrors_ = 0;
        Toast(L"Loaded " + util::FormatCount(root_->items) + L" items (read-only)");
    };
    StartTask(std::move(t), [count, result, err, file] { *result = ImportNcduJson(file, *count, *err); });
}

void App::StartExport(int kind, const std::wstring& file, Node* scope) {
    auto count = std::make_shared<std::atomic<uint64_t>>(0);
    auto err = std::make_shared<std::wstring>();
    auto ok = std::make_shared<bool>(false);
    auto t = std::make_unique<Task>();
    t->title = L"Exporting";
    uint64_t total = scope->items + 1;
    t->lines = [count, file] { return std::vector<KV>{{L"Written", util::FormatCount(*count) + L" items"}, {L"", file}}; };
    t->progress = [count, total] { return static_cast<double>(*count) / total; };
    t->onDone = [this, ok, err, file] {
        if (*ok)
            Toast(L"Exported to " + file);
        else
            ShowMessage(L"Export failed", {*err}, true);
    };
    StartTask(std::move(t), [kind, file, scope, count, err, ok] {
        *ok = kind == 0 ? ExportNcduJson(scope, file, *count, *err) : ExportCsv(scope, file, *count, *err);
    });
}

void App::StartDuplicates(Node* scope, uint64_t minSize) {
    auto prog = std::make_shared<DupProgress>();
    auto result = std::make_shared<std::vector<DupGroup>>();
    auto t = std::make_unique<Task>();
    t->title = L"Finding duplicates";
    t->lines = [prog] {
        static const wchar_t* stages[] = {L"Grouping files by size", L"Quick check (first 64 KiB)",
                                          L"Comparing full contents (SHA-256)"};
        std::vector<KV> l;
        l.push_back({L"Stage", stages[std::clamp(prog->stage.load(), 0, 2)]});
        l.push_back({L"Files", util::FormatCount(prog->filesDone) + L" / " + util::FormatCount(prog->filesTotal)});
        l.push_back({L"Read", util::FormatSizeShort(prog->bytesDone) + L" / " + util::FormatSizeShort(prog->bytesTotal)});
        return l;
    };
    t->progress = [prog] {
        uint64_t tot = prog->bytesTotal;
        return tot ? static_cast<double>(prog->bytesDone) / tot : -1.0;
    };
    t->cancel = [prog] { prog->cancel = true; };
    t->onDone = [this, prog, result, scope] {
        if (prog->cancel) {
            Toast(L"Duplicate search cancelled", true);
            return;
        }
        if (result->empty()) {
            Toast(L"No duplicate files found");
            return;
        }
        auto rv = std::make_unique<ResultsView>();
        rv->kind = RKind::Dups;
        rv->scope = scope;
        rv->title = L"Duplicates in " + NodePath(scope);
        rv->groups = std::move(*result);
        RebuildDupEntries(*rv);
        rstack_.clear();
        PushResults(std::move(rv));
    };
    int threads = cfg_.scan.threads;
    StartTask(std::move(t), [scope, minSize, threads, prog, result] {
        *result = FindDuplicates(scope, minSize, threads, *prog);
    });
}

// =============================================================================================
// Model helpers

void App::SetRoot(std::unique_ptr<Node> r, bool readOnly) {
    rstack_.clear();
    marks_.clear();
    view_.clear();
    graveyard_.clear();
    root_ = std::move(r);
    cur_ = root_.get();
    readOnly_ = readOnly;
    filter_.clear();
    sel_ = top_ = 0;
    viewDirty_ = true;
    pendingSel_ = nullptr;
    screen_ = Screen::Browser;
    UpdateFree();
}

void App::PostMutation() {
    while (cur_ && !IsAlive(cur_)) cur_ = cur_->parent;
    if (!cur_) cur_ = root_.get();
    for (auto it = marks_.begin(); it != marks_.end();) {
        if (!IsAlive(*it))
            it = marks_.erase(it);
        else
            ++it;
    }
    for (auto& rv : rstack_) PruneResults(*rv);
    rstack_.erase(std::remove_if(rstack_.begin(), rstack_.end(), [](const auto& rv) { return rv->entries.empty(); }),
                  rstack_.end());
    if (screen_ == Screen::Results && rstack_.empty()) screen_ = Screen::Browser;
    if (pendingSel_ && !IsAlive(pendingSel_)) pendingSel_ = nullptr;
    viewDirty_ = true;
    EnsureView();
    graveyard_.clear();
    UpdateFree();
}

void App::UpdateFree() {
    haveVol_ = false;
    if (root_ && !readOnly_) haveVol_ = QueryFreeSpace(root_->name, volTotal_, volFree_);
}

void App::SortNodes(std::vector<Node*>& v) const {
    auto cmp3 = [](auto a, auto b) { return a < b ? -1 : (a > b ? 1 : 0); };
    std::sort(v.begin(), v.end(), [&](Node* a, Node* b) {
        if (dirsFirst_ && a->IsDir() != b->IsDir()) return a->IsDir();
        int r = 0;
        switch (sortCol_) {
            case SORT_SIZE: r = cmp3(Val(a), Val(b)); break;
            case SORT_ITEMS: r = cmp3(a->items, b->items); break;
            case SORT_MTIME: r = cmp3(a->mtime, b->mtime); break;
            default: r = StrCmpLogicalW(a->name.c_str(), b->name.c_str()); break;
        }
        if (r == 0) return StrCmpLogicalW(a->name.c_str(), b->name.c_str()) < 0;
        return sortDesc_ ? r > 0 : r < 0;
    });
}

void App::EnsureView() {
    if (!cur_) return;
    if (viewDirty_) {
        Node* keep = (sel_ >= 0 && sel_ < static_cast<int>(view_.size())) ? view_[sel_] : nullptr;
        int oldSel = sel_;
        view_.clear();
        std::wstring f = util::ToLower(filter_);
        bool glob = util::HasWildcards(f);
        for (auto& c : cur_->children) {
            if (hideHidden_ && (c->attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            if (!f.empty() && !(glob ? util::GlobMatchI(f, c->name) : util::ContainsI(c->name, f))) continue;
            view_.push_back(c.get());
        }
        SortNodes(view_);
        sel_ = std::min(oldSel, static_cast<int>(view_.size()) - 1);
        if (keep) {
            auto it = std::find(view_.begin(), view_.end(), keep);
            if (it != view_.end()) sel_ = static_cast<int>(it - view_.begin());
        }
        viewDirty_ = false;
    }
    if (pendingSel_) {
        auto it = std::find(view_.begin(), view_.end(), pendingSel_);
        if (it != view_.end()) sel_ = static_cast<int>(it - view_.begin());
        pendingSel_ = nullptr;
    }
    sel_ = std::clamp(sel_, 0, std::max(0, static_cast<int>(view_.size()) - 1));
}

Node* App::SelectedNode() {
    if (screen_ == Screen::Browser) {
        EnsureView();
        return sel_ < static_cast<int>(view_.size()) ? view_[sel_] : nullptr;
    }
    if (screen_ == Screen::Results && !rstack_.empty()) {
        auto& rv = *rstack_.back();
        if (rv.sel >= 0 && rv.sel < static_cast<int>(rv.entries.size())) return rv.entries[rv.sel].node;
    }
    return nullptr;
}

std::vector<Node*> App::DeleteTargets() {
    std::vector<Node*> t;
    if (!marks_.empty()) {
        for (Node* m : marks_)
            if (IsAlive(m)) t.push_back(m);
    } else if (Node* n = SelectedNode()) {
        t.push_back(n);
    }
    std::unordered_set<Node*> set(t.begin(), t.end());
    std::vector<Node*> out;
    for (Node* n : t) {
        if (!n->parent) continue;  // never the scan root
        bool inside = false;
        for (Node* p = n->parent; p && !inside; p = p->parent) inside = set.count(p) > 0;
        if (!inside) out.push_back(n);
    }
    std::sort(out.begin(), out.end(), [this](Node* a, Node* b) { return Val(a) > Val(b); });
    return out;
}

void App::RebuildDupEntries(ResultsView& rv) {
    rv.entries.clear();
    for (size_t g = 0; g < rv.groups.size(); ++g) {
        auto& grp = rv.groups[g];
        REntry h;
        h.group = static_cast<int>(g);
        h.size = grp.size;
        h.count = grp.files.size();
        rv.entries.push_back(h);
        for (Node* f : grp.files) {
            REntry e;
            e.node = f;
            e.group = static_cast<int>(g);
            rv.entries.push_back(e);
        }
    }
}

void App::SortResults(ResultsView& rv) {
    if (rv.kind == RKind::Files) {
        std::stable_sort(rv.entries.begin(), rv.entries.end(),
                         [this](const REntry& a, const REntry& b) { return Val(a.node) > Val(b.node); });
    } else if (rv.kind == RKind::Types) {
        std::stable_sort(rv.entries.begin(), rv.entries.end(), [this](const REntry& a, const REntry& b) {
            return (useDisk_ ? a.disk : a.size) > (useDisk_ ? b.disk : b.size);
        });
    }
}

void App::PruneResults(ResultsView& rv) {
    if (rv.kind == RKind::Files) {
        rv.entries.erase(std::remove_if(rv.entries.begin(), rv.entries.end(),
                                        [](const REntry& e) { return !e.node || !IsAlive(e.node); }),
                         rv.entries.end());
    } else if (rv.kind == RKind::Dups) {
        for (auto& g : rv.groups)
            g.files.erase(std::remove_if(g.files.begin(), g.files.end(), [](Node* n) { return !IsAlive(n); }),
                          g.files.end());
        rv.groups.erase(std::remove_if(rv.groups.begin(), rv.groups.end(),
                                       [](const DupGroup& g) { return g.files.size() < 2; }),
                        rv.groups.end());
        RebuildDupEntries(rv);
    } else {
        rv.entries.clear();
        if (rv.scope && IsAlive(rv.scope)) {
            for (auto& s : ExtensionStats(rv.scope, useDisk_)) {
                REntry e;
                e.label = s.ext;
                e.size = s.size;
                e.disk = s.disk;
                e.count = s.count;
                rv.entries.push_back(e);
            }
        }
    }
    if (rv.scope && !IsAlive(rv.scope)) rv.scope = nullptr;
    rv.sel = std::clamp(rv.sel, 0, std::max(0, static_cast<int>(rv.entries.size()) - 1));
}

void App::PushResults(std::unique_ptr<ResultsView> rv) {
    rstack_.push_back(std::move(rv));
    screen_ = Screen::Results;
}

void App::JumpTo(Node* n) {
    if (!n || !IsAlive(n)) return;
    cur_ = n->parent ? n->parent : n;
    filter_.clear();
    if (hideHidden_ && (n->attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) hideHidden_ = false;
    viewDirty_ = true;
    pendingSel_ = n;
    screen_ = Screen::Browser;
}

void App::LoadPlaces() {
    places_.clear();
    for (auto& d : ListDrives()) {
        PlaceRow r;
        r.isDrive = true;
        r.drive = d;
        r.path = d.root;
        r.title = d.root;
        r.sub = d.label;
        places_.push_back(r);
    }
    std::wstring home = HomeDirectory();
    if (!home.empty()) places_.push_back({L"\u2302", L"Home folder", home, home});
    std::wstring cwd = CurrentDirectory();
    if (!cwd.empty()) places_.push_back({L"\u2022", L"Current folder", cwd, cwd});
    PlaceRow typed{L"\u203A", L"Enter a path\u2026", L"Scan any folder or network share (\\\\server\\share)", L""};
    typed.action = 1;
    places_.push_back(typed);
    PlaceRow load{L"\u00BB", L"Load ncdu export\u2026", L"Browse a JSON file saved with E / -o (also from Linux ncdu)", L""};
    load.action = 2;
    places_.push_back(load);
    placeSel_ = std::clamp(placeSel_, 0, static_cast<int>(places_.size()) - 1);
}

// =============================================================================================
// Actions

void App::Activate() {
    Node* n = SelectedNode();
    if (!n) return;
    if (n->CanEnter()) {
        cur_ = n;
        filter_.clear();
        viewDirty_ = true;
        sel_ = top_ = 0;
        EnsureView();
        sel_ = 0;
    } else {
        showInfo_ = true;
    }
}

void App::GoUp() {
    if (!cur_ || !cur_->parent) return;
    Node* from = cur_;
    cur_ = cur_->parent;
    filter_.clear();
    viewDirty_ = true;
    pendingSel_ = from;
}

void App::SetSort(int col) {
    if (sortCol_ == col)
        sortDesc_ = !sortDesc_;
    else {
        sortCol_ = col;
        sortDesc_ = col != SORT_NAME;
    }
    viewDirty_ = true;
}

void App::ToggleMark(Node* n) {
    if (!n) return;
    if (!marks_.erase(n)) marks_.insert(n);
}

void App::MarkAll(const std::vector<Node*>& nodes) {
    bool all = !nodes.empty();
    for (Node* n : nodes)
        if (!marks_.count(n)) {
            all = false;
            break;
        }
    for (Node* n : nodes) {
        if (all)
            marks_.erase(n);
        else
            marks_.insert(n);
    }
}

void App::RequestDelete(bool permanent) {
    if (readOnly_) return Toast(L"Deletion is not available for imported scans", true);
    if (cfg_.noDelete) return Toast(L"Deletion is disabled (--no-delete)", true);
    auto targets = DeleteTargets();
    if (targets.empty()) return;

    // Protection rules (protect.cpp): blocked items are skipped, important data needs a "yes"
    // first and starts out going to the Recycle Bin, caution items get a note.
    RefreshProtection();
    DeleteDialog d;
    std::vector<std::wstring> blocked, important;
    bool ai = false;
    for (Node* n : targets) {
        ProtectReport rep = CheckProtection(NodePath(n), n);
        if (rep.level == Protect::Blocked) {
            for (auto& h : rep.hits)
                if (h.level == Protect::Blocked) {
                    blocked.push_back(NodePath(n) + L"  \u2014 " + h.name + L": " + h.reason);
                    break;
                }
            continue;
        }
        d.targets.push_back(n);
        d.worst = std::max(d.worst, rep.level);
        for (auto& h : rep.hits) {
            if (h.group == L"AI") ai = true;
            std::wstring line = (h.level == Protect::Important ? L"! " : L"\u00B7 ") + DescribeHit(h);
            auto& list = h.level == Protect::Important ? important : d.notes;
            if (std::find(list.begin(), list.end(), line) == list.end() && list.size() < 6) list.push_back(line);
        }
    }
    for (auto& b : blocked) d.notes.insert(d.notes.begin(), L"Skipped (protected): " + b);
    if (d.targets.empty()) {
        blocked.push_back(L"");
        blocked.push_back(L"WinNCDU never deletes system folders. Rules: " + ProtectRulesFile());
        return ShowMessage(targets.size() == 1 ? L"This location is protected" : L"These locations are protected",
                           blocked, true);
    }
    d.active = true;
    d.button = permanent && d.worst < Protect::Important ? 1 : 0;
    for (Node* n : d.targets) d.total += Val(n);
    if (d.worst < Protect::Important) {
        del_ = std::move(d);
        return;
    }
    for (auto& l : important) d.notes.insert(d.notes.begin(), l);
    confirm_ = Confirm{};
    confirm_.active = true;
    confirm_.warn = true;
    confirm_.button = 1;  // "No" preselected
    confirm_.title = ai ? L"This deletes AI assistant data" : L"This contains data you may not get back";
    confirm_.lines = important;
    if (!important.empty()) {
        // The first hit's reason, so people see what exactly would be lost.
        ProtectReport first = CheckProtection(NodePath(d.targets[0]), d.targets[0]);
        for (auto& h : first.hits)
            if (h.level == Protect::Important) {
                confirm_.lines.push_back(L"");
                confirm_.lines.push_back(h.reason);
                break;
            }
    }
    confirm_.lines.push_back(L"");
    confirm_.lines.push_back(L"Continue? The Recycle Bin will be preselected.");
    auto pending = std::make_shared<DeleteDialog>(std::move(d));
    confirm_.onYes = [this, pending] { del_ = *pending; };
}

void App::RequestRescan() {
    if (readOnly_) return Toast(L"Rescan is not available for imported scans", true);
    if (cur_) StartRescan({cur_});
}

void App::Reveal() {
    Node* n = SelectedNode();
    if (!n && screen_ == Screen::Browser) n = cur_;
    if (!n) return;
    if (RevealInExplorer(NodePath(n)))
        Toast(L"Shown in Explorer: " + n->name);
    else
        Toast(L"Could not open Explorer", true);
}

void App::OpenSelected() {
    Node* n = SelectedNode();
    if (!n) return;
    if (OpenWithDefault(NodePath(n)))
        Toast(L"Opened " + n->name);
    else
        Toast(L"Could not open " + n->name, true);
}

void App::CopyPath() {
    Node* n = SelectedNode();
    if (!n && screen_ == Screen::Browser) n = cur_;
    if (!n) return;
    std::wstring p = NodePath(n);
    if (CopyTextToClipboard(p))
        Toast(L"Copied: " + p);
    else
        Toast(L"Clipboard unavailable", true);
}

void App::OpenTerminal() {
    Node* dir = cur_;
    if (screen_ == Screen::Results) {
        Node* n = SelectedNode();
        if (n) dir = n->IsDir() ? n : n->parent;
    }
    if (!dir) return;
    if (OpenTerminalAt(NodePath(dir)))
        Toast(L"Terminal opened in " + NodePath(dir));
    else
        Toast(L"Could not start a terminal", true);
}

void App::OpenPrompt(const std::wstring& title, const std::wstring& initial, std::vector<std::wstring> hints,
                     std::function<void(const std::wstring&)> onAccept) {
    prompt_ = Prompt{};
    prompt_.active = true;
    prompt_.title = title;
    prompt_.text = initial;
    prompt_.cursor = initial.size();
    prompt_.hints = std::move(hints);
    prompt_.onAccept = std::move(onAccept);
}

void App::BeginFilter() {
    prompt_ = Prompt{};
    prompt_.active = true;
    prompt_.inlineFilter = true;
    prompt_.text = filter_;
    prompt_.cursor = filter_.size();
    prompt_.onChange = [this](const std::wstring& t) {
        filter_ = t;
        viewDirty_ = true;
        sel_ = 0;
    };
    prompt_.onAccept = [this](const std::wstring& t) {
        filter_ = t;
        viewDirty_ = true;
    };
    prompt_.onCancel = [this] {
        filter_.clear();
        viewDirty_ = true;
    };
}

void App::BeginFind() {
    if (!cur_) return;
    Node* scope = cur_;
    OpenPrompt(L"Find in " + NodePath(scope), L"",
               {L"name text   *.iso   >1G   <10M   older:1y   newer:7d",
                L"type:f  type:d   ext:mp4,mkv   empty   \"name with spaces\""},
               [this, scope](const std::wstring& text) {
                   SearchQuery q;
                   std::wstring err;
                   if (!ParseQuery(text, q, err)) return Toast(err, true);
                   auto found = RunSearch(scope, q, useDisk_);
                   if (found.empty()) return Toast(L"No matches for: " + text, true);
                   auto rv = std::make_unique<ResultsView>();
                   rv->kind = RKind::Files;
                   rv->scope = scope;
                   rv->title = L"Find \u201C" + text + L"\u201D";
                   for (Node* n : found) rv->entries.push_back(REntry{n});
                   rstack_.clear();
                   PushResults(std::move(rv));
               });
}

void App::ShowLargest() {
    if (!cur_) return;
    auto files = LargestFiles(cur_, 1000, useDisk_);
    if (files.empty()) return Toast(L"No files here", true);
    auto rv = std::make_unique<ResultsView>();
    rv->kind = RKind::Files;
    rv->scope = cur_;
    rv->title = L"Largest files in " + NodePath(cur_);
    for (Node* n : files) rv->entries.push_back(REntry{n});
    rstack_.clear();
    PushResults(std::move(rv));
}

void App::ShowTypes() {
    if (!cur_) return;
    auto rv = std::make_unique<ResultsView>();
    rv->kind = RKind::Types;
    rv->scope = cur_;
    rv->title = L"File types in " + NodePath(cur_);
    PruneResults(*rv);  // fills the entries
    if (rv->entries.empty()) return Toast(L"No files here", true);
    rstack_.clear();
    PushResults(std::move(rv));
}

void App::BeginDuplicates() {
    if (!cur_) return;
    if (readOnly_) return Toast(L"Duplicate search needs the real files (not available for imports)", true);
    Node* scope = cur_;
    OpenPrompt(L"Find duplicate files in " + NodePath(scope), L"1M",
               {L"Minimum file size to consider (e.g. 100k, 1M, 50M).",
                L"Files are compared by size, then SHA-256 of their content."},
               [this, scope](const std::wstring& text) {
                   uint64_t v = 0;
                   if (!util::ParseSize(text, v)) return Toast(L"Bad size: " + text, true);
                   StartDuplicates(scope, std::max<uint64_t>(v, 1));
               });
}

void App::BeginExport() {
    if (!root_) return;
    Node* scope = screen_ == Screen::Browser && cur_ ? cur_ : root_.get();
    menu_ = Menu{};
    menu_.active = true;
    menu_.title = L"Export";
    menu_.items = {{L"ncdu JSON \u2014 whole scan", L"Reload later with  winncdu -f FILE  or in Linux ncdu"},
                   {L"CSV spreadsheet \u2014 whole scan", L"One row per file/folder, opens in Excel"},
                   {L"CSV spreadsheet \u2014 current folder", NodePath(scope)}};
    menu_.onPick = [this, scope](int i) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[64];
        swprintf(stamp, 64, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::wstring def = JoinPath(CurrentDirectory(), std::wstring(L"winncdu-") + stamp + (i == 0 ? L".json" : L".csv"));
        int kind = i == 0 ? 0 : 1;
        Node* what = i == 2 ? scope : root_.get();
        OpenPrompt(L"Save export as", def, {L"Enter a file name. Existing files are overwritten."},
                   [this, kind, what](const std::wstring& file) {
                       if (!file.empty()) StartExport(kind, file, what);
                   });
    };
}

void App::BeginTypePath() {
    OpenPrompt(L"Scan folder", L"", {L"Example: D:\\Projects   or   \\\\server\\share   (Ctrl+V pastes)"},
               [this](const std::wstring& p) {
                   if (!p.empty()) StartScan(p);
               });
}

void App::BeginLoad() {
    OpenPrompt(L"Load ncdu JSON export", L"", {L"Path of a file created with  winncdu -o  or  ncdu -o"},
               [this](const std::wstring& p) {
                   std::wstring f = p;
                   f.erase(std::remove(f.begin(), f.end(), L'"'), f.end());
                   if (!f.empty()) StartImport(f);
               });
}

void App::ShowDrives() {
    LoadPlaces();
    screen_ = Screen::Drives;
}

void App::OpenHelp() {
    helpReturn_ = screen_;
    helpTop_ = 0;
    screen_ = Screen::Help;
}

void App::RequestQuit() {
    if (!root_) {
        running_ = false;
        return;
    }
    confirm_ = Confirm{};
    confirm_.active = true;
    confirm_.title = L"Quit WinNCDU?";
    confirm_.lines = {L"The current scan results will be discarded."};
    confirm_.onYes = [this] { running_ = false; };
}

void App::Toast(const std::wstring& msg, bool error) {
    toast_ = msg;
    toastErr_ = error;
    toastUntil_ = GetTickCount64() + (error ? 4500 : 3000);
}

void App::ShowMessage(const std::wstring& title, std::vector<std::wstring> lines, bool error) {
    msg_ = Message{};
    msg_.active = true;
    msg_.title = title;
    msg_.lines = std::move(lines);
    msg_.error = error;
}

// =============================================================================================
// Input

void App::HandleEvent(const Event& e) {
    if (e.type == EvType::Resize) {
        term_.Invalidate();
        return;
    }
    if (e.type == EvType::Key && e.ctrl && !e.alt && e.vk == 'C') {
        if (task_ && task_->cancel) task_->cancel();
        running_ = false;
        return;
    }
    if (task_) {
        if (e.type == EvType::Key && e.vk == VK_ESCAPE && task_->cancel && !task_->cancelRequested) {
            task_->cancelRequested = true;
            task_->cancel();
        }
        return;
    }
    if (e.type == EvType::Mouse) return HandleMouse(e);
    if (e.type != EvType::Key) return;
    if (msg_.active) {
        msg_.active = false;
        return;
    }
    if (del_.active) return KeyDelete(e);
    if (confirm_.active) return KeyConfirm(e);
    if (menu_.active) return KeyMenu(e);
    if (prompt_.active) return KeyPrompt(e);
    switch (screen_) {
        case Screen::Browser: return KeyBrowser(e);
        case Screen::Results: return KeyResults(e);
        case Screen::Drives: return KeyDrives(e);
        case Screen::Help: return KeyHelp(e);
    }
}

void App::HandleMouse(const Event& e) {
    if (msg_.active) {
        if (e.mouse != MouseKind::WheelUp && e.mouse != MouseKind::WheelDown) msg_.active = false;
        return;
    }
    if (del_.active || confirm_.active || menu_.active || (prompt_.active && !prompt_.inlineFilter)) return;
    int* sel = nullptr;
    int count = 0;
    if (screen_ == Screen::Browser) {
        sel = &sel_;
        count = static_cast<int>(view_.size());
    } else if (screen_ == Screen::Results && !rstack_.empty()) {
        sel = &rstack_.back()->sel;
        count = static_cast<int>(rstack_.back()->entries.size());
    } else if (screen_ == Screen::Drives) {
        sel = &placeSel_;
        count = static_cast<int>(places_.size());
    } else if (screen_ == Screen::Help) {
        if (e.mouse == MouseKind::WheelUp) helpTop_ = std::max(0, helpTop_ - 3);
        if (e.mouse == MouseKind::WheelDown) helpTop_ += 3;
        if (e.mouse == MouseKind::RightClick) screen_ = helpReturn_;
        return;
    }
    if (!sel) return;
    if (prompt_.active) prompt_.active = false;  // clicking leaves the inline filter
    switch (e.mouse) {
        case MouseKind::WheelUp: *sel = std::max(0, *sel - 3); break;
        case MouseKind::WheelDown: *sel = std::min(std::max(0, count - 1), *sel + 3); break;
        case MouseKind::Click:
        case MouseKind::DoubleClick: {
            if (e.my < 0 || e.my >= static_cast<int>(rowMap_.size())) return;
            int idx = rowMap_[e.my];
            if (idx < 0 || idx >= count) return;
            *sel = idx;
            if (e.mouse == MouseKind::DoubleClick) {
                Event enter;
                enter.type = EvType::Key;
                enter.vk = VK_RETURN;
                HandleEvent(enter);
            }
            break;
        }
        case MouseKind::RightClick: {
            Event back;
            back.type = EvType::Key;
            back.vk = screen_ == Screen::Drives ? VK_ESCAPE : VK_BACK;
            HandleEvent(back);
            break;
        }
    }
}

bool App::NavKey(const Event& e, int& sel, int count) {
    int last = std::max(0, count - 1);
    switch (e.vk) {
        case VK_UP: sel = std::max(0, sel - 1); return true;
        case VK_DOWN: sel = std::min(last, sel + 1); return true;
        case VK_PRIOR: sel = std::max(0, sel - listH_); return true;
        case VK_NEXT: sel = std::min(last, sel + listH_); return true;
        case VK_HOME: sel = 0; return true;
        case VK_END: sel = last; return true;
    }
    if (e.ch == L'k') {
        sel = std::max(0, sel - 1);
        return true;
    }
    if (e.ch == L'j') {
        sel = std::min(last, sel + 1);
        return true;
    }
    return false;
}

bool App::KeyCommon(const Event& e) {
    if (e.vk == VK_DELETE) {
        RequestDelete(e.shift);
        return true;
    }
    if (e.vk == VK_F1) {
        OpenHelp();
        return true;
    }
    switch (e.ch) {
        case L'd': RequestDelete(false); return true;
        case L'o': Reveal(); return true;
        case L'O': OpenSelected(); return true;
        case L'p': CopyPath(); return true;
        case L'b': OpenTerminal(); return true;
        case L'i': showInfo_ = !showInfo_; return true;
        case L'U':
            if (!marks_.empty()) Toast(L"Cleared " + util::FormatCount(marks_.size()) + L" marks");
            marks_.clear();
            return true;
        case L'?': OpenHelp(); return true;
        case L'q': RequestQuit(); return true;
        case L'E': BeginExport(); return true;
        case L'v': ShowDrives(); return true;
    }
    return false;
}

void App::KeyBrowser(const Event& e) {
    EnsureView();
    if (NavKey(e, sel_, static_cast<int>(view_.size()))) return;
    if (e.ctrl && e.vk == 'A') return MarkAll(view_);
    switch (e.vk) {
        case VK_RIGHT:
        case VK_RETURN: return Activate();
        case VK_LEFT:
        case VK_BACK: return GoUp();
        case VK_ESCAPE:
            if (!filter_.empty()) {
                filter_.clear();
                viewDirty_ = true;
            } else if (showInfo_) {
                showInfo_ = false;
            }
            return;
        case VK_TAB:
            if (!rstack_.empty()) screen_ = Screen::Results;
            else Toast(L"No results list yet (try f, L, y or D)", true);
            return;
        case VK_F5: return RequestRescan();
    }
    if (KeyCommon(e)) return;
    switch (e.ch) {
        case L'l': return Activate();
        case L'h':
        case L'u': return GoUp();
        case L's': return SetSort(SORT_SIZE);
        case L'n': return SetSort(SORT_NAME);
        case L'C': return SetSort(SORT_ITEMS);
        case L'M': return SetSort(SORT_MTIME);
        case L't':
            dirsFirst_ = !dirsFirst_;
            viewDirty_ = true;
            return Toast(dirsFirst_ ? L"Folders listed first" : L"Folders mixed with files");
        case L'a':
            useDisk_ = !useDisk_;
            viewDirty_ = true;
            return Toast(useDisk_ ? L"Showing disk usage (allocated size)" : L"Showing apparent size");
        case L'g': graphMode_ = (graphMode_ + 1) % 4; return;
        case L'c': showItems_ = !showItems_; return;
        case L'm': showMtime_ = !showMtime_; return;
        case L'H':
            hideHidden_ = !hideHidden_;
            viewDirty_ = true;
            return Toast(hideHidden_ ? L"Hidden & system items hidden" : L"Showing hidden & system items");
        case L'r': return RequestRescan();
        case L' ':
            if (!view_.empty()) {
                ToggleMark(view_[sel_]);
                sel_ = std::min(sel_ + 1, static_cast<int>(view_.size()) - 1);
            }
            return;
        case L'/': return BeginFilter();
        case L'f': return BeginFind();
        case L'L': return ShowLargest();
        case L'y': return ShowTypes();
        case L'D': return BeginDuplicates();
    }
}

void App::KeyResults(const Event& e) {
    if (rstack_.empty()) {
        screen_ = Screen::Browser;
        return;
    }
    ResultsView& rv = *rstack_.back();
    if (NavKey(e, rv.sel, static_cast<int>(rv.entries.size()))) return;
    auto pop = [this] {
        rstack_.pop_back();
        if (rstack_.empty()) screen_ = Screen::Browser;
    };
    if (e.ctrl && e.vk == 'A') {
        std::vector<Node*> nodes;
        for (auto& en : rv.entries)
            if (en.node) nodes.push_back(en.node);
        return MarkAll(nodes);
    }
    switch (e.vk) {
        case VK_ESCAPE:
        case VK_BACK:
        case VK_LEFT: return pop();
        case VK_TAB: screen_ = Screen::Browser; return;
        case VK_RIGHT:
        case VK_RETURN: {
            if (rv.entries.empty()) return;
            REntry& en = rv.entries[rv.sel];
            if (rv.kind == RKind::Types) {
                if (!rv.scope) return;
                auto files = FilesWithExt(rv.scope, en.label, useDisk_);
                auto sub = std::make_unique<ResultsView>();
                sub->kind = RKind::Files;
                sub->scope = rv.scope;
                sub->title = (en.label.empty() ? std::wstring(L"Files without extension") : L"*." + en.label + L" files") +
                             L" in " + NodePath(rv.scope);
                for (Node* n : files) sub->entries.push_back(REntry{n});
                PushResults(std::move(sub));
            } else if (en.node) {
                JumpTo(en.node);
            }
            return;
        }
    }
    if (KeyCommon(e)) return;
    switch (e.ch) {
        case L' ':
            if (!rv.entries.empty()) {
                ToggleMark(rv.entries[rv.sel].node);
                rv.sel = std::min(rv.sel + 1, static_cast<int>(rv.entries.size()) - 1);
            }
            return;
        case L'a':
            useDisk_ = !useDisk_;
            viewDirty_ = true;
            for (auto& r : rstack_) SortResults(*r);
            return Toast(useDisk_ ? L"Showing disk usage (allocated size)" : L"Showing apparent size");
        case L'A':
            if (rv.kind == RKind::Dups) {
                size_t n = 0;
                uint64_t bytes = 0;
                for (auto& g : rv.groups)
                    for (size_t i = 1; i < g.files.size(); ++i)
                        if (marks_.insert(g.files[i]).second) {
                            ++n;
                            bytes += Val(g.files[i]);
                        }
                Toast(L"Marked " + util::FormatCount(n) + L" extra copies (" + util::FormatSizeShort(bytes) +
                      L") \u2014 the oldest copy in each group is kept");
            }
            return;
        case L'm': showMtime_ = !showMtime_; return;
        case L'h': return pop();
        case L'l': {
            Event enter;
            enter.type = EvType::Key;
            enter.vk = VK_RETURN;
            return KeyResults(enter);
        }
    }
}

void App::KeyDrives(const Event& e) {
    if (NavKey(e, placeSel_, static_cast<int>(places_.size()))) return;
    if (e.vk == VK_RETURN || e.vk == VK_RIGHT || e.ch == L'l') {
        if (places_.empty()) return;
        PlaceRow& r = places_[placeSel_];
        if (r.action == 1) return BeginTypePath();
        if (r.action == 2) return BeginLoad();
        if (r.isDrive && !r.drive.ready) return Toast(r.drive.root + L" is not ready", true);
        auto go = [this, path = r.path] { StartScan(path); };
        if (root_) {
            confirm_ = Confirm{};
            confirm_.active = true;
            confirm_.title = L"Start a new scan?";
            confirm_.lines = {L"Scan " + r.path, L"The current results will be replaced."};
            confirm_.onYes = go;
        } else {
            go();
        }
        return;
    }
    if (e.vk == VK_ESCAPE || e.vk == VK_BACK || e.vk == VK_LEFT) {
        if (root_)
            screen_ = Screen::Browser;
        else if (e.vk == VK_ESCAPE)
            running_ = false;
        return;
    }
    if (e.vk == VK_F1) return OpenHelp();
    switch (e.ch) {
        case L'p':
        case L'/': return BeginTypePath();
        case L'f':
        case L'i': return BeginLoad();
        case L'r':
            LoadPlaces();
            return Toast(L"Drive list refreshed");
        case L'?': return OpenHelp();
        case L'q': return RequestQuit();
    }
}

void App::KeyHelp(const Event& e) {
    switch (e.vk) {
        case VK_UP: helpTop_ = std::max(0, helpTop_ - 1); return;
        case VK_DOWN: ++helpTop_; return;
        case VK_PRIOR: helpTop_ = std::max(0, helpTop_ - listH_); return;
        case VK_NEXT: helpTop_ += listH_; return;
        case VK_HOME: helpTop_ = 0; return;
    }
    if (e.ch == L'j') {
        ++helpTop_;
        return;
    }
    if (e.ch == L'k') {
        helpTop_ = std::max(0, helpTop_ - 1);
        return;
    }
    screen_ = helpReturn_;
}

void App::KeyPrompt(const Event& e) {
    Prompt& p = prompt_;
    bool changed = false;
    switch (e.vk) {
        case VK_RETURN: {
            auto cb = p.onAccept;
            std::wstring text = p.text;
            p.active = false;
            if (cb) cb(text);
            return;
        }
        case VK_ESCAPE: {
            auto cb = p.onCancel;
            p.active = false;
            if (cb) cb();
            return;
        }
        case VK_LEFT:
            if (p.cursor > 0) --p.cursor;
            return;
        case VK_RIGHT:
            if (p.cursor < p.text.size()) ++p.cursor;
            return;
        case VK_HOME: p.cursor = 0; return;
        case VK_END: p.cursor = p.text.size(); return;
        case VK_BACK:
            if (p.cursor > 0) {
                p.text.erase(--p.cursor, 1);
                changed = true;
            }
            break;
        case VK_DELETE:
            if (p.cursor < p.text.size()) {
                p.text.erase(p.cursor, 1);
                changed = true;
            }
            break;
        case VK_UP:
        case VK_DOWN:
            if (p.inlineFilter) {
                // Let the user move through the filtered list while typing.
                auto cb = p.onAccept;
                std::wstring text = p.text;
                p.active = false;
                if (cb) cb(text);
                KeyBrowser(e);
            }
            return;
        default:
            if (e.ctrl && !e.alt && e.vk == 'V') {
                std::wstring clip = ReadClipboardText();
                clip.erase(std::remove_if(clip.begin(), clip.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }),
                           clip.end());
                p.text.insert(p.cursor, clip);
                p.cursor += clip.size();
                changed = true;
            } else if (e.ctrl && !e.alt && e.vk == 'U') {
                p.text.clear();
                p.cursor = 0;
                changed = true;
            } else if (e.ch >= 32 && !(e.ctrl && !e.alt)) {
                p.text.insert(p.cursor, 1, e.ch);
                ++p.cursor;
                changed = true;
            }
    }
    if (changed && p.onChange) p.onChange(p.text);
}

void App::KeyMenu(const Event& e) {
    int n = static_cast<int>(menu_.items.size());
    if (NavKey(e, menu_.sel, n)) return;
    int pick = -1;
    if (e.vk == VK_RETURN) pick = menu_.sel;
    if (e.ch >= L'1' && e.ch <= L'9' && e.ch - L'1' < n) pick = e.ch - L'1';
    if (e.vk == VK_ESCAPE || e.ch == L'q') {
        menu_.active = false;
        return;
    }
    if (pick >= 0) {
        auto cb = menu_.onPick;
        menu_.active = false;
        if (cb) cb(pick);
    }
}

void App::KeyConfirm(const Event& e) {
    if (e.vk == VK_LEFT || e.vk == VK_RIGHT || e.vk == VK_TAB) {
        confirm_.button ^= 1;
        return;
    }
    bool yes = e.ch == L'y' || e.ch == L'Y' || (e.vk == VK_RETURN && confirm_.button == 0);
    bool no = e.ch == L'n' || e.ch == L'N' || e.vk == VK_ESCAPE || (e.vk == VK_RETURN && confirm_.button == 1);
    if (yes) {
        auto cb = confirm_.onYes;
        confirm_.active = false;
        if (cb) cb();
    } else if (no) {
        confirm_.active = false;
    }
}

void App::KeyDelete(const Event& e) {
    if (e.vk == VK_LEFT) {
        del_.button = (del_.button + 2) % 3;
        return;
    }
    if (e.vk == VK_RIGHT || e.vk == VK_TAB) {
        del_.button = (del_.button + 1) % 3;
        return;
    }
    int choice = -1;
    if (e.vk == VK_RETURN) choice = del_.button;
    if (e.ch == L'r' || e.ch == L'R' || e.ch == L'y') choice = 0;
    if (e.ch == L'p' || e.ch == L'P') choice = 1;
    if (e.ch == L'c' || e.ch == L'n' || e.vk == VK_ESCAPE) choice = 2;
    if (choice < 0) return;
    del_.active = false;
    if (choice == 2) return;
    StartDelete(del_.targets, choice == 1);
}

// =============================================================================================
// Drawing

void App::Render() {
    term_.BeginFrame(St(T.fg, T.bg));
    rowMap_.assign(term_.Height(), -1);
    switch (screen_) {
        case Screen::Browser: DrawBrowser(); break;
        case Screen::Results: DrawResults(); break;
        case Screen::Drives: DrawDrives(); break;
        case Screen::Help: DrawHelp(); break;
    }
    if (prompt_.active && !prompt_.inlineFilter) DrawPrompt();
    if (menu_.active) DrawMenu();
    if (confirm_.active) DrawConfirm();
    if (del_.active) DrawDelete();
    if (msg_.active) DrawMessage();
    if (task_) DrawBusy();
    term_.EndFrame();
}

void App::ScrollFix(int& sel, int& top, int n, int h) {
    if (n <= 0) {
        sel = top = 0;
        return;
    }
    sel = std::clamp(sel, 0, n - 1);
    if (sel < top) top = sel;
    if (sel >= top + h) top = sel - h + 1;
    top = std::clamp(top, 0, std::max(0, n - h));
}

void App::DrawHeader(const std::wstring& subtitle, const std::wstring& right) {
    int W = term_.Width();
    term_.Fill(0, 0, W, St(T.fg, T.panel));
    int x = 1;
    x += term_.Put(x, 0, L" \u25C6 WinNCDU ", St(T.bg, T.accent, A_BOLD));
    x += term_.Put(x, 0, L" " WINNCDU_VERSION L" ", St(T.faint, T.panel));
    int rw = StrWidth(right);
    int avail = W - x - rw - 6;
    if (!subtitle.empty() && avail > 4) term_.Put(x + 1, 0, FitMiddle(subtitle, avail, cfg_.ascii), St(T.fg, T.panel, A_BOLD));
    if (rw && rw < W - x - 2) term_.Put(W - rw - 1, 0, right, St(T.dim, T.panel));
}

void App::DrawHints(const std::vector<std::pair<std::wstring, std::wstring>>& hints) {
    int W = term_.Width(), y = term_.Height() - 1;
    term_.Fill(0, y, W, St(T.fg, T.panel));
    int toastW = 0;
    if (!toast_.empty()) {
        std::wstring t = L"  " + toast_ + L"  ";
        int maxW = std::max(10, W * 2 / 3);
        t = FitMiddle(t, maxW, cfg_.ascii);
        toastW = StrWidth(t);
        term_.Put(W - toastW, y, t, St(T.bg, toastErr_ ? T.red : T.green));
    }
    int x = 1;
    int limit = W - toastW - 1;
    for (auto& [k, label] : hints) {
        int need = StrWidth(k) + 2 + StrWidth(label) + 2;
        if (x + need > limit) break;
        x += term_.Put(x, y, L" " + k + L" ", St(T.bright, T.panel2));
        x += term_.Put(x, y, L" " + label + L"  ", St(T.dim, T.panel));
    }
}

void App::DrawBox(int x, int y, int w, int h, const std::wstring& title, uint32_t border, uint32_t titleColor) {
    bool a = cfg_.ascii;
    Style bs = St(border, T.panel);
    for (int r = 0; r < h; ++r) term_.Fill(x, y + r, w, St(T.fg, T.panel));
    term_.Put(x, y, a ? L"+" : L"\u256D", bs);
    term_.Put(x + w - 1, y, a ? L"+" : L"\u256E", bs);
    term_.Put(x, y + h - 1, a ? L"+" : L"\u2570", bs);
    term_.Put(x + w - 1, y + h - 1, a ? L"+" : L"\u256F", bs);
    term_.Fill(x + 1, y, w - 2, bs, a ? L'-' : L'\u2500');
    term_.Fill(x + 1, y + h - 1, w - 2, bs, a ? L'-' : L'\u2500');
    for (int r = 1; r < h - 1; ++r) {
        term_.Put(x, y + r, a ? L"|" : L"\u2502", bs);
        term_.Put(x + w - 1, y + r, a ? L"|" : L"\u2502", bs);
    }
    if (!title.empty()) term_.Put(x + 2, y, L" " + FitMiddle(title, w - 6, a) + L" ", St(titleColor, T.panel, A_BOLD));
    // soft shadow
    if (x + w < term_.Width())
        for (int r = 1; r <= h; ++r) term_.Fill(x + w, y + r, 1, St(T.fg, 0x101118));
    term_.Fill(x + 1, y + h, w, St(T.fg, 0x101118));
}

int App::DrawButton(int x, int y, const std::wstring& label, bool active, uint32_t color) {
    std::wstring t = L"  " + label + L"  ";
    return term_.Put(x, y, t, active ? St(T.bg, color, A_BOLD) : St(T.fg, T.panel2));
}

void App::DrawBar(int x, int y, int w, double frac, bool gradient, uint32_t solid) {
    if (w <= 0) return;
    frac = std::clamp(frac, 0.0, 1.0);
    if (cfg_.ascii) {
        int n = static_cast<int>(std::lround(frac * w));
        term_.Fill(x, y, n, St(gradient ? T.accent : solid, T.track), L'#');
        term_.Fill(x + n, y, w - n, St(T.faint, T.track), L' ');
        return;
    }
    // Whole cells are painted with a background color; a half block refines the end.
    // (Only U+258C is used: finer block glyphs are missing from common console fonts.)
    double cells = frac * w;
    int full = static_cast<int>(cells);
    bool half = (cells - full) >= 0.5 || (frac > 0 && full == 0);
    for (int i = 0; i < w; ++i) {
        uint32_t col = gradient ? Lerp(T.barA, T.barB, w > 1 ? static_cast<double>(i) / (w - 1) : 0) : solid;
        if (i < full)
            term_.Fill(x + i, y, 1, St(col, col));
        else if (i == full && half)
            term_.Put(x + i, y, L"\u258C", St(col, T.track));
        else
            term_.Fill(x + i, y, 1, St(T.fg, T.track));
    }
}

void App::DrawPathBar(int y, const std::wstring& path, const std::wstring& right) {
    int W = term_.Width();
    term_.Fill(0, y, W, St(T.fg, T.bg));
    int rw = StrWidth(right);
    int avail = W - 4 - (rw ? rw + 2 : 0);
    // Breadcrumb: dim separators, bright last segment.
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\' || path[i] == L'/') {
            if (i > start) parts.push_back(path.substr(start, i - start));
            start = i + 1;
        }
    }
    if (path.rfind(L"\\\\", 0) == 0 && !parts.empty()) parts[0] = L"\\\\" + parts[0];
    const std::wstring sep = cfg_.ascii ? L" > " : L" \u203A ";
    auto width = [&](size_t from) {
        int w = from > 0 ? 2 : 0;
        for (size_t i = from; i < parts.size(); ++i) w += StrWidth(parts[i]) + (i + 1 < parts.size() ? 3 : 0);
        return w;
    };
    size_t from = 0;
    while (from + 1 < parts.size() && width(from) > avail) ++from;
    int x = 2;
    term_.Put(0, y, cfg_.ascii ? L" >" : L" \u25B8", St(T.accent, T.bg, A_BOLD));
    if (from > 0) x += term_.Put(x, y, cfg_.ascii ? L"~ " : L"\u2026 ", St(T.faint, T.bg));
    for (size_t i = from; i < parts.size(); ++i) {
        bool last = i + 1 == parts.size();
        int room = W - x - (rw ? rw + 2 : 0) - 1;
        x += term_.Put(x, y, FitMiddle(parts[i], std::max(1, room), cfg_.ascii),
                       last ? St(T.bright, T.bg, A_BOLD) : St(T.dim, T.bg));
        if (!last) x += term_.Put(x, y, sep, St(T.faint, T.bg));
    }
    if (rw) term_.Put(W - rw - 1, y, right, St(T.dim, T.bg));
}

App::Cols App::ComputeCols(bool graphAllowed) const {
    int W = term_.Width();
    Cols c;
    int x = 15;
    bool bar = graphAllowed && (graphMode_ == 0 || graphMode_ == 2) && W >= 60;
    bool pct = graphAllowed && (graphMode_ == 0 || graphMode_ == 1);
    if (bar) {
        c.bar = x;
        c.barW = std::clamp((W - 50) / 4, 8, 24);
        x += c.barW + 1;
    }
    if (pct) {
        c.pct = x;
        x += 8;
    } else if (bar) {
        x += 1;
    }
    if (showItems_ && W >= 70) {
        c.items = x;
        x += 8;
    }
    if (showMtime_ && W >= 80) {
        c.mtime = x;
        x += 18;
    }
    c.name = x;
    return c;
}

std::wstring App::FlagChar(const Node* n, uint32_t& color) const {
    color = T.dim;
    if (n->Has(NF_ERROR)) {
        color = T.red;
        return L"!";
    }
    if (n->Has(NF_SUBERR)) {
        color = T.orange;
        return L".";
    }
    if (n->Has(NF_EXCLUDED)) return L"<";
    if (n->Has(NF_LINK)) {
        color = T.accent2;
        return L"@";
    }
    if (n->Has(NF_HARDLINK)) {
        color = T.accent2;
        return L"H";
    }
    if (n->Has(NF_CLOUD)) {
        color = T.dir;
        return L"~";
    }
    if (n->IsDir() && n->children.empty()) return L"e";
    return L"";
}

void App::DrawColumns(int y, bool results) {
    int W = term_.Width();
    term_.Fill(0, y, W, St(T.faint, T.bg));
    Cols c = ComputeCols(!results);
    auto label = [&](int x, const std::wstring& text, int col, bool right = false, int w = 0) {
        if (x < 0) return;
        bool active = !results && sortCol_ == col;
        std::wstring t = text;
        if (active) t += sortDesc_ ? (cfg_.ascii ? L" v" : L" \u2193") : (cfg_.ascii ? L" ^" : L" \u2191");
        if (right) t = PadLeft(t, w);
        term_.Put(x, y, t, active ? St(T.accent, T.bg, A_BOLD) : St(T.faint, T.bg));
    };
    label(c.size, useDisk_ ? L"Disk" : L"Size", SORT_SIZE, true, 9);
    if (c.bar >= 0) label(c.bar, L"Usage", -1);
    if (c.pct >= 0) label(c.pct, L" Share", -1);
    if (!results && c.items >= 0) label(c.items, L"Items", SORT_ITEMS, true, 6);
    int mt = results ? 15 : c.mtime;
    if (mt >= 0 && (results ? showMtime_ : true)) label(mt, L"Modified", SORT_MTIME);
    int nx = results ? (showMtime_ ? 33 : 15) : c.name;
    label(nx, results ? L"Path" : L"Name", SORT_NAME);
}

void App::DrawNodeRow(int y, Node* n, bool selected, bool zebra, uint64_t total, uint64_t maxv, bool showPath,
                      Node* relTo) {
    int W = term_.Width();
    uint32_t bg = selected ? T.sel : (zebra ? T.bgAlt : T.bg);
    term_.Fill(0, y, W, St(T.fg, bg));
    if (selected) term_.Put(0, y, cfg_.ascii ? L">" : L"\u258C", St(T.accent, bg));
    if (marks_.count(n)) term_.Put(1, y, cfg_.ascii ? L"*" : L"\u25CF", St(T.yellow, bg));
    uint32_t fc;
    std::wstring flag = FlagChar(n, fc);
    if (!flag.empty()) term_.Put(2, y, flag, St(fc, bg, A_BOLD));

    uint64_t v = Val(n);
    bool dim = n->Has(NF_HARDLINK | NF_EXCLUDED | NF_LINK);
    term_.Put(4, y, util::FormatSize(v), St(selected ? T.bright : (dim ? T.faint : T.fg), bg, selected ? A_BOLD : 0));

    int nameX;
    if (showPath) {
        int x = 15;
        if (showMtime_) {
            term_.Put(x, y, util::FormatTime(n->mtime), St(T.dim, bg));
            x += 18;
        }
        nameX = x;
    } else {
        Cols c = ComputeCols(true);
        if (c.bar >= 0) DrawBar(c.bar, y, c.barW, maxv ? static_cast<double>(v) / maxv : 0);
        if (c.pct >= 0) term_.Put(c.pct, y, util::FormatPercent(total ? 100.0 * v / total : 0), St(T.dim, bg));
        if (c.items >= 0)
            term_.Put(c.items, y, n->IsDir() ? util::FormatCountShort(n->items) : std::wstring(L"     \u00B7"),
                      St(T.dim, bg));
        if (c.mtime >= 0) term_.Put(c.mtime, y, util::FormatTime(n->mtime), St(T.dim, bg));
        nameX = c.name;
    }

    bool hidden = (n->attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
    uint32_t nameColor = n->IsDir() ? T.dir : (selected ? T.bright : T.fg);
    if (dim || hidden) nameColor = selected ? T.dim : T.faint;
    uint8_t attr = n->IsDir() ? A_BOLD : 0;
    if (n->Has(NF_LINK)) attr |= A_ITALIC;
    int avail = W - nameX - 1;
    std::wstring suffix = n->IsDir() ? L"\\" : L"";
    if (showPath) {
        std::wstring full;
        if (relTo && IsAncestor(relTo, n)) {
            // Show the path below the search scope; the scope itself is in the path bar.
            for (Node* p = n; p && p != relTo; p = p->parent) full = full.empty() ? p->name : p->name + L"\\" + full;
        } else {
            full = NodePath(n);
        }
        full += suffix;
        std::wstring fit = FitLeft(full, avail, cfg_.ascii);
        // Path dimmed, file name highlighted.
        size_t cut = fit.size() - suffix.size();
        size_t pos = fit.find_last_of(L"\\/", cut > 0 ? cut - 1 : 0);
        if (pos == std::wstring::npos || cut == 0) {
            term_.Put(nameX, y, fit, St(nameColor, bg, attr));
        } else {
            int x = nameX + term_.Put(nameX, y, fit.substr(0, pos + 1), St(selected ? T.fg : T.dim, bg));
            term_.Put(x, y, fit.substr(pos + 1), St(nameColor, bg, attr));
        }
    } else {
        term_.Put(nameX, y, FitMiddle(n->name + suffix, avail, cfg_.ascii), St(nameColor, bg, attr));
    }
}

void App::DrawSummary(int y, Node* scope) {
    int W = term_.Width();
    term_.Fill(0, y, W, St(T.fg, T.bg));
    int x = 1;
    auto item = [&](const std::wstring& k, const std::wstring& v, uint32_t vc = 0) {
        if (x >= W - 4) return;
        x += term_.Put(x, y, k + L" ", St(T.faint, T.bg));
        x += term_.Put(x, y, v, St(vc ? vc : T.fg, T.bg, A_BOLD));
        x += term_.Put(x, y, L"   ", St(T.fg, T.bg));
    };
    if (scope) {
        item(L"Disk usage", util::FormatSizeShort(scope->disk));
        item(L"Apparent", util::FormatSizeShort(scope->size));
        item(L"Items", util::FormatCount(scope->items));
    }
    if (!marks_.empty()) {
        uint64_t sum = 0;
        std::unordered_set<Node*> set(marks_.begin(), marks_.end());
        for (Node* m : marks_) {
            bool inside = false;
            for (Node* p = m->parent; p && !inside; p = p->parent) inside = set.count(p) > 0;
            if (!inside) sum += Val(m);
        }
        item(cfg_.ascii ? L"*" : L"\u25CF", util::FormatCount(marks_.size()) + L" marked \u00B7 " + util::FormatSizeShort(sum),
             T.yellow);
    }
    if (incomplete_) item(L"!", L"incomplete scan", T.orange);
    if (scanErrors_) item(L"!", util::FormatCount(scanErrors_) + L" unreadable", T.red);

    if (haveVol_ && volTotal_) {
        std::wstring txt = util::FormatSizeShort(volFree_) + L" free of " + util::FormatSizeShort(volTotal_);
        int bw = 12;
        int need = StrWidth(txt) + bw + 3;
        if (W - x > need) {
            int bx = W - need;
            double used = 1.0 - static_cast<double>(volFree_) / volTotal_;
            uint32_t col = used > 0.9 ? T.red : (used > 0.75 ? T.yellow : T.green);
            DrawBar(bx, y, bw, used, false, col);
            term_.Put(bx + bw + 1, y, txt, St(T.dim, T.bg));
        }
    } else if (readOnly_) {
        std::wstring txt = L"read-only import";
        if (W - x > StrWidth(txt) + 2) term_.Put(W - StrWidth(txt) - 2, y, txt, St(T.accent2, T.bg, A_ITALIC));
    }
}

void App::DrawInfo(int y, int h, Node* n) {
    int W = term_.Width();
    DrawBox(1, y, W - 3, h, n ? (n->IsDir() ? L"Folder details" : L"File details") : L"Details", T.border, T.accent);
    if (!n) {
        term_.Put(4, y + 2, L"Nothing selected", St(T.dim, T.panel));
        return;
    }
    int lx = 4, vx = 17, vw = W - vx - 5;
    int r = y + 1;
    auto row = [&](const std::wstring& k, const std::wstring& v, uint32_t vc = 0) {
        if (r >= y + h - 1) return;
        term_.Put(lx, r, k, St(T.faint, T.panel));
        term_.Put(vx, r, FitLeft(v, vw, cfg_.ascii), St(vc ? vc : T.fg, T.panel));
        ++r;
    };
    row(L"Name", n->name, T.bright);
    row(L"Path", NodePath(n));
    std::wstring attrs;
    auto add = [&](const wchar_t* s) {
        if (!attrs.empty()) attrs += L", ";
        attrs += s;
    };
    if (n->attrs & FILE_ATTRIBUTE_READONLY) add(L"read-only");
    if (n->attrs & FILE_ATTRIBUTE_HIDDEN) add(L"hidden");
    if (n->attrs & FILE_ATTRIBUTE_SYSTEM) add(L"system");
    if (n->attrs & FILE_ATTRIBUTE_COMPRESSED) add(L"compressed");
    if (n->attrs & FILE_ATTRIBUTE_ENCRYPTED) add(L"encrypted");
    if (n->attrs & FILE_ATTRIBUTE_SPARSE_FILE) add(L"sparse");
    if (n->attrs & FILE_ATTRIBUTE_REPARSE_POINT) add(L"reparse point");
    std::wstring type = n->IsDir() ? L"Folder" : L"File";
    if (!n->IsDir()) {
        std::wstring ext = FileExt(n->name);
        if (!ext.empty()) type += L" (." + ext + L")";
    }
    row(L"Type", type + (attrs.empty() ? L"" : L"  \u00B7  " + attrs));
    row(L"Size", util::FormatSizeShort(n->size) + L"  (" + util::FormatCount(n->size) + L" bytes)    Disk usage  " +
                     util::FormatSizeShort(n->disk) + L"  (" + util::FormatCount(n->disk) + L" bytes)");
    std::wstring line = L"";
    if (n->IsDir()) line = util::FormatCount(n->items) + L" items inside    ";
    row(n->IsDir() ? L"Contents" : L"Modified", n->IsDir() ? line + L"Modified  " + util::FormatTime(n->mtime)
                                                           : util::FormatTime(n->mtime));
    std::wstring note;
    if (n->Has(NF_ERROR)) note = L"Could not be read completely (access denied?) \u2014 sizes may be low.";
    else if (n->Has(NF_SUBERR)) note = L"Some subfolders could not be read \u2014 sizes may be low.";
    else if (n->Has(NF_LINK)) note = L"Symbolic link / junction \u2014 not followed, not counted.";
    else if (n->Has(NF_HARDLINK)) note = L"Hard link to a file counted elsewhere \u2014 not counted again.";
    else if (n->Has(NF_EXCLUDED)) note = L"Excluded by pattern \u2014 not scanned.";
    else if (n->Has(NF_CLOUD)) note = L"Cloud placeholder \u2014 content may not be stored locally.";
    if (!note.empty()) row(L"Note", note, T.yellow);
}

void App::DrawEmpty(int y, int h, const std::wstring& text) {
    int W = term_.Width();
    int w = StrWidth(text);
    term_.Put(std::max(0, (W - w) / 2), y + h / 2, text, St(T.faint, T.bg, A_ITALIC));
}

void App::DrawBrowser() {
    int W = term_.Width(), H = term_.Height();
    EnsureView();
    std::wstring right = std::wstring(useDisk_ ? L"disk usage" : L"apparent size") + L"  \u00B7  " +
                         (dirsFirst_ ? L"folders first  \u00B7  " : L"") + (hideHidden_ ? L"hiding hidden  \u00B7  " : L"") +
                         L"press ? for help";
    DrawHeader(L"", right);

    std::wstring pathRight;
    if (prompt_.active && prompt_.inlineFilter) {
        // Drawn below as an editable chip.
    } else if (!filter_.empty()) {
        pathRight = L"filter: " + filter_ + L"  (Esc clears)";
    } else {
        pathRight = util::FormatCount(view_.size()) + L" entries";
    }
    DrawPathBar(1, cur_ ? NodePath(cur_) : L"", pathRight);
    if (prompt_.active && prompt_.inlineFilter) {
        std::wstring t = L" / " + prompt_.text;
        int cw = std::min(StrWidth(t) + 2, W / 2);
        int x = W - cw - 1;
        term_.Fill(x, 1, cw, St(T.bg, T.yellow));
        term_.Put(x, 1, FitLeft(t, cw - 1, cfg_.ascii), St(T.bg, T.yellow, A_BOLD));
        term_.Put(x + std::min(StrWidth(t), cw - 1), 1, L" ", St(T.yellow, T.bg));
    }
    DrawColumns(2, false);

    int infoH = showInfo_ ? 8 : 0;
    int listTop = 3;
    int listH = std::max(1, H - listTop - 2 - infoH);
    listH_ = listH;
    int n = static_cast<int>(view_.size());
    ScrollFix(sel_, top_, n, listH);
    uint64_t total = cur_ ? Val(cur_) : 0, maxv = 0;
    for (Node* v : view_) maxv = std::max(maxv, Val(v));
    for (int r = 0; r < listH; ++r) {
        int idx = top_ + r;
        int y = listTop + r;
        if (idx >= n) break;
        rowMap_[y] = idx;
        DrawNodeRow(y, view_[idx], idx == sel_, (idx & 1) != 0, total, maxv, false);
    }
    if (n == 0)
        DrawEmpty(listTop, listH, filter_.empty() ? L"This folder is empty" : L"Nothing matches \u201C" + filter_ + L"\u201D");
    else if (n > listH) {
        // Scroll indicator on the right edge.
        int thumb = std::max(1, listH * listH / n);
        int pos = (listH - thumb) * top_ / std::max(1, n - listH);
        for (int r = 0; r < listH; ++r)
            term_.Put(W - 1, listTop + r, r >= pos && r < pos + thumb ? L"\u2590" : L" ",
                      St(r >= pos && r < pos + thumb ? T.border : T.bg, T.bg));
    }
    if (showInfo_) DrawInfo(listTop + listH, infoH, SelectedNode());
    DrawSummary(H - 2, cur_);
    DrawHints({{L"\u2191\u2193", L"move"},
               {L"Enter", L"open"},
               {L"\u2190", L"up"},
               {L"d", L"delete"},
               {L"Space", L"mark"},
               {L"o", L"explorer"},
               {L"/", L"filter"},
               {L"f", L"find"},
               {L"L", L"largest"},
               {L"y", L"types"},
               {L"D", L"dupes"},
               {L"?", L"help"},
               {L"q", L"quit"}});
}

void App::DrawResults() {
    if (rstack_.empty()) return DrawBrowser();
    int W = term_.Width(), H = term_.Height();
    ResultsView& rv = *rstack_.back();
    DrawHeader(rv.title, std::wstring(useDisk_ ? L"disk usage" : L"apparent size") + L"  \u00B7  press ? for help");

    // Summary line.
    std::wstring sum;
    if (rv.kind == RKind::Dups) {
        uint64_t wasted = 0, files = 0;
        for (auto& g : rv.groups) {
            wasted += g.size * (g.files.size() - 1);
            files += g.files.size();
        }
        sum = util::FormatCount(rv.groups.size()) + L" groups  \u00B7  " + util::FormatCount(files) + L" files  \u00B7  " +
              util::FormatSizeShort(wasted) + L" reclaimable";
    } else if (rv.kind == RKind::Types) {
        sum = util::FormatCount(rv.entries.size()) + L" file types";
    } else {
        uint64_t s = 0;
        for (auto& e : rv.entries) s += Val(e.node);
        sum = util::FormatCount(rv.entries.size()) + L" results  \u00B7  " + util::FormatSizeShort(s) + L" total";
    }
    if (rstack_.size() > 1) sum += L"  \u00B7  Esc: back";
    DrawPathBar(1, rv.scope ? NodePath(rv.scope) : L"(removed)", sum);

    int infoH = showInfo_ && rv.kind != RKind::Types ? 8 : 0;
    int listTop = 3, listH = std::max(1, H - listTop - 2 - infoH);
    listH_ = listH;
    int n = static_cast<int>(rv.entries.size());
    ScrollFix(rv.sel, rv.top, n, listH);

    if (rv.kind == RKind::Types) {
        term_.Fill(0, 2, W, St(T.faint, T.bg));
        term_.Put(4, 2, PadLeft(useDisk_ ? L"Disk" : L"Size", 9), St(T.faint, T.bg));
        int barW = std::clamp((W - 50) / 4, 8, 24);
        term_.Put(15, 2, L"Usage", St(T.faint, T.bg));
        term_.Put(15 + barW + 1, 2, L" Share", St(T.faint, T.bg));
        term_.Put(15 + barW + 9, 2, L"   Files", St(T.faint, T.bg));
        term_.Put(15 + barW + 20, 2, L"Type", St(T.faint, T.bg));
        uint64_t total = 0, maxv = 0;
        for (auto& e : rv.entries) {
            uint64_t v = useDisk_ ? e.disk : e.size;
            total += v;
            maxv = std::max(maxv, v);
        }
        for (int r = 0; r < listH; ++r) {
            int idx = rv.top + r, y = listTop + r;
            if (idx >= n) break;
            rowMap_[y] = idx;
            REntry& e = rv.entries[idx];
            bool selected = idx == rv.sel;
            uint32_t bg = selected ? T.sel : ((idx & 1) ? T.bgAlt : T.bg);
            term_.Fill(0, y, W, St(T.fg, bg));
            if (selected) term_.Put(0, y, cfg_.ascii ? L">" : L"\u258C", St(T.accent, bg));
            uint64_t v = useDisk_ ? e.disk : e.size;
            term_.Put(4, y, util::FormatSize(v), St(selected ? T.bright : T.fg, bg, selected ? A_BOLD : 0));
            DrawBar(15, y, barW, maxv ? static_cast<double>(v) / maxv : 0);
            term_.Put(15 + barW + 1, y, util::FormatPercent(total ? 100.0 * v / total : 0), St(T.dim, bg));
            term_.Put(15 + barW + 9, y, PadLeft(util::FormatCount(e.count), 8), St(T.dim, bg));
            term_.Put(15 + barW + 20, y, e.label.empty() ? L"(no extension)" : L"." + e.label,
                      St(e.label.empty() ? T.faint : T.accent2, bg, A_BOLD));
        }
    } else {
        DrawColumns(2, true);
        uint64_t maxv = 0;
        for (int r = 0; r < listH; ++r) {
            int idx = rv.top + r, y = listTop + r;
            if (idx >= n) break;
            rowMap_[y] = idx;
            REntry& e = rv.entries[idx];
            bool selected = idx == rv.sel;
            if (!e.node) {
                // Duplicate group header.
                uint32_t bg = selected ? T.sel : T.panel;
                term_.Fill(0, y, W, St(T.fg, bg));
                if (selected) term_.Put(0, y, cfg_.ascii ? L">" : L"\u258C", St(T.accent, bg));
                std::wstring t = (cfg_.ascii ? L"# " : L"\u25C6 ") + util::FormatCount(e.count) + L" identical copies of " +
                                 util::FormatSizeShort(e.size) + L"   \u00B7   " +
                                 util::FormatSizeShort(e.size * (e.count - 1)) + L" reclaimable";
                term_.Put(4, y, FitMiddle(t, W - 6, cfg_.ascii), St(T.accent2, bg, A_BOLD));
                continue;
            }
            bool zebra = rv.kind == RKind::Dups ? false : (idx & 1) != 0;
            DrawNodeRow(y, e.node, selected, zebra, 0, maxv, true, rv.scope);
        }
    }
    if (n == 0) DrawEmpty(listTop, listH, L"No results left");
    if (infoH) DrawInfo(listTop + listH, infoH, SelectedNode());
    DrawSummary(H - 2, rv.scope);
    if (rv.kind == RKind::Types)
        DrawHints({{L"Enter", L"list files"}, {L"Esc", L"back"}, {L"Tab", L"browser"}, {L"a", L"apparent/disk"}, {L"?", L"help"}});
    else if (rv.kind == RKind::Dups)
        DrawHints({{L"Enter", L"go to"},
                   {L"A", L"mark all but oldest"},
                   {L"Space", L"mark"},
                   {L"d", L"delete"},
                   {L"o", L"explorer"},
                   {L"Esc", L"back"},
                   {L"?", L"help"}});
    else
        DrawHints({{L"Enter", L"go to"},
                   {L"Space", L"mark"},
                   {L"d", L"delete"},
                   {L"o", L"explorer"},
                   {L"O", L"open"},
                   {L"p", L"copy path"},
                   {L"Esc", L"back"},
                   {L"?", L"help"}});
}

void App::DrawDrives() {
    int W = term_.Width(), H = term_.Height();
    DrawHeader(L"Disk usage analyzer", L"press ? for help");
    int y = 2;
    term_.Put(2, y, L"What would you like to scan?", St(T.bright, T.bg, A_BOLD));
    y += 2;
    int listH = H - y - 2;
    listH_ = listH;
    int barW = std::clamp((W - 70) / 2, 10, 30);
    bool placesHeader = false;
    term_.Put(2, y++, L"DRIVES", St(T.faint, T.bg, A_BOLD));
    for (int i = 0; i < static_cast<int>(places_.size()) && y < H - 2; ++i) {
        PlaceRow& p = places_[i];
        if (!p.isDrive && !placesHeader) {
            placesHeader = true;
            ++y;
            if (y >= H - 3) break;
            term_.Put(2, y++, L"PLACES", St(T.faint, T.bg, A_BOLD));
        }
        bool selected = i == placeSel_;
        uint32_t bg = selected ? T.sel : T.bg;
        term_.Fill(1, y, W - 2, St(T.fg, bg));
        if (selected) term_.Put(1, y, cfg_.ascii ? L">" : L"\u258C", St(T.accent, bg));
        rowMap_[y] = i;
        if (p.isDrive) {
            const DriveInfo& d = p.drive;
            term_.Put(4, y, d.root, St(T.bright, bg, A_BOLD));
            term_.Put(9, y, PadTo(d.label.empty() ? DriveTypeName(d.type) : d.label, 18), St(d.ready ? T.fg : T.faint, bg));
            std::wstring kind = (d.fs.empty() ? L"" : d.fs + L" \u00B7 ") + DriveTypeName(d.type);
            term_.Put(28, y, PadTo(kind, 16), St(T.faint, bg));
            if (d.ready && d.total) {
                double used = 1.0 - static_cast<double>(d.free) / d.total;
                uint32_t col = used > 0.9 ? T.red : (used > 0.75 ? T.yellow : T.green);
                if (W >= 70) {
                    DrawBar(45, y, barW, used, false, col);
                    term_.Put(45 + barW + 1, y, util::FormatPercent(used * 100), St(col, bg, A_BOLD));
                    term_.Put(45 + barW + 9, y,
                              util::FormatSizeShort(d.free) + L" free of " + util::FormatSizeShort(d.total),
                              St(T.dim, bg));
                }
            } else {
                term_.Put(45, y, d.label == L"(not responding)" ? L"not responding" : L"not ready", St(T.faint, bg, A_ITALIC));
            }
        } else {
            term_.Put(4, y, p.icon, St(T.accent, bg, A_BOLD));
            term_.Put(6, y, PadTo(p.title, 21), St(T.bright, bg, A_BOLD));
            term_.Put(28, y, FitLeft(p.sub, W - 31, cfg_.ascii), St(T.dim, bg));
        }
        ++y;
    }
    term_.Fill(0, H - 2, W, St(T.fg, T.bg));
    DrawHints({{L"\u2191\u2193", L"select"},
               {L"Enter", L"scan"},
               {L"p", L"type a path"},
               {L"f", L"load export"},
               {L"r", L"refresh"},
               {L"?", L"help"},
               {root_ ? L"Esc" : L"q", root_ ? L"back" : L"quit"}});
}

namespace {
const std::vector<HelpLine>& HelpLines() {
    static const std::vector<HelpLine> lines = {
        {L"", L"Navigation"},
        {L"\u2191 \u2193  k j", L"Move the selection (PgUp/PgDn, Home/End jump)"},
        {L"Enter \u2192 l", L"Open the selected folder (on a file: show details)"},
        {L"\u2190 Bksp h u", L"Go to the parent folder"},
        {L"Mouse", L"Click selects \u00B7 double-click opens \u00B7 right-click goes back \u00B7 wheel scrolls"},
        {L"", L"Locate & open"},
        {L"o", L"Show the item in Explorer (locate it)"},
        {L"O", L"Open with the default application"},
        {L"p", L"Copy the full path to the clipboard"},
        {L"b", L"Open a terminal in the current folder"},
        {L"", L"Delete"},
        {L"Space", L"Mark / unmark item (marks work across folders and result lists)"},
        {L"Ctrl+A", L"Mark / unmark everything in the list"},
        {L"U", L"Clear all marks"},
        {L"d  Del", L"Delete the marked items (or the selected one) \u2014 Recycle Bin or permanent"},
        {L"Shift+Del", L"Delete, with \u201Cpermanently\u201D preselected"},
        {L"", L"Find & analyze"},
        {L"/", L"Filter the current folder as you type (wildcards allowed, Esc clears)"},
        {L"f", L"Find in the current folder tree:  *.iso  >1G  older:1y  type:d  ext:mp4,mkv  empty"},
        {L"L", L"Largest files in the current folder tree"},
        {L"y", L"Breakdown by file type (extension); Enter lists the files"},
        {L"D", L"Find duplicate files (size + SHA-256); A marks every copy except the oldest"},
        {L"Tab", L"Switch between the browser and the last results list"},
        {L"", L"View"},
        {L"a", L"Toggle disk usage (allocated) / apparent size"},
        {L"s n C M", L"Sort by size / name / item count / modified time (again = reverse)"},
        {L"t", L"Folders before files"},
        {L"g", L"Cycle usage bar / percentage display"},
        {L"c  m", L"Toggle item count / modified time columns"},
        {L"H", L"Hide / show hidden and system items"},
        {L"i", L"Toggle the details panel"},
        {L"", L"Other"},
        {L"r  F5", L"Rescan the current folder"},
        {L"E", L"Export: ncdu JSON (reload with -f) or CSV"},
        {L"v", L"Choose another drive or folder"},
        {L"q", L"Quit"},
        {L"", L"Flags"},
        {L"!", L"Read error (access denied) \u2014 run as administrator for full results"},
        {L".", L"A read error occurred somewhere inside this folder"},
        {L"@", L"Symbolic link / junction \u2014 not followed, so nothing is counted twice"},
        {L"H", L"Hard link already counted elsewhere (enable detection with -H)"},
        {L"~", L"Cloud placeholder (e.g. OneDrive online-only file)"},
        {L"<", L"Excluded by --exclude pattern"},
        {L"e", L"Empty folder"},
    };
    return lines;
}
}  // namespace

void App::DrawHelp() {
    int W = term_.Width(), H = term_.Height();
    DrawHeader(L"Help", L"any key to close");
    const auto& lines = HelpLines();
    int listTop = 2, listH = H - 3;
    listH_ = listH;
    int n = static_cast<int>(lines.size());
    helpTop_ = std::clamp(helpTop_, 0, std::max(0, n - listH));
    for (int r = 0; r < listH; ++r) {
        int idx = helpTop_ + r, y = listTop + r;
        if (idx >= n) break;
        const HelpLine& l = lines[idx];
        if (l.key.empty()) {
            term_.Put(3, y, l.text, St(T.accent, T.bg, A_BOLD));
            term_.Fill(4 + StrWidth(l.text), y, std::min(40, W - 8 - StrWidth(l.text)), St(T.panel2, T.bg),
                       cfg_.ascii ? L'-' : L'\u2500');
        } else {
            term_.Put(5, y, PadTo(L" " + l.key, 13), St(T.bright, T.panel2));
            term_.Put(20, y, FitMiddle(l.text, W - 22, cfg_.ascii), St(T.fg, T.bg));
        }
    }
    DrawHints({{L"\u2191\u2193", L"scroll"}, {L"Esc", L"close"}});
    std::wstring credit = L"WinNCDU " WINNCDU_VERSION L" \u00B7 inspired by ncdu";
    if (toast_.empty()) term_.Put(W - StrWidth(credit) - 2, H - 1, credit, St(T.faint, T.panel));
}

void App::DrawPrompt() {
    int W = term_.Width(), H = term_.Height();
    int w = std::min(W - 4, 84);
    int h = 6 + static_cast<int>(prompt_.hints.size());
    int x = (W - w) / 2, y = std::max(1, (H - h) / 2 - 2);
    DrawBox(x, y, w, h, prompt_.title, T.accent, T.accent);
    int fx = x + 3, fw = w - 6;
    term_.Fill(fx, y + 2, fw, St(T.bright, T.bg));
    // Scroll the text horizontally so the cursor stays visible.
    std::wstring before = prompt_.text.substr(0, prompt_.cursor);
    std::wstring after = prompt_.text.substr(prompt_.cursor);
    std::wstring shownBefore = FitLeft(before, fw - 2, cfg_.ascii);
    int cx = fx + 1 + term_.Put(fx + 1, y + 2, shownBefore, St(T.bright, T.bg));
    std::wstring curCh = after.empty() ? L" " : after.substr(0, 1);
    term_.Put(cx, y + 2, curCh, St(T.bg, T.accent));
    term_.Put(cx + 1, y + 2, after.size() > 1 ? after.substr(1) : L"", St(T.bright, T.bg), fx + fw - cx - 1);
    for (size_t i = 0; i < prompt_.hints.size(); ++i)
        term_.Put(fx, y + 4 + static_cast<int>(i), FitMiddle(prompt_.hints[i], fw, cfg_.ascii), St(T.dim, T.panel));
    std::wstring keys = L"Enter ok   Esc cancel";
    term_.Put(x + w - StrWidth(keys) - 3, y + h - 1, L" " + keys + L" ", St(T.faint, T.panel));
}

void App::DrawMenu() {
    int W = term_.Width(), H = term_.Height();
    int w = std::min(W - 4, 76);
    int h = static_cast<int>(menu_.items.size()) * 2 + 3;
    int x = (W - w) / 2, y = std::max(1, (H - h) / 2);
    DrawBox(x, y, w, h, menu_.title, T.accent, T.accent);
    for (size_t i = 0; i < menu_.items.size(); ++i) {
        int ry = y + 1 + static_cast<int>(i) * 2;
        bool selected = static_cast<int>(i) == menu_.sel;
        uint32_t bg = selected ? T.sel : T.panel;
        term_.Fill(x + 1, ry, w - 2, St(T.fg, bg));
        term_.Fill(x + 1, ry + 1, w - 2, St(T.fg, bg));
        rowMap_[ry] = static_cast<int>(i);
        term_.Put(x + 3, ry, std::to_wstring(i + 1), St(T.accent, bg, A_BOLD));
        term_.Put(x + 6, ry, FitMiddle(menu_.items[i].first, w - 9, cfg_.ascii), St(T.bright, bg, A_BOLD));
        term_.Put(x + 6, ry + 1, FitLeft(menu_.items[i].second, w - 9, cfg_.ascii), St(T.dim, bg));
    }
}

void App::DrawConfirm() {
    int W = term_.Width(), H = term_.Height();
    int w = std::min(W - 4, confirm_.warn ? 96 : 64);
    int h = 6 + static_cast<int>(confirm_.lines.size());
    int x = (W - w) / 2, y = std::max(1, (H - h) / 2);
    uint32_t tone = confirm_.warn ? T.orange : T.accent;
    DrawBox(x, y, w, h, confirm_.title, tone, tone);
    for (size_t i = 0; i < confirm_.lines.size(); ++i) {
        const std::wstring& l = confirm_.lines[i];
        bool bang = confirm_.warn && l.rfind(L"! ", 0) == 0;
        term_.Put(x + 3, y + 2 + static_cast<int>(i), FitLeft(l, w - 6, cfg_.ascii),
                  St(bang ? T.orange : T.fg, T.panel, bang ? A_BOLD : 0));
    }
    int by = y + h - 2;
    int bx = x + w - 3 - 20;
    bx += DrawButton(bx, by, L"Yes", confirm_.button == 0, tone) + 2;
    DrawButton(bx, by, L"No", confirm_.button == 1, tone);
}

void App::DrawDelete() {
    int W = term_.Width(), H = term_.Height();
    int notes = std::min<int>(static_cast<int>(del_.notes.size()), std::max(0, H - 20));
    int shown = std::min<int>(static_cast<int>(del_.targets.size()), std::max(1, std::min(8, H - 14 - notes)));
    int w = std::min(W - 4, 96);
    int extra = static_cast<int>(del_.targets.size()) > shown ? 1 : 0;
    int h = 9 + shown + extra + (notes ? notes + 1 : 0);
    int x = (W - w) / 2, y = std::max(1, (H - h) / 2);
    DrawBox(x, y, w, h, L"Delete", T.red, T.red);
    size_t n = del_.targets.size();
    std::wstring q = L"Delete " + (n == 1 ? std::wstring(L"this item") : util::FormatCount(n) + L" items") + L"  \u00B7  " +
                     util::FormatSizeShort(del_.total);
    term_.Put(x + 3, y + 2, q, St(T.bright, T.panel, A_BOLD));
    for (int i = 0; i < shown; ++i) {
        Node* t = del_.targets[i];
        int ry = y + 4 + i;
        term_.Put(x + 3, ry, util::FormatSize(Val(t)), St(T.dim, T.panel));
        std::wstring p = NodePath(t) + (t->IsDir() ? L"\\" : L"");
        term_.Put(x + 14, ry, FitLeft(p, w - 17, cfg_.ascii), St(t->IsDir() ? T.dir : T.fg, T.panel, t->IsDir() ? A_BOLD : 0));
    }
    if (static_cast<int>(n) > shown)
        term_.Put(x + 14, y + 4 + shown, L"\u2026 and " + util::FormatCount(n - shown) + L" more", St(T.faint, T.panel, A_ITALIC));
    for (int i = 0; i < notes; ++i) {
        const std::wstring& l = del_.notes[i];
        bool strong = l.rfind(L"! ", 0) == 0 || l.rfind(L"Skipped", 0) == 0;
        term_.Put(x + 3, y + 5 + shown + extra + i, FitLeft(l, w - 6, cfg_.ascii),
                  St(strong ? T.orange : T.yellow, T.panel, strong ? A_BOLD : 0));
    }
    int by = y + h - 3;
    std::wstring note = del_.button == 1 ? L"Permanent deletion cannot be undone!" : L"Items can be restored from the Recycle Bin.";
    term_.Put(x + 3, by - 1, note, St(del_.button == 1 ? T.red : T.dim, T.panel, del_.button == 1 ? A_BOLD : 0));
    int bx = x + 3;
    bx += DrawButton(bx, by + 1, L"R  Recycle Bin", del_.button == 0, T.green) + 2;
    bx += DrawButton(bx, by + 1, L"P  Delete permanently", del_.button == 1, T.red) + 2;
    DrawButton(bx, by + 1, L"C  Cancel", del_.button == 2, T.accent);
}

void App::DrawMessage() {
    int W = term_.Width(), H = term_.Height();
    int w = std::min(W - 4, 90);
    int lines = std::min<int>(static_cast<int>(msg_.lines.size()), H - 8);
    int h = lines + 5;
    int x = (W - w) / 2, y = std::max(1, (H - h) / 2);
    uint32_t c = msg_.error ? T.red : T.accent;
    DrawBox(x, y, w, h, msg_.title, c, c);
    for (int i = 0; i < lines; ++i)
        term_.Put(x + 3, y + 2 + i, FitMiddle(msg_.lines[i], w - 6, cfg_.ascii), St(T.fg, T.panel));
    std::wstring k = L" any key to continue ";
    term_.Put(x + w - StrWidth(k) - 3, y + h - 1, k, St(T.faint, T.panel));
}

void App::DrawBusy() {
    int W = term_.Width(), H = term_.Height();
    auto lines = task_->lines ? task_->lines() : std::vector<KV>{};
    double prog = task_->progress ? task_->progress() : -1.0;
    int w = std::min(W - 4, 80);
    int h = static_cast<int>(lines.size()) + 5 + (prog >= 0 ? 2 : 0);
    int x = (W - w) / 2, y = std::max(1, (H - h) / 2);
    ULONGLONG ms = GetTickCount64() - task_->start;
    wchar_t spin[2] = {cfg_.ascii ? L"|/-\\"[(ms / 120) % 4] : kSpinner[(ms / 90) % 10], 0};
    DrawBox(x, y, w, h, L"   " + task_->title, T.accent, T.accent);
    term_.Put(x + 3, y, spin, St(T.yellow, T.panel));
    int r = y + 2;
    for (auto& kv : lines) {
        if (kv.k.empty()) {
            term_.Put(x + 3, r, FitLeft(kv.v, w - 6, cfg_.ascii), St(T.faint, T.panel, A_ITALIC));
        } else {
            term_.Put(x + 3, r, kv.k, St(T.dim, T.panel));
            term_.Put(x + 15, r, FitMiddle(kv.v, w - 18, cfg_.ascii), St(T.bright, T.panel, A_BOLD));
        }
        ++r;
    }
    if (prog >= 0) {
        ++r;
        int bw = w - 14;
        DrawBar(x + 3, r, bw, prog);
        term_.Put(x + 4 + bw, r, util::FormatPercent(prog * 100), St(T.fg, T.panel, A_BOLD));
    }
    std::wstring k = task_->cancel ? (task_->cancelRequested ? L" cancelling\u2026 " : L" Esc  cancel ") : L" please wait ";
    term_.Put(x + w - StrWidth(k) - 3, y + h - 1, k, St(task_->cancelRequested ? T.orange : T.faint, T.panel));
}

}  // namespace

int RunApp(const AppConfig& cfg) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int rc;
    {
        App app(cfg);
        rc = app.Run();
    }
    CoUninitialize();
    return rc;
}
