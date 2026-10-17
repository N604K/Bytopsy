#include "icons.h"

#include <commctrl.h>
#include <shellapi.h>
#include <commoncontrols.h>

#include "../analysis.h"

namespace {

ID2D1Bitmap* BitmapFromIcon(Gfx& g, HICON icon) {
    if (!icon || !g.Wic()) return nullptr;
    IWICBitmap* wbmp = nullptr;
    IWICFormatConverter* conv = nullptr;
    ID2D1Bitmap* out = nullptr;
    if (SUCCEEDED(g.Wic()->CreateBitmapFromHICON(icon, &wbmp)) && SUCCEEDED(g.Wic()->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wbmp, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeMedianCut))) {
        g.RT()->CreateBitmapFromWicBitmap(conv, nullptr, &out);
    }
    SafeRelease(conv);
    SafeRelease(wbmp);
    return out;
}

}  // namespace

void IconCache::Clear() {
    for (auto& kv : cache_) SafeRelease(kv.second);
    cache_.clear();
}

ID2D1Bitmap* IconCache::Get(Gfx& g, const std::wstring& key, const std::wstring& name, DWORD attrs, bool useAttrs) {
    if (gen_ != g.Generation()) {
        Clear();
        gen_ = g.Generation();
    }
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    SHFILEINFOW sfi{};
    UINT flags = SHGFI_SYSICONINDEX | (useAttrs ? SHGFI_USEFILEATTRIBUTES : 0);
    ID2D1Bitmap* bmp = nullptr;
    if (SHGetFileInfoW(name.c_str(), attrs, &sfi, sizeof(sfi), flags)) {
        IImageList* list = nullptr;
        int which = g.Scale() > 1.4f ? SHIL_EXTRALARGE : SHIL_LARGE;
        if (SUCCEEDED(SHGetImageList(which, IID_PPV_ARGS(&list)))) {
            HICON icon = nullptr;
            if (SUCCEEDED(list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon)) && icon) {
                bmp = BitmapFromIcon(g, icon);
                DestroyIcon(icon);
            }
            list->Release();
        }
    }
    cache_[key] = bmp;
    return bmp;
}

ID2D1Bitmap* IconCache::Folder(Gfx& g) { return Get(g, L"<folder>", L"folder", FILE_ATTRIBUTE_DIRECTORY, true); }

ID2D1Bitmap* IconCache::ForExt(Gfx& g, const std::wstring& ext) {
    return Get(g, L"." + ext, L"file." + ext, FILE_ATTRIBUTE_NORMAL, true);
}

ID2D1Bitmap* IconCache::ForNode(Gfx& g, const Node* n) {
    if (n->IsDir()) return Folder(g);
    return ForExt(g, FileExt(n->name));
}

ID2D1Bitmap* IconCache::ForPath(Gfx& g, const std::wstring& path) { return Get(g, L"path:" + path, path, 0, false); }
