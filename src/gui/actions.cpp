#include "app.h"

#include <shlwapi.h>

#include <algorithm>
#include <mutex>
#include <thread>

#include "../export.h"
#include "../util.h"
#include "../version.h"

// =============================================================================================
// Model

void App::SetRoot(std::unique_ptr<Node> r, bool readOnly) {
    sel_.clear();
    expanded_.clear();
    backStack_.clear();
    fwdStack_.clear();
    largest_.clear();
    searchRes_.clear();
    oldRes_.clear();
    emptyRes_.clear();
    typeFiles_.clear();
    dups_.clear();
    dupsDone_ = false;
    devJunk_.clear();
    devJunkDone_ = false;
    types_.clear();
    diff_.clear();
    tm_.clear();
    tmShields_.clear();
    tmRoot_ = nullptr;
    rows_.clear();
    graveyard_.clear();
    root_ = std::move(r);
    RefreshProtection();  // picks up edits to protect.txt
    badges_.Clear();
    readOnly_ = readOnly;
    browseDir_ = root_.get();
    for (auto& ls : lists_) ls = ListState{};
    rowsDirty_ = true;
    tmDirty_ = true;
    haveVol_ = !readOnly && QueryFreeSpace(root_->name, volTotal_, volFree_);
    view_ = View::Browse;
}

void App::PostMutation() {
    while (browseDir_ && !IsAlive(browseDir_)) browseDir_ = browseDir_->parent;
    if (!browseDir_) browseDir_ = root_.get();
    auto dead = [](Node* n) { return !n || !IsAlive(n); };
    for (auto it = sel_.begin(); it != sel_.end();) it = IsAlive(*it) ? std::next(it) : sel_.erase(it);
    for (auto it = expanded_.begin(); it != expanded_.end();) it = IsAlive(*it) ? std::next(it) : expanded_.erase(it);
    for (auto* v : {&largest_, &searchRes_, &oldRes_, &emptyRes_, &typeFiles_, &backStack_, &fwdStack_})
        v->erase(std::remove_if(v->begin(), v->end(), dead), v->end());
    for (auto& g : dups_) g.files.erase(std::remove_if(g.files.begin(), g.files.end(), dead), g.files.end());
    dups_.erase(std::remove_if(dups_.begin(), dups_.end(), [](const DupGroup& g) { return g.files.size() < 2; }), dups_.end());
    for (auto& d : diff_)
        if (d.node && !IsAlive(d.node)) d.node = nullptr;
    if (!types_.empty()) RebuildTypes();
    if (view_ == View::Empty) RebuildEmpty();
    // Project groups hold nodes and last-change dates: find them again (now, or when shown next).
    if (view_ == View::DevJunk) {
        RebuildDevJunk();
    } else {
        devJunk_.clear();
        devJunkDone_ = false;
    }
    tm_.clear();
    tmShields_.clear();
    tmRoot_ = nullptr;
    tmHover_ = -1;
    badges_.Clear();
    tmDirty_ = true;
    rowsDirty_ = true;
    graveyard_.clear();
    if (root_ && !readOnly_) haveVol_ = QueryFreeSpace(root_->name, volTotal_, volFree_);
    drives_ = ListDrives();
}

std::vector<Node*> App::SortedChildren(Node* dir) {
    std::vector<Node*> v;
    v.reserve(dir->children.size());
    for (auto& c : dir->children) {
        if (!settings_.showHidden && (c->attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        v.push_back(c.get());
    }
    auto cmp3 = [](auto a, auto b) { return a < b ? -1 : (a > b ? 1 : 0); };
    std::sort(v.begin(), v.end(), [&](Node* a, Node* b) {
        int r = 0;
        switch (sortKey_) {
            case SK_SIZE: r = cmp3(Val(a), Val(b)); break;
            case SK_ITEMS: r = cmp3(a->items, b->items); break;
            case SK_MTIME: r = cmp3(a->mtime, b->mtime); break;
            default: r = StrCmpLogicalW(a->name.c_str(), b->name.c_str()); break;
        }
        if (r == 0) return StrCmpLogicalW(a->name.c_str(), b->name.c_str()) < 0;
        return sortDesc_ ? r > 0 : r < 0;
    });
    return v;
}

void App::BuildRows() {
    Node* keepFocus = nullptr;
    ListState& ls = lists_[static_cast<int>(view_)];
    if (ls.focus >= 0 && ls.focus < static_cast<int>(rows_.size())) keepFocus = rows_[ls.focus].node;
    rows_.clear();
    auto addNodes = [&](const std::vector<Node*>& v) {
        for (Node* n : v) rows_.push_back({n, 0, RK_NODE, 0});
    };
    switch (view_) {
        case View::Browse: {
            if (!browseDir_) break;
            // Depth-first flatten of the expanded tree.
            std::vector<std::pair<Node*, int>> stack;
            auto kids = SortedChildren(browseDir_);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back({*it, 0});
            while (!stack.empty()) {
                auto [n, d] = stack.back();
                stack.pop_back();
                rows_.push_back({n, d, RK_NODE, 0});
                if (expanded_.count(n) && n->CanEnter()) {
                    auto sub = SortedChildren(n);
                    for (auto it = sub.rbegin(); it != sub.rend(); ++it) stack.push_back({*it, d + 1});
                }
            }
            break;
        }
        case View::Largest: addNodes(largest_); break;
        case View::Search: addNodes(searchRes_); break;
        case View::Old: addNodes(oldRes_); break;
        case View::Empty: addNodes(emptyRes_); break;
        case View::TypeFiles: addNodes(typeFiles_); break;
        case View::Types:
            for (size_t i = 0; i < types_.size(); ++i) rows_.push_back({nullptr, 0, RK_TYPE, static_cast<int>(i)});
            break;
        case View::Duplicates:
            for (size_t g = 0; g < dups_.size(); ++g) {
                rows_.push_back({nullptr, 0, RK_GROUP, static_cast<int>(g)});
                for (Node* f : dups_[g].files) rows_.push_back({f, 1, RK_NODE, static_cast<int>(g)});
            }
            break;
        case View::Compare:
            for (size_t i = 0; i < diff_.size(); ++i) rows_.push_back({diff_[i].node, 0, RK_DIFF, static_cast<int>(i)});
            break;
        case View::DevJunk:
            for (size_t p = 0; p < devJunk_.size(); ++p) {
                rows_.push_back({nullptr, 0, RK_GROUP, static_cast<int>(p)});
                for (auto& a : devJunk_[p].artifacts) rows_.push_back({a.node, 1, RK_NODE, static_cast<int>(p)});
            }
            break;
        case View::AiModels:
            for (size_t s = 0; s < ai_.size(); ++s) {
                rows_.push_back({nullptr, 0, RK_GROUP, static_cast<int>(s)});
                for (size_t i = 0; i < ai_[s].items.size(); ++i)
                    rows_.push_back({nullptr, ai_[s].items[i].parent >= 0 ? 1 : 0, RK_AI, AiRowIdx(s, i)});
            }
            break;
        case View::CleanReview:
            if (reviewCat_ >= 0 && reviewCat_ < static_cast<int>(cleanup_.size()))
                for (size_t i = 0; i < cleanup_[reviewCat_].entries.size(); ++i) rows_.push_back({nullptr, 0, RK_CLEAN, static_cast<int>(i)});
            break;
        default: break;
    }
    if (keepFocus) {
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].node == keepFocus) {
                ls.focus = static_cast<int>(i);
                break;
            }
    }
    if (ls.focus >= static_cast<int>(rows_.size())) ls.focus = static_cast<int>(rows_.size()) - 1;
    rowsDirty_ = false;
}

std::vector<Node*> App::SelectedNodes() {
    std::vector<Node*> v;
    for (Node* n : sel_)
        if (IsAlive(n)) v.push_back(n);
    return v;
}

std::vector<Node*> App::NormalizeTargets(std::vector<Node*> t) {
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

Node* App::FindNodeByPath(const std::wstring& path) {
    if (!root_) return nullptr;
    std::wstring lp = util::ToLower(path), lr = util::ToLower(root_->name);
    if (lp.rfind(lr, 0) != 0) return nullptr;
    std::wstring rest = path.substr(root_->name.size());
    Node* cur = root_.get();
    size_t pos = 0;
    while (pos < rest.size()) {
        while (pos < rest.size() && rest[pos] == L'\\') ++pos;
        size_t end = rest.find(L'\\', pos);
        std::wstring seg = rest.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        if (seg.empty()) break;
        Node* next = nullptr;
        for (auto& c : cur->children)
            if (_wcsicmp(c->name.c_str(), seg.c_str()) == 0) {
                next = c.get();
                break;
            }
        if (!next) return nullptr;
        cur = next;
        if (end == std::wstring::npos) break;
        pos = end + 1;
    }
    return cur;
}

Node* App::FocusNode() {
    ListState& ls = lists_[static_cast<int>(view_)];
    if (ls.focus >= 0 && ls.focus < static_cast<int>(rows_.size())) return rows_[ls.focus].node;
    return nullptr;
}

void App::SelectRow(int row, bool ctrl, bool shift) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    ListState& ls = lists_[static_cast<int>(view_)];
    if (shift && ls.anchor >= 0 && ls.anchor < static_cast<int>(rows_.size())) {
        if (!ctrl) sel_.clear();
        int a = std::min(ls.anchor, row), b = std::max(ls.anchor, row);
        for (int i = a; i <= b; ++i)
            if (rows_[i].node) sel_.insert(rows_[i].node);
    } else if (ctrl) {
        if (Node* n = rows_[row].node)
            if (!sel_.erase(n)) sel_.insert(n);
        ls.anchor = row;
    } else {
        sel_.clear();
        if (rows_[row].node) sel_.insert(rows_[row].node);
        ls.anchor = row;
    }
    ls.focus = row;
}

void App::EnsureVisible(int row) {
    ListState& ls = lists_[static_cast<int>(view_)];
    float rowH = 32.0f, viewH = H(listArea_);
    float y = row * rowH;
    if (y < ls.scroll) ls.scroll = y;
    if (y + rowH > ls.scroll + viewH) ls.scroll = y + rowH - viewH;
}

void App::RevealInBrowse(Node* n) {
    if (!n || !root_) return;
    if (view_ != View::Browse) ShowView(View::Browse);
    if (!IsAncestor(browseDir_, n) && n != browseDir_) NavigateTo(n->parent ? n->parent : root_.get());
    for (Node* p = n->parent; p && p != browseDir_; p = p->parent) expanded_.insert(p);
    rowsDirty_ = true;
    BuildRows();
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].node == n) {
            SelectRow(static_cast<int>(i), false, false);
            EnsureVisible(static_cast<int>(i));
            break;
        }
}

void App::SortResults() {
    auto sorter = [this](std::vector<Node*>& v) {
        auto cmp3 = [](auto a, auto b) { return a < b ? -1 : (a > b ? 1 : 0); };
        std::stable_sort(v.begin(), v.end(), [&](Node* a, Node* b) {
            int r = 0;
            switch (resSortKey_) {
                case SK_SIZE: r = cmp3(Val(a), Val(b)); break;
                case SK_MTIME: r = cmp3(a->mtime, b->mtime); break;
                case SK_FOLDER: r = _wcsicmp(NodePath(a->parent ? a->parent : a).c_str(), NodePath(b->parent ? b->parent : b).c_str()); break;
                default: r = StrCmpLogicalW(a->name.c_str(), b->name.c_str()); break;
            }
            return resSortDesc_ ? r > 0 : r < 0;
        });
    };
    for (auto* v : {&largest_, &searchRes_, &oldRes_, &emptyRes_, &typeFiles_}) sorter(*v);
    rowsDirty_ = true;
}

void App::RebuildTypes() {
    if (!root_) return;
    types_ = ExtensionStats(root_.get(), !settings_.apparent);
    std::fill(std::begin(catTotals_), std::end(catTotals_), 0);
    for (auto& t : types_) catTotals_[CategoryOfExt(t.ext)] += settings_.apparent ? t.size : t.disk;
    rowsDirty_ = true;
}

void App::RebuildEmpty() {
    emptyRes_.clear();
    if (!root_) return;
    // A folder is "empty" if no file exists anywhere below it; report only the topmost ones.
    std::unordered_map<Node*, bool> hasFile;
    std::vector<std::pair<Node*, bool>> st{{root_.get(), false}};
    while (!st.empty()) {
        auto [n, visited] = st.back();
        st.pop_back();
        if (!visited) {
            st.push_back({n, true});
            for (auto& c : n->children)
                if (c->CanEnter()) st.push_back({c.get(), false});
            continue;
        }
        bool f = false;
        for (auto& c : n->children) {
            if (c->CanEnter()) f = f || hasFile[c.get()];
            else if (!c->IsDir() || c->Has(NF_LINK)) f = true;
        }
        if (n->Has(NF_ERROR)) f = true;  // unreadable: don't call it empty
        hasFile[n] = f;
    }
    std::vector<Node*> stack{root_.get()};
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        for (auto& c : n->children) {
            if (!c->CanEnter()) continue;
            if (!hasFile[c.get()])
                emptyRes_.push_back(c.get());
            else
                stack.push_back(c.get());
        }
    }
    std::sort(emptyRes_.begin(), emptyRes_.end(), [](Node* a, Node* b) { return a->items > b->items; });
    rowsDirty_ = true;
}

void App::RebuildDevJunk() {
    devJunk_ = root_ ? FindDevJunk(root_.get()) : std::vector<DevProject>();
    devJunkDone_ = true;
    SortDevJunk();
}

void App::SortDevJunk() {
    bool disk = !settings_.apparent;
    if (devJunkByAge_)
        std::stable_sort(devJunk_.begin(), devJunk_.end(), [](const DevProject& a, const DevProject& b) { return a.lastChange < b.lastChange; });
    else
        std::stable_sort(devJunk_.begin(), devJunk_.end(),
                         [disk](const DevProject& a, const DevProject& b) { return DevProjectSize(a, disk) > DevProjectSize(b, disk); });
    rowsDirty_ = true;
}

// Clicking a project selects all of its artifact folders (or clears them if all are selected).
void App::ToggleDevProject(int project) {
    if (project < 0 || project >= static_cast<int>(devJunk_.size())) return;
    auto& arts = devJunk_[project].artifacts;
    bool all = std::all_of(arts.begin(), arts.end(), [&](const DevArtifact& a) { return sel_.count(a.node) > 0; });
    for (auto& a : arts)
        if (all)
            sel_.erase(a.node);
        else
            sel_.insert(a.node);
}

// =============================================================================================
// AI models (Hugging Face, Ollama, LM Studio): works without a scan.

void App::LoadAiModels() {
    ai_ = FindAiModels();
    aiAtime_ = LastAccessTracked();
    aiLoaded_ = true;
    aiSel_.clear();
    rowsDirty_ = true;
    Invalidate();
}

// A click on a repository selects it whole (its revisions show as checked); a click on a revision
// toggles just that one, and choosing every revision of a repository is the same as choosing it.
void App::ToggleAiRow(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[row].kind != RK_AI) return;
    size_t s = static_cast<size_t>(rows_[row].idx) >> 20, i = static_cast<size_t>(rows_[row].idx) & 0xFFFFF;
    if (s >= ai_.size() || i >= ai_[s].items.size()) return;
    const auto& items = ai_[s].items;
    lists_[static_cast<int>(view_)].focus = row;
    int parent = items[i].parent;
    if (parent < 0) {
        bool on = !aiSel_.count({s, i});
        for (size_t k = 0; k < items.size(); ++k)
            if (items[k].parent == static_cast<int>(i)) aiSel_.erase({s, k});
        if (on)
            aiSel_.insert({s, i});
        else
            aiSel_.erase({s, i});
        return;
    }
    size_t p = static_cast<size_t>(parent);
    if (aiSel_.erase({s, p})) {
        for (size_t k = 0; k < items.size(); ++k)
            if (items[k].parent == parent && k != i) aiSel_.insert({s, k});
        return;
    }
    if (!aiSel_.erase({s, i})) aiSel_.insert({s, i});
    bool all = true;
    for (size_t k = 0; k < items.size(); ++k)
        if (items[k].parent == parent && !aiSel_.count({s, k})) all = false;
    if (all) {
        for (size_t k = 0; k < items.size(); ++k)
            if (items[k].parent == parent) aiSel_.erase({s, k});
        aiSel_.insert({s, p});
    }
}

uint64_t App::AiSelectedBytes() const {
    uint64_t b = 0;
    for (auto& [s, i] : aiSel_)
        if (s < ai_.size() && i < ai_[s].items.size()) b += ai_[s].items[i].bytes;
    return b;
}

void App::StartAiDelete() {
    if (aiSel_.empty()) return Toast(L"Select the models to delete first", 2);
    Dialog d;
    size_t n = aiSel_.size();
    d.title = L"Delete " + util::FormatCount(n) + (n == 1 ? L" model (" : L" models (") + util::FormatSizeShort(AiSelectedBytes()) + L")?";
    d.message = L"The model files are deleted permanently, not moved to the Recycle Bin. Chats, history, settings and sign-in "
                L"tokens are not touched.";
    std::vector<std::wstring> restore;
    size_t shown = 0;
    for (auto& [s, i] : aiSel_) {
        const AiItem& it = ai_[s].items[i];
        if (std::find(restore.begin(), restore.end(), it.restore) == restore.end()) restore.push_back(it.restore);
        if (++shown > 8) continue;
        std::wstring name = it.parent >= 0 ? ai_[s].items[static_cast<size_t>(it.parent)].name + L" " + it.name : it.name;
        d.lines.push_back(util::FormatSizeShort(it.bytes) + L"\t" + name);
    }
    if (n > 8) d.lines.push_back(L"\t… and " + util::FormatCount(n - 8) + L" more");
    if (restore.size() <= 3) {
        d.message += L"\n\nTo get " + std::wstring(n == 1 ? L"it" : L"them") + L" back:";
        for (auto& r : restore) d.message += L"\n" + r;
    } else {
        d.message += L" Recently deleted lists how to download each one again.";
    }
    d.icon = 0xE74D;
    d.tone = 1;
    d.buttons = {L"Cancel", L"Delete permanently"};
    d.primary = 1;
    d.danger = 1;
    d.onClose = [this](int b, bool) {
        if (b != 1) return;
        struct State {
            std::vector<AiStore> stores;
            std::vector<std::vector<size_t>> chosen;
            std::atomic<size_t> index{0};
            std::atomic<bool> cancel{false};
            AiDeleteResult total;  // written by the worker, read after it finished
        };
        auto st = std::make_shared<State>();
        for (size_t s = 0; s < ai_.size(); ++s) {
            std::vector<size_t> c;
            for (auto& [ss, i] : aiSel_)
                if (ss == s) c.push_back(i);
            if (c.empty()) continue;
            st->stores.push_back(ai_[s]);
            st->chosen.push_back(std::move(c));
        }
        if (st->stores.empty()) return;
        auto t = std::make_unique<Task>();
        t->title = L"Deleting models";
        t->lines = [st] {
            size_t i = std::min(st->index.load(), st->stores.size() - 1);
            return std::vector<std::pair<std::wstring, std::wstring>>{{L"From", st->stores[i].name}};
        };
        t->progress = [st] { return static_cast<double>(st->index) / st->stores.size(); };
        t->cancel = [st] { st->cancel = true; };
        t->onDone = [this, st] {
            const AiDeleteResult& r = st->total;
            std::wstring msg = L"Deleted " + util::FormatCount(r.deleted) + (r.deleted == 1 ? L" model" : L" models") + L", freed " +
                               util::FormatSizeShort(r.freed);
            if (!r.lockedBy.empty())
                msg += L" · some files are in use (close " + AppNames(r.lockedBy) + L" and try again)";
            else if (!r.errors.empty())
                msg += L" · " + r.errors[0];
            Toast(msg, r.errors.empty() ? 0 : 1);
            std::vector<Node*> touched;
            for (auto& s : st->stores)
                if (Node* nd = FindNodeByPath(s.folder)) touched.push_back(nd);
            LoadAiModels();
            if (!touched.empty() && !readOnly_) StartRescan(NormalizeTargets(touched));
            if (root_) haveVol_ = QueryFreeSpace(root_->name, volTotal_, volFree_);
            drives_ = ListDrives();
        };
        StartTask(std::move(t), [st] {
            for (size_t i = 0; i < st->stores.size() && !st->cancel; ++i) {
                st->index = i;
                AiDeleteResult r = DeleteAiItems(st->stores[i], st->chosen[i], st->cancel);
                st->total.freed += r.freed;
                st->total.deleted += r.deleted;
                st->total.errors.insert(st->total.errors.end(), r.errors.begin(), r.errors.end());
                for (auto& a : r.lockedBy)
                    if (std::find(st->total.lockedBy.begin(), st->total.lockedBy.end(), a) == st->total.lockedBy.end())
                        st->total.lockedBy.push_back(a);
            }
        });
    };
    ShowDialog(std::move(d));
}

void App::RebuildOld() {
    oldRes_.clear();
    if (!root_) return;
    SearchQuery q;
    q.type = 1;
    q.olderThan = util::NowFileTime() - static_cast<int64_t>(oldAgeDays_) * 864000000000LL;
    q.minSize = oldMinSize_;
    oldRes_ = RunSearch(root_.get(), q, !settings_.apparent);
    if (oldRes_.size() > 5000) oldRes_.resize(5000);
    rowsDirty_ = true;
}

// =============================================================================================
// Tasks

void App::StartTask(std::unique_ptr<Task> t, std::function<void()> work) {
    auto done = t->done;
    HWND hwnd = hwnd_;
    t->start = GetTickCount64();
    t->thread = util::StartThread([work = std::move(work), done, hwnd] {
        work();
        done->store(true);
        PostMessageW(hwnd, WM_APP_TASKDONE, 0, 0);
    });
    task_ = std::move(t);
    EnsureTimer();
    Invalidate();
}

void App::OnTaskDone() {
    if (!task_ || !task_->done->load()) return;
    if (task_->thread) {
        WaitForSingleObject(task_->thread, INFINITE);
        CloseHandle(task_->thread);
    }
    std::unique_ptr<Task> t = std::move(task_);
    if (t->onDone) t->onDone();
    Invalidate();
}

namespace {
std::vector<std::pair<std::wstring, std::wstring>> ScanLines(ScanStats& s, ULONGLONG start) {
    double secs = (GetTickCount64() - start) / 1000.0;
    std::vector<std::pair<std::wstring, std::wstring>> l;
    l.push_back({L"Files", util::FormatCount(s.files)});
    l.push_back({L"Folders", util::FormatCount(s.dirs)});
    l.push_back({L"Size", util::FormatSizeShort(s.bytes)});
    wchar_t rate[64];
    swprintf(rate, 64, L"%s  ·  %.0f items/s", util::FormatDuration(secs).c_str(),
             secs > 0.2 ? (s.files + s.dirs) / secs : 0.0);
    l.push_back({L"Elapsed", rate});
    if (s.errors) l.push_back({L"Skipped", util::FormatCount(s.errors) + L" unreadable folders"});
    l.push_back({L"", s.GetCurrent()});
    return l;
}

ScanOptions MakeOptions(const Settings& s) {
    ScanOptions o;
    o.threads = s.threads > 0 ? s.threads : static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 8u));
    o.hardlinks = s.hardlinks;
    for (auto e : s.excludes) {
        for (auto& c : e)
            if (c == L'/') c = L'\\';
        o.excludes.push_back(util::ToLower(e));
    }
    return o;
}
}  // namespace

void App::ConfirmNewScan(const std::wstring& path) {
    if (!root_ || readOnly_) return StartScan(path);
    Dialog d;
    d.title = L"Start a new scan?";
    d.message = L"Scan " + path + L". The current results will be replaced (they stay in History).";
    d.icon = 0xE721;
    d.buttons = {L"Cancel", L"Scan"};
    d.primary = 1;
    d.onClose = [this, path](int b, bool) {
        if (b == 1) StartScan(path);
    };
    ShowDialog(std::move(d));
}

void App::StartScan(const std::wstring& input) {
    std::wstring err;
    std::wstring path = NormalizeDirPath(input, err);
    if (path.empty()) {
        Dialog d;
        d.title = L"Can't scan this location";
        d.message = input + L"\n" + err;
        d.icon = 0xE7BA;
        d.tone = 1;
        d.buttons = {L"OK"};
        d.primary = 0;
        ShowDialog(std::move(d));
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
        LoadHistory();
        Toast(incomplete_ ? L"Scan stopped — showing partial results"
                          : L"Scanned " + util::FormatCount(root_->items) + L" items in " + util::FormatDuration(scanSeconds_),
              incomplete_ ? 2 : 0);
    };
    ScanOptions opt = MakeOptions(settings_);
    StartTask(std::move(t), [n, path, opt, stats] {
        ScanDirectory(n, path, opt, *stats);
        RecomputeTotals(n);
        if (!stats->cancel) {
            std::wstring err2;
            SaveSnapshot(n, err2);
        }
    });
}

void App::StartRescan(std::vector<Node*> dirs) {
    if (readOnly_) return Toast(L"Imported scans can't be rescanned", 1);
    struct Job {
        Node* target;
        std::wstring path;
        bool exists = true;
        std::unique_ptr<Node> fresh;
    };
    auto jobs = std::make_shared<std::vector<Job>>();
    for (Node* d : dirs)
        if (d && IsAlive(d)) jobs->push_back(Job{d, NodePath(d)});
    if (jobs->empty()) return;
    auto stats = std::make_shared<ScanStats>();
    auto t = std::make_unique<Task>();
    t->title = jobs->size() == 1 ? L"Rescanning " + (*jobs)[0].path : L"Rescanning " + std::to_wstring(jobs->size()) + L" folders";
    ULONGLONG start = GetTickCount64();
    t->lines = [stats, start] { return ScanLines(*stats, start); };
    t->cancel = [stats] { stats->cancel = true; };
    t->onDone = [this, jobs, stats] {
        bool cancelled = stats->cancel.load();
        for (auto& j : *jobs) {
            Node* tgt = j.target;
            if (!IsAlive(tgt)) continue;
            if (!j.exists) {
                if (tgt->parent) graveyard_.push_back(DetachNode(tgt));
                continue;
            }
            if (cancelled) continue;
            if (j.fresh) {
                for (auto& c : tgt->children) {
                    c->flags |= NF_DELETED;
                    graveyard_.push_back(std::move(c));
                }
                tgt->children = std::move(j.fresh->children);
                for (auto& c : tgt->children) c->parent = tgt;
                tgt->flags = static_cast<uint16_t>((tgt->flags & ~(NF_ERROR | NF_SUBERR)) | (j.fresh->flags & (NF_ERROR | NF_SUBERR)));
                tgt->mtime = j.fresh->mtime;
                RecomputeTotals(tgt);
                RecomputeUp(tgt->parent);
            } else {
                ReadNodeMeta(tgt, j.path);
                tgt->disk = tgt->size;
                RecomputeUp(tgt->parent);
            }
        }
        PostMutation();
        Toast(cancelled ? L"Rescan cancelled" : L"Rescan complete", cancelled ? 2 : 0);
    };
    ScanOptions opt = MakeOptions(settings_);
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

namespace {

// "AI\tClaude Code (sessions, memory, settings) · contains C:\Users\me\.claude"
std::wstring HitLine(const ProtectHit& h) {
    ProtectHit c = h;
    c.group.clear();
    return h.group + L"\t" + DescribeHit(c);
}

}  // namespace

// Deleting goes through the protection rules first (protect.cpp):
//   Blocked   -> never deleted; the rest of the selection can still go ahead
//   Important -> an extra warning step, and the Recycle Bin is preselected
//   Caution   -> a heads-up inside the normal delete dialog
void App::RequestDelete(bool permanent) {
    if (readOnly_) return Toast(L"Deleting isn't available for imported scans", 1);
    auto targets = NormalizeTargets(SelectedNodes());
    if (targets.empty()) {
        if (Node* f = FocusNode()) targets = NormalizeTargets({f});
    }
    if (targets.empty()) return;

    RefreshProtection();
    std::vector<Node*> allowed;
    std::vector<std::wstring> blockedLines, warnLines, notes;
    std::wstring firstBlockedReason, heads;
    std::vector<ProtectHit> important;
    Protect worst = Protect::None;
    bool ai = false;
    for (Node* n : targets) {
        ProtectReport rep = CheckProtection(NodePath(n), n);
        if (rep.level == Protect::Blocked) {
            for (auto& h : rep.hits) {
                if (h.level != Protect::Blocked) continue;
                if (firstBlockedReason.empty()) firstBlockedReason = h.reason;
                if (blockedLines.size() < 6) blockedLines.push_back(h.group + L"\t" + NodePath(n) + L" \u00B7 " + h.name);
                break;
            }
            continue;
        }
        allowed.push_back(n);
        worst = std::max(worst, rep.level);
        for (auto& h : rep.hits) {
            if (h.group == L"AI") ai = true;
            std::wstring line = HitLine(h);
            if (h.level == Protect::Important) {
                if (std::find(warnLines.begin(), warnLines.end(), line) == warnLines.end() && warnLines.size() < 8) {
                    warnLines.push_back(line);
                    important.push_back(h);
                }
            } else if (h.level == Protect::Caution) {
                if (heads.empty()) heads = h.name + L": " + h.reason;
                if (std::find(notes.begin(), notes.end(), line) == notes.end() && notes.size() < 3) notes.push_back(line);
            }
        }
    }
    for (auto& l : blockedLines) notes.insert(notes.begin(), L"Skipped" + l.substr(l.find(L'\t')));

    if (allowed.empty()) {
        Dialog d;
        d.icon = 0xEA18;
        d.tone = 1;
        d.title = targets.size() == 1 ? L"This location is protected" : L"These locations are protected";
        d.message = firstBlockedReason + L"\n\nWinNCDU never deletes system folders. Edit protect.txt only if you are sure "
                                         L"(menu \u22EF \u2192 Protected locations).";
        d.lines = blockedLines;
        d.buttons = {L"OK"};
        d.primary = 0;
        ShowDialog(std::move(d));
        return;
    }
    if (worst < Protect::Important) {
        ShowDeleteDialog(allowed, permanent, worst, notes, heads);
        return;
    }

    // Important data: say exactly what would be lost before the usual delete dialog.
    Dialog d;
    d.icon = 0xEA18;
    d.tone = 2;
    d.title = ai ? L"This deletes AI assistant data" : L"This contains data you may not get back";
    d.message = important[0].reason;
    if (important.size() > 1)
        d.message += L" (+" + std::to_wstring(important.size() - 1) + L" more below)";
    d.message += L"\n\nIf you continue, the Recycle Bin is preselected so you can still restore it.";
    if (!blockedLines.empty())
        d.message += L" " + std::to_wstring(blockedLines.size()) + L" protected system item(s) will be skipped.";
    d.lines = warnLines;
    d.buttons = {L"Cancel", L"Continue\u2026"};
    d.primary = 0;  // Enter and Escape both cancel
    d.cancel = 0;
    d.danger = 1;
    d.onClose = [this, allowed, permanent, worst, notes, heads](int b, bool) {
        if (b == 1) ShowDeleteDialog(allowed, permanent, worst, notes, heads);
    };
    ShowDialog(std::move(d));
}

void App::ShowDeleteDialog(std::vector<Node*> targets, bool permanent, Protect worst, std::vector<std::wstring> notes,
                           std::wstring heads) {
    uint64_t total = 0;
    for (Node* n : targets) total += Val(n);
    Dialog d;
    d.icon = 0xE74D;
    // Protected data always starts out going to the Recycle Bin.
    d.checked = worst >= Protect::Important ? false : (permanent || settings_.permanentDefault);
    d.hasCheck = true;
    d.checkText = L"Delete permanently (skip the Recycle Bin)";
    size_t shown = notes.empty() ? 6 : 4;
    for (size_t i = 0; i < targets.size() && i < shown; ++i)
        d.lines.push_back(util::FormatSizeShort(Val(targets[i])) + L"\t" + NodePath(targets[i]));
    if (targets.size() > shown) d.lines.push_back(L"\t… and " + util::FormatCount(targets.size() - shown) + L" more");
    for (auto& n : notes) d.lines.push_back(n);
    std::wstring what = targets.size() == 1 ? (targets[0]->IsDir() ? L"this folder" : L"this file")
                                            : util::FormatCount(targets.size()) + L" items";
    if (!heads.empty()) heads = L"\nHeads-up \u2014 " + heads;
    auto refresh = [what, total, worst, heads](Dialog& dd) {
        dd.title = dd.checked ? L"Permanently delete " + what + L"?" : L"Move " + what + L" to the Recycle Bin?";
        dd.message = dd.checked ? util::FormatSizeShort(total) + L" will be freed. This can't be undone."
                                : util::FormatSizeShort(total) + L" · you can restore it from the Recycle Bin.";
        dd.message += heads;
        dd.tone = dd.checked ? 1 : (worst != Protect::None ? 2 : 0);
        dd.buttons = {L"Cancel", dd.checked ? L"Delete permanently" : L"Move to Recycle Bin"};
        dd.danger = dd.checked ? 1 : -1;
    };
    refresh(d);
    d.primary = 1;
    d.onCheck = refresh;
    d.onClose = [this, targets](int b, bool perm) {
        if (b == 1) StartDelete(targets, perm);
    };
    ShowDialog(std::move(d));
}

void App::StartDelete(std::vector<Node*> targets, bool permanent) {
    // Last line of defence: whatever led here, blocked locations are never touched.
    targets.erase(std::remove_if(targets.begin(), targets.end(),
                                 [](Node* n) { return CheckProtection(NodePath(n)).level == Protect::Blocked; }),
                  targets.end());
    if (targets.empty()) return Toast(L"Protected locations are never deleted", 1);
    struct State {
        std::vector<std::wstring> paths, errors;
        std::vector<int64_t> times;  // when each item was done (for the journal)
        std::vector<std::wstring> lockedBy;  // apps holding files of items that failed
        std::atomic<size_t> index{0}, attempted{0};
        std::atomic<uint64_t> removed{0};
        std::atomic<bool> cancel{false};
    };
    auto st = std::make_shared<State>();
    for (Node* n : targets) st->paths.push_back(NodePath(n));
    st->errors.resize(targets.size());
    st->times.resize(targets.size());
    st->lockedBy.resize(targets.size());
    auto t = std::make_unique<Task>();
    t->title = permanent ? L"Deleting" : L"Moving to the Recycle Bin";
    t->lines = [st, permanent] {
        std::vector<std::pair<std::wstring, std::wstring>> l;
        size_t i = std::min(st->index.load(), st->paths.size() - 1);
        l.push_back({L"Item", std::to_wstring(i + 1) + L" of " + std::to_wstring(st->paths.size())});
        if (permanent) l.push_back({L"Removed", util::FormatCount(st->removed) + L" files and folders"});
        l.push_back({L"", st->paths[i]});
        return l;
    };
    t->progress = [st] { return static_cast<double>(st->index) / std::max<size_t>(1, st->paths.size()); };
    t->cancel = [st] { st->cancel = true; };
    t->onDone = [this, st, targets, permanent] {
        std::vector<Node*> partial;
        std::vector<std::wstring> errs, apps;
        uint64_t freed = 0;
        size_t deleted = 0;
        for (size_t i = 0; i < targets.size(); ++i) {
            Node* n = targets[i];
            sel_.erase(n);
            if (!IsAlive(n)) continue;
            JournalEntry je;
            je.time = st->times[i];
            je.action = permanent ? JournalAction::Delete : JournalAction::Recycle;
            je.bytes = n->size;
            je.items = n->items + 1;
            je.path = st->paths[i];
            if (!PathExists(st->paths[i])) {
                JournalAppend(je);
                freed += Val(n);
                ++deleted;
                graveyard_.push_back(DetachNode(n));
            } else if (i < st->attempted.load()) {
                if (permanent) {  // some of it is gone for good: that belongs in the journal too
                    je.note = L"partly deleted: " + st->errors[i] + (st->lockedBy[i].empty() ? L"" : L" (in use by " + st->lockedBy[i] + L")");
                    JournalAppend(je);
                }
                partial.push_back(n);
            }
            if (!st->errors[i].empty() && errs.size() < 8) {
                errs.push_back(st->errors[i]);
                if (!st->lockedBy[i].empty()) errs.push_back(L"In use by " + st->lockedBy[i]);
            }
            if (!st->lockedBy[i].empty()) apps.push_back(st->lockedBy[i]);
        }
        PostMutation();
        if (deleted)
            Toast((permanent ? L"Deleted " : L"Moved to Recycle Bin: ") + util::FormatCount(deleted) +
                      (deleted == 1 ? L" item · " : L" items · ") + util::FormatSizeShort(freed) +
                      (permanent ? L" freed" : L""),
                  0);
        if (!errs.empty()) {
            Dialog d;
            d.title = L"Some items could not be deleted";
            d.message = apps.empty() ? L"They may be in use or need administrator rights."
                                     : L"Close " + AppNames(apps) + L", then try again.";
            d.lines = errs;
            d.icon = 0xE7BA;
            d.tone = 2;
            d.buttons = {L"OK"};
            d.primary = 0;
            ShowDialog(std::move(d));
        }
        if (!partial.empty()) StartRescan(partial);
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
            st->times[i] = util::NowFileTime();
            if (!ok && PathExists(st->paths[i])) {
                std::vector<std::wstring> names;
                for (auto& a : AppsUsing(st->paths[i])) names.push_back(a.name);
                st->lockedBy[i] = AppNames(names);
            }
        }
        if (!permanent) CoUninitialize();
    });
}

void App::StartImport(const std::wstring& file) {
    auto count = std::make_shared<std::atomic<uint64_t>>(0);
    auto result = std::make_shared<std::unique_ptr<Node>>();
    auto err = std::make_shared<std::wstring>();
    auto t = std::make_unique<Task>();
    t->title = L"Opening saved scan";
    t->lines = [count, file] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{L"Items", util::FormatCount(*count)}, {L"", file}};
    };
    t->onDone = [this, result, err] {
        if (!*result) {
            Dialog d;
            d.title = L"Can't open this file";
            d.message = *err;
            d.icon = 0xE7BA;
            d.tone = 1;
            d.buttons = {L"OK"};
            d.primary = 0;
            ShowDialog(std::move(d));
            return;
        }
        SetRoot(std::move(*result), true);
        incomplete_ = false;
        scanErrors_ = 0;
        Toast(L"Opened " + util::FormatCount(root_->items) + L" items (read-only)", 2);
    };
    StartTask(std::move(t), [count, result, err, file] { *result = ImportNcduJson(file, *count, *err); });
}

void App::StartExport(bool csv) {
    if (!root_) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[96];
    swprintf(name, 96, L"winncdu-%04u%02u%02u-%02u%02u%s", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             csv ? L".csv" : L".json");
    std::wstring file = PickSaveFile(hwnd_, name, csv);
    if (file.empty()) return;
    auto count = std::make_shared<std::atomic<uint64_t>>(0);
    auto err = std::make_shared<std::wstring>();
    auto ok = std::make_shared<bool>(false);
    uint64_t total = root_->items + 1;
    Node* r = root_.get();
    auto t = std::make_unique<Task>();
    t->title = csv ? L"Exporting CSV" : L"Saving scan";
    t->lines = [count, file] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{L"Written", util::FormatCount(*count) + L" items"}, {L"", file}};
    };
    t->progress = [count, total] { return static_cast<double>(*count) / total; };
    t->onDone = [this, ok, err, file] {
        if (*ok)
            Toast(L"Saved " + file, 0);
        else
            Toast(L"Export failed: " + *err, 1);
    };
    StartTask(std::move(t), [csv, file, r, count, err, ok] {
        *ok = csv ? ExportCsv(r, file, *count, *err) : ExportNcduJson(r, file, *count, *err);
    });
}

void App::StartDuplicates() {
    if (!root_ || readOnly_) return;
    auto prog = std::make_shared<DupProgress>();
    auto result = std::make_shared<std::vector<DupGroup>>();
    auto t = std::make_unique<Task>();
    t->title = L"Looking for duplicate files";
    t->lines = [prog] {
        static const wchar_t* stages[] = {L"Grouping files by size", L"Quick check (first 64 KB)", L"Comparing contents (SHA-256)"};
        return std::vector<std::pair<std::wstring, std::wstring>>{
            {L"Stage", stages[std::clamp(prog->stage.load(), 0, 2)]},
            {L"Files", util::FormatCount(prog->filesDone) + L" / " + util::FormatCount(prog->filesTotal)},
            {L"Read", util::FormatSizeShort(prog->bytesDone) + L" / " + util::FormatSizeShort(prog->bytesTotal)}};
    };
    t->progress = [prog] {
        uint64_t tot = prog->bytesTotal;
        return tot ? static_cast<double>(prog->bytesDone) / tot : -1.0;
    };
    t->cancel = [prog] { prog->cancel = true; };
    t->onDone = [this, prog, result] {
        if (prog->cancel) return Toast(L"Duplicate search cancelled", 2);
        dups_ = std::move(*result);
        dupsDone_ = true;
        rowsDirty_ = true;
        lists_[static_cast<int>(View::Duplicates)] = ListState{};
        if (dups_.empty()) Toast(L"No duplicate files found", 0);
    };
    Node* scope = root_.get();
    uint64_t minSize = dupMin_;
    int threads = MakeOptions(settings_).threads;
    StartTask(std::move(t), [scope, minSize, threads, prog, result] { *result = FindDuplicates(scope, minSize, threads, *prog); });
}

void App::MeasureCleanup() {
    if (cleanupMeasuring_) return;
    // Rules are read again every time, so edits to cleaners.ini show up on "Measure". Choices made
    // so far (selection, unchecked items) stay with their category.
    std::vector<std::wstring> errs;
    std::vector<CleanCategory> fresh = BuildCleanupCategories(&errs);
    std::wstring reviewId = reviewCat_ >= 0 && reviewCat_ < static_cast<int>(cleanup_.size()) ? cleanup_[reviewCat_].id : L"";
    reviewCat_ = -1;
    for (size_t i = 0; i < fresh.size(); ++i) {
        CleanCategory& c = fresh[i];
        auto old = std::find_if(cleanup_.begin(), cleanup_.end(), [&](const CleanCategory& o) { return o.id == c.id; });
        if (old != cleanup_.end()) {
            c.selected = old->selected;
            c.entries = old->entries;
            c.bytes = old->bytes;
            c.files = old->files;
            c.guarded = old->guarded;
            c.measured = old->measured;
            c.denied = old->denied;
        } else {
            c.selected = c.safe && !(c.needsAdmin && !elevated_) && !c.external;
        }
        if (c.id == reviewId) reviewCat_ = static_cast<int>(i);
    }
    cleanup_ = std::move(fresh);
    rowsDirty_ = true;
    if (!errs.empty())
        Toast(errs[0] + (errs.size() > 1 ? L"  (+" + std::to_wstring(errs.size() - 1) + L" more)" : L""), 1);
    cleanupMeasuring_ = true;
    auto* copy = new std::vector<CleanCategory>(cleanup_);
    HWND hwnd = hwnd_;
    util::StartThread([copy, hwnd] {
        std::atomic<bool> cancel{false};
        for (auto& c : *copy) MeasureCategory(c, cancel);
        PostMessageW(hwnd, WM_APP_CLEANUP, 0, reinterpret_cast<LPARAM>(copy));
    });
    EnsureTimer();
}

void App::OnCleanupMeasured(std::vector<CleanCategory>* result) {
    std::unique_ptr<std::vector<CleanCategory>> owned(result);
    cleanupMeasuring_ = false;
    if (owned->size() != cleanup_.size()) return;
    for (size_t i = 0; i < cleanup_.size(); ++i) {
        bool sel = cleanup_[i].selected;
        // What was unchecked in the preview stays unchecked after measuring again.
        std::unordered_set<std::wstring> kept;
        for (auto& e : cleanup_[i].entries)
            if (e.keep) kept.insert(util::ToLower(e.binData.empty() ? e.path : e.binData));
        cleanup_[i] = (*owned)[i];
        for (auto& e : cleanup_[i].entries) e.keep = kept.count(util::ToLower(e.binData.empty() ? e.path : e.binData)) > 0;
        cleanup_[i].selected = sel && (cleanup_[i].bytes > 0 || cleanup_[i].recycleBin) && !cleanup_[i].toolMissing;
    }
    rowsDirty_ = true;
    Invalidate();
}

CleanEntry* App::ReviewEntry(int row) {
    if (view_ != View::CleanReview || reviewCat_ < 0 || reviewCat_ >= static_cast<int>(cleanup_.size())) return nullptr;
    if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
    auto& v = cleanup_[reviewCat_].entries;
    return rows_[row].idx < static_cast<int>(v.size()) ? &v[rows_[row].idx] : nullptr;
}

void App::ToggleKeep(int row) {
    if (CleanEntry* e = ReviewEntry(row)) e->keep = !e->keep;
    lists_[static_cast<int>(view_)].focus = row;
}

void App::StartCleanup() {
    std::vector<size_t> chosen;
    uint64_t total = 0;
    for (size_t i = 0; i < cleanup_.size(); ++i)
        if (cleanup_[i].selected && cleanup_[i].ToClean() > 0 && !cleanup_[i].toolMissing) {
            chosen.push_back(i);
            total += cleanup_[i].ToClean();
        }
    if (chosen.empty()) return Toast(L"Select at least one category with something to clean", 2);
    Dialog d;
    d.title = L"Clean up " + util::FormatSizeShort(total) + L"?";
    d.message = L"Exactly the files listed under Review are deleted, permanently. Files that are in use, or "
                L"that changed since they were measured, are skipped.";
    bool commands = false;
    for (size_t i : chosen) {
        size_t kept = cleanup_[i].KeptCount();
        const std::wstring& cmd = cleanup_[i].command;
        commands = commands || !cmd.empty();
        d.lines.push_back(util::FormatSizeShort(cleanup_[i].ToClean()) + L"\t" + cleanup_[i].name +
                          (!cmd.empty() ? L"  (runs " + cmd + L")" : kept ? L"  (" + util::FormatCount(kept) + L" kept)" : L""));
    }
    if (commands) d.message += L" Categories marked “runs” are cleaned by that tool's own command instead.";
    d.icon = 0xE74D;
    d.tone = 1;
    d.buttons = {L"Cancel", L"Clean up"};
    d.primary = 1;
    d.danger = 1;
    // System-level categories (Windows temp, updates) can come with a restore point first.
    bool system = false;
    for (size_t i : chosen) system = system || cleanup_[i].needsAdmin;
    if (system && elevated_) {
        d.hasCheck = true;
        d.checked = true;
        d.checkText = L"Create a System Restore point first";
        d.checkDanger = false;
    }
    d.onClose = [this, chosen, offered = d.hasCheck](int b, bool restorePoint) {
        if (b != 1) return;
        struct State {
            bool restorePoint = false;
            std::wstring restoreErr;  // set if the restore point failed: nothing is cleaned then
            std::vector<CleanCategory> cats;
            std::atomic<size_t> index{0};
            std::atomic<uint64_t> freed{0}, failed{0};
            std::vector<std::wstring> lockedBy;  // written by the worker, read after it finished
            std::vector<std::wstring> errors;    // the same: commands that didn't run or failed
            std::atomic<bool> cancel{false};
            std::mutex m;
            std::wstring current;
        };
        auto st = std::make_shared<State>();
        st->restorePoint = restorePoint && offered;
        for (size_t i : chosen) st->cats.push_back(cleanup_[i]);
        auto t = std::make_unique<Task>();
        t->title = L"Cleaning up";
        t->lines = [st] {
            size_t i = std::min(st->index.load(), st->cats.size() - 1);
            std::lock_guard<std::mutex> lk(st->m);
            return std::vector<std::pair<std::wstring, std::wstring>>{
                {L"Category", st->cats[i].name}, {L"Freed", util::FormatSizeShort(st->freed)}, {L"", st->current}};
        };
        t->progress = [st] { return static_cast<double>(st->index) / st->cats.size(); };
        t->cancel = [st] { st->cancel = true; };
        t->onDone = [this, st] {
            if (!st->restoreErr.empty()) {
                Dialog e;
                e.title = L"No restore point, so nothing was cleaned";
                e.message = st->restoreErr + L"\n\nClean up again without the restore point option, or turn System Protection on first.";
                e.icon = 0xE7BA;
                e.tone = 2;
                e.buttons = {L"OK"};
                e.primary = 0;
                ShowDialog(std::move(e));
                return;
            }
            if (!st->errors.empty())
                Toast(L"Cleanup freed " + util::FormatSizeShort(st->freed) + L" · " + st->errors[0], 1);
            else
                Toast(L"Cleanup freed " + util::FormatSizeShort(st->freed) +
                          (st->failed ? L" · " + util::FormatCount(st->failed) + L" items in use were skipped" +
                                            (st->lockedBy.empty() ? L"" : L" (close " + AppNames(st->lockedBy) + L")")
                                      : L""),
                      0);
            // Refresh the affected folders of the current scan.
            std::vector<Node*> touched;
            for (auto& c : st->cats)
                for (auto& p : ExpandedPaths(c))
                    if (Node* n = FindNodeByPath(p)) touched.push_back(n);
            MeasureCleanup();
            if (!touched.empty() && !readOnly_) StartRescan(NormalizeTargets(touched));
            if (root_) haveVol_ = QueryFreeSpace(root_->name, volTotal_, volFree_);
            drives_ = ListDrives();
        };
        StartTask(std::move(t), [st] {
            if (st->restorePoint) {
                {
                    std::lock_guard<std::mutex> lk(st->m);
                    st->current = L"Creating a System Restore point…";
                }
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                std::wstring err;
                if (!CreateRestorePoint(L"WinNCDU cleanup", err)) st->restoreErr = err;
                CoUninitialize();
                if (!st->restoreErr.empty()) return;
            }
            for (size_t i = 0; i < st->cats.size() && !st->cancel; ++i) {
                st->index = i;
                const CleanCategory& c = st->cats[i];
                CleanResult r = CleanCategoryNow(c, st->cancel, [&](const std::wstring& p) {
                    std::lock_guard<std::mutex> lk(st->m);
                    st->current = p;
                });
                st->freed += r.freed;
                st->failed += r.failed;
                st->lockedBy.insert(st->lockedBy.end(), r.lockedBy.begin(), r.lockedBy.end());
                if (!r.error.empty()) st->errors.push_back(c.name + L": " + r.error);
                if (!c.external) {
                    JournalEntry je;
                    je.action = JournalAction::Clean;
                    je.bytes = r.freed;
                    je.items = r.files;
                    je.path = c.name;
                    std::wstring note;
                    if (r.failed) note = util::FormatCount(r.failed) + L" items in use were skipped";
                    if (!r.lockedBy.empty()) note += L" (by " + AppNames(r.lockedBy) + L")";
                    if (size_t kept = c.KeptCount()) note += (note.empty() ? L"" : L"; ") + util::FormatCount(kept) + L" kept";
                    if (!c.command.empty()) note += (note.empty() ? L"ran " : L"; ran ") + c.command + (r.error.empty() ? L"" : L": " + r.error);
                    je.note = note;
                    JournalAppend(je);
                }
            }
        });
    };
    ShowDialog(std::move(d));
}

// =============================================================================================
// Navigation & views

void App::NavigateTo(Node* dir, bool remember) {
    if (!dir || !IsAlive(dir) || !dir->IsDir()) return;
    if (remember && browseDir_ && browseDir_ != dir) {
        backStack_.push_back(browseDir_);
        fwdStack_.clear();
    }
    Node* from = browseDir_;
    browseDir_ = dir;
    view_ = View::Browse;
    rowsDirty_ = true;
    tmDirty_ = true;
    ListState& ls = lists_[static_cast<int>(View::Browse)];
    ls = ListState{};
    BuildRows();
    // Coming up from a child: keep it selected.
    if (from && from->parent == dir) {
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].node == from) {
                SelectRow(static_cast<int>(i), false, false);
                EnsureVisible(static_cast<int>(i));
            }
    } else {
        sel_.clear();
    }
    Invalidate();
}

void App::ShowView(View v) {
    if (v != View::Home && v != View::Cleanup && v != View::History && v != View::Deleted && v != View::AiModels &&
        v != View::CleanReview && !root_)
        return Toast(L"Scan a drive or folder first", 2);
    if (v != view_) sel_.clear();
    view_ = v;
    switch (v) {
        case View::Largest:
            if (largest_.empty() && root_) largest_ = LargestFiles(root_.get(), 1000, !settings_.apparent);
            break;
        case View::Types:
            if (types_.empty()) RebuildTypes();
            break;
        case View::Old:
            if (oldRes_.empty()) RebuildOld();
            break;
        case View::Empty: RebuildEmpty(); break;
        case View::DevJunk:
            if (!devJunkDone_) RebuildDevJunk();
            break;
        case View::Cleanup:
            if (cleanup_.empty()) MeasureCleanup();
            break;
        case View::History: LoadHistory(); break;
        case View::Deleted: LoadDeleted(); break;
        case View::AiModels:
            if (!aiLoaded_) LoadAiModels();
            break;
        default: break;
    }
    rowsDirty_ = true;
    Invalidate();
}

void App::RunSearchQuery(const std::wstring& text) {
    if (!root_) return Toast(L"Scan a drive or folder first", 2);
    std::wstring trimmed = text;
    while (!trimmed.empty() && trimmed.back() == L' ') trimmed.pop_back();
    if (trimmed.empty()) return;
    SearchQuery q;
    std::wstring err;
    if (!ParseQuery(trimmed, q, err)) return Toast(err, 1);
    searchQuery_ = trimmed;
    searchRes_ = RunSearch(root_.get(), q, !settings_.apparent);
    if (searchRes_.size() > 20000) searchRes_.resize(20000);
    lists_[static_cast<int>(View::Search)] = ListState{};
    SetFocus(hwnd_);
    ShowView(View::Search);
}

void App::OpenRow(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const Row& r = rows_[row];
    if (r.kind == RK_TYPE) {
        if (r.idx >= static_cast<int>(types_.size())) return;
        typeFilesExt_ = types_[r.idx].ext;
        typeFiles_ = FilesWithExt(root_.get(), typeFilesExt_, !settings_.apparent);
        lists_[static_cast<int>(View::TypeFiles)] = ListState{};
        ShowView(View::TypeFiles);
        return;
    }
    if (r.kind == RK_DIFF) {
        if (r.node) RevealInBrowse(r.node);
        return;
    }
    if (r.kind == RK_CLEAN) {
        if (CleanEntry* e = ReviewEntry(row)) RevealInExplorer(e->binData.empty() ? e->path : e->binData);
        return;
    }
    if (view_ == View::AiModels) {
        size_t s = r.kind == RK_AI ? static_cast<size_t>(r.idx) >> 20 : static_cast<size_t>(r.idx);
        if (s >= ai_.size()) return;
        size_t i = static_cast<size_t>(r.idx) & 0xFFFFF;
        RevealInExplorer(r.kind == RK_AI && i < ai_[s].items.size() ? ai_[s].items[i].location : ai_[s].folder);
        return;
    }
    if (r.node) OpenNode(r.node);
}

void App::OpenNode(Node* n) {
    if (view_ != View::Browse) {
        RevealInBrowse(n);
        return;
    }
    if (n->CanEnter())
        NavigateTo(n);
    else if (!n->IsDir())
        OnCommand(CMD_OPEN_DEFAULT);
}

void App::CompareSnapshot(size_t index) {
    if (index >= snapshots_.size() || !root_) return;
    std::unordered_map<std::wstring, DirSize> old;
    SnapshotInfo info;
    if (!LoadSnapshot(snapshots_[index].file, old, info)) return Toast(L"Could not read this snapshot", 1);
    if (_wcsicmp(info.root.c_str(), root_->name.c_str()) != 0)
        return Toast(L"This snapshot belongs to " + info.root + L" — scan it to compare", 2);
    diff_ = CompareWithSnapshot(root_.get(), old, !settings_.apparent);
    diffInfo_ = info;
    lists_[static_cast<int>(View::Compare)] = ListState{};
    ShowView(View::Compare);
}

void App::LoadDeleted() {
    bin_ = ListRecycleBin();
    deleted_.clear();
    std::vector<char> claimed(bin_.size());
    for (auto& e : JournalRead(500)) {  // newest first, so a path deleted twice matches its latest bin item
        if (e.action == JournalAction::Restore) continue;
        DeletedRow r;
        r.e = e;
        if (e.action == JournalAction::Recycle) {
            const RecycledItem* it = FindRecycled(bin_, e);
            size_t bi = it ? static_cast<size_t>(it - bin_.data()) : 0;
            if (it && !claimed[bi]) {
                claimed[bi] = 1;
                r.bin = static_cast<int>(bi);
            } else {
                r.back = PathExists(e.path);
            }
        }
        deleted_.push_back(std::move(r));
    }
}

void App::RestoreDeleted(int index) {
    if (index < 0 || index >= static_cast<int>(deleted_.size()) || deleted_[index].bin < 0) return;
    RecycledItem it = bin_[deleted_[index].bin];
    std::wstring err;
    if (!RestoreRecycled(it, err)) {
        Dialog d;
        d.title = L"Can't restore this item";
        d.message = err;
        d.icon = 0xE7BA;
        d.tone = 2;
        d.buttons = {L"OK"};
        d.primary = 0;
        ShowDialog(std::move(d));
        LoadDeleted();
        return;
    }
    size_t slash = it.original.find_last_of(L'\\');
    Toast(L"Restored " + (slash == std::wstring::npos ? it.original : it.original.substr(slash + 1)), 0);
    LoadDeleted();
    // Show it in the current scan too: rescan the closest folder of the scan that holds it.
    if (root_ && !readOnly_ && !task_) {
        std::wstring p = it.original;
        Node* n = nullptr;
        while (!n && (slash = p.find_last_of(L'\\')) != std::wstring::npos && slash >= 2) {
            p = p.substr(0, slash);
            if (p.size() == 2) p += L'\\';
            n = FindNodeByPath(p);
            if (p.size() <= 3) break;
        }
        if (n && n->IsDir()) StartRescan({n});
    }
}

void App::LoadHistory() {
    snapshots_ = ListSnapshots(root_ && !readOnly_ ? root_->name : L"");
    if (!root_ || readOnly_) {
        // Home screen: one entry (the newest) per scanned location.
        std::vector<SnapshotInfo> unique;
        for (auto& s : snapshots_) {
            bool dup = false;
            for (auto& u : unique) dup = dup || _wcsicmp(u.root.c_str(), s.root.c_str()) == 0;
            if (!dup) unique.push_back(s);
        }
        snapshots_ = unique;
    }
}

void App::Toast(const std::wstring& text, int tone) {
    toast_.text = text;
    toast_.tone = tone;
    toast_.until = GetTickCount64() + (tone == 1 ? 6000 : 4000);
    EnsureTimer();
    Invalidate();
}

void App::ShowProtectedLocations() {
    RefreshProtection();
    auto all = ProtectedLocationsOnThisPC();
    // Show what people care about most: AI sessions, keys, wallets, synced and VM folders.
    static const wchar_t* kOrder[] = {L"AI", L"Keys", L"Wallet", L"Cloud", L"VM", L"Email", L"Messages", L"Yours", L"AI models"};
    std::vector<std::wstring> lines;
    size_t more = 0;
    for (const wchar_t* g : kOrder)
        for (auto& h : all) {
            if (h.group != g) continue;
            if (lines.size() < 12)
                lines.push_back(h.group + L"\t" + h.name + L" \u00B7 " + h.path);
            else
                ++more;
        }
    if (more) lines.push_back(L"\t… and " + util::FormatCount(more) + L" more");
    Dialog d;
    d.icon = 0xEA18;
    d.title = L"Protected locations";
    d.message = L"System folders are never deleted. For the data below (and Git repositories, password databases, keys "
                L"and wallets anywhere) WinNCDU asks twice and preselects the Recycle Bin. Add your own rules in protect.txt.";
    d.lines = lines.empty() ? std::vector<std::wstring>{L"\tNo AI, key, wallet or synced folders found on this PC."} : lines;
    d.buttons = {L"Edit protect.txt", L"OK"};
    d.primary = 1;
    d.cancel = 1;
    d.onClose = [](int b, bool) {
        if (b == 0) {
            std::wstring f = ProtectRulesFile();
            if (!f.empty()) ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + f + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        }
    };
    ShowDialog(std::move(d));
}

void App::ShowDialog(Dialog d) {
    d.active = true;
    dialog_ = std::move(d);
    Invalidate();
}

void App::ShowAbout() {
    Dialog d;
    d.title = L"WinNCDU " WINNCDU_VERSION;
    d.message =
        L"A fast disk usage analyzer for Windows, inspired by ncdu.\n\n"
        L"Shortcuts:  Enter open · Backspace up · Del delete · Shift+Del delete permanently · "
        L"Ctrl+F search · Ctrl+E show in Explorer · Ctrl+C copy path · F5 rescan · Alt+←/→ back/forward.\n\n"
        L"Search syntax:  *.iso   >1G   <10M   older:1y   newer:7d   type:f   type:d   ext:mp4,mkv   empty";
    d.icon = 0xE946;
    d.buttons = {L"Close"};
    d.primary = 0;
    ShowDialog(std::move(d));
}

// =============================================================================================
// Menus & commands

void App::ShowContextMenu(int sx, int sy) {
    std::vector<Node*> nodes = SelectedNodes();
    if (nodes.empty()) {
        if (Node* f = FocusNode()) nodes.push_back(f);
    }
    if (nodes.empty()) return;
    Node* n = nodes.front();
    bool single = nodes.size() == 1;
    HMENU m = CreatePopupMenu();
    if (single) {
        if (n->CanEnter() && view_ == View::Browse) AppendMenuW(m, MF_STRING, CMD_OPEN, L"Open\tEnter");
        if (view_ != View::Browse) AppendMenuW(m, MF_STRING, CMD_GOTO, L"Go to in folder view");
        AppendMenuW(m, MF_STRING, CMD_OPEN_DEFAULT, n->IsDir() ? L"Open in Explorer" : L"Open with default app");
        AppendMenuW(m, MF_STRING, CMD_REVEAL, L"Show in Explorer\tCtrl+E");
    }
    AppendMenuW(m, MF_STRING, CMD_COPY_PATH, single ? L"Copy path\tCtrl+C" : L"Copy paths\tCtrl+C");
    if (single) {
        AppendMenuW(m, MF_STRING, CMD_TERMINAL, L"Open terminal here");
        AppendMenuW(m, MF_STRING, CMD_PROPERTIES, L"Properties");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (!readOnly_) {
        AppendMenuW(m, MF_STRING, CMD_RESCAN_ITEM, L"Rescan");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CMD_DELETE, L"Move to Recycle Bin\tDel");
        AppendMenuW(m, MF_STRING, CMD_DELETE_PERM, L"Delete permanently\tShift+Del");
    }
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd) OnCommand(cmd);
}

void App::ShowMoreMenu() {
    HMENU m = CreatePopupMenu();
    HMENU theme = CreatePopupMenu();
    AppendMenuW(theme, MF_STRING | (settings_.theme == 0 ? MF_CHECKED : 0), CMD_SET_THEME_SYS, L"Use Windows setting");
    AppendMenuW(theme, MF_STRING | (settings_.theme == 1 ? MF_CHECKED : 0), CMD_SET_THEME_LIGHT, L"Light");
    AppendMenuW(theme, MF_STRING | (settings_.theme == 2 ? MF_CHECKED : 0), CMD_SET_THEME_DARK, L"Dark");
    AppendMenuW(m, MF_STRING, CMD_SCAN_FOLDER, L"Scan a folder…\tCtrl+O");
    AppendMenuW(m, MF_STRING, CMD_OPEN_SCAN, L"Open saved scan…");
    AppendMenuW(m, MF_STRING | (root_ ? 0 : MF_GRAYED), CMD_EXPORT_JSON, L"Save scan as JSON…\tCtrl+S");
    AppendMenuW(m, MF_STRING | (root_ ? 0 : MF_GRAYED), CMD_EXPORT_CSV, L"Export as CSV…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), L"Theme");
    AppendMenuW(m, MF_STRING | (settings_.si ? MF_CHECKED : 0), CMD_SET_SI, L"Decimal units (GB instead of GiB)");
    AppendMenuW(m, MF_STRING | (settings_.showHidden ? MF_CHECKED : 0), CMD_SET_HIDDEN, L"Show hidden && system files");
    AppendMenuW(m, MF_STRING | (settings_.hardlinks ? MF_CHECKED : 0), CMD_SET_HARDLINKS, L"Count hard links once (slower scans)");
    AppendMenuW(m, MF_STRING | (settings_.permanentDefault ? MF_CHECKED : 0), CMD_SET_PERMANENT,
                L"Delete permanently by default");
    AppendMenuW(m, MF_STRING | (IsShellIntegrated() ? MF_CHECKED : 0), CMD_SET_SHELL,
                L"Add “Analyze with WinNCDU” to Explorer");
    AppendMenuW(m, MF_STRING, CMD_PROTECTED, L"Protected locations…");
    AppendMenuW(m, MF_STRING, CMD_CLEANERS, L"Cleanup rules…");
    AppendMenuW(m, MF_STRING, CMD_WINAPP2, L"Import winapp2.ini…");
    if (PathExists(Winapp2File())) AppendMenuW(m, MF_STRING, CMD_WINAPP2_REMOVE, L"Remove winapp2.ini cleaners");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (!elevated_) AppendMenuW(m, MF_STRING, CMD_ADMIN, L"Restart as administrator");
    AppendMenuW(m, MF_STRING, CMD_ABOUT, L"About && shortcuts\tF1");
    RECT rc;
    GetClientRect(hwnd_, &rc);
    POINT pt{rc.right - Dip2Px(12), Dip2Px(50)};
    ClientToScreen(hwnd_, &pt);
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTALIGN, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd) OnCommand(cmd);
}

void App::OnCommand(int cmd) {
    Node* single = nullptr;
    {
        auto s = SelectedNodes();
        if (s.size() == 1) single = s[0];
        if (s.empty()) single = FocusNode();
    }
    switch (cmd) {
        case CMD_HOME: view_ = View::Home; LoadHistory(); drives_ = ListDrives(); break;
        case CMD_SCAN_FOLDER: {
            std::wstring p = PickFolder(hwnd_, L"Choose a folder to analyze");
            if (!p.empty()) ConfirmNewScan(p);
            break;
        }
        case CMD_OPEN_SCAN: {
            std::wstring f = PickOpenFile(hwnd_);
            if (!f.empty()) StartImport(f);
            break;
        }
        case CMD_RESCAN:
            if (!root_) break;
            if (view_ == View::Browse && browseDir_)
                StartRescan({browseDir_});
            else
                StartRescan({root_.get()});
            break;
        case CMD_RESCAN_ITEM: {
            std::vector<Node*> dirs;
            for (Node* n : SelectedNodes()) dirs.push_back(n->CanEnter() ? n : n->parent);
            if (dirs.empty() && single) dirs.push_back(single->CanEnter() ? single : single->parent);
            StartRescan(NormalizeTargets(dirs).empty() ? dirs : NormalizeTargets(dirs));
            break;
        }
        case CMD_THEME_TOGGLE:
            settings_.theme = P_.dark ? 1 : 2;
            ApplyTheme();
            SaveSettings(settings_);
            break;
        case CMD_MORE: ShowMoreMenu(); break;
        case CMD_BACK:
            if (!backStack_.empty()) {
                fwdStack_.push_back(browseDir_);
                Node* t = backStack_.back();
                backStack_.pop_back();
                NavigateTo(t, false);
            }
            break;
        case CMD_FORWARD:
            if (!fwdStack_.empty()) {
                backStack_.push_back(browseDir_);
                Node* t = fwdStack_.back();
                fwdStack_.pop_back();
                NavigateTo(t, false);
            }
            break;
        case CMD_UP:
            if (browseDir_ && browseDir_->parent) NavigateTo(browseDir_->parent);
            break;
        case CMD_TOGGLE_TREEMAP:
            settings_.treemap = !settings_.treemap;
            tmDirty_ = true;
            break;
        case CMD_MODE_DISK:
        case CMD_MODE_APPARENT:
            settings_.apparent = cmd == CMD_MODE_APPARENT;
            rowsDirty_ = tmDirty_ = true;
            if (!types_.empty()) RebuildTypes();
            SortResults();
            break;
        case CMD_DELETE: RequestDelete(false); break;
        case CMD_DELETE_PERM: RequestDelete(true); break;
        case CMD_CLEAR_SEL: sel_.clear(); break;
        case CMD_DUP_FIND: StartDuplicates(); break;
        case CMD_DUP_MARK: {
            sel_.clear();
            uint64_t bytes = 0;
            for (auto& g : dups_)
                for (size_t i = 1; i < g.files.size(); ++i) {
                    sel_.insert(g.files[i]);
                    bytes += Val(g.files[i]);
                }
            Toast(L"Selected " + util::FormatCount(sel_.size()) + L" extra copies (" + util::FormatSizeShort(bytes) +
                      L") — the oldest copy of each file is kept",
                  2);
            break;
        }
        case CMD_DEV_MARK_OLD: {
            // Projects nobody has touched for three months: their build output can go.
            int64_t cutoff = util::NowFileTime() - 90LL * 864000000000LL;
            sel_.clear();
            for (auto& p : devJunk_)
                if (p.lastChange && p.lastChange < cutoff)
                    for (auto& a : p.artifacts) sel_.insert(a.node);
            if (sel_.empty()) Toast(L"Every project here was changed in the last three months", 0);
            break;
        }
        case CMD_AI_REFRESH: LoadAiModels(); break;
        case CMD_AI_UNUSED: {
            // Hugging Face revisions no branch, tag or PR points to any more.
            size_t n = 0;
            for (size_t s = 0; s < ai_.size(); ++s)
                for (size_t i = 0; i < ai_[s].items.size(); ++i) {
                    const AiItem& it = ai_[s].items[i];
                    if (it.unused && !aiSel_.count({s, static_cast<size_t>(it.parent)})) {
                        aiSel_.insert({s, i});
                        ++n;
                    }
                }
            if (!n) Toast(L"Every revision here is still referenced by a branch, tag or PR", 0);
            break;
        }
        case CMD_AI_DELETE: StartAiDelete(); break;
        case CMD_CLEAN_SELECTED: StartCleanup(); break;
        case CMD_CLEAN_MEASURE: MeasureCleanup(); break;
        case CMD_ADMIN: {
            std::wstring args = root_ && !readOnly_ ? L"\"" + root_->name + L"\"" : L"";
            if (!args.empty() && args[args.size() - 2] == L'\\') args.insert(args.size() - 1, L"\\");
            if (RelaunchElevated(args)) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        }
        case CMD_CANCEL_TASK:
            if (task_ && task_->cancel && !task_->cancelRequested) {
                task_->cancelRequested = true;
                task_->cancel();
            }
            break;
        case CMD_BACK_TO_TYPES: ShowView(View::Types); break;
        case CMD_SEARCH_CLEAR:
            SetWindowTextW(search_, L"");
            if (view_ == View::Search) ShowView(View::Browse);
            break;
        case CMD_OPEN:
            if (single) OpenNode(single);
            break;
        case CMD_GOTO:
            if (single) RevealInBrowse(single);
            break;
        case CMD_OPEN_DEFAULT:
            if (single && !OpenWithDefault(NodePath(single))) Toast(L"Couldn't open " + single->name, 1);
            break;
        case CMD_REVEAL:
            if (single) RevealInExplorer(NodePath(single));
            else if (browseDir_) RevealInExplorer(NodePath(browseDir_));
            break;
        case CMD_COPY_PATH: {
            std::wstring text;
            auto nodes = SelectedNodes();
            if (nodes.empty() && single) nodes.push_back(single);
            for (Node* n : nodes) text += (text.empty() ? L"" : L"\r\n") + NodePath(n);
            if (!text.empty() && CopyTextToClipboard(text))
                Toast(nodes.size() == 1 ? L"Copied: " + text : L"Copied " + std::to_wstring(nodes.size()) + L" paths", 2);
            break;
        }
        case CMD_TERMINAL:
            if (single) OpenTerminalAt(NodePath(single->CanEnter() ? single : single->parent));
            break;
        case CMD_PROPERTIES:
            if (single) ShowProperties(hwnd_, NodePath(single));
            break;
        case CMD_SELECT_ALL:
            for (auto& r : rows_)
                if (r.node) sel_.insert(r.node);
            break;
        case CMD_SET_THEME_SYS:
        case CMD_SET_THEME_LIGHT:
        case CMD_SET_THEME_DARK:
            settings_.theme = cmd - CMD_SET_THEME_SYS;
            ApplyTheme();
            break;
        case CMD_SET_SI:
            settings_.si = !settings_.si;
            util::g_si = settings_.si;
            break;
        case CMD_SET_HIDDEN:
            settings_.showHidden = !settings_.showHidden;
            rowsDirty_ = true;
            break;
        case CMD_SET_HARDLINKS:
            settings_.hardlinks = !settings_.hardlinks;
            Toast(settings_.hardlinks ? L"Hard links will be counted once from the next scan" : L"Hard link detection off", 2);
            break;
        case CMD_SET_PERMANENT: settings_.permanentDefault = !settings_.permanentDefault; break;
        case CMD_SET_SHELL: {
            std::wstring err;
            bool on = !IsShellIntegrated();
            if (SetShellIntegration(on, err))
                Toast(on ? L"Added “Analyze with WinNCDU” to the Explorer right-click menu" : L"Removed from the Explorer menu", 0);
            else
                Toast(err, 1);
            break;
        }
        case CMD_EXPORT_JSON: StartExport(false); break;
        case CMD_EXPORT_CSV: StartExport(true); break;
        case CMD_ABOUT: ShowAbout(); break;
        case CMD_PROTECTED: ShowProtectedLocations(); break;
        case CMD_REVIEW_ALL:
        case CMD_REVIEW_NONE:
            if (reviewCat_ >= 0 && reviewCat_ < static_cast<int>(cleanup_.size()))
                for (auto& e : cleanup_[reviewCat_].entries) e.keep = cmd == CMD_REVIEW_NONE;
            break;
        case CMD_CLEANERS: {
            std::wstring f = UserCleanersFile(true);
            if (f.empty() || !OpenWithDefault(f)) return Toast(L"Can't open " + f, 1);
            Toast(L"Add your own cleanup categories, then click Measure in Cleanup", 2);
            break;
        }
        case CMD_WINAPP2: {
            std::wstring f = PickOpenFile(hwnd_, L"Import winapp2.ini", L"winapp2.ini", L"*.ini");
            if (f.empty()) break;
            Winapp2Stats st;
            std::wstring err;
            if (!ImportWinapp2(f, st, err)) return Toast(err, 1);
            bool busy = cleanupMeasuring_;
            if (!busy) MeasureCleanup();
            ShowView(View::Cleanup);
            Toast(busy ? L"Imported. Click Measure in a moment to include it."
                       : L"Imported " + util::FormatCount(st.usable) + L" app cleaners (" + util::FormatCount(st.personal) +
                             L" about personal data left out)",
                  2);
            break;
        }
        case CMD_WINAPP2_REMOVE: {
            if (!DeleteFileW(Winapp2File().c_str())) return Toast(util::Win32ErrorMessage(GetLastError()), 1);
            bool busy = cleanupMeasuring_;
            if (!busy) MeasureCleanup();
            ShowView(View::Cleanup);
            Toast(busy ? L"Removed. Click Measure in a moment to update the list." : L"winapp2.ini cleaners removed", 2);
            break;
        }
        case CMD_OPEN_JOURNAL:
            if (!PathExists(JournalFile())) return Toast(L"Nothing has been deleted yet", 2);
            OpenWithDefault(JournalFile());
            break;
    }
    if (cmd >= CMD_SET_THEME_SYS) SaveSettings(settings_);
    Invalidate();
}
