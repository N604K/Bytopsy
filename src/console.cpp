#include "console.h"

#include <algorithm>
#include <cstdio>

uint32_t Lerp(uint32_t a, uint32_t b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto ch = [&](int shift) {
        int ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
        return static_cast<uint32_t>(ca + (cb - ca) * t + 0.5) << shift;
    };
    return ch(16) | ch(8) | ch(0);
}

int CharWidth(char32_t c) {
    if (c == 0) return 0;
    if (c < 0x300) return 1;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x483 && c <= 0x489) || (c >= 0x591 && c <= 0x5BD) ||
        (c >= 0x610 && c <= 0x61A) || (c >= 0x64B && c <= 0x65F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
        (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x200B && c <= 0x200F) || (c >= 0x20D0 && c <= 0x20FF) ||
        (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) || c == 0xFEFF ||
        (c >= 0xE0100 && c <= 0xE01EF))
        return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
        (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F680 && c <= 0x1F6FF) || (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

namespace {

// Decodes one code point at s[i]; returns number of UTF-16 units consumed.
size_t Decode(std::wstring_view s, size_t i, char32_t& cp) {
    wchar_t c = s[i];
    if (IS_HIGH_SURROGATE(c) && i + 1 < s.size() && IS_LOW_SURROGATE(s[i + 1])) {
        cp = 0x10000 + ((static_cast<char32_t>(c) - 0xD800) << 10) + (s[i + 1] - 0xDC00);
        return 2;
    }
    cp = c;
    return 1;
}

// Longest prefix with display width <= w.
std::wstring_view TakeLeft(std::wstring_view s, int w) {
    size_t i = 0;
    int used = 0;
    while (i < s.size()) {
        char32_t cp;
        size_t n = Decode(s, i, cp);
        int cw = CharWidth(cp);
        if (used + cw > w) break;
        used += cw;
        i += n;
    }
    return s.substr(0, i);
}

// Longest suffix with display width <= w.
std::wstring_view TakeRight(std::wstring_view s, int w) {
    size_t i = s.size();
    int used = 0;
    while (i > 0) {
        size_t start = i - 1;
        if (start > 0 && IS_LOW_SURROGATE(s[start]) && IS_HIGH_SURROGATE(s[start - 1])) --start;
        char32_t cp;
        Decode(s, start, cp);
        int cw = CharWidth(cp);
        if (used + cw > w) break;
        used += cw;
        i = start;
    }
    return s.substr(i);
}

}  // namespace

int StrWidth(std::wstring_view s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        char32_t cp;
        i += Decode(s, i, cp);
        w += CharWidth(cp);
    }
    return w;
}

std::wstring FitMiddle(std::wstring_view s, int w, bool ascii) {
    if (w <= 0) return L"";
    if (StrWidth(s) <= w) return std::wstring(s);
    if (w == 1) return ascii ? L"~" : L"…";
    int left = (w - 1 + 1) / 2;
    int right = w - 1 - left;
    std::wstring out(TakeLeft(s, left));
    out += ascii ? L"~" : L"…";
    out += TakeRight(s, right);
    return out;
}

std::wstring FitLeft(std::wstring_view s, int w, bool ascii) {
    if (w <= 0) return L"";
    if (StrWidth(s) <= w) return std::wstring(s);
    std::wstring out = ascii ? L"~" : L"…";
    out += TakeRight(s, w - 1);
    return out;
}

std::wstring PadTo(std::wstring_view s, int w) {
    if (w <= 0) return L"";
    std::wstring out(TakeLeft(s, w));
    int cw = StrWidth(out);
    if (cw < w) out.append(static_cast<size_t>(w - cw), L' ');
    return out;
}

std::wstring PadLeft(std::wstring_view s, int w) {
    int cw = StrWidth(s);
    if (cw >= w) return std::wstring(s);
    return std::wstring(static_cast<size_t>(w - cw), L' ') + std::wstring(s);
}

// ---------------------------------------------------------------------------------------------

namespace {
Terminal* g_term = nullptr;

BOOL WINAPI CtrlHandler(DWORD type) {
    if (g_term && (type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
                   type == CTRL_SHUTDOWN_EVENT)) {
        g_term->Shutdown();
    }
    return FALSE;
}
}  // namespace

bool Terminal::Init(bool mouse, std::wstring& err) {
    hIn_ = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
    hOut_ = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
    if (hIn_ == INVALID_HANDLE_VALUE || hOut_ == INVALID_HANDLE_VALUE) {
        err = L"No console is attached.";
        return false;
    }
    GetConsoleMode(hIn_, &origIn_);
    GetConsoleMode(hOut_, &origOut_);
    DWORD outMode = ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(hOut_, outMode)) {
        outMode &= ~DISABLE_NEWLINE_AUTO_RETURN;
        if (!SetConsoleMode(hOut_, outMode)) {
            err = L"This console does not support virtual terminal sequences (Windows 10 1607+ required).";
            return false;
        }
    }
    DWORD inMode = ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
    if (mouse) inMode |= ENABLE_MOUSE_INPUT;
    SetConsoleMode(hIn_, inMode);

    wchar_t title[1024] = {};
    if (GetConsoleTitleW(title, 1024)) origTitle_ = title;
    SetConsoleTitleW(L"Bytopsy");

    active_ = true;
    g_term = this;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    Write(L"\x1b[?1049h\x1b[?25l\x1b[H\x1b[2J");
    UpdateSize();
    return true;
}

void Terminal::Shutdown() {
    if (!active_) return;
    active_ = false;
    Write(L"\x1b[0m\x1b[2J\x1b[?25h\x1b[?1049l");
    SetConsoleMode(hIn_, origIn_);
    SetConsoleMode(hOut_, origOut_);
    if (!origTitle_.empty()) SetConsoleTitleW(origTitle_.c_str());
}

void Terminal::Write(const std::wstring& s) {
    size_t pos = 0;
    const size_t kChunk = 16384;
    while (pos < s.size()) {
        size_t n = std::min(kChunk, s.size() - pos);
        if (pos + n < s.size() && IS_HIGH_SURROGATE(s[pos + n - 1])) --n;
        DWORD written = 0;
        WriteConsoleW(hOut_, s.data() + pos, static_cast<DWORD>(n), &written, nullptr);
        pos += n;
    }
}

void Terminal::UpdateSize() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hOut_, &csbi)) {
        int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        int h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        top_ = csbi.srWindow.Top;
        w = std::max(w, 20);
        h = std::max(h, 6);
        if (w != w_ || h != h_ || back_.empty()) {
            w_ = w;
            h_ = h;
            full_ = true;
        }
    }
}

bool Terminal::ReadEvent(Event& ev, DWORD timeoutMs) {
    if (queue_.empty()) {
        if (WaitForSingleObject(hIn_, timeoutMs) != WAIT_OBJECT_0) return false;
        INPUT_RECORD recs[128];
        DWORD n = 0;
        if (!ReadConsoleInputW(hIn_, recs, 128, &n)) return false;
        for (DWORD i = 0; i < n; ++i) {
            const INPUT_RECORD& r = recs[i];
            if (r.EventType == KEY_EVENT) {
                const KEY_EVENT_RECORD& k = r.Event.KeyEvent;
                if (!k.bKeyDown) continue;
                WORD vk = k.wVirtualKeyCode;
                if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
                    vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)
                    continue;
                Event e;
                e.type = EvType::Key;
                e.vk = vk;
                e.ch = k.uChar.UnicodeChar;
                e.ctrl = (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                e.alt = (k.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
                e.shift = (k.dwControlKeyState & SHIFT_PRESSED) != 0;
                int rep = std::clamp<int>(k.wRepeatCount, 1, 16);
                for (int j = 0; j < rep; ++j) queue_.push_back(e);
            } else if (r.EventType == MOUSE_EVENT) {
                const MOUSE_EVENT_RECORD& m = r.Event.MouseEvent;
                Event e;
                e.type = EvType::Mouse;
                e.mx = m.dwMousePosition.X;
                e.my = m.dwMousePosition.Y - top_;
                if (m.dwEventFlags == MOUSE_WHEELED) {
                    e.mouse = static_cast<short>(HIWORD(m.dwButtonState)) > 0 ? MouseKind::WheelUp : MouseKind::WheelDown;
                } else if (m.dwEventFlags == DOUBLE_CLICK) {
                    if (!(m.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED)) continue;
                    e.mouse = MouseKind::DoubleClick;
                } else if (m.dwEventFlags == 0) {
                    if (m.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED)
                        e.mouse = MouseKind::Click;
                    else if (m.dwButtonState & RIGHTMOST_BUTTON_PRESSED)
                        e.mouse = MouseKind::RightClick;
                    else
                        continue;
                } else {
                    continue;
                }
                queue_.push_back(e);
            } else if (r.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                Event e;
                e.type = EvType::Resize;
                queue_.push_back(e);
            }
        }
    }
    if (queue_.empty()) return false;
    ev = queue_.front();
    queue_.pop_front();
    return true;
}

void Terminal::BeginFrame(Style clear) {
    UpdateSize();
    Cell blank{};
    blank.txt[0] = L' ';
    blank.len = 1;
    blank.width = 1;
    blank.st = clear;
    back_.assign(static_cast<size_t>(w_) * h_, blank);
    if (front_.size() != back_.size()) {
        front_.assign(back_.size(), Cell{});
        full_ = true;
    }
}

void Terminal::SetCell(int x, int y, const wchar_t* txt, int len, int width, Style st) {
    Cell& c = At(x, y);
    // Don't leave half of a wide character behind.
    if (c.width == 0 && x > 0) {
        Cell& p = At(x - 1, y);
        p.txt[0] = L' ';
        p.len = 1;
        p.width = 1;
    }
    if (c.width == 2 && x + 1 < w_ && width == 1) {
        Cell& n = At(x + 1, y);
        n.txt[0] = L' ';
        n.len = 1;
        n.width = 1;
    }
    c.len = static_cast<uint8_t>(len);
    for (int i = 0; i < len; ++i) c.txt[i] = txt[i];
    c.width = static_cast<uint8_t>(width);
    c.st = st;
    if (width == 2 && x + 1 < w_) {
        Cell& n = At(x + 1, y);
        n.len = 0;
        n.width = 0;
        n.st = st;
    }
}

int Terminal::Put(int x, int y, std::wstring_view s, Style st, int maxW) {
    if (y < 0 || y >= h_ || maxW <= 0) return 0;
    int limit = std::min(w_, x + maxW);
    int cx = x;
    for (size_t i = 0; i < s.size();) {
        char32_t cp;
        size_t n = Decode(s, i, cp);
        wchar_t buf[2] = {s[i], n == 2 ? s[i + 1] : L'\0'};
        i += n;
        int cw = CharWidth(cp);
        if (cp < 0x20 || cp == 0x7F) {
            buf[0] = L'?';
            cw = 1;
        } else if (n == 1 && (IS_HIGH_SURROGATE(buf[0]) || IS_LOW_SURROGATE(buf[0]))) {
            buf[0] = 0xFFFD;
            cw = 1;
        }
        if (cw == 0) {
            // Combining mark: attach to the previous cell.
            int px = cx - 1;
            if (px >= 0 && px < w_ && px >= x) {
                if (At(px, y).width == 0 && px > 0) --px;
                Cell& pc = At(px, y);
                if (pc.len + n <= 4)
                    for (size_t k = 0; k < n; ++k) pc.txt[pc.len++] = buf[k];
            }
            continue;
        }
        if (cx + cw > limit) break;
        if (cx >= 0) SetCell(cx, y, buf, static_cast<int>(n), cw, st);
        cx += cw;
    }
    return cx - x;
}

void Terminal::Fill(int x, int y, int w, Style st, wchar_t ch) {
    if (y < 0 || y >= h_) return;
    int end = std::min(w_, x + w);
    for (int i = std::max(0, x); i < end; ++i) SetCell(i, y, &ch, 1, 1, st);
}

namespace {
void AppendColor(std::wstring& out, uint32_t c, bool fg) {
    if (c == kDefault) {
        out += fg ? L";39" : L";49";
        return;
    }
    wchar_t buf[32];
    swprintf(buf, 32, L";%d;2;%u;%u;%u", fg ? 38 : 48, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    out += buf;
}
}  // namespace

void Terminal::EndFrame() {
    std::wstring out;
    out.reserve(static_cast<size_t>(w_) * h_ * 4);
    if (full_) out += L"\x1b[0m\x1b[2J";
    for (int y = 0; y < h_; ++y) {
        const Cell* b = &back_[static_cast<size_t>(y) * w_];
        const Cell* f = &front_[static_cast<size_t>(y) * w_];
        if (!full_ && std::equal(b, b + w_, f)) continue;
        wchar_t pos[32];
        swprintf(pos, 32, L"\x1b[%d;1H", y + 1);
        out += pos;
        bool first = true;
        Style cur;
        for (int x = 0; x < w_; ++x) {
            const Cell& c = b[x];
            if (c.width == 0) continue;
            if (first || !(c.st == cur)) {
                out += L"\x1b[0";
                if (c.st.attr & A_BOLD) out += L";1";
                if (c.st.attr & A_DIM) out += L";2";
                if (c.st.attr & A_ITALIC) out += L";3";
                if (c.st.attr & A_UNDERLINE) out += L";4";
                AppendColor(out, c.st.fg, true);
                AppendColor(out, c.st.bg, false);
                out += L'm';
                cur = c.st;
                first = false;
            }
            out.append(c.txt, c.len);
        }
        out += L"\x1b[0m";
    }
    if (!out.empty()) Write(out);
    front_ = back_;
    full_ = false;
}
