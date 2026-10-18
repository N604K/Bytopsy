#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "../alerts.h"
#include "app.h"

namespace {

// `bytopsy.exe --check-space`: the low disk space check the scheduled task runs. No window (a
// console program would flash one every time); the notifications in the app's language.
int CheckSpace() {
    Settings s = LoadSettings();
    util::g_si = s.si;
    i18n::LoadLanguage(s.language);
    wchar_t dec[8] = L".", grp[8] = L",";
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, dec, 8)) wcscpy_s(dec, L".");
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, grp, 8)) wcscpy_s(grp, L",");
    util::SetNumberFormat(dec, grp);
    return SpaceCheckExitCode(RunSpaceCheck(false));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

    // Usage: bytopsy.exe [folder]          scan a folder or drive
    //        bytopsy.exe -f export.json    open a saved scan (Bytopsy or ncdu JSON)
    //        bytopsy.exe --check-space     the scheduled low disk space check (no window)
    //        bytopsy.exe --alert-link URI  a notification's link: bytopsy:disk-health, bytopsy:scan?drive=C
    std::wstring path, importFile;
    bool checkSpace = false, diskHealth = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if ((a == L"-f" || a == L"--file") && i + 1 < argc) {
            importFile = argv[++i];
        } else if (a == L"--check-space") {
            checkSpace = true;
        } else if (a == L"--alert-link" && i + 1 < argc) {
            // Only the links Bytopsy's notifications use; anything else just opens the app.
            AlertLink l = ParseAlertLink(argv[++i]);
            if (l.kind == AlertLink::Scan)
                path = std::wstring(1, l.drive) + L":\\";
            else if (l.kind == AlertLink::DiskHealth)
                diskHealth = true;
        } else if (!a.empty() && a[0] != L'-') {
            // Explorer passes "C:\" as "C:\"" when the path ends in a backslash; strip stray quotes.
            while (!a.empty() && a.back() == L'"') a.pop_back();
            if (a.size() == 2 && a[1] == L':') a += L'\\';  // "C:" alone would mean the current directory on C:
            path = a;
        }
    }
    if (argv) LocalFree(argv);
    if (checkSpace) {
        int code = CheckSpace();
        CoUninitialize();
        return code;
    }
    if (path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 5, L".json") == 0 &&
        !(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) {
        importFile = path;
        path.clear();
    }

    int rc = 1;
    {
        App app;
        if (app.Create(inst, path, importFile, show)) {
            if (diskHealth) app.OpenDiskHealth();
            rc = app.Run();
        }
    }
    CoUninitialize();
    return rc;
}
