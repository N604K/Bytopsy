#include "platform.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <sstream>

#include "../util.h"
#include "history.h"

#pragma comment(lib, "dwmapi.lib")

namespace {

std::wstring SettingsFile() { return AppDataDir() + L"\\settings.ini"; }

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    size_t e = s.find_last_not_of(L" \t\r\n");
    return b == std::wstring::npos ? L"" : s.substr(b, e - b + 1);
}

bool RegSetString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY h;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

const wchar_t* kShellKeys[] = {L"Software\\Classes\\Directory\\shell\\WinNCDU", L"Software\\Classes\\Drive\\shell\\WinNCDU",
                               L"Software\\Classes\\Directory\\Background\\shell\\WinNCDU"};

}  // namespace

Settings LoadSettings() {
    Settings s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, SettingsFile().c_str(), L"rb") != 0 || !f) return s;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    std::wstringstream ss(util::Utf8ToWide(data));
    std::wstring line;
    while (std::getline(ss, line)) {
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        int iv = _wtoi(v.c_str());
        if (k == L"theme") s.theme = iv;
        else if (k == L"apparent") s.apparent = iv != 0;
        else if (k == L"si") s.si = iv != 0;
        else if (k == L"showHidden") s.showHidden = iv != 0;
        else if (k == L"treemap") s.treemap = iv != 0;
        else if (k == L"hardlinks") s.hardlinks = iv != 0;
        else if (k == L"permanentDefault") s.permanentDefault = iv != 0;
        else if (k == L"split") s.split = static_cast<float>(_wtof(v.c_str()));
        else if (k == L"threads") s.threads = iv;
        else if (k == L"exclude" && !v.empty()) s.excludes.push_back(v);
        else if (k == L"window") {
            RECT r{};
            if (swscanf_s(v.c_str(), L"%ld,%ld,%ld,%ld", &r.left, &r.top, &r.right, &r.bottom) == 4 && r.right > r.left) {
                s.window = r;
                s.hasWindow = true;
            }
        } else if (k == L"maximized") s.maximized = iv != 0;
    }
    if (s.split < 0.2f || s.split > 0.9f) s.split = 0.56f;
    return s;
}

void SaveSettings(const Settings& s) {
    std::wstringstream ss;
    ss << L"theme=" << s.theme << L"\n";
    ss << L"apparent=" << s.apparent << L"\n";
    ss << L"si=" << s.si << L"\n";
    ss << L"showHidden=" << s.showHidden << L"\n";
    ss << L"treemap=" << s.treemap << L"\n";
    ss << L"hardlinks=" << s.hardlinks << L"\n";
    ss << L"permanentDefault=" << s.permanentDefault << L"\n";
    ss << L"split=" << s.split << L"\n";
    ss << L"threads=" << s.threads << L"\n";
    for (auto& e : s.excludes) ss << L"exclude=" << e << L"\n";
    if (s.hasWindow)
        ss << L"window=" << s.window.left << L"," << s.window.top << L"," << s.window.right << L"," << s.window.bottom << L"\n";
    ss << L"maximized=" << s.maximized << L"\n";
    std::string data = util::WideToUtf8(ss.str());
    FILE* f = nullptr;
    if (_wfopen_s(&f, SettingsFile().c_str(), L"wb") != 0 || !f) return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

bool SystemUsesDarkTheme() {
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

void ApplyWindowTheme(HWND hwnd, bool dark, unsigned captionRgb) {
    BOOL d = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &d, sizeof(d))))
        DwmSetWindowAttribute(hwnd, 19, &d, sizeof(d));
    // Windows 11: caption color matching the app chrome, rounded corners.
    COLORREF cap = RGB((captionRgb >> 16) & 0xFF, (captionRgb >> 8) & 0xFF, captionRgb & 0xFF);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
    COLORREF border = cap;
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    int corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
}

void SetMenuTheme(bool dark) {
    // Undocumented but stable uxtheme exports used by Explorer for dark context menus (Win10 1903+).
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    OSVERSIONINFOW vi{sizeof(vi)};
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto rtl = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!rtl || rtl(&vi) != 0 || vi.dwBuildNumber < 18362) return;
    if (setMode) setMode(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush) flush();
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::wstring(buf, n);
}

bool IsShellIntegrated() {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kShellKeys[0], 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

bool SetShellIntegration(bool enable, std::wstring& err) {
    if (!enable) {
        for (auto k : kShellKeys) RegDeleteTreeW(HKEY_CURRENT_USER, k);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return true;
    }
    std::wstring exe = ExePath();
    std::wstring cmd = L"\"" + exe + L"\" \"%V\"";
    for (auto k : kShellKeys) {
        std::wstring key = k;
        if (!RegSetString(HKEY_CURRENT_USER, key, nullptr, L"Analyze with WinNCDU") ||
            !RegSetString(HKEY_CURRENT_USER, key, L"Icon", L"\"" + exe + L"\",0") ||
            !RegSetString(HKEY_CURRENT_USER, key + L"\\command", nullptr, cmd)) {
            err = L"Could not write to the registry";
            return false;
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool RelaunchElevated(const std::wstring& args) {
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

std::wstring PickFolder(HWND owner, const std::wstring& title) {
    IFileOpenDialog* dlg = nullptr;
    std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title.c_str());
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

std::wstring PickOpenFile(HWND owner, const wchar_t* title, const wchar_t* filterName, const wchar_t* filter) {
    IFileOpenDialog* dlg = nullptr;
    std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
    COMDLG_FILTERSPEC spec[] = {{filterName, filter}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, spec);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

std::wstring PickSaveFile(HWND owner, const std::wstring& defaultName, bool csv) {
    IFileSaveDialog* dlg = nullptr;
    std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
    COMDLG_FILTERSPEC json[] = {{L"ncdu JSON export (*.json)", L"*.json"}};
    COMDLG_FILTERSPEC csvs[] = {{L"CSV spreadsheet (*.csv)", L"*.csv"}};
    dlg->SetFileTypes(1, csv ? csvs : json);
    dlg->SetDefaultExtension(csv ? L"csv" : L"json");
    dlg->SetFileName(defaultName.c_str());
    dlg->SetTitle(csv ? L"Export as CSV" : L"Save scan");
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

void ShowProperties(HWND owner, const std::wstring& path) { SHObjectProperties(owner, SHOP_FILEPATH, path.c_str(), nullptr); }
