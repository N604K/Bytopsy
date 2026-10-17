#include "tree.h"

namespace {

void RecomputeOne(Node* n) {
    if (!n->IsDir()) return;
    uint64_t s = 0, d = 0, it = 0;
    bool err = false;
    for (auto& c : n->children) {
        if (!c->Has(NF_HARDLINK)) {
            s += c->size;
            d += c->disk;
        }
        it += 1 + c->items;
        if (c->Has(NF_ERROR | NF_SUBERR)) err = true;
    }
    n->size = s;
    n->disk = d;
    n->items = it;
    if (err)
        n->flags |= NF_SUBERR;
    else
        n->flags &= ~NF_SUBERR;
}

}  // namespace

std::wstring NodePath(const Node* n) {
    std::vector<const Node*> chain;
    for (const Node* p = n; p; p = p->parent) chain.push_back(p);
    const std::wstring& rootName = chain.back()->name;
    wchar_t sep = (rootName.find(L'\\') == std::wstring::npos && rootName.find(L'/') != std::wstring::npos) ? L'/' : L'\\';
    std::wstring out = rootName;
    for (size_t i = chain.size() - 1; i-- > 0;) {
        if (out.empty() || (out.back() != L'\\' && out.back() != L'/')) out += sep;
        out += chain[i]->name;
    }
    return out;
}

std::wstring ToLongPath(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    std::wstring r = dir;
    if (r.empty() || (r.back() != L'\\' && r.back() != L'/')) r += L'\\';
    r += name;
    return r;
}

void RecomputeTotals(Node* n) {
    if (!n->IsDir()) return;
    for (auto& c : n->children)
        if (c->IsDir()) RecomputeTotals(c.get());
    RecomputeOne(n);
}

void RecomputeUp(Node* n) {
    for (Node* p = n; p; p = p->parent) RecomputeOne(p);
}

bool IsAlive(const Node* n) {
    for (const Node* p = n; p; p = p->parent)
        if (p->Has(NF_DELETED)) return false;
    return true;
}

bool IsAncestor(const Node* ancestor, const Node* n) {
    for (const Node* p = n ? n->parent : nullptr; p; p = p->parent)
        if (p == ancestor) return true;
    return false;
}

std::unique_ptr<Node> DetachNode(Node* n) {
    Node* parent = n->parent;
    std::unique_ptr<Node> owned;
    if (parent) {
        auto& ch = parent->children;
        for (size_t i = 0; i < ch.size(); ++i) {
            if (ch[i].get() == n) {
                owned = std::move(ch[i]);
                ch.erase(ch.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
        RecomputeUp(parent);
    }
    n->flags |= NF_DELETED;
    return owned;
}
