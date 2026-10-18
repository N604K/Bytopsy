#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "theme.h"

template <class T>
inline void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

using RectF = D2D1_RECT_F;
inline RectF R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
inline RectF RWH(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
inline float W(const RectF& r) { return r.right - r.left; }
inline float H(const RectF& r) { return r.bottom - r.top; }
inline bool Inside(const RectF& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
inline RectF Inset(const RectF& r, float dx, float dy) { return R(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy); }

enum class Align { Left, Center, Right };
enum class FontKind { UI, Display, Icons, Mono };

struct TextStyle {
    float size = 13.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    Align align = Align::Left;
    bool center = true;  // vertically centered
    bool wrap = false;
    FontKind font = FontKind::UI;
};

inline TextStyle TS(float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, Align a = Align::Left) {
    TextStyle s;
    s.size = size;
    s.weight = w;
    s.align = a;
    return s;
}

class Gfx {
public:
    bool Init(HWND hwnd);
    void Shutdown();
    void Resize();
    void SetDpi(float dpi);
    float Dpi() const { return dpi_; }
    float Scale() const { return dpi_ / 96.0f; }

    bool Begin();
    // Returns false if the device was lost (everything device dependent must be recreated).
    bool End();
    unsigned Generation() const { return generation_; }

    ID2D1RenderTarget* RT() { return rt_; }
    ID2D1Factory* Factory() { return d2d_; }
    IWICImagingFactory* Wic() { return wic_; }
    D2D1_SIZE_F Size() const;

    void Clear(D2D1_COLOR_F c);
    void Fill(const RectF& r, D2D1_COLOR_F c);
    void FillRound(const RectF& r, float radius, D2D1_COLOR_F c);
    void StrokeRound(const RectF& r, float radius, D2D1_COLOR_F c, float width = 1.0f);
    void Stroke(const RectF& r, D2D1_COLOR_F c, float width = 1.0f);
    void Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width = 1.0f);
    void FillCircle(float cx, float cy, float radius, D2D1_COLOR_F c);
    void Arc(float cx, float cy, float radius, float startDeg, float sweepDeg, D2D1_COLOR_F c, float width);
    void GradientH(const RectF& r, float radius, D2D1_COLOR_F a, D2D1_COLOR_F b);
    void Shadow(const RectF& r, float radius, float spread);

    void Text(const std::wstring& s, const RectF& r, D2D1_COLOR_F c, const TextStyle& st);
    float Measure(const std::wstring& s, const TextStyle& st);
    float MeasureHeight(const std::wstring& s, float width, const TextStyle& st);
    void Icon(wchar_t glyph, const RectF& r, D2D1_COLOR_F c, float size = 16.0f);
    void Bitmap(ID2D1Bitmap* bmp, const RectF& r, float opacity = 1.0f);

    void PushClip(const RectF& r);
    void PopClip();

    bool HasFluentIcons() const { return fluentIcons_; }

    // While recording (a UI Automation client is attached), Text() also remembers what it drew and
    // where, for the accessible element tree. SetRecording clears what was recorded.
    struct TextRun {
        std::wstring s;
        RectF r;
    };
    void SetRecording(bool on);
    bool Recording() const { return recording_; }
    const std::vector<TextRun>& Runs() const { return runs_; }

private:
    IDWriteTextFormat* Format(const TextStyle& st);
    void CreateTarget();
    void DiscardTarget();
    void Record(const std::wstring& s, const RectF& r, const TextStyle& st);

    HWND hwnd_ = nullptr;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    IWICImagingFactory* wic_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    IDWriteInlineObject* ellipsis_ = nullptr;
    std::map<std::tuple<int, int, int, int, int, int>, IDWriteTextFormat*> formats_;
    std::wstring uiFont_, displayFont_, iconFont_;
    bool fluentIcons_ = false;
    float dpi_ = 96.0f;
    unsigned generation_ = 1;
    bool recording_ = false;
    std::vector<TextRun> runs_;
    std::vector<RectF> clips_;
};
