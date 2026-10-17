#pragma once

#include <vector>

#include "../tree.h"
#include "gfx.h"

struct TmRect {
    RectF r;
    Node* node;
    int depth;
    bool leaf;
    int category;  // for leaves
};

// Squarified treemap of `root`. Folders are emitted before their contents, so the last
// rectangle containing a point is the deepest one.
std::vector<TmRect> LayoutTreemap(Node* root, RectF area, bool useDisk, size_t maxRects = 80000);
int HitTreemap(const std::vector<TmRect>& rects, float x, float y);
int CategoryOfNode(const Node* n);
