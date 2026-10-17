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

// Recursive delete that never follows junctions/symlinks. Clears read-only attributes.
bool DeletePermanently(const std::wstring& path, std::atomic<bool>& cancel, std::atomic<uint64_t>& removed,
                       std::wstring& err);
// Moves to the Recycle Bin via IFileOperation. The calling thread must have COM initialized.
bool DeleteToRecycleBin(const std::wstring& path, std::wstring& err);

bool RevealInExplorer(const std::wstring& path);
bool OpenWithDefault(const std::wstring& path);
bool OpenTerminalAt(const std::wstring& dir);
bool CopyTextToClipboard(const std::wstring& text);
std::wstring ReadClipboardText();
std::wstring CurrentDirectory();
std::wstring HomeDirectory();
