// Self-check of the accessibility tree behind the UI Automation provider: bytopsy-uia-test.exe
// Element identity across frames, reading order, which drawn text names which element, nesting,
// navigation and hit testing, and which UIA patterns and control types an element gets. Pure: no
// window, no COM.
#include <windows.h>
#include <uiautomation.h>

#include <cstdio>
#include <string>

#include "accmodel.h"

static int g_fail = 0;

static void Check(bool ok, const char* what) {
    if (!ok) ++g_fail;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static void Tidy(const std::wstring& in, const std::wstring& want, const char* what) {
    std::wstring got = AccTidy(in);
    Check(got == want, what);
    if (got != want) wprintf(L"     got [%ls]\n", got.c_str());
}

static AccNode Node(AccRole role, const RectF& r, uint64_t key, int parent, uint32_t flags = 0, const wchar_t* name = L"") {
    AccNode n;
    n.role = role;
    n.r = r;
    n.key = key;
    n.parent = parent;
    n.flags = flags;
    n.name = name;
    return n;
}

// A window like the app's: a content pane with a list of rows (one with a switch inside), and a
// status bar; rows start at `top` (scrolling moves them).
static void BuildList(AccTree& t, float top, int firstRow, int rowCount) {
    t.Clear();
    int main = t.Add(Node(AccRole::Pane, R(240, 56, 1000, 700), AccKey({0xA11, 3}), -1, AF_MAIN, L"Folders"));
    int list = t.Add(Node(AccRole::List, R(240, 100, 1000, 700), AccKey({0xA11, 5}), main, AF_SELECTION | AF_MULTI, L"Folders list"));
    for (int i = firstRow; i < firstRow + rowCount; ++i) {
        float y = top + (i - firstRow) * 32.0f;
        // Identity comes from what the row shows (its node), not from where it is drawn.
        int row = t.Add(Node(AccRole::ListItem, R(248, y, 990, y + 30), AccKey({7, static_cast<uint64_t>(i) * 1000}), list,
                             AF_SELECTABLE | AF_INVOKE | AF_SCROLLITEM | (i == 3 ? AF_FOCUSED : 0)));
        if (i == 2) t.Add(Node(AccRole::Button, R(260, y + 6, 294, y + 24), AccKey({9, 2}), row, AF_TOGGLE | AF_ON, L"Run at startup"));
    }
    t.Add(Node(AccRole::StatusBar, R(0, 700, 1000, 730), AccKey({0xA11, 4}), -1, 0, L"Status"));
    t.Finish();
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // ---- Names from drawn text
    Tidy(L"Review 1,234 files  ›", L"Review 1,234 files", "trailing chevron dropped");
    Tidy(L"120 GB   ·   3 items   ·   2 h ago", L"120 GB, 3 items, 2 h ago", "dot separators become commas");
    Tidy(L"NTFS  ·  Fixed", L"NTFS, Fixed", "two-space separators too");
    Tidy(L"‹", L"", "a lone stepper arrow has no name");
    Tidy(L"Keep me signed in to…  ›", L"Keep me signed in to…", "ellipsis kept, chevron dropped");
    Tidy(L"  a\tb\r\nc   d  ", L"a b c d", "whitespace collapsed and trimmed");
    Tidy(L"· leading", L"leading", "no comma before the first word");

    // ---- Which text belongs to which element (the last-drawn region containing its center)
    {
        std::vector<RectF> targets = {R(0, 0, 400, 32), R(360, 4, 396, 28), R(0, 40, 400, 72)};
        std::vector<RectF> runs = {R(10, 8, 120, 24), R(362, 8, 394, 24), R(10, 48, 60, 64), R(500, 0, 600, 20), R(380, 20, 420, 40)};
        std::vector<int> at = AccAssign(targets, runs);
        Check(at[0] == 0, "row text goes to the row");
        Check(at[1] == 1, "text inside a nested button goes to the button (drawn on top)");
        Check(at[2] == 2, "second row's text goes to the second row");
        Check(at[3] == -1, "text outside everything stays loose (becomes a text element)");
        Check(at[4] == -1, "text whose middle is in no region stays loose");
    }

    // ---- Nesting: a region inside an earlier one is its child
    {
        std::vector<RectF> t = {R(24, 100, 900, 172), R(700, 137, 880, 157), R(24, 182, 900, 254), R(24, 182, 900, 254)};
        std::vector<int> p = AccNest(t);
        Check(p[0] == -1 && p[1] == 0, "a review link inside a cleanup card is the card's child");
        Check(p[2] == -1, "the next card is not inside the first");
        Check(p[3] == -1, "a region the same size as another is not its child");
    }

    // ---- Reading order: lines top to bottom, left to right (dialog buttons are drawn right to left)
    {
        std::vector<RectF> rects = {R(600, 300, 700, 338), R(490, 300, 590, 338), R(300, 100, 700, 128), R(300, 140, 700, 200),
                                    R(300, 302, 400, 330)};
        std::vector<int> ids = {0, 1, 2, 3, 4};
        AccReadingOrder(ids, rects);
        Check(ids == std::vector<int>({2, 3, 4, 1, 0}), "title, message, then the buttons left to right");
    }

    // ---- Tree: navigation, identity, focus, hit testing
    {
        AccTree t;
        BuildList(t, 132, 0, 6);
        int main = t.Top()[0];
        Check(t.Top().size() == 2, "two top-level elements (content, status bar)");
        int list = t.FirstChild(main);
        Check(list >= 0 && t[list].role == AccRole::List && t.Parent(list) == main, "the list is the content's first child");
        int r0 = t.FirstChild(list), r1 = t.Next(r0);
        Check(r0 >= 0 && r1 >= 0 && t[r0].r.top < t[r1].r.top, "rows in order, top to bottom");
        Check(t.Prev(r0) == -1 && t.Prev(r1) == r0, "previous sibling");
        Check(t.Parent(main) == -1 && t.Next(main) == t.Top()[1], "the content's parent is the window, the status bar follows");
        int last = t.LastChild(list);
        Check(last >= 0 && t.Next(last) == -1, "last row has no next sibling");
        int r2 = t.Next(r1);
        int sw = t.FirstChild(r2);
        Check(sw >= 0 && t[sw].role == AccRole::Button && t.Parent(sw) == r2, "the switch is a child of its row");
        Check(t.FirstChild(sw) == -1, "a leaf has no children");
        Check(t.FromPoint(270, t[r2].r.top + 10) == sw, "hit test finds the switch inside the row");
        Check(t.FromPoint(600, t[r1].r.top + 10) == r1, "hit test finds a row");
        Check(t.FromPoint(10, 710) == t.Top()[1], "hit test finds the status bar");
        Check(t.FromPoint(10, 10) == -1, "nothing at an empty point");
        int f = t.Focused();
        Check(f >= 0 && t[f].key == AccKey({7, 3000}), "the focused row is found");

        // Scrolling down by one row: rows keep their identity, one leaves and one arrives.
        AccTree s;
        BuildList(s, 132 - 32, 1, 6);
        Check(s.Find(t[r1].key) >= 0 && s[s.Find(t[r1].key)].r.top < t[r1].r.top, "a row keeps its key when it moves");
        Check(s.Find(t[r0].key) == -1, "a row scrolled away is no longer found (its element fails gracefully)");
        Check(s.Find(t[sw].key) >= 0, "the nested switch keeps its key too");
        Check(s.Signature() != t.Signature(), "the structure signature changes when rows come and go");
        AccTree same;
        BuildList(same, 132, 0, 6);
        Check(same.Signature() == t.Signature(), "an identical frame has the same signature");
        AccTree moved;
        BuildList(moved, 140, 0, 6);
        Check(moved.Signature() == t.Signature(), "moving without changing the elements keeps the signature");
    }

    // ---- Duplicate keys are made unique, the same way every frame
    {
        auto build = [](AccTree& t) {
            t.Clear();
            int p = t.Add(Node(AccRole::Pane, R(0, 0, 100, 100), 42, -1));
            t.Add(Node(AccRole::Text, R(0, 0, 50, 20), 77, p, 0, L"same"));
            t.Add(Node(AccRole::Text, R(0, 30, 50, 50), 77, p, 0, L"same"));
            t.Add(Node(AccRole::Text, R(0, 60, 50, 80), 0, p, 0, L"no key"));
            t.Finish();
        };
        AccTree a, b;
        build(a);
        build(b);
        Check(a[1].key != a[2].key, "two elements with the same key get different ones");
        Check(a[1].key == b[1].key && a[2].key == b[2].key && a[3].key == b[3].key, "... the same in every frame");
        Check(a[3].key != 0 && a.Find(a[3].key) == 3, "an element without a key still gets one");
        Check(a.Find(a[1].key) == 1 && a.Find(a[2].key) == 2, "every key finds its element");
    }

    // ---- Patterns and control types
    {
        AccNode button = Node(AccRole::Button, R(0, 0, 1, 1), 1, -1, AF_INVOKE);
        Check(AccSupports(button, UIA_InvokePatternId) && !AccSupports(button, UIA_TogglePatternId), "a button is invoked, not toggled");
        AccNode check = Node(AccRole::CheckBox, R(0, 0, 1, 1), 1, -1, AF_TOGGLE | AF_ON);
        Check(AccSupports(check, UIA_TogglePatternId) && !AccSupports(check, UIA_InvokePatternId), "a checkbox toggles");
        AccNode row = Node(AccRole::ListItem, R(0, 0, 1, 1), 1, -1, AF_SELECTABLE | AF_INVOKE | AF_SCROLLITEM | AF_EXPANDABLE);
        Check(AccSupports(row, UIA_SelectionItemPatternId) && AccSupports(row, UIA_ScrollItemPatternId) &&
                  AccSupports(row, UIA_ExpandCollapsePatternId) && AccSupports(row, UIA_InvokePatternId),
              "a folder row: select, scroll into view, expand, open");
        AccNode list = Node(AccRole::List, R(0, 0, 1, 1), 1, -1, AF_SELECTION | AF_MULTI);
        Check(AccSupports(list, UIA_SelectionPatternId) && !AccSupports(list, UIA_SelectionItemPatternId), "a list is a selection container");
        AccNode text = Node(AccRole::Text, R(0, 0, 1, 1), 1, -1);
        Check(!AccSupports(text, UIA_InvokePatternId) && !AccSupports(text, UIA_ValuePatternId), "text has no patterns");
        Check(AccControlType(AccRole::Navigation) == UIA_ListControlTypeId, "the sidebar is a list");
        Check(AccControlType(AccRole::CheckBox) == UIA_CheckBoxControlTypeId && AccControlType(AccRole::Window) == UIA_WindowControlTypeId &&
                  AccControlType(AccRole::ListItem) == UIA_ListItemControlTypeId && AccControlType(AccRole::HeaderItem) == UIA_HeaderItemControlTypeId,
              "control types");
    }

    // ---- Keys
    Check(AccKey({1, 2}) != AccKey({2, 1}), "key parts are ordered");
    Check(AccKey(L"Cancel", 5) != AccKey(L"Cancel", 6) && AccKey(L"Cancel", 5) == AccKey(L"Cancel", 5), "text keys depend on their parent");

    printf(g_fail ? "\n%d FAILED\n" : "\nALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
