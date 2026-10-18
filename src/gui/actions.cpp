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
    photos_.clear();
    photosDone_ = false;
    ClearThumbs();
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
    for (auto& g : photos_)
        g.photos.erase(std::remove_if(g.photos.begin(), g.photos.end(), [&](const Photo& p) { return dead(p.node); }), g.photos.end());
    photos_.erase(std::remove_if(photos_.begin(), photos_.end(), [](const PhotoGroup& g) { return g.photos.size() < 2; }), photos_.end());
    for (auto it = thumbs_.begin(); it != thumbs_.end();) {
        if (IsAlive(it->first)) {
            ++it;
            continue;
        }
        SafeRelease(it->second);
        it = thumbs_.erase(it);
    }
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
            if (dupPhotos_) {
                for (size_t g = 0; g < photos_.size(); ++g) {
                    rows_.push_back({nullptr, 0, RK_GROUP, static_cast<int>(g)});
                    for (auto& p : photos_[g].photos) rows_.push_back({p.node, 1, RK_NODE, static_cast<int>(g)});
                }
                break;
            }
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
        case View::Wsl:
            if (!vdisks_.empty()) rows_.push_back({nullptr, 0, RK_GROUP, 0});
            for (size_t i = 0; i < vdisks_.size(); ++i) rows_.push_back({nullptr, 0, RK_WSL, static_cast<int>(i)});
            if (docker_.cli) {
                rows_.push_back({nullptr, 0, RK_GROUP, 1});
                if (docker_.running)
                    for (int k = 0; k < 4; ++k) rows_.push_back({nullptr, 0, RK_WSL, kWslDocker + k});
            }
            break;
        case View::Games:
            for (int leftovers = 0; leftovers < 2; ++leftovers) {
                bool any = false;
                for (size_t i = 0; i < games_.size(); ++i) {
                    if (games_[i].leftover != (leftovers == 1)) continue;
                    if (!any) rows_.push_back({nullptr, 0, RK_GROUP, leftovers});
                    any = true;
                    rows_.push_back({nullptr, 0, RK_GAME, static_cast<int>(i)});
                }
            }
            break;
        case View::Startup: {
            // Groups: 0 Run keys (and Run once), 1 Startup folders, 2 scheduled tasks, 3 services.
            auto group = [](StartupKind k) {
                return k == StartupKind::Folder ? 1 : k == StartupKind::Task ? 2 : k == StartupKind::Service ? 3 : 0;
            };
            for (int g = 0; g < 4; ++g) {
                bool any = false;
                for (size_t i = 0; i < startup_.entries.size(); ++i) {
                    const StartupEntry& e = startup_.entries[i];
                    if (group(e.kind) != g || !StartupVisible(e)) continue;
                    if (!any) rows_.push_back({nullptr, 0, RK_GROUP, g});
                    any = true;
                    rows_.push_back({nullptr, 0, RK_STARTUP, static_cast<int>(i)});
                }
            }
            break;
        }
        case View::Apps: {
            bool left = !appLeftFor_.empty();
            if (left) {
                rows_.push_back({nullptr, 0, RK_GROUP, 0});
                for (size_t i = 0; i < appLeft_.size(); ++i) rows_.push_back({nullptr, 0, RK_APPLEFT, static_cast<int>(i)});
                rows_.push_back({nullptr, 0, RK_GROUP, 1});
            }
            for (size_t i = 0; i < apps_.size(); ++i)
                if (AppVisible(apps_[i])) rows_.push_back({nullptr, 0, RK_APP, static_cast<int>(i)});
            break;
        }
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
    if (aiSel_.empty()) return Toast(Tr(L"Select the models to delete first"), 2);
    Dialog d;
    size_t n = aiSel_.size();
    d.title = TrN(n, L"Delete {0} model ({1})?", L"Delete {0} models ({1})?", util::FormatSizeShort(AiSelectedBytes()));
    d.message = Tr(L"The model files are deleted permanently, not moved to the Recycle Bin. Chats, history, settings and sign-in "
                   L"tokens are not touched.");
    std::vector<std::wstring> restore;
    size_t shown = 0;
    for (auto& [s, i] : aiSel_) {
        const AiItem& it = ai_[s].items[i];
        if (std::find(restore.begin(), restore.end(), it.restore) == restore.end()) restore.push_back(it.restore);
        if (++shown > 8) continue;
        std::wstring name = it.parent >= 0 ? ai_[s].items[static_cast<size_t>(it.parent)].name + L" " + it.name : it.name;
        d.lines.push_back(util::FormatSizeShort(it.bytes) + L"\t" + name);
    }
    if (n > 8) d.lines.push_back(L"\t" + TrN(n - 8, L"… and {0} more", L"… and {0} more"));
    if (restore.size() <= 3) {
        d.message += L"\n\n" + (n == 1 ? Tr(L"To get it back:") : Tr(L"To get them back:"));
        for (auto& r : restore) d.message += L"\n" + Tr(r);
    } else {
        d.message += L" " + Tr(L"Recently deleted lists how to download each one again.");
    }
    d.icon = 0xE74D;
    d.tone = 1;
    d.buttons = {Tr(L"Cancel"), Tr(L"Delete permanently")};
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
        t->title = Tr(L"Deleting models");
        t->lines = [st] {
            size_t i = std::min(st->index.load(), st->stores.size() - 1);
            return std::vector<std::pair<std::wstring, std::wstring>>{{Tr(L"From"), Tr(st->stores[i].name)}};
        };
        t->progress = [st] { return static_cast<double>(st->index) / st->stores.size(); };
        t->cancel = [st] { st->cancel = true; };
        t->onDone = [this, st] {
            const AiDeleteResult& r = st->total;
            std::wstring msg = TrN(r.deleted, L"Deleted {0} model, freed {1}", L"Deleted {0} models, freed {1}", util::FormatSizeShort(r.freed));
            if (!r.lockedBy.empty())
                msg += L" · " + TrF(L"some files are in use (close {0} and try again)", AppNames(r.lockedBy));
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

// =============================================================================================
// WSL and Docker: works without a scan.

void App::LoadWsl() {
    if (task_) return;
    struct State {
        std::vector<VDisk> disks;
        DockerInfo docker;
    };
    auto st = std::make_shared<State>();
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Looking at WSL and Docker");
    t->lines = [] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{L"", Tr(L"Asking running distros and Docker how much they use")}};
    };
    t->onDone = [this, st] {
        vdisks_ = std::move(st->disks);
        docker_ = std::move(st->docker);
        wslLoaded_ = true;
        wslSel_.clear();
        rowsDirty_ = true;
        Invalidate();
    };
    StartTask(std::move(t), [st] {
        st->disks = FindVDisks();
        st->docker = QueryDocker();
    });
}

uint64_t App::WslRowBytes(int id) const {
    if (id >= 0 && id < static_cast<int>(vdisks_.size())) return vdisks_[id].Reclaimable();
    static const wchar_t* kTypes[] = {L"Build Cache", L"Images", L"Containers", L"Local Volumes"};
    int k = id - kWslDocker;
    if (k < 0 || k > 3) return 0;
    const DockerRow* r = docker_.Find(kTypes[k]);
    return r ? r->reclaimable : 0;
}

void App::ToggleWslRow(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[row].kind != RK_WSL) return;
    int id = rows_[row].idx;
    lists_[static_cast<int>(view_)].focus = row;
    if (id >= kWslDocker + 2) return;  // containers and volumes can hold data: shown, never offered
    if (id < static_cast<int>(vdisks_.size()) && !vdisks_[id].usedKnown) return;
    if (!wslSel_.erase(id)) wslSel_.insert(id);
}

void App::StartWslFree() {
    if (wslSel_.empty()) return Toast(Tr(L"Select disks to compact or Docker data to remove first"), 2);
    std::vector<VDisk> disks;
    bool build = wslSel_.count(kWslDocker) > 0, images = wslSel_.count(kWslDocker + 1) > 0;
    uint64_t total = 0;
    for (int id : wslSel_) {
        total += WslRowBytes(id);
        if (id < static_cast<int>(vdisks_.size())) disks.push_back(vdisks_[id]);
    }
    if ((build || images) && !docker_.running) return Toast(Tr(L"Docker isn't running: start Docker Desktop first"), 2);
    bool anyRunning = false;
    for (auto& d : vdisks_) anyRunning = anyRunning || d.running;
    Dialog d;
    d.title = TrF(L"Free up to {0}?", util::FormatSizeShort(total));
    if (build || images) {
        d.message = build && images ? Tr(L"Docker removes its build cache and the images no container uses: the next build or docker pull brings them back.")
                    : build         ? Tr(L"Docker removes its build cache: the next build brings it back.")
                                    : Tr(L"Docker removes the images no container uses: docker pull brings them back when needed.");
        d.message += L" " + Tr(L"Containers and volumes are kept.");
    }
    if (!disks.empty()) {
        if (!d.message.empty()) d.message += L"\n\n";
        d.message += Tr(L"Compacting gives the free space inside each disk back to Windows.") + L" ";
        d.message += anyRunning ? Tr(L"WSL is shut down first, so every running Linux distro stops: save your work there.") + L" " : L"";
        d.message += docker_.desktopRunning ? Tr(L"Docker Desktop is stopped and started again.") + L" " : L"";
        d.message += Tr(L"Windows asks for administrator permission to run diskpart, and large disks can take several minutes.");
    }
    for (int id : wslSel_) {
        std::wstring name = id < static_cast<int>(vdisks_.size()) ? TrF(L"Compact {0}", vdisks_[id].name)
                            : id == kWslDocker                   ? Tr(L"Docker build cache")
                                                                 : Tr(L"Docker images no container uses");
        d.lines.push_back(util::FormatSizeShort(WslRowBytes(id)) + L"\t" + name);
    }
    d.icon = 0xE74D;
    d.tone = 2;
    d.buttons = {Tr(L"Cancel"), Tr(L"Free up space")};
    d.primary = 1;
    d.onClose = [this, disks, build, images](int b, bool) {
        if (b != 1) return;
        struct State {
            std::vector<VDisk> disks;
            bool build = false, images = false;
            DockerPruneResult prune;
            CompactResult compact;
            std::atomic<bool> cancel{false};
            std::mutex m;
            std::wstring status;
        };
        auto st = std::make_shared<State>();
        st->disks = disks;
        st->build = build;
        st->images = images;
        auto t = std::make_unique<Task>();
        t->title = Tr(L"Freeing up space");
        t->lines = [st] {
            std::lock_guard<std::mutex> lk(st->m);
            return std::vector<std::pair<std::wstring, std::wstring>>{{Tr(L"Now"), Tr(st->status)}};
        };
        t->cancel = [st] { st->cancel = true; };
        t->onDone = [this, st] {
            const CompactResult& c = st->compact;
            std::wstring pruned = util::FormatSizeShort(st->prune.freed);
            std::wstring compacted = util::FormatSizeShort(c.before > c.after ? c.before - c.after : 0);
            bool docker = st->build || st->images, compact = !st->disks.empty();
            std::wstring msg = docker && compact ? TrF(L"Docker removed {0}, compacting gave {1} back to Windows", pruned, compacted)
                               : docker          ? TrF(L"Docker removed {0}", pruned)
                               : compact         ? TrF(L"Compacting gave {0} back to Windows", compacted)
                                                 : L"";
            std::vector<std::wstring> errors = st->prune.errors;
            errors.insert(errors.end(), c.errors.begin(), c.errors.end());
            if (!errors.empty()) msg += L" · " + errors[0];
            Toast(msg, errors.empty() ? 0 : 1);
            if (root_) haveVol_ = QueryFreeSpace(root_->name, volTotal_, volFree_);
            drives_ = ListDrives();
            LoadWsl();
        };
        StartTask(std::move(t), [st] {
            auto status = [st](const std::wstring& s) {
                std::lock_guard<std::mutex> lk(st->m);
                st->status = s;
            };
            if (st->build || st->images) {
                status(N_(L"Docker is removing what comes back by itself"));
                st->prune = DockerPrune(st->build, st->images, st->cancel);
            }
            if (!st->disks.empty() && !st->cancel) st->compact = CompactVDisks(st->disks, st->cancel, status);
        });
    };
    ShowDialog(std::move(d));
}

// =============================================================================================
// Windows: space Windows keeps for itself (works without a scan).

void App::LoadWinTools() {
    if (task_) return;
    auto st = std::make_shared<WinToolsInfo>();
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Looking at Windows");
    t->lines = [] { return std::vector<std::pair<std::wstring, std::wstring>>{{L"", Tr(L"Drivers, System Restore, hibernation, event logs")}}; };
    t->onDone = [this, st] {
        st->store = win_.store;  // DISM's analysis takes minutes: kept until something changes it
        st->storeAnalyzed = win_.storeAnalyzed;
        win_ = std::move(*st);
        winLoaded_ = true;
        Invalidate();
    };
    StartTask(std::move(t), [st] {
        st->drivers = ListDriverPackages();
        st->hiber = QueryHibernation();
        st->compactOs = CompactOsOn();
        st->logs = QueryEventLogs();
        st->shadows = QueryShadowStorage(st->shadowErr);
    });
}

// Runs one of Windows' own tools as administrator, then says what it freed on the system drive.
// journalName and done are English (the journal is); doneShown is done as the toast says it.
void App::RunWinTool(const std::wstring& title, const std::wstring& journalName, const std::wstring& done,
                     std::function<AdminRun()> work, const std::wstring& doneShown) {
    struct State {
        AdminRun r;
        uint64_t before = 0, after = 0;
    };
    auto st = std::make_shared<State>();
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring drive = std::wstring(win).substr(0, 3);
    auto t = std::make_unique<Task>();
    t->title = title;
    t->lines = [] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{L"", Tr(L"Windows' own tool is working; this can take several minutes")}};
    };
    std::wstring shown = doneShown.empty() ? Tr(done) : doneShown;
    t->onDone = [this, st, title, journalName, done, shown] {
        uint64_t freed = st->after > st->before ? st->after - st->before : 0;
        if (!st->r.error.empty()) {
            Toast(TrF(L"{0}: {1}", title, st->r.error), st->r.declined ? 2 : 1);
        } else {
            bool ok = st->r.code == 0;
            Toast(ok ? shown + (freed ? L" · " + TrF(L"{0} more free space", util::FormatSizeShort(freed)) : L"")
                     : TrF(L"{0} failed: {1}", title, LastLine(st->r.out)),
                  ok ? 0 : 1);
            JournalEntry je;
            je.action = JournalAction::Clean;
            je.bytes = freed;
            je.path = journalName;
            je.note = ok ? done : L"failed: " + LastLine(st->r.out);
            JournalAppend(je);
        }
        if (root_) haveVol_ = QueryFreeSpace(root_->name, volTotal_, volFree_);
        drives_ = ListDrives();
        LoadWinTools();
    };
    StartTask(std::move(t), [st, drive, work] {
        ULARGE_INTEGER f{};
        if (GetDiskFreeSpaceExW(drive.c_str(), &f, nullptr, nullptr)) st->before = f.QuadPart;
        st->r = work();
        if (GetDiskFreeSpaceExW(drive.c_str(), &f, nullptr, nullptr)) st->after = f.QuadPart;
    });
}

void App::WinToolAction(int action) {
    Dialog d;
    d.icon = 0xE770;
    d.tone = 2;
    d.buttons = {Tr(L"Cancel"), L""};
    d.primary = 1;
    std::function<void()> run;
    std::wstring admin = elevated_ ? L"" : L" " + Tr(L"Windows asks for administrator permission.");
    switch (action) {
        case WA_ANALYZE: {
            struct State {
                ComponentStore cs;
            };
            auto st = std::make_shared<State>();
            auto t = std::make_unique<Task>();
            t->title = Tr(L"Analyzing the component store");
            t->lines = [] { return std::vector<std::pair<std::wstring, std::wstring>>{{L"", Tr(L"DISM takes a few minutes")}}; };
            t->onDone = [this, st] {
                if (!st->cs.ok) return Toast(TrF(L"Component store: {0}", st->cs.error), 1);
                win_.store = st->cs;
                win_.storeAnalyzed = true;
                Invalidate();
            };
            StartTask(std::move(t), [st] { st->cs = AnalyzeComponentStore(); });
            return;
        }
        case WA_CLEAN_STORE:
            d.title = Tr(L"Clean up the component store?");
            d.message = Tr(L"DISM removes the Windows components that newer updates replaced. Installed updates can still be "
                           L"uninstalled afterwards. This takes several minutes and can't be interrupted.") + admin;
            d.buttons[1] = Tr(L"Clean up");
            run = [this] {
                RunWinTool(Tr(L"Cleaning up the component store"), N_(L"Component store (WinSxS)"), N_(L"Component store cleaned up"), [] {
                    return CleanComponentStore();
                });
                win_.storeAnalyzed = false;
            };
            break;
        case WA_DRIVERS: {
            std::vector<DriverPackage> old;
            uint64_t bytes = 0;
            for (auto& p : win_.drivers)
                if (p.old) {
                    old.push_back(p);
                    bytes += p.bytes;
                }
            if (old.empty()) return;
            d.title = TrN(old.size(), L"Remove {0} old driver package ({1})?", L"Remove {0} old driver packages ({1})?", util::FormatSizeShort(bytes));
            d.message = Tr(L"Newer versions of these drivers are installed, and no device uses these ones. pnputil removes them from the "
                           L"driver store; it refuses any that a device starts using.") + admin;
            for (size_t i = 0; i < old.size() && i < 8; ++i)
                d.lines.push_back(util::FormatSizeShort(old[i].bytes) + L"\t" + old[i].provider.substr(0, old[i].provider.find(L' ')) +
                                  L"  " + old[i].originalInf + L"  " + old[i].version);
            if (old.size() > 8) d.lines.push_back(L"\t" + TrN(old.size() - 8, L"… and {0} more", L"… and {0} more"));
            d.buttons[1] = Tr(L"Remove");
            run = [this, old] {
                RunWinTool(Tr(L"Removing old drivers"), N_(L"Old driver packages"), N_(L"Old driver packages removed"), [old] { return DeleteDriverPackages(old); });
            };
            break;
        }
        case WA_SHADOW: {
            if (win_.shadows.empty()) return;
            wchar_t win[MAX_PATH];
            GetWindowsDirectoryW(win, MAX_PATH);
            ShadowStorage s = win_.shadows[0];
            for (auto& x : win_.shadows)
                if (_wcsnicmp(x.forDrive.c_str(), win, 2) == 0) s = x;
            d.title = TrF(L"Limit System Restore on {0}?", s.forDrive);
            d.message = TrF(L"Restore points may then use at most this share of {0}. When they need more, Windows deletes the oldest ones first.",
                            s.onDrive) + admin;
            d.buttons = {Tr(L"Cancel"), L"3%", L"5%", L"10%"};
            d.primary = 2;
            d.onClose = [this, s](int b, bool) {
                if (b < 1 || b > 3) return;
                int pct = b == 1 ? 3 : b == 2 ? 5 : 10;
                RunWinTool(Tr(L"Limiting System Restore"), L"System Restore storage on " + s.forDrive,
                           L"System Restore limited to " + std::to_wstring(pct) + L"% of " + s.onDrive,
                           [s, pct] { return LimitShadowStorage(s, pct); }, TrF(L"System Restore limited to {0}% of {1}", pct, s.onDrive));
            };
            ShowDialog(std::move(d));
            return;
        }
        case WA_HIBER_OFF:
            d.title = Tr(L"Turn hibernation off?");
            d.message = TrF(L"Deletes hiberfil.sys ({0}). Hibernate and Fast Startup stop working until you turn it on again here.",
                            util::FormatSizeShort(win_.hiber.bytes)) + admin;
            d.buttons[1] = Tr(L"Turn off");
            run = [this] { RunWinTool(Tr(L"Turning hibernation off"), N_(L"Hibernation file"), N_(L"Hibernation is off"), [] { return SetHibernation(L"off"); }); };
            break;
        case WA_HIBER_REDUCED:
            d.title = Tr(L"Reduce the hibernation file?");
            d.message = Tr(L"hiberfil.sys keeps only what Fast Startup needs (about half the size). Hibernate from the power menu stops "
                           L"working; Fast Startup keeps working.") + admin;
            d.buttons[1] = Tr(L"Reduce");
            run = [this] {
                RunWinTool(Tr(L"Reducing the hibernation file"), N_(L"Hibernation file"), N_(L"Hibernation file reduced"), [] { return SetHibernation(L"reduced"); });
            };
            break;
        case WA_HIBER_ON:
            d.title = Tr(L"Turn hibernation on?");
            d.message = Tr(L"Windows creates hiberfil.sys again (about 40% of your memory) for Hibernate and Fast Startup.") + admin;
            d.tone = 0;
            d.buttons[1] = Tr(L"Turn on");
            run = [this] { RunWinTool(Tr(L"Turning hibernation on"), N_(L"Hibernation file"), N_(L"Hibernation is on"), [] { return SetHibernation(L"on"); }); };
            break;
        case WA_COMPACT_ON:
            d.title = Tr(L"Turn on Compact OS?");
            d.message = Tr(L"Windows stores its own files compressed, usually saving 2 GB or more. It takes 10 to 20 minutes and uses the "
                           L"processor while it runs; afterwards the cost is barely noticeable. You can turn it off again here.") + admin;
            d.tone = 0;
            d.buttons[1] = Tr(L"Turn on");
            run = [this] { RunWinTool(Tr(L"Compressing Windows"), N_(L"Compact OS"), N_(L"Compact OS is on"), [] { return SetCompactOs(true); }); };
            break;
        case WA_COMPACT_OFF:
            d.title = Tr(L"Turn off Compact OS?");
            d.message = Tr(L"Windows stores its own files uncompressed again, which takes back the space it saved.") + admin;
            d.buttons[1] = Tr(L"Turn off");
            run = [this] { RunWinTool(Tr(L"Decompressing Windows"), N_(L"Compact OS"), N_(L"Compact OS is off"), [] { return SetCompactOs(false); }); };
            break;
        case WA_LOGS:
            d.title = Tr(L"Clear the event logs?");
            d.message = Tr(L"Every Windows event log except Security is emptied. The history that helps troubleshoot crashes and errors "
                           L"is gone afterwards; new events are logged as usual.") + admin;
            d.buttons[1] = Tr(L"Clear");
            run = [this] { RunWinTool(Tr(L"Clearing event logs"), N_(L"Event logs"), N_(L"Event logs cleared"), [] { return ClearEventLogs(); }); };
            break;
        default: return;
    }
    d.onClose = [run](int b, bool) {
        if (b == 1 && run) run();
    };
    ShowDialog(std::move(d));
}

// =============================================================================================
// Startup: what starts with Windows (works without a scan). Turning something off never deletes
// it: StartupApproved (as Task Manager), Task Scheduler's own switch, or Manual start for services.

void App::LoadStartup() {
    if (task_) return;
    auto st = std::make_shared<StartupList>();
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Looking at what starts with Windows");
    t->lines = [] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{L"", Tr(L"Run keys, Startup folders, scheduled tasks, services")}};
    };
    t->onDone = [this, st] {
        startup_ = std::move(*st);
        startupLoaded_ = true;
        // Within each group: highest impact first, then by name.
        std::stable_sort(startup_.entries.begin(), startup_.entries.end(), [](const StartupEntry& a, const StartupEntry& b) {
            if (a.impact != b.impact) return a.impact > b.impact;
            return _wcsicmp(a.display.c_str(), b.display.c_str()) < 0;
        });
        lists_[static_cast<int>(View::Startup)].focus = -1;
        rowsDirty_ = true;
        Invalidate();
    };
    StartTask(std::move(t), [st] { *st = FindStartupEntries(); });
}

bool App::StartupVisible(const StartupEntry& e) const {
    return startupShowMs_ || !e.microsoft || (e.kind != StartupKind::Service && e.kind != StartupKind::Task);
}

void App::OnStartupSwitch(int index) {
    if (task_ || index < 0 || index >= static_cast<int>(startup_.entries.size())) return;
    const StartupEntry& e = startup_.entries[index];
    if (!e.canToggle) return Toast(Tr(e.cantToggle), 2);
    bool enable = !e.enabled;
    bool admin = StartupNeedsAdmin(e);
    // This user's Run keys and Startup folder items flip at once, as in Task Manager; anything that
    // needs administrator rights (or may: tasks) is confirmed first.
    if (!admin && e.kind != StartupKind::Task && e.kind != StartupKind::Service) return ApplyStartupChange(index, enable);
    Dialog d;
    d.icon = 0xE7E8;
    d.tone = enable ? 0 : 2;
    d.title = enable ? TrF(L"Turn on {0} at startup?", e.display) : TrF(L"Turn off {0} at startup?", e.display);
    std::wstring adminNote = admin ? L" " + Tr(L"Windows asks for administrator permission.") : L"";
    switch (e.kind) {
        case StartupKind::Task:
            d.message = (enable ? Tr(L"The task is enabled again in Task Scheduler.")
                                : Tr(L"The task is disabled in Task Scheduler, not deleted: it stops running at log on or startup until it is turned on again here or in Task Scheduler.")) +
                        L" " + Tr(L"If Task Scheduler doesn't let this account change it, Windows asks for administrator permission.");
            break;
        case StartupKind::Service: {
            std::wstring automatic = (enable ? e.restoreStart == L"delayed-auto" : e.delayed) ? Tr(L"Automatic (delayed start)") : Tr(L"Automatic");
            d.message = enable ? TrF(L"Its start type is set back to {0}, so it starts with Windows again.", automatic) + adminNote
                               : TrF(L"Its start type changes from {0} to Manual: it no longer starts with Windows, but programs that need it can still start it. "
                                     L"The previous start type is kept in the journal, so it can be set back here.",
                                     automatic) + adminNote;
            break;
        }
        default:
            d.message = (enable ? Tr(L"It is marked as enabled again in StartupApproved, exactly as Task Manager does it; the entry itself stays where it is.")
                                : Tr(L"It is marked as disabled in StartupApproved, exactly as Task Manager does it; the entry itself stays where it is.")) +
                        (e.machine ? L" " + Tr(L"It applies to every user of this PC.") : L"") + adminNote;
    }
    d.lines.push_back(Tr(StartupKindName(e.kind)) + L"\t" + (e.kind == StartupKind::Task || e.kind == StartupKind::Service ? e.name : e.where));
    if (!e.command.empty()) d.lines.push_back(Tr(L"Runs") + L"\t" + e.command);
    d.buttons = {Tr(L"Cancel"), enable ? Tr(L"Turn on") : Tr(L"Turn off")};
    d.primary = 1;
    std::wstring name = e.name;
    d.onClose = [this, index, enable, name](int b, bool) {
        if (b == 1 && index < static_cast<int>(startup_.entries.size()) && startup_.entries[index].name == name) ApplyStartupChange(index, enable);
    };
    ShowDialog(std::move(d));
}

void App::ApplyStartupChange(int index, bool enable) {
    if (task_ || index < 0 || index >= static_cast<int>(startup_.entries.size())) return;
    struct State {
        StartupEntry e;
        StartupChange r;
    };
    auto st = std::make_shared<State>();
    st->e = startup_.entries[index];
    auto t = std::make_unique<Task>();
    t->title = enable ? Tr(L"Turning on at startup") : Tr(L"Turning off at startup");
    t->lines = [st] { return std::vector<std::pair<std::wstring, std::wstring>>{{L"", st->e.display}}; };
    t->onDone = [this, st, index, enable] {
        if (!st->r.ok) return Toast(Tr(st->r.error), st->r.declined ? 2 : 1);
        if (index < static_cast<int>(startup_.entries.size()) && startup_.entries[index].name == st->e.name) {
            StartupEntry& e = startup_.entries[index];
            e.enabled = enable;
            e.disabledAt = enable ? 0 : util::NowFileTime();
            if (e.kind == StartupKind::Service) {
                if (enable) {
                    e.startType = SERVICE_AUTO_START;
                    e.delayed = e.restoreStart == L"delayed-auto";
                    e.restoreStart.clear();
                } else {
                    e.startType = SERVICE_DEMAND_START;
                    e.restoreStart = e.delayed ? L"delayed-auto" : L"auto";
                }
            }
        }
        Toast(enable ? TrF(L"Turned on at startup: {0}", st->e.display)
                     : TrF(L"Turned off at startup: {0}", st->e.display) + L" · " + Tr(L"turn it on again here any time"),
              0);
    };
    StartTask(std::move(t), [st, enable] { st->r = SetStartupEnabled(st->e, enable); });
}

// =============================================================================================
// Games: every launcher's installed games (works without a scan).

void App::LoadGames() {
    if (task_) return;
    auto st = std::make_shared<std::vector<Game>>();
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Looking for games");
    t->lines = [] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{L"", L"Steam, Epic, GOG, EA app, Ubisoft Connect, Battle.net, Xbox"}};
    };
    t->onDone = [this, st] {
        games_ = std::move(*st);
        gamesLoaded_ = true;
        gameChosen_ = -1;
        gameLeftSel_.clear();
        SortGames();
    };
    StartTask(std::move(t), [st] { *st = FindGames(); });
}

void App::SortGames() {
    gameChosen_ = -1;
    gameLeftSel_.clear();
    std::stable_sort(games_.begin(), games_.end(), [this](const Game& a, const Game& b) {
        if (gamesByAge_ && a.lastPlayed != b.lastPlayed) {
            // Never-played (unknown) last, then oldest first.
            if (!a.lastPlayed || !b.lastPlayed) return a.lastPlayed != 0;
            return a.lastPlayed < b.lastPlayed;
        }
        return a.bytes > b.bytes;
    });
    rowsDirty_ = true;
    Invalidate();
}

// An installed game is chosen for Uninstall (one at a time: its launcher asks); leftovers are checked.
void App::OnGameRow(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[row].kind != RK_GAME) return;
    size_t i = static_cast<size_t>(rows_[row].idx);
    if (i >= games_.size()) return;
    lists_[static_cast<int>(view_)].focus = row;
    if (games_[i].leftover) {
        if (!gameLeftSel_.erase(i)) gameLeftSel_.insert(i);
    } else {
        gameChosen_ = gameChosen_ == static_cast<int>(i) ? -1 : static_cast<int>(i);
    }
}

void App::StartGameUninstall() {
    if (gameChosen_ < 0 || gameChosen_ >= static_cast<int>(games_.size())) return Toast(Tr(L"Choose a game first"), 2);
    Game g = games_[gameChosen_];
    Dialog d;
    d.title = TrF(L"Uninstall {0} ({1})?", g.name, util::FormatSizeShort(g.bytes));
    d.message = TrF(L"{0}, just as from its own library. WinNCDU deletes nothing itself here; press Refresh when it is done.", Tr(g.uninstallHow));
    d.icon = 0xE7FC;
    d.tone = 1;
    d.buttons = {Tr(L"Cancel"), Tr(L"Uninstall")};
    d.primary = 1;
    d.onClose = [this, g](int b, bool) {
        if (b != 1) return;
        std::wstring err;
        if (StartUninstall(g, err))
            Toast(TrF(L"Finish in {0}, then press Refresh", g.launcher), 0);
        else
            Toast(Tr(err), 1);
    };
    ShowDialog(std::move(d));
}

void App::StartGameLeftoversDelete() {
    std::vector<Game> chosen;
    uint64_t bytes = 0;
    for (size_t i : gameLeftSel_)
        if (i < games_.size() && games_[i].leftover) {
            chosen.push_back(games_[i]);
            bytes += games_[i].bytes;
        }
    if (chosen.empty()) return;
    Dialog d;
    d.title = TrN(chosen.size(), L"Move {0} leftover ({1}) to the Recycle Bin?", L"Move {0} leftovers ({1}) to the Recycle Bin?",
                  util::FormatSizeShort(bytes));
    d.message = Tr(L"No launcher has these games installed any more. If a game still keeps saves or settings in its folder, you can "
                   L"restore it from Recently deleted.");
    for (size_t i = 0; i < chosen.size() && i < 8; ++i) d.lines.push_back(util::FormatSizeShort(chosen[i].bytes) + L"\t" + chosen[i].folder);
    if (chosen.size() > 8) d.lines.push_back(L"\t" + TrN(chosen.size() - 8, L"… and {0} more", L"… and {0} more"));
    d.icon = 0xE74D;
    d.tone = 1;
    d.buttons = {Tr(L"Cancel"), Tr(L"Move to Recycle Bin")};
    d.primary = 1;
    d.onClose = [this, chosen](int b, bool) {
        if (b != 1) return;
        struct State {
            std::vector<Game> items;
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
            return std::vector<std::pair<std::wstring, std::wstring>>{{L"", st->items[i].folder}};
        };
        t->progress = [st] { return static_cast<double>(st->index) / st->items.size(); };
        t->onDone = [this, st] {
            Toast(st->errors.empty() ? TrF(L"Moved {0} of leftovers to the Recycle Bin", util::FormatSizeShort(st->freed))
                                     : st->errors[0],
                  st->errors.empty() ? 0 : 1);
            LoadGames();
        };
        StartTask(std::move(t), [st] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            RefreshProtection();
            for (size_t i = 0; i < st->items.size(); ++i) {
                st->index = i;
                const Game& g = st->items[i];
                ProtectReport pr = CheckProtection(g.folder);
                if (pr.level == Protect::Important || pr.level == Protect::Blocked) {
                    st->errors.push_back(TrF(L"{0} is protected and was left alone", g.folder));
                    continue;
                }
                std::wstring err;
                if (!DeleteToRecycleBin(g.folder, err)) {
                    st->errors.push_back(g.folder + L": " + err);
                    continue;
                }
                st->freed += g.bytes;
                JournalEntry je;
                je.action = JournalAction::Recycle;
                je.bytes = g.bytes;
                je.items = 1;
                je.path = g.folder;
                je.note = L"leftover of an uninstalled " + g.launcher + L" game";
                JournalAppend(je);
            }
            CoUninitialize();
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
// Disk health: physical disks, TRIM and used space over time (works without a scan; read-only).

void App::LoadDiskHealth() {
    if (task_) return;
    struct State {
        DiskHealthReport rep;
        std::vector<DriveTrend> trends;
    };
    auto st = std::make_shared<State>();
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Asking the disks");
    t->lines = [] { return std::vector<std::pair<std::wstring, std::wstring>>{{L"", Tr(L"Health, wear, temperature and TRIM (read-only)")}}; };
    t->onDone = [this, st] {
        disk_ = std::move(st->rep);
        diskTrends_ = std::move(st->trends);
        diskLoaded_ = true;
        Invalidate();
    };
    StartTask(std::move(t), [st] {
        std::vector<DriveInfo> drives = ListDrives();
        std::vector<std::wstring> roots;
        for (auto& d : drives)
            if (d.ready && d.type == DRIVE_FIXED) roots.push_back(d.root);
        RecordFreeSpace(roots);
        st->rep = QueryDiskHealth();
        auto log = LoadFreeSpaceLog();
        auto snaps = ListSnapshots();
        int64_t now = util::NowFileTime();
        for (auto& d : drives) {
            if (!d.ready || d.type != DRIVE_FIXED) continue;
            std::vector<UsagePoint> scans;  // earlier scans of the drive root
            for (auto& s : snaps)
                if (s.root.size() >= 2 && s.root.size() <= 3 && s.root[1] == L':' && towupper(s.root[0]) == towupper(d.root[0]))
                    scans.push_back({s.time, s.disk});
            st->trends.push_back(BuildDriveTrend(d, VolumeSerial(d.root), log, scans, now));
        }
    });
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
    l.push_back({Tr(L"Files"), util::FormatCount(s.files)});
    l.push_back({Tr(L"Folders"), util::FormatCount(s.dirs)});
    l.push_back({Tr(L"Size"), util::FormatSizeShort(s.bytes)});
    uint64_t rate = secs > 0.2 ? static_cast<uint64_t>((s.files + s.dirs) / secs + 0.5) : 0;
    l.push_back({Tr(L"Elapsed"), util::FormatDuration(secs) + L"  ·  " + TrF(L"{0} items/s", util::FormatCount(rate))});
    if (s.errors) l.push_back({Tr(L"Skipped"), TrN(s.errors, L"{0} unreadable folder", L"{0} unreadable folders")});
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
    d.title = Tr(L"Start a new scan?");
    d.message = TrF(L"Scan {0}. The current results will be replaced (they stay in History).", path);
    d.icon = 0xE721;
    d.buttons = {Tr(L"Cancel"), Tr(L"Scan")};
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
        d.title = Tr(L"Can't scan this location");
        d.message = input + L"\n" + Tr(err);
        d.icon = 0xE7BA;
        d.tone = 1;
        d.buttons = {Tr(L"OK")};
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
    t->title = TrF(L"Scanning {0}", path);
    ULONGLONG start = GetTickCount64();
    t->lines = [stats, start] { return ScanLines(*stats, start); };
    t->cancel = [stats] { stats->cancel = true; };
    t->onDone = [this, holder, stats, start] {
        SetRoot(std::move(*holder), false);
        incomplete_ = stats->cancel.load();
        scanErrors_ = stats->errors.load();
        scanSeconds_ = (GetTickCount64() - start) / 1000.0;
        LoadHistory();
        Toast(incomplete_ ? Tr(L"Scan stopped — showing partial results")
                          : TrN(root_->items, L"Scanned {0} item in {1}", L"Scanned {0} items in {1}", util::FormatDuration(scanSeconds_)),
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
        RecordFreeSpace({path});  // Disk health's used-space trend
    });
}

void App::StartRescan(std::vector<Node*> dirs) {
    if (readOnly_) return Toast(Tr(L"Imported scans can't be rescanned"), 1);
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
    t->title = jobs->size() == 1 ? TrF(L"Rescanning {0}", (*jobs)[0].path) : TrN(jobs->size(), L"Rescanning {0} folder", L"Rescanning {0} folders");
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
        Toast(cancelled ? Tr(L"Rescan cancelled") : Tr(L"Rescan complete"), cancelled ? 2 : 0);
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
    return Tr(h.group) + L"\t" + DescribeHit(c);
}

}  // namespace

// Deleting goes through the protection rules first (protect.cpp):
//   Blocked   -> never deleted; the rest of the selection can still go ahead
//   Important -> an extra warning step, and the Recycle Bin is preselected
//   Caution   -> a heads-up inside the normal delete dialog
void App::RequestDelete(bool permanent) {
    if (readOnly_) return Toast(Tr(L"Deleting isn't available for imported scans"), 1);
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
                if (firstBlockedReason.empty()) firstBlockedReason = Tr(h.reason);
                if (blockedLines.size() < 6) blockedLines.push_back(Tr(h.group) + L"\t" + NodePath(n) + L" · " + Tr(h.name));
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
                if (heads.empty()) heads = TrF(L"{0}: {1}", Tr(h.name), Tr(h.reason));
                if (std::find(notes.begin(), notes.end(), line) == notes.end() && notes.size() < 3) notes.push_back(line);
            }
        }
    }
    for (auto& l : blockedLines) notes.insert(notes.begin(), Tr(L"Skipped") + l.substr(l.find(L'\t')));

    if (allowed.empty()) {
        Dialog d;
        d.icon = 0xEA18;
        d.tone = 1;
        d.title = targets.size() == 1 ? Tr(L"This location is protected") : Tr(L"These locations are protected");
        d.message = firstBlockedReason + L"\n\n" + Tr(L"WinNCDU never deletes system folders. Edit protect.txt only if you are sure "
                                                      L"(menu ⋯ → Protected locations).");
        d.lines = blockedLines;
        d.buttons = {Tr(L"OK")};
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
    d.title = ai ? Tr(L"This deletes AI assistant data") : Tr(L"This contains data you may not get back");
    d.message = Tr(important[0].reason);
    if (important.size() > 1)
        d.message += L" " + TrN(important.size() - 1, L"(+{0} more below)", L"(+{0} more below)");
    d.message += L"\n\n" + Tr(L"If you continue, the Recycle Bin is preselected so you can still restore it.");
    if (!blockedLines.empty())
        d.message += L" " + TrN(blockedLines.size(), L"{0} protected system item will be skipped.", L"{0} protected system items will be skipped.");
    d.lines = warnLines;
    d.buttons = {Tr(L"Cancel"), Tr(L"Continue…")};
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
    d.checkText = Tr(L"Delete permanently (skip the Recycle Bin)");
    size_t shown = notes.empty() ? 6 : 4;
    for (size_t i = 0; i < targets.size() && i < shown; ++i)
        d.lines.push_back(util::FormatSizeShort(Val(targets[i])) + L"\t" + NodePath(targets[i]));
    if (targets.size() > shown) d.lines.push_back(L"\t" + TrN(targets.size() - shown, L"… and {0} more", L"… and {0} more"));
    for (auto& n : notes) d.lines.push_back(n);
    // Whole sentences per case: in other languages "this folder" and "3 items" change the words around them.
    size_t count = targets.size();
    bool folder = count == 1 && targets[0]->IsDir();
    if (!heads.empty()) heads = L"\n" + TrF(L"Heads-up — {0}", heads);
    auto refresh = [count, folder, total, worst, heads](Dialog& dd) {
        if (dd.checked)
            dd.title = count > 1 ? TrN(count, L"Permanently delete {0} item?", L"Permanently delete {0} items?")
                       : folder  ? Tr(L"Permanently delete this folder?")
                                 : Tr(L"Permanently delete this file?");
        else
            dd.title = count > 1 ? TrN(count, L"Move {0} item to the Recycle Bin?", L"Move {0} items to the Recycle Bin?")
                       : folder  ? Tr(L"Move this folder to the Recycle Bin?")
                                 : Tr(L"Move this file to the Recycle Bin?");
        dd.message = dd.checked ? TrF(L"{0} will be freed. This can't be undone.", util::FormatSizeShort(total))
                                : TrF(L"{0} · you can restore it from the Recycle Bin.", util::FormatSizeShort(total));
        dd.message += heads;
        dd.tone = dd.checked ? 1 : (worst != Protect::None ? 2 : 0);
        dd.buttons = {Tr(L"Cancel"), dd.checked ? Tr(L"Delete permanently") : Tr(L"Move to Recycle Bin")};
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
    if (targets.empty()) return Toast(Tr(L"Protected locations are never deleted"), 1);
    struct State {
        std::vector<std::wstring> paths, errors;
        std::vector<int64_t> times;  // when each item was done (for the journal)
        std::vector<std::vector<std::wstring>> lockedBy;  // apps holding files of items that failed
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
    t->title = permanent ? Tr(L"Deleting") : Tr(L"Moving to the Recycle Bin");
    t->lines = [st, permanent] {
        std::vector<std::pair<std::wstring, std::wstring>> l;
        size_t i = std::min(st->index.load(), st->paths.size() - 1);
        l.push_back({Tr(L"Item"), TrF(L"{0} of {1}", util::FormatCount(i + 1), util::FormatCount(st->paths.size()))});
        if (permanent) l.push_back({Tr(L"Removed"), TrN(st->removed, L"{0} file or folder", L"{0} files and folders")});
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
                if (permanent) {  // some of it is gone for good: that belongs in the journal too (in English)
                    i18n::EnglishScope english;
                    je.note = L"partly deleted: " + st->errors[i] + (st->lockedBy[i].empty() ? L"" : L" (in use by " + AppNames(st->lockedBy[i]) + L")");
                    JournalAppend(je);
                }
                partial.push_back(n);
            }
            if (!st->errors[i].empty() && errs.size() < 8) {
                errs.push_back(st->errors[i]);
                if (!st->lockedBy[i].empty()) errs.push_back(TrF(L"In use by {0}", AppNames(st->lockedBy[i])));
            }
            for (auto& a : st->lockedBy[i])
                if (std::find(apps.begin(), apps.end(), a) == apps.end()) apps.push_back(a);
        }
        PostMutation();
        if (deleted)
            Toast(permanent ? TrN(deleted, L"Deleted {0} item · {1} freed", L"Deleted {0} items · {1} freed", util::FormatSizeShort(freed))
                            : TrN(deleted, L"Moved to Recycle Bin: {0} item · {1}", L"Moved to Recycle Bin: {0} items · {1}", util::FormatSizeShort(freed)),
                  0);
        if (!errs.empty()) {
            Dialog d;
            d.title = Tr(L"Some items could not be deleted");
            d.message = apps.empty() ? Tr(L"They may be in use or need administrator rights.")
                                     : TrF(L"Close {0}, then try again.", AppNames(apps));
            d.lines = errs;
            d.icon = 0xE7BA;
            d.tone = 2;
            d.buttons = {Tr(L"OK")};
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
            if (!ok) st->errors[i] = err.empty() ? TrF(L"{0}: failed", st->paths[i]) : err;
            st->times[i] = util::NowFileTime();
            if (!ok && PathExists(st->paths[i])) {
                for (auto& a : AppsUsing(st->paths[i])) st->lockedBy[i].push_back(a.name);
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
    t->title = Tr(L"Opening saved scan");
    t->lines = [count, file] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{Tr(L"Items"), util::FormatCount(*count)}, {L"", file}};
    };
    t->onDone = [this, result, err] {
        if (!*result) {
            Dialog d;
            d.title = Tr(L"Can't open this file");
            d.message = *err;
            d.icon = 0xE7BA;
            d.tone = 1;
            d.buttons = {Tr(L"OK")};
            d.primary = 0;
            ShowDialog(std::move(d));
            return;
        }
        SetRoot(std::move(*result), true);
        incomplete_ = false;
        scanErrors_ = 0;
        Toast(TrN(root_->items, L"Opened {0} item (read-only)", L"Opened {0} items (read-only)"), 2);
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
    t->title = csv ? Tr(L"Exporting CSV") : Tr(L"Saving scan");
    t->lines = [count, file] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{Tr(L"Written"), TrN(*count, L"{0} item", L"{0} items")}, {L"", file}};
    };
    t->progress = [count, total] { return static_cast<double>(*count) / total; };
    t->onDone = [this, ok, err, file] {
        if (*ok)
            Toast(TrF(L"Saved {0}", file), 0);
        else
            Toast(TrF(L"Export failed: {0}", *err), 1);
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
    t->title = Tr(L"Looking for duplicate files");
    t->lines = [prog] {
        static const wchar_t* stages[] = {N_(L"Grouping files by size"), N_(L"Quick check (first 64 KB)"), N_(L"Comparing contents (SHA-256)")};
        return std::vector<std::pair<std::wstring, std::wstring>>{
            {Tr(L"Stage"), Tr(stages[std::clamp(prog->stage.load(), 0, 2)])},
            {Tr(L"Files"), util::FormatCount(prog->filesDone) + L" / " + util::FormatCount(prog->filesTotal)},
            {Tr(L"Read"), util::FormatSizeShort(prog->bytesDone) + L" / " + util::FormatSizeShort(prog->bytesTotal)}};
    };
    t->progress = [prog] {
        uint64_t tot = prog->bytesTotal;
        return tot ? static_cast<double>(prog->bytesDone) / tot : -1.0;
    };
    t->cancel = [prog] { prog->cancel = true; };
    t->onDone = [this, prog, result] {
        if (prog->cancel) return Toast(Tr(L"Duplicate search cancelled"), 2);
        dups_ = std::move(*result);
        dupsDone_ = true;
        rowsDirty_ = true;
        lists_[static_cast<int>(View::Duplicates)] = ListState{};
        if (dups_.empty()) Toast(Tr(L"No duplicate files found"), 0);
    };
    Node* scope = root_.get();
    uint64_t minSize = dupMin_;
    int threads = MakeOptions(settings_).threads;
    StartTask(std::move(t), [scope, minSize, threads, prog, result] { *result = FindDuplicates(scope, minSize, threads, *prog); });
}

void App::StartSimilarPhotos() {
    if (!root_ || readOnly_) return;
    if (!photoCache_) photoCache_ = std::make_shared<PhotoHashCache>(PhotoHashCache::DefaultFile());
    auto prog = std::make_shared<PhotoProgress>();
    auto result = std::make_shared<std::vector<PhotoGroup>>();
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Looking for similar photos");
    t->lines = [prog] {
        static const wchar_t* stages[] = {N_(L"Finding images"), N_(L"Reading images (small, read-only)"), N_(L"Comparing pictures")};
        std::vector<std::pair<std::wstring, std::wstring>> l = {
            {Tr(L"Stage"), Tr(stages[std::clamp(prog->stage.load(), 0, 2)])},
            {Tr(L"Images"), util::FormatCount(prog->filesDone) + L" / " + util::FormatCount(prog->filesTotal)}};
        if (prog->cached) l.push_back({Tr(L"Remembered"), TrF(L"{0} unchanged since an earlier search", util::FormatCount(prog->cached))});
        if (prog->failed) l.push_back({Tr(L"Unreadable"), TrF(L"{0} (no decoder, damaged or offline)", util::FormatCount(prog->failed))});
        return l;
    };
    t->progress = [prog] {
        uint64_t tot = prog->filesTotal;
        return tot && prog->stage == 1 ? static_cast<double>(prog->filesDone) / tot : -1.0;
    };
    t->cancel = [prog] { prog->cancel = true; };
    t->onDone = [this, prog, result] {
        if (prog->cancel) return Toast(Tr(L"Photo search cancelled"), 2);
        photos_ = std::move(*result);
        photosDone_ = true;
        photoImages_ = prog->filesTotal;
        photoFailed_ = prog->failed;
        sel_.clear();
        rowsDirty_ = true;
        lists_[static_cast<int>(View::Duplicates)] = ListState{};
        if (photos_.empty()) Toast(photoImages_ ? Tr(L"No similar photos found") : Tr(L"No images in this scan"), 0);
    };
    Node* scope = root_.get();
    uint64_t minSize = photoMin_;
    PhotoMatch match = photoMatch_;
    int threads = MakeOptions(settings_).threads;
    auto cache = photoCache_;
    StartTask(std::move(t), [scope, minSize, match, threads, prog, result, cache] {
        *result = FindSimilarPhotos(scope, minSize, match, threads, *prog, cache.get());
    });
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
            c.keep = old->keep;
        } else {
            c.selected = c.safe && !(c.needsAdmin && !elevated_) && !c.external;
        }
        if (c.id == reviewId) reviewCat_ = static_cast<int>(i);
    }
    cleanup_ = std::move(fresh);
    rowsDirty_ = true;
    if (!errs.empty())
        Toast(errs[0] + (errs.size() > 1 ? L"  " + TrN(errs.size() - 1, L"(+{0} more)", L"(+{0} more)") : L""), 1);
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
    if (chosen.empty()) return Toast(Tr(L"Select at least one category with something to clean"), 2);
    Dialog d;
    d.title = TrF(L"Clean up {0}?", util::FormatSizeShort(total));
    d.message = Tr(L"Exactly the files listed under Review are deleted, permanently. Files that are in use, or "
                   L"that changed since they were measured, are skipped.");
    bool commands = false;
    for (size_t i : chosen) {
        size_t kept = cleanup_[i].KeptCount();
        const std::wstring& cmd = cleanup_[i].command;
        commands = commands || !cmd.empty();
        std::wstring extra = !cmd.empty() ? L"  " + TrF(L"(runs {0})", cmd) : kept ? L"  " + TrN(kept, L"({0} kept)", L"({0} kept)") : L"";
        if (cleanup_[i].browserData == BrowserData::Cookies)
            extra = L"  " + TrN(cleanup_[i].keep.size(), L"(you stay signed in to {0} listed site)", L"(you stay signed in to {0} listed sites)");
        d.lines.push_back(util::FormatSizeShort(cleanup_[i].ToClean()) + L"\t" + CatName(cleanup_[i]) + extra);
    }
    if (commands) d.message += L" " + Tr(L"Categories marked “runs” are cleaned by that tool's own command instead.");
    bool browser = std::any_of(chosen.begin(), chosen.end(), [&](size_t i) { return cleanup_[i].browserData != BrowserData::None; });
    if (browser)
        d.message += L" " + Tr(L"Cookies and history are removed inside the browsers' own databases; close the browsers first, "
                               L"or they are skipped.");
    d.icon = 0xE74D;
    d.tone = 1;
    d.buttons = {Tr(L"Cancel"), Tr(L"Clean up")};
    d.primary = 1;
    d.danger = 1;
    // System-level categories (Windows temp, updates) can come with a restore point first.
    bool system = false;
    for (size_t i : chosen) system = system || cleanup_[i].needsAdmin;
    if (system && elevated_) {
        d.hasCheck = true;
        d.checked = true;
        d.checkText = Tr(L"Create a System Restore point first");
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
        t->title = Tr(L"Cleaning up");
        t->lines = [st] {
            size_t i = std::min(st->index.load(), st->cats.size() - 1);
            std::lock_guard<std::mutex> lk(st->m);
            return std::vector<std::pair<std::wstring, std::wstring>>{
                {Tr(L"Category"), CatName(st->cats[i])}, {Tr(L"Freed"), util::FormatSizeShort(st->freed)}, {L"", st->current}};
        };
        t->progress = [st] { return static_cast<double>(st->index) / st->cats.size(); };
        t->cancel = [st] { st->cancel = true; };
        t->onDone = [this, st] {
            if (!st->restoreErr.empty()) {
                Dialog e;
                e.title = Tr(L"No restore point, so nothing was cleaned");
                e.message = st->restoreErr + L"\n\n" + Tr(L"Clean up again without the restore point option, or turn System Protection on first.");
                e.icon = 0xE7BA;
                e.tone = 2;
                e.buttons = {Tr(L"OK")};
                e.primary = 0;
                ShowDialog(std::move(e));
                return;
            }
            std::wstring freed = TrF(L"Cleanup freed {0}", util::FormatSizeShort(st->freed));
            if (!st->errors.empty())
                Toast(freed + L" · " + st->errors[0], 1);
            else
                Toast(freed + (st->failed ? L" · " + TrN(st->failed, L"{0} item in use was skipped", L"{0} items in use were skipped") +
                                                (st->lockedBy.empty() ? L"" : L" " + TrF(L"(close {0})", AppNames(st->lockedBy)))
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
                    st->current = Tr(L"Creating a System Restore point…");
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
                CleanResult r = CleanAndJournal(c, st->cancel, [&](const std::wstring& p) {
                    std::lock_guard<std::mutex> lk(st->m);
                    st->current = p;
                });
                st->freed += r.freed;
                st->failed += r.failed;
                st->lockedBy.insert(st->lockedBy.end(), r.lockedBy.begin(), r.lockedBy.end());
                if (!r.error.empty()) st->errors.push_back(TrF(L"{0}: {1}", CatName(c), r.error));
            }
        });
    };
    ShowDialog(std::move(d));
}

void App::LoadSchedule() {
    TaskInfo info;
    std::wstring err;
    if (TaskQuery(ScheduleFolder(), kScheduleTask, info, err)) {
        schedule_ = info;
        scheduleErr_.clear();
    } else {
        schedule_ = TaskInfo{};
        scheduleErr_ = err;
    }
    Invalidate();
}

// A per-user Task Scheduler task that runs `winncdu-cli --clean <checked ids> --yes --quiet`.
void App::ShowScheduleDialog() {
    LoadSchedule();
    if (!scheduleErr_.empty()) return Toast(scheduleErr_, 1);
    // What can run unattended: checked categories that need no administrator rights (the task never
    // runs elevated) and aren't community rules (those are reviewed by hand first).
    std::vector<std::wstring> ids, leftOut;
    std::vector<std::wstring> lines;
    for (auto& c : cleanup_) {
        if (!c.selected || c.external) continue;
        if (c.needsAdmin)
            leftOut.push_back(TrF(L"{0} (needs administrator rights)", CatName(c)));
        else if (c.imported)
            leftOut.push_back(TrF(L"{0} (winapp2.ini rules are reviewed by hand)", CatName(c)));
        else {
            ids.push_back(c.id);
            lines.push_back((c.measured ? util::FormatSizeShort(c.ToClean()) : L"…") + L"\t" + CatName(c));
        }
    }
    bool exists = schedule_.exists;
    if (ids.empty() && exists && !schedule_.spec.ids.empty()) {
        ids = schedule_.spec.ids;  // nothing checked: keep what the schedule cleans
        for (auto& id : ids) {
            auto it = std::find_if(cleanup_.begin(), cleanup_.end(), [&](const CleanCategory& c) { return c.id == id; });
            lines.push_back(L"\t" + (it != cleanup_.end() ? CatName(*it) : id));
        }
    }
    if (ids.empty() && !exists) return Toast(Tr(L"Check the categories to clean on a schedule first"), 2);
    std::wstring exe = ExePath();
    std::wstring cli = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"winncdu-cli.exe";
    if (!PathExists(cli)) return Toast(Tr(L"winncdu-cli.exe was not found next to winncdu.exe"), 1);

    ScheduleSpec spec;
    spec.freq = ScheduleFreq::Weekly;
    spec.weekday = 0;
    spec.hour = 12;
    if (exists && schedule_.specKnown) spec = schedule_.spec;
    Dialog d;
    d.title = exists ? Tr(L"Scheduled cleanup") : Tr(L"Schedule cleanup");
    d.icon = 0xE787;  // calendar
    d.message = TrF(L"Windows runs “winncdu-cli {0}” as you, without administrator rights, only while you're signed in and never on battery; a missed run starts as "
                    L"soon as possible. Each run deletes what these categories list at that moment, permanently. Protected items are "
                    L"never touched, but items unchecked under Review are not remembered. Every run is journaled and logged in "
                    L"cleanup.log.",
                    CleanArguments(ids));
    if (!leftOut.empty()) {
        std::wstring s;
        for (auto& l : leftOut) s += (s.empty() ? L"" : L", ") + l;
        d.message += L"\n\n" + TrF(L"Left out: {0}.", s);
    }
    if (ids.empty()) d.message = Tr(L"No categories are checked. Remove the schedule, or check categories and come back to change it.");
    d.lines = lines;
    DialogChoice repeat{Tr(L"Repeat"), {Tr(L"Daily"), Tr(L"Weekly"), Tr(L"Monthly"), Tr(L"At logon")}, static_cast<int>(spec.freq)};
    DialogChoice day{Tr(L"Day"), {Tr(L"Sun"), Tr(L"Mon"), Tr(L"Tue"), Tr(L"Wed"), Tr(L"Thu"), Tr(L"Fri"), Tr(L"Sat")}, std::clamp(spec.weekday, 0, 6)};
    DialogChoice mday{Tr(L"Day"), {}, std::clamp(spec.monthDay, 1, 28) - 1, true};
    for (int i = 1; i <= 28; ++i) mday.options.push_back(TrF(L"day {0}", i));
    DialogChoice time{Tr(L"Time"), {}, spec.hour * 2 + (spec.minute >= 30 ? 1 : 0), true};
    for (int i = 0; i < 48; ++i) {
        wchar_t t[8];
        swprintf(t, 8, L"%02d:%02d", i / 2, i % 2 ? 30 : 0);
        time.options.push_back(t);
    }
    d.choices = {repeat, day, mday, time};
    d.onChoice = [](Dialog& dd) {
        int f = dd.choices[0].value;
        dd.choices[1].hidden = f != static_cast<int>(ScheduleFreq::Weekly);
        dd.choices[2].hidden = f != static_cast<int>(ScheduleFreq::Monthly);
        dd.choices[3].hidden = f == static_cast<int>(ScheduleFreq::Logon);
    };
    d.onChoice(d);
    if (ids.empty()) d.choices.clear();
    if (exists) {
        d.buttons = ids.empty() ? std::vector<std::wstring>{Tr(L"Cancel"), Tr(L"Remove schedule")}
                                : std::vector<std::wstring>{Tr(L"Cancel"), Tr(L"Remove schedule"), Tr(L"Save")};
        d.primary = ids.empty() ? -1 : 2;
    } else {
        d.buttons = {Tr(L"Cancel"), Tr(L"Schedule")};
        d.primary = 1;
    }
    d.cancel = 0;
    d.onCloseWith = [this, ids, cli, exists](int b, const Dialog& dd) {
        if (b == 0) return;
        std::wstring err;
        std::wstring where = ScheduleFolder() + L"\\" + kScheduleTask;
        if (exists && b == 1) {
            if (TaskRemove(ScheduleFolder(), kScheduleTask, err)) {
                CleanLogAppend(L"schedule removed (" + where + L")");
                Toast(Tr(L"Scheduled cleanup removed"), 0);
            } else {
                Toast(err, 1);
            }
            LoadSchedule();
            return;
        }
        if (dd.choices.size() < 4) return;
        ScheduleSpec s;
        s.freq = static_cast<ScheduleFreq>(std::clamp(dd.choices[0].value, 0, 3));
        s.weekday = dd.choices[1].value;
        s.monthDay = dd.choices[2].value + 1;
        s.hour = dd.choices[3].value / 2;
        s.minute = dd.choices[3].value % 2 ? 30 : 0;
        s.ids = ids;
        std::wstring desc = L"WinNCDU cleanup of " + JoinIds(ids) + L" (deleted permanently, protected items never). Change or remove it in "
                            L"WinNCDU: Cleanup, Schedule.";
        if (TaskSave(ScheduleFolder(), kScheduleTask, s, cli, CleanArguments(ids), desc, err)) {
            std::wstring english;  // cleanup.log is kept in English
            {
                i18n::EnglishScope en;
                english = DescribeSchedule(s);
            }
            CleanLogAppend(L"schedule " + std::wstring(exists ? L"changed: " : L"set: ") + english + L", --clean " + JoinIds(ids) +
                           L" (" + where + L")");
            Toast(TrF(L"Cleanup scheduled: {0}", DescribeSchedule(s)), 0);
        } else {
            Toast(err, 1);
        }
        LoadSchedule();
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
        v != View::Wsl && v != View::WinTools && v != View::Games && v != View::Startup && v != View::Apps && v != View::CleanReview && v != View::DiskHealth && !root_)
        return Toast(Tr(L"Scan a drive or folder first"), 2);
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
            LoadSchedule();
            break;
        case View::History: LoadHistory(); break;
        case View::Deleted: LoadDeleted(); break;
        case View::AiModels:
            if (!aiLoaded_) LoadAiModels();
            break;
        case View::Wsl:
            if (!wslLoaded_) LoadWsl();
            break;
        case View::WinTools:
            if (!winLoaded_) LoadWinTools();
            break;
        case View::Games:
            if (!gamesLoaded_) LoadGames();
            break;
        case View::DiskHealth:
            if (!diskLoaded_) LoadDiskHealth();
            break;
        case View::Startup:
            if (!startupLoaded_) LoadStartup();
            break;
        case View::Apps:
            if (!appsLoaded_) LoadApps();
            break;
        default: break;
    }
    SyncSearchBox();
    rowsDirty_ = true;
    Invalidate();
}

void App::RunSearchQuery(const std::wstring& text) {
    if (!root_) return Toast(Tr(L"Scan a drive or folder first"), 2);
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
    if (view_ == View::Wsl) {
        if (r.kind == RK_WSL && r.idx < static_cast<int>(vdisks_.size())) RevealInExplorer(vdisks_[r.idx].path);
        return;
    }
    if (view_ == View::Games) {
        if (r.kind == RK_GAME && r.idx < static_cast<int>(games_.size()) && !games_[r.idx].folder.empty())
            OpenWithDefault(games_[r.idx].folder);
        return;
    }
    if (view_ == View::Startup) {
        if (r.kind == RK_STARTUP && r.idx < static_cast<int>(startup_.entries.size())) {
            const StartupEntry& e = startup_.entries[r.idx];
            if (e.programFound)
                RevealInExplorer(e.program);
            else
                Toast(e.program.empty() ? Tr(L"No program to show") : TrF(L"Not found: {0}", e.program), 2);
        }
        return;
    }
    if (view_ == View::Apps) {
        if (r.kind == RK_APP && r.idx < static_cast<int>(apps_.size())) {
            const std::wstring& f = apps_[r.idx].folder;
            if (f.empty() || !PathExists(f)) return Toast(TrF(L"No install folder is known for {0}", apps_[r.idx].e.displayName), 2);
            OpenWithDefault(f);
        } else if (r.kind == RK_APPLEFT && r.idx < static_cast<int>(appLeft_.size())) {
            OpenWithDefault(appLeft_[r.idx].path);
        }
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
    if (!LoadSnapshot(snapshots_[index].file, old, info)) return Toast(Tr(L"Could not read this snapshot"), 1);
    if (_wcsicmp(info.root.c_str(), root_->name.c_str()) != 0)
        return Toast(TrF(L"This snapshot belongs to {0} — scan it to compare", info.root), 2);
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
        d.title = Tr(L"Can't restore this item");
        d.message = Tr(err);
        d.icon = 0xE7BA;
        d.tone = 2;
        d.buttons = {Tr(L"OK")};
        d.primary = 0;
        ShowDialog(std::move(d));
        LoadDeleted();
        return;
    }
    size_t slash = it.original.find_last_of(L'\\');
    Toast(TrF(L"Restored {0}", slash == std::wstring::npos ? it.original : it.original.substr(slash + 1)), 0);
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
    if (acc_) acc_->Notify(text);  // screen readers say it
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
                lines.push_back(Tr(h.group) + L"\t" + Tr(h.name) + L" · " + h.path);
            else
                ++more;
        }
    if (more) lines.push_back(L"\t" + TrN(more, L"… and {0} more", L"… and {0} more"));
    Dialog d;
    d.icon = 0xEA18;
    d.title = Tr(L"Protected locations");
    d.message = Tr(L"System folders are never deleted. For the data below (and Git repositories, password databases, keys "
                   L"and wallets anywhere) WinNCDU asks twice and preselects the Recycle Bin. Add your own rules in protect.txt.");
    d.lines = lines.empty() ? std::vector<std::wstring>{L"\t" + Tr(L"No AI, key, wallet or synced folders found on this PC.")} : lines;
    d.buttons = {Tr(L"Edit protect.txt"), Tr(L"OK")};
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
    ++dialogGen_;  // a new dialog: new accessible elements (its title may change while it is open)
    Invalidate();
}

void App::ShowAbout() {
    Dialog d;
    d.title = L"WinNCDU " WINNCDU_VERSION;
    d.message = Tr(L"A fast disk usage analyzer for Windows, inspired by ncdu.") + L"\n\n" +
                Tr(L"Shortcuts:  Enter open · Backspace up · Del delete · Shift+Del delete permanently · "
                   L"Ctrl+F search · Ctrl+E show in Explorer · Ctrl+C copy path · F5 rescan · Alt+←/→ back/forward.") +
                L"\n\n" + Tr(L"Search syntax:") + L"  *.iso   >1G   <10M   older:1y   newer:7d   type:f   type:d   ext:mp4,mkv   empty";
    d.icon = 0xE946;
    d.buttons = {Tr(L"Close")};
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
    auto add = [&](UINT flags, UINT_PTR id, const std::wstring& text) { AppendMenuW(m, flags, id, text.c_str()); };
    if (single) {
        if (n->CanEnter() && view_ == View::Browse) add(MF_STRING, CMD_OPEN, Tr(L"Open\tEnter"));
        if (view_ != View::Browse) add(MF_STRING, CMD_GOTO, Tr(L"Go to in folder view"));
        add(MF_STRING, CMD_OPEN_DEFAULT, n->IsDir() ? Tr(L"Open in Explorer") : Tr(L"Open with default app"));
        add(MF_STRING, CMD_REVEAL, Tr(L"Show in Explorer\tCtrl+E"));
    }
    add(MF_STRING, CMD_COPY_PATH, single ? Tr(L"Copy path\tCtrl+C") : Tr(L"Copy paths\tCtrl+C"));
    if (single) {
        add(MF_STRING, CMD_TERMINAL, Tr(L"Open terminal here"));
        add(MF_STRING, CMD_PROPERTIES, Tr(L"Properties"));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (!readOnly_) {
        add(MF_STRING, CMD_RESCAN_ITEM, Tr(L"Rescan"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        add(MF_STRING, CMD_DELETE, Tr(L"Move to Recycle Bin\tDel"));
        add(MF_STRING, CMD_DELETE_PERM, Tr(L"Delete permanently\tShift+Del"));
    }
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd) OnCommand(cmd);
}

void App::ShowMoreMenu() {
    HMENU m = CreatePopupMenu();
    HMENU theme = CreatePopupMenu();
    HMENU lang = CreatePopupMenu();
    auto add = [](HMENU menu, UINT flags, UINT_PTR id, const std::wstring& text) { AppendMenuW(menu, flags, id, text.c_str()); };
    add(theme, MF_STRING | (settings_.theme == 0 ? MF_CHECKED : 0), CMD_SET_THEME_SYS, Tr(L"Use Windows setting"));
    add(theme, MF_STRING | (settings_.theme == 1 ? MF_CHECKED : 0), CMD_SET_THEME_LIGHT, Tr(L"Light"));
    add(theme, MF_STRING | (settings_.theme == 2 ? MF_CHECKED : 0), CMD_SET_THEME_DARK, Tr(L"Dark"));
    // Language: Windows' display language (when WinNCDU has it), every language found, and the
    // pseudo-language that shows untranslated and cut-off text.
    langMenu_ = i18n::AvailableLanguages();
    std::wstring winCode = i18n::WindowsLanguage(langMenu_), winName = L"English";
    for (auto& l : langMenu_)
        if (l.code == winCode) winName = l.name;
    add(lang, MF_STRING | (settings_.language.empty() ? MF_CHECKED : 0), CMD_LANG, TrF(L"Windows default ({0})", winName));
    AppendMenuW(lang, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < langMenu_.size() && i < 150; ++i)
        add(lang, MF_STRING | (_wcsicmp(settings_.language.c_str(), langMenu_[i].code.c_str()) == 0 ? MF_CHECKED : 0),
            CMD_LANG + 2 + i, langMenu_[i].name);
    AppendMenuW(lang, MF_SEPARATOR, 0, nullptr);
    add(lang, MF_STRING | (settings_.language == i18n::kPseudoCode ? MF_CHECKED : 0), CMD_LANG + 1, Tr(L"Pseudo-language (layout test)"));
    add(m, MF_STRING, CMD_SCAN_FOLDER, Tr(L"Scan a folder…\tCtrl+O"));
    add(m, MF_STRING, CMD_OPEN_SCAN, Tr(L"Open saved scan…"));
    add(m, MF_STRING | (root_ ? 0 : MF_GRAYED), CMD_EXPORT_JSON, Tr(L"Save scan as JSON…\tCtrl+S"));
    add(m, MF_STRING | (root_ ? 0 : MF_GRAYED), CMD_EXPORT_CSV, Tr(L"Export as CSV…"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(m, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), Tr(L"Theme"));
    add(m, MF_POPUP, reinterpret_cast<UINT_PTR>(lang), Tr(L"Language"));
    add(m, MF_STRING | (settings_.si ? MF_CHECKED : 0), CMD_SET_SI, Tr(L"Decimal units (GB instead of GiB)"));
    add(m, MF_STRING | (settings_.showHidden ? MF_CHECKED : 0), CMD_SET_HIDDEN, Tr(L"Show hidden && system files"));
    add(m, MF_STRING | (settings_.hardlinks ? MF_CHECKED : 0), CMD_SET_HARDLINKS, Tr(L"Count hard links once (slower scans)"));
    add(m, MF_STRING | (settings_.permanentDefault ? MF_CHECKED : 0), CMD_SET_PERMANENT, Tr(L"Delete permanently by default"));
    add(m, MF_STRING | (IsShellIntegrated() ? MF_CHECKED : 0), CMD_SET_SHELL, Tr(L"Add “Analyze with WinNCDU” to Explorer"));
    add(m, MF_STRING, CMD_PROTECTED, Tr(L"Protected locations…"));
    add(m, MF_STRING, CMD_CLEANERS, Tr(L"Cleanup rules…"));
    add(m, MF_STRING, CMD_KEEP_SIGNED_IN, Tr(L"Keep me signed in to…"));
    add(m, MF_STRING, CMD_WINAPP2, Tr(L"Import winapp2.ini…"));
    if (PathExists(Winapp2File())) add(m, MF_STRING, CMD_WINAPP2_REMOVE, Tr(L"Remove winapp2.ini cleaners"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (!elevated_) add(m, MF_STRING, CMD_ADMIN, Tr(L"Restart as administrator"));
    add(m, MF_STRING, CMD_ABOUT, Tr(L"About && shortcuts\tF1"));
    RECT rc;
    GetClientRect(hwnd_, &rc);
    POINT pt{rc.right - Dip2Px(12), Dip2Px(50)};
    ClientToScreen(hwnd_, &pt);
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTALIGN, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd) OnCommand(cmd);
}

void App::OnCommand(int cmd) {
    if (cmd >= CMD_WIN_ACT && cmd < CMD_WIN_ACT + static_cast<int>(WA_COUNT)) return WinToolAction(cmd - CMD_WIN_ACT);
    if (cmd >= CMD_LANG && cmd < CMD_LANG + 2 + static_cast<int>(langMenu_.size())) {
        settings_.language = cmd == CMD_LANG ? L"" : cmd == CMD_LANG + 1 ? std::wstring(i18n::kPseudoCode) : langMenu_[cmd - CMD_LANG - 2].code;
        ApplyLanguage(true);
        SaveSettings(settings_);
        return;
    }
    Node* single = nullptr;
    {
        auto s = SelectedNodes();
        if (s.size() == 1) single = s[0];
        if (s.empty()) single = FocusNode();
    }
    switch (cmd) {
        case CMD_HOME: view_ = View::Home; LoadHistory(); drives_ = ListDrives(); break;
        case CMD_SCAN_FOLDER: {
            std::wstring p = PickFolder(hwnd_, Tr(L"Choose a folder to analyze"));
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
        case CMD_PHOTO_FIND: StartSimilarPhotos(); break;
        case CMD_PHOTO_MARK: {
            // Nothing is chosen for deletion until asked: this marks every photo except the suggested one.
            sel_.clear();
            uint64_t bytes = 0;
            for (auto& g : photos_)
                for (size_t i = 1; i < g.photos.size(); ++i) {
                    sel_.insert(g.photos[i].node);
                    bytes += Val(g.photos[i].node);
                }
            Toast(TrN(sel_.size(), L"Selected {0} photo ({1}) — the suggested photo of each group is kept. Review before deleting.",
                      L"Selected {0} photos ({1}) — the suggested photo of each group is kept. Review before deleting.", util::FormatSizeShort(bytes)),
                  2);
            break;
        }
        case CMD_DUP_MARK: {
            sel_.clear();
            uint64_t bytes = 0;
            for (auto& g : dups_)
                for (size_t i = 1; i < g.files.size(); ++i) {
                    sel_.insert(g.files[i]);
                    bytes += Val(g.files[i]);
                }
            Toast(TrN(sel_.size(), L"Selected {0} extra copy ({1}) — the oldest copy of each file is kept",
                      L"Selected {0} extra copies ({1}) — the oldest copy of each file is kept", util::FormatSizeShort(bytes)),
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
            if (sel_.empty()) Toast(Tr(L"Every project here was changed in the last three months"), 0);
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
            if (!n) Toast(Tr(L"Every revision here is still referenced by a branch, tag or PR"), 0);
            break;
        }
        case CMD_AI_DELETE: StartAiDelete(); break;
        case CMD_WSL_REFRESH: LoadWsl(); break;
        case CMD_WSL_FREE: StartWslFree(); break;
        case CMD_WIN_REFRESH: LoadWinTools(); break;
        case CMD_GAMES_REFRESH: LoadGames(); break;
        case CMD_GAME_UNINSTALL: StartGameUninstall(); break;
        case CMD_GAMES_DELETE: StartGameLeftoversDelete(); break;
        case CMD_DISK_REFRESH: LoadDiskHealth(); break;
        case CMD_STARTUP_REFRESH: LoadStartup(); break;
            break;
        case CMD_APPS_REFRESH: LoadApps(); break;
        case CMD_APP_UNINSTALL: StartAppUninstall(); break;
        case CMD_APP_LEFTOVERS: LookForAppLeftovers(true); break;
        case CMD_APP_STOPWAIT:
            if (appWait_) appWait_->stop = true;
            appWait_.reset();
            break;
        case CMD_APPS_DELETE: StartAppLeftoversDelete(); break;
        case CMD_APP_LEFT_DONE:
            appLeftFor_.clear();
            appLeft_.clear();
            appLeftSel_.clear();
            rowsDirty_ = true;
            break;
        case CMD_CLEAN_SELECTED: StartCleanup(); break;
        case CMD_CLEAN_MEASURE:
            MeasureCleanup();
            LoadSchedule();
            break;
        case CMD_CLEAN_SCHEDULE: ShowScheduleDialog(); break;
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
            if (single && !OpenWithDefault(NodePath(single))) Toast(TrF(L"Couldn't open {0}", single->name), 1);
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
                Toast(nodes.size() == 1 ? TrF(L"Copied: {0}", text) : TrN(nodes.size(), L"Copied {0} path", L"Copied {0} paths"), 2);
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
            Toast(settings_.hardlinks ? Tr(L"Hard links will be counted once from the next scan") : Tr(L"Hard link detection off"), 2);
            break;
        case CMD_SET_PERMANENT: settings_.permanentDefault = !settings_.permanentDefault; break;
        case CMD_SET_SHELL: {
            std::wstring err;
            bool on = !IsShellIntegrated();
            if (SetShellIntegration(on, err))
                Toast(on ? Tr(L"Added “Analyze with WinNCDU” to the Explorer right-click menu") : Tr(L"Removed from the Explorer menu"), 0);
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
            if (f.empty() || !OpenWithDefault(f)) return Toast(TrF(L"Can't open {0}", f), 1);
            Toast(Tr(L"Add your own cleanup categories, then click Measure in Cleanup"), 2);
            break;
        }
        case CMD_KEEP_SIGNED_IN: {
            // Made on first use from the most visited sites (as measuring Browser cookies does).
            std::wstring f = KeepSignedInFile();
            if (!f.empty() && !PathExists(f)) {
                std::vector<std::wstring> dirs;
                for (auto& c : cleanup_)
                    if (c.browserData == BrowserData::Cookies)
                        for (auto& d : ExpandedPaths(c)) dirs.push_back(d);
                CreateKeepList(f, MostVisitedSites(dirs, 15));
            }
            if (f.empty() || !OpenWithDefault(f)) return Toast(TrF(L"Can't open {0}", f), 1);
            Toast(Tr(L"One site per line; cookies of these sites stay. Save, then click Measure in Cleanup"), 2);
            break;
        }
        case CMD_WINAPP2: {
            std::wstring f = PickOpenFile(hwnd_, Tr(L"Import winapp2.ini"), L"winapp2.ini", L"*.ini");
            if (f.empty()) break;
            Winapp2Stats st;
            std::wstring err;
            if (!ImportWinapp2(f, st, err)) return Toast(err, 1);
            bool busy = cleanupMeasuring_;
            if (!busy) MeasureCleanup();
            ShowView(View::Cleanup);
            Toast(busy ? Tr(L"Imported. Click Measure in a moment to include it.")
                       : TrN(st.usable, L"Imported {0} app cleaner ({1} about personal data left out)",
                             L"Imported {0} app cleaners ({1} about personal data left out)", util::FormatCount(st.personal)),
                  2);
            break;
        }
        case CMD_WINAPP2_REMOVE: {
            if (!DeleteFileW(Winapp2File().c_str())) return Toast(util::Win32ErrorMessage(GetLastError()), 1);
            bool busy = cleanupMeasuring_;
            if (!busy) MeasureCleanup();
            ShowView(View::Cleanup);
            Toast(busy ? Tr(L"Removed. Click Measure in a moment to update the list.") : Tr(L"winapp2.ini cleaners removed"), 2);
            break;
        }
        case CMD_OPEN_JOURNAL:
            if (!PathExists(JournalFile())) return Toast(Tr(L"Nothing has been deleted yet"), 2);
            OpenWithDefault(JournalFile());
            break;
    }
    if (cmd >= CMD_SET_THEME_SYS) SaveSettings(settings_);
    Invalidate();
}
