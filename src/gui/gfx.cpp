#include "gfx.h"

#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

bool FontExists(IDWriteFactory* dw, const wchar_t* family) {
    IDWriteFontCollection* coll = nullptr;
    if (FAILED(dw->GetSystemFontCollection(&coll))) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    coll->FindFamilyName(family, &idx, &exists);
    coll->Release();
    return exists != FALSE;
}

std::vector<IDWriteInlineObject*> g_ellipses;

}  // namespace

bool Gfx::Init(HWND hwnd) {
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dw_))))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    uiFont_ = FontExists(dw_, L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    displayFont_ = FontExists(dw_, L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    fluentIcons_ = FontExists(dw_, L"Segoe Fluent Icons");
    iconFont_ = fluentIcons_ ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    CreateTarget();
    return rt_ != nullptr;
}

void Gfx::Shutdown() {
    DiscardTarget();
    for (auto& kv : formats_) kv.second->Release();
    formats_.clear();
    for (auto* e : g_ellipses) e->Release();
    g_ellipses.clear();
    SafeRelease(wic_);
    SafeRelease(dw_);
    SafeRelease(d2d_);
}

void Gfx::CreateTarget() {
    if (rt_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(std::max<LONG>(1, rc.right - rc.left), std::max<LONG>(1, rc.bottom - rc.top));
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = dpi_;
    if (FAILED(d2d_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, size), &rt_))) return;
    rt_->SetDpi(dpi_, dpi_);
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_);
    ++generation_;
}

void Gfx::DiscardTarget() {
    SafeRelease(brush_);
    SafeRelease(rt_);
}

void Gfx::Resize() {
    if (!rt_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    rt_->Resize(D2D1::SizeU(std::max<LONG>(1, rc.right - rc.left), std::max<LONG>(1, rc.bottom - rc.top)));
}

void Gfx::SetDpi(float dpi) {
    dpi_ = dpi;
    if (rt_) rt_->SetDpi(dpi, dpi);
}

D2D1_SIZE_F Gfx::Size() const { return rt_ ? rt_->GetSize() : D2D1::SizeF(1, 1); }

bool Gfx::Begin() {
    CreateTarget();
    if (!rt_) return false;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

bool Gfx::End() {
    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardTarget();
        return false;
    }
    return true;
}

void Gfx::Clear(D2D1_COLOR_F c) { rt_->Clear(c); }

void Gfx::Fill(const RectF& r, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    rt_->FillRectangle(r, brush_);
}

void Gfx::FillRound(const RectF& r, float radius, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_);
}

void Gfx::StrokeRound(const RectF& r, float radius, D2D1_COLOR_F c, float width) {
    brush_->SetColor(c);
    float h = width / 2;
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(Inset(r, h, h), radius, radius), brush_, width);
}

void Gfx::Stroke(const RectF& r, D2D1_COLOR_F c, float width) {
    brush_->SetColor(c);
    float h = width / 2;
    rt_->DrawRectangle(Inset(r, h, h), brush_, width);
}

void Gfx::Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width) {
    brush_->SetColor(c);
    rt_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush_, width);
}

void Gfx::FillCircle(float cx, float cy, float radius, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_);
}

void Gfx::Arc(float cx, float cy, float radius, float startDeg, float sweepDeg, D2D1_COLOR_F c, float width) {
    if (sweepDeg <= 0.01f) return;
    if (sweepDeg >= 359.9f) {
        brush_->SetColor(c);
        rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_, width);
        return;
    }
    const float kPi = 3.14159265f;
    float a0 = (startDeg - 90) * kPi / 180, a1 = (startDeg + sweepDeg - 90) * kPi / 180;
    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(d2d_->CreatePathGeometry(&geo))) return;
    ID2D1GeometrySink* sink = nullptr;
    geo->Open(&sink);
    sink->BeginFigure(D2D1::Point2F(cx + radius * std::cos(a0), cy + radius * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + radius * std::cos(a1), cy + radius * std::sin(a1)),
                                  D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  sweepDeg > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    ID2D1StrokeStyle* style = nullptr;
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
    d2d_->CreateStrokeStyle(sp, nullptr, 0, &style);
    brush_->SetColor(c);
    rt_->DrawGeometry(geo, brush_, width, style);
    SafeRelease(style);
    geo->Release();
}

void Gfx::GradientH(const RectF& r, float radius, D2D1_COLOR_F a, D2D1_COLOR_F b) {
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, a}, {1.0f, b}};
    ID2D1GradientStopCollection* coll = nullptr;
    if (FAILED(rt_->CreateGradientStopCollection(stops, 2, &coll))) return;
    ID2D1LinearGradientBrush* lg = nullptr;
    rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, 0), D2D1::Point2F(r.right, 0)),
                                   coll, &lg);
    if (lg) {
        if (radius > 0)
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), lg);
        else
            rt_->FillRectangle(r, lg);
        lg->Release();
    }
    coll->Release();
}

void Gfx::Shadow(const RectF& r, float radius, float spread) {
    const int steps = 6;
    D2D1_COLOR_F c = D2D1::ColorF(0, 0, 0, 0.05f);
    for (int i = steps; i >= 1; --i) {
        float d = spread * i / steps;
        FillRound(R(r.left - d * 0.6f, r.top - d * 0.3f, r.right + d * 0.6f, r.bottom + d), radius + d, c);
    }
}

IDWriteTextFormat* Gfx::Format(const TextStyle& st) {
    auto key = std::make_tuple(static_cast<int>(st.size * 10), static_cast<int>(st.weight), static_cast<int>(st.align),
                               st.center ? 1 : 0, st.wrap ? 1 : 0, static_cast<int>(st.font));
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second;
    const std::wstring& family = st.font == FontKind::Icons ? iconFont_
                                 : st.font == FontKind::Display ? displayFont_
                                 : st.font == FontKind::Mono    ? std::wstring(L"Cascadia Mono")
                                                                : uiFont_;
    IDWriteTextFormat* f = nullptr;
    dw_->CreateTextFormat(family.c_str(), nullptr, st.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, st.size,
                          locale_.c_str(), &f);
    if (!f) return nullptr;
    f->SetTextAlignment(st.align == Align::Left    ? DWRITE_TEXT_ALIGNMENT_LEADING
                        : st.align == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                   : DWRITE_TEXT_ALIGNMENT_CENTER);
    f->SetParagraphAlignment(st.center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    f->SetWordWrapping(st.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!st.wrap && st.font != FontKind::Icons) {
        IDWriteInlineObject* ell = nullptr;
        dw_->CreateEllipsisTrimmingSign(f, &ell);
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trim, ell);
        if (ell) g_ellipses.push_back(ell);
    }
    formats_[key] = f;
    return f;
}

void Gfx::SetLocale(const std::wstring& name) {
    if (name == locale_) return;
    locale_ = name;
    for (auto& kv : formats_) kv.second->Release();
    formats_.clear();
}

void Gfx::Text(const std::wstring& s, const RectF& r, D2D1_COLOR_F c, const TextStyle& st) {
    if (s.empty() || W(r) <= 0) return;
    IDWriteTextFormat* f = Format(st);
    if (!f) return;
    brush_->SetColor(c);
    rt_->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, r, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (recording_) Record(s, r, st);
}

// Where the text actually is (not its layout box), clipped like the drawing: what UI Automation
// reports and what decides which element the text belongs to.
void Gfx::Record(const std::wstring& s, const RectF& r, const TextStyle& st) {
    RectF v;
    if (st.wrap) {
        v = R(r.left, r.top, r.right, r.top + std::min(H(r), MeasureHeight(s, W(r), st)));
    } else {
        float w = std::min(Measure(s, st), W(r));
        float h = std::min(H(r), st.size * 1.4f);
        float x = st.align == Align::Left ? r.left : st.align == Align::Right ? r.right - w : (r.left + r.right - w) / 2;
        float y = st.center ? (r.top + r.bottom - h) / 2 : r.top;
        v = R(x, y, x + w, y + h);
    }
    if (!clips_.empty()) {
        const RectF& c = clips_.back();
        v = R(std::max(v.left, c.left), std::max(v.top, c.top), std::min(v.right, c.right), std::min(v.bottom, c.bottom));
    }
    if (W(v) <= 0.5f || H(v) <= 0.5f) return;
    runs_.push_back({s, v});
}

float Gfx::Measure(const std::wstring& s, const TextStyle& st) {
    if (s.empty()) return 0;
    TextStyle m = st;
    m.align = Align::Left;
    IDWriteTextFormat* f = Format(m);
    IDWriteTextLayout* layout = nullptr;
    if (!f || FAILED(dw_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 10000, 1000, &layout))) return 0;
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    layout->Release();
    return tm.widthIncludingTrailingWhitespace;
}

float Gfx::MeasureHeight(const std::wstring& s, float width, const TextStyle& st) {
    if (s.empty()) return 0;
    IDWriteTextFormat* f = Format(st);
    IDWriteTextLayout* layout = nullptr;
    if (!f || FAILED(dw_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, width, 10000, &layout))) return 0;
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    layout->Release();
    return tm.height;
}

void Gfx::Icon(wchar_t glyph, const RectF& r, D2D1_COLOR_F c, float size) {
    TextStyle st;
    st.size = size;
    st.align = Align::Center;
    st.font = FontKind::Icons;
    wchar_t s[2] = {glyph, 0};
    IDWriteTextFormat* f = Format(st);
    if (!f) return;
    brush_->SetColor(c);
    rt_->DrawText(s, 1, f, r, brush_, D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Gfx::Bitmap(ID2D1Bitmap* bmp, const RectF& r, float opacity) {
    if (bmp) rt_->DrawBitmap(bmp, r, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Gfx::PushClip(const RectF& r) {
    rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
    RectF c = r;
    if (!clips_.empty()) {
        const RectF& o = clips_.back();
        c = R(std::max(c.left, o.left), std::max(c.top, o.top), std::min(c.right, o.right), std::min(c.bottom, o.bottom));
    }
    clips_.push_back(c);
}

void Gfx::PopClip() {
    rt_->PopAxisAlignedClip();
    if (!clips_.empty()) clips_.pop_back();
}

void Gfx::SetRecording(bool on) {
    recording_ = on;
    runs_.clear();
    clips_.clear();
}
