#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "app.h"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

    // Usage: bytopsy.exe [folder]          scan a folder or drive
    //        bytopsy.exe -f export.json    open a saved scan (Bytopsy or ncdu JSON)
    std::wstring path, importFile;
    bool updated = false;  // started by the self-update that just replaced the program
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if ((a == L"-f" || a == L"--file") && i + 1 < argc) {
            importFile = argv[++i];
        } else if (a == L"--updated") {
            updated = true;
        } else if (!a.empty() && a[0] != L'-') {
            // Explorer passes "C:\" as "C:\"" when the path ends in a backslash; strip stray quotes.
            while (!a.empty() && a.back() == L'"') a.pop_back();
            if (a.size() == 2 && a[1] == L':') a += L'\\';  // "C:" alone would mean the current directory on C:
            path = a;
        }
    }
    if (argv) LocalFree(argv);
    if (path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 5, L".json") == 0 &&
        !(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) {
        importFile = path;
        path.clear();
    }

    int rc = 1;
    {
        App app;
        if (app.Create(inst, path, importFile, show, updated)) rc = app.Run();
    }
    CoUninitialize();
    return rc;
}
