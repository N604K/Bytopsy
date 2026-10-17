#pragma once

#include <string>

#include "scanner.h"

struct AppConfig {
    std::wstring path;        // directory to scan at startup (empty = drive picker)
    std::wstring importFile;  // ncdu JSON file to load at startup
    ScanOptions scan;
    bool apparent = false;  // start in apparent-size mode
    bool ascii = false;     // ASCII-only drawing
    bool mouse = true;
    bool noDelete = false;
};

int RunApp(const AppConfig& cfg);
