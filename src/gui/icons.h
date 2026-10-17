#pragma once

#include <string>
#include <unordered_map>

#include "../tree.h"
#include "gfx.h"

// Explorer's file icons (by extension), converted to Direct2D bitmaps and cached.
class IconCache {
public:
    ID2D1Bitmap* ForNode(Gfx& g, const Node* n);
    ID2D1Bitmap* ForExt(Gfx& g, const std::wstring& extLower);
    ID2D1Bitmap* Folder(Gfx& g);
    ID2D1Bitmap* ForPath(Gfx& g, const std::wstring& path);  // real item (drives etc.)
    void Clear();

private:
    ID2D1Bitmap* Get(Gfx& g, const std::wstring& key, const std::wstring& name, DWORD attrs, bool useAttrs);
    std::unordered_map<std::wstring, ID2D1Bitmap*> cache_;
    unsigned gen_ = 0;
};
