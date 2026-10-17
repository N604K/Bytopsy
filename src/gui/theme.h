#pragma once

#include <d2d1.h>

#include <cstdint>
#include <string>

inline D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}
inline D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}
inline D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

struct Palette {
    bool dark = true;
    D2D1_COLOR_F bg, surface, surface2, surface3, border, text, textDim, textFaint;
    D2D1_COLOR_F accent, accent2, accentText, danger, success, warning;
    D2D1_COLOR_F hover, selected, selectedFocus, track, scrim, shadow;
    uint32_t bgRgb, surfaceRgb;
};

inline Palette DarkPalette() {
    Palette p;
    p.dark = true;
    p.bgRgb = 0x17181D;
    p.surfaceRgb = 0x1F2027;
    p.bg = Hex(p.bgRgb);
    p.surface = Hex(p.surfaceRgb);
    p.surface2 = Hex(0x272933);
    p.surface3 = Hex(0x30333F);
    p.border = Hex(0x343745);
    p.text = Hex(0xECEEF4);
    p.textDim = Hex(0xA3A8B8);
    p.textFaint = Hex(0x6E7487);
    p.accent = Hex(0x7C8CFF);
    p.accent2 = Hex(0xB07CFF);
    p.accentText = Hex(0xFFFFFF);
    p.danger = Hex(0xFF5F6D);
    p.success = Hex(0x3DD68C);
    p.warning = Hex(0xFFB547);
    p.hover = Hex(0xFFFFFF, 0.045f);
    p.selected = Hex(0x7C8CFF, 0.20f);
    p.selectedFocus = Hex(0x7C8CFF, 0.30f);
    p.track = Hex(0xFFFFFF, 0.07f);
    p.scrim = Hex(0x000000, 0.55f);
    p.shadow = Hex(0x000000, 0.35f);
    return p;
}

inline Palette LightPalette() {
    Palette p;
    p.dark = false;
    p.bgRgb = 0xF4F5F9;
    p.surfaceRgb = 0xFFFFFF;
    p.bg = Hex(p.bgRgb);
    p.surface = Hex(p.surfaceRgb);
    p.surface2 = Hex(0xEEF0F6);
    p.surface3 = Hex(0xE4E7EF);
    p.border = Hex(0xDDE1EA);
    p.text = Hex(0x1A1C23);
    p.textDim = Hex(0x585F70);
    p.textFaint = Hex(0x9097A6);
    p.accent = Hex(0x4D63F5);
    p.accent2 = Hex(0x8B54F0);
    p.accentText = Hex(0xFFFFFF);
    p.danger = Hex(0xE5384A);
    p.success = Hex(0x17A866);
    p.warning = Hex(0xD98A00);
    p.hover = Hex(0x000000, 0.04f);
    p.selected = Hex(0x4D63F5, 0.14f);
    p.selectedFocus = Hex(0x4D63F5, 0.22f);
    p.track = Hex(0x000000, 0.07f);
    p.scrim = Hex(0x10131A, 0.40f);
    p.shadow = Hex(0x000000, 0.18f);
    return p;
}

// File categories used for the treemap colors and the file type summary.
enum Category { CAT_VIDEO, CAT_IMAGE, CAT_AUDIO, CAT_DOCUMENT, CAT_ARCHIVE, CAT_CODE, CAT_EXECUTABLE, CAT_SYSTEM, CAT_OTHER, CAT_COUNT };

inline const wchar_t* CategoryName(int c) {
    static const wchar_t* names[] = {L"Video", L"Images", L"Audio", L"Documents", L"Archives", L"Code", L"Programs", L"System", L"Other"};
    return names[c];
}

inline uint32_t CategoryRgb(int c) {
    static const uint32_t colors[] = {0x8B5CF6, 0x14B8A6, 0xF59E0B, 0x3B82F6, 0xEF4444, 0x22C55E, 0xEC4899, 0x64748B, 0x94A3B8};
    return colors[c];
}

int CategoryOfExt(const std::wstring& extLower);
