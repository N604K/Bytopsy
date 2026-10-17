#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct DriveInfo {
    std::wstring root;  // "C:\"
    std::wstring label;
    std::wstring fs;
    unsigned type = 0;  // DRIVE_*
    bool ready = false;
    uint64_t total = 0, free = 0;
};

std::vector<DriveInfo> ListDrives();
const wchar_t* DriveTypeName(unsigned type);
bool QueryFreeSpace(const std::wstring& path, uint64_t& total, uint64_t& free);

bool PathExists(const std::wstring& path);
// Resolves a user supplied path to an absolute directory path. Empty result + err on failure.
std::wstring NormalizeDirPath(const std::wstring& in, std::wstring& err);

// Recursive delete through handles: everything below `path` is opened relative to its folder's
// handle and links are removed as links, so nothing can redirect it mid-way. Read-only items go too.
bool DeletePermanently(const std::wstring& path, std::atomic<bool>& cancel, std::atomic<uint64_t>& removed,
                       std::wstring& err);
// True if opening `path` reaches a different place than it names, because a folder above it is a
// link or mount point; `actual` gets the real location. When running as administrator,
// DeletePermanently refuses such paths (a folder swapped for a junction can't redirect it).
bool PathLeadsElsewhere(const std::wstring& path, std::wstring& actual);
// Moves to the Recycle Bin via IFileOperation. The calling thread must have COM initialized.
bool DeleteToRecycleBin(const std::wstring& path, std::wstring& err);

struct AppUsingFile {
    std::wstring name;  // as Task Manager shows it, e.g. "Google Chrome"
    unsigned long pid = 0;
};
// Apps that have `path` open (a file, or files inside a folder: the first `maxFiles` found).
// Asks the Restart Manager, so it names the app instead of just failing with "in use".
std::vector<AppUsingFile> AppsUsing(const std::wstring& path, size_t maxFiles = 1000);
// "Google Chrome, Microsoft Teams and 2 more"
std::wstring AppNames(const std::vector<std::wstring>& names, size_t max = 3);

bool RevealInExplorer(const std::wstring& path);
bool OpenWithDefault(const std::wstring& path);
bool OpenTerminalAt(const std::wstring& dir);
bool CopyTextToClipboard(const std::wstring& text);
std::wstring ReadClipboardText();
std::wstring CurrentDirectory();
std::wstring HomeDirectory();
