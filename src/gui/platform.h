#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct Settings {
    int theme = 0;  // 0 follow Windows, 1 light, 2 dark
    bool apparent = false;
    bool si = false;
    bool showHidden = true;
    bool treemap = true;
    bool hardlinks = false;
    bool permanentDefault = false;
    float split = 0.56f;  // share of the content height used by the list (rest: treemap)
    int threads = 0;      // 0 = automatic
    std::vector<std::wstring> excludes;
    RECT window{};
    bool hasWindow = false;
    bool maximized = false;
    std::wstring language;  // "" Windows' display language, "en", or a code from res/lang
    // Update checks: 0 off, 1 once a day (notify). Asked once, on the first start; off until answered.
    int updates = 0;
    bool updatesAsked = false;
    int64_t updateLastCheck = 0;  // Unix seconds of the last automatic check
    std::wstring updateSkip;      // a version the user chose to skip
};

Settings LoadSettings();
void SaveSettings(const Settings& s);

bool SystemUsesDarkTheme();
void ApplyWindowTheme(HWND hwnd, bool dark, unsigned captionRgb);
void SetMenuTheme(bool dark);

std::wstring ExePath();
bool IsShellIntegrated();
bool SetShellIntegration(bool enable, std::wstring& err);
// An Explorer entry added while the app was called WinNCDU is replaced by the Bytopsy one.
void MigrateShellIntegration();
bool RelaunchElevated(const std::wstring& args);

std::wstring PickFolder(HWND owner, const std::wstring& title);
// Empty title / filter name: "Open a saved scan" / "Scan files (*.json)".
std::wstring PickOpenFile(HWND owner, const std::wstring& title = L"", const std::wstring& filterName = L"", const wchar_t* filter = L"*.json");
std::wstring PickSaveFile(HWND owner, const std::wstring& defaultName, bool csv);
void ShowProperties(HWND owner, const std::wstring& path);
