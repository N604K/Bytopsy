#pragma once

#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

// 24-bit colors packed as 0xRRGGBB; kDefault means "terminal default".
constexpr uint32_t kDefault = 0xFF000000u;

enum Attr : uint8_t { A_BOLD = 1, A_DIM = 2, A_ITALIC = 4, A_UNDERLINE = 8 };

struct Style {
    uint32_t fg = kDefault;
    uint32_t bg = kDefault;
    uint8_t attr = 0;
    bool operator==(const Style&) const = default;
};

inline Style St(uint32_t fg, uint32_t bg = kDefault, uint8_t attr = 0) { return Style{fg, bg, attr}; }

uint32_t Lerp(uint32_t a, uint32_t b, double t);

int CharWidth(char32_t cp);
int StrWidth(std::wstring_view s);
// Shortens to `w` columns by cutting the middle.
std::wstring FitMiddle(std::wstring_view s, int w, bool ascii);
// Shortens to `w` columns keeping the end (good for paths).
std::wstring FitLeft(std::wstring_view s, int w, bool ascii);
// Shortens to `w` columns keeping the start, then pads with spaces to exactly `w`.
std::wstring PadTo(std::wstring_view s, int w);
std::wstring PadLeft(std::wstring_view s, int w);

enum class EvType { None, Key, Mouse, Resize };
enum class MouseKind { Click, DoubleClick, RightClick, WheelUp, WheelDown };

struct Event {
    EvType type = EvType::None;
    WORD vk = 0;
    wchar_t ch = 0;
    bool ctrl = false, alt = false, shift = false;
    MouseKind mouse = MouseKind::Click;
    int mx = 0, my = 0;
};

class Terminal {
public:
    bool Init(bool mouse, std::wstring& err);
    void Shutdown();

    int Width() const { return w_; }
    int Height() const { return h_; }

    bool ReadEvent(Event& ev, DWORD timeoutMs);

    void BeginFrame(Style clear);
    // Draws text at (x,y), clipped to maxW columns. Returns the number of columns used.
    int Put(int x, int y, std::wstring_view s, Style st, int maxW = 1 << 30);
    void Fill(int x, int y, int w, Style st, wchar_t ch = L' ');
    void EndFrame();
    void Invalidate() { full_ = true; }

private:
    struct Cell {
        wchar_t txt[4];
        uint8_t len;
        uint8_t width;  // 0 = continuation of a wide character
        Style st;
        bool operator==(const Cell& o) const {
            return len == o.len && width == o.width && st == o.st &&
                   std::char_traits<wchar_t>::compare(txt, o.txt, len) == 0;
        }
    };
    Cell& At(int x, int y) { return back_[static_cast<size_t>(y) * w_ + x]; }
    void SetCell(int x, int y, const wchar_t* txt, int len, int width, Style st);
    void Write(const std::wstring& s);
    void UpdateSize();

    HANDLE hIn_ = INVALID_HANDLE_VALUE, hOut_ = INVALID_HANDLE_VALUE;
    DWORD origIn_ = 0, origOut_ = 0;
    bool active_ = false;
    int w_ = 80, h_ = 25, top_ = 0;
    bool full_ = true;
    std::vector<Cell> back_, front_;
    std::deque<Event> queue_;
    std::wstring origTitle_;
};
