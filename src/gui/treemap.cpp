#include "treemap.h"

#include <algorithm>
#include <unordered_map>

#include "../analysis.h"

int CategoryOfExt(const std::wstring& e) {
    static const std::unordered_map<std::wstring, int> map = [] {
        std::unordered_map<std::wstring, int> m;
        auto add = [&](int cat, std::initializer_list<const wchar_t*> exts) {
            for (auto x : exts) m[x] = cat;
        };
        add(CAT_VIDEO, {L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"flv", L"webm", L"m4v", L"mpg", L"mpeg", L"ts", L"m2ts",
                        L"vob", L"3gp"});
        add(CAT_IMAGE, {L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"tif", L"tiff", L"webp", L"heic", L"heif", L"raw", L"cr2",
                        L"nef", L"arw", L"dng", L"psd", L"svg", L"ico", L"avif", L"xcf", L"kra"});
        add(CAT_AUDIO, {L"mp3", L"wav", L"flac", L"aac", L"ogg", L"wma", L"m4a", L"opus", L"aiff", L"mid", L"ape"});
        add(CAT_DOCUMENT, {L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"odt", L"ods", L"odp", L"txt",
                           L"rtf", L"md", L"epub", L"csv", L"one", L"pst", L"ost", L"msg", L"eml"});
        add(CAT_ARCHIVE, {L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"xz", L"zst", L"iso", L"img", L"cab", L"vhd",
                          L"vhdx", L"vmdk", L"qcow2", L"wim", L"esd", L"dmg", L"tgz"});
        add(CAT_CODE, {L"c", L"cpp", L"h", L"hpp", L"cs", L"java", L"py", L"js", L"ts", L"tsx", L"jsx", L"go", L"rs",
                       L"rb", L"php", L"html", L"css", L"json", L"xml", L"yml", L"yaml", L"sql", L"sh", L"ps1", L"lua",
                       L"kt", L"swift", L"obj", L"pdb", L"ilk", L"pch", L"ipch", L"class", L"jar", L"pyc", L"map"});
        add(CAT_EXECUTABLE, {L"exe", L"dll", L"msi", L"msix", L"appx", L"sys", L"ocx", L"com", L"bat", L"cmd", L"scr",
                             L"apk", L"node", L"so", L"lib", L"a"});
        add(CAT_SYSTEM, {L"tmp", L"log", L"dat", L"db", L"sqlite", L"cache", L"etl", L"evtx", L"dmp", L"bak", L"old",
                         L"cab", L"mui", L"cat", L"manifest", L"pak", L"bin", L"blob", L"ldb", L"idx", L"pf"});
        return m;
    }();
    auto it = map.find(e);
    return it == map.end() ? CAT_OTHER : it->second;
}

int CategoryOfNode(const Node* n) {
    if (n->IsDir()) return CAT_OTHER;
    return CategoryOfExt(FileExt(n->name));
}

namespace {

struct Item {
    Node* node;
    double area;
};

double Worst(double sum, double minA, double maxA, double side) {
    double s2 = sum * sum, w2 = side * side;
    return std::max(w2 * maxA / s2, s2 / (w2 * minA));
}

class Builder {
public:
    Builder(bool useDisk, size_t maxRects) : useDisk_(useDisk), max_(maxRects) {}
    std::vector<TmRect> out;

    void Node_(Node* n, RectF r, int depth) {
        if (out.size() >= max_) return;
        bool dir = n->IsDir() && !n->children.empty();
        if (!dir || W(r) < 6 || H(r) < 6) {
            out.push_back({r, n, depth, true, n->IsDir() ? -1 : CategoryOfNode(n)});
            return;
        }
        out.push_back({r, n, depth, false, -1});
        float pad = depth == 0 ? 0.0f : (W(r) > 40 && H(r) > 40 ? 2.0f : 1.0f);
        RectF inner = Inset(r, pad, pad);
        if (W(inner) < 2 || H(inner) < 2) return;
        std::vector<Node*> kids;
        kids.reserve(n->children.size());
        double total = 0;
        for (auto& c : n->children) {
            uint64_t v = Val(c.get());
            if (v == 0 || c->Has(NF_HARDLINK)) continue;
            kids.push_back(c.get());
            total += static_cast<double>(v);
        }
        if (kids.empty() || total <= 0) return;
        std::sort(kids.begin(), kids.end(), [this](Node* a, Node* b) { return Val(a) > Val(b); });
        double scale = (static_cast<double>(W(inner)) * H(inner)) / total;
        std::vector<Item> items;
        items.reserve(kids.size());
        for (Node* k : kids) {
            double a = Val(k) * scale;
            if (a < 1.0) break;  // smaller than a pixel: the rest is folded into the parent color
            items.push_back({k, a});
        }
        Squarify(items, inner, depth + 1);
    }

private:
    uint64_t Val(const Node* n) const { return useDisk_ ? n->disk : n->size; }

    void Squarify(std::vector<Item>& items, RectF r, int depth) {
        size_t i = 0;
        while (i < items.size()) {
            double w = W(r), h = H(r);
            if (w < 1 || h < 1) return;
            double side = std::min(w, h);
            size_t j = i;
            double sum = 0, minA = 1e300, maxA = 0, worst = 1e300;
            while (j < items.size()) {
                double a = items[j].area;
                double nsum = sum + a, nmin = std::min(minA, a), nmax = std::max(maxA, a);
                double nw = Worst(nsum, nmin, nmax, side);
                if (j > i && nw > worst) break;
                sum = nsum;
                minA = nmin;
                maxA = nmax;
                worst = nw;
                ++j;
            }
            // Lay out items[i..j) as a strip along the shorter side.
            double thick = sum / side;
            if (w >= h) {
                double y = r.top;
                for (size_t k = i; k < j; ++k) {
                    double len = items[k].area / thick;
                    RectF cr = R(r.left, static_cast<float>(y), static_cast<float>(r.left + thick), static_cast<float>(y + len));
                    Node_(items[k].node, cr, depth);
                    y += len;
                }
                r.left = static_cast<float>(r.left + thick);
            } else {
                double x = r.left;
                for (size_t k = i; k < j; ++k) {
                    double len = items[k].area / thick;
                    RectF cr = R(static_cast<float>(x), r.top, static_cast<float>(x + len), static_cast<float>(r.top + thick));
                    Node_(items[k].node, cr, depth);
                    x += len;
                }
                r.top = static_cast<float>(r.top + thick);
            }
            i = j;
            if (out.size() >= max_) return;
        }
    }

    bool useDisk_;
    size_t max_;
};

}  // namespace

std::vector<TmRect> LayoutTreemap(Node* root, RectF area, bool useDisk, size_t maxRects) {
    Builder b(useDisk, maxRects);
    if (root && W(area) > 2 && H(area) > 2) b.Node_(root, area, 0);
    return std::move(b.out);
}

int HitTreemap(const std::vector<TmRect>& rects, float x, float y) {
    for (size_t i = rects.size(); i-- > 0;)
        if (Inside(rects[i].r, x, y)) return static_cast<int>(i);
    return -1;
}
