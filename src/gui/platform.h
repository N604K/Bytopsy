#pragma once

#include <windows.h>

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
};

Settings LoadSettings();
void SaveSettings(const Settings& s);

bool SystemUsesDarkTheme();
void ApplyWindowTheme(HWND hwnd, bool dark, unsigned captionRgb);
void SetMenuTheme(bool dark);

std::wstring ExePath();
bool IsShellIntegrated();
bool SetShellIntegration(bool enable, std::wstring& err);
bool RelaunchElevated(const std::wstring& args);

std::wstring PickFolder(HWND owner, const std::wstring& title);
std::wstring PickOpenFile(HWND owner, const wchar_t* title = L"Open a saved scan", const wchar_t* filterName = L"Scan files (*.json)",
                          const wchar_t* filter = L"*.json");
std::wstring PickSaveFile(HWND owner, const std::wstring& defaultName, bool csv);
void ShowProperties(HWND owner, const std::wstring& path);
