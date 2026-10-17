#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

typedef void* HANDLE;

namespace util {

// Use base-10 units (kB, MB) instead of base-2 (KiB, MiB).
extern bool g_si;

// Fixed width (9 columns), e.g. "123.4 MiB".
std::wstring FormatSize(uint64_t bytes);
// Same as FormatSize but without padding.
std::wstring FormatSizeShort(uint64_t bytes);
// "1,234,567"
std::wstring FormatCount(uint64_t n);
// Compact count, fixed width 6, e.g. "123456", "  1.2M".
std::wstring FormatCountShort(uint64_t n);
// Local time "YYYY-MM-DD HH:MM" from FILETIME ticks (100ns since 1601).
std::wstring FormatTime(int64_t filetime);
std::wstring FormatDuration(double seconds);
std::wstring FormatPercent(double pct);  // fixed width 6: " 45.2%"

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);

wchar_t LowerChar(wchar_t c);
std::wstring ToLower(std::wstring_view s);
// Case-insensitive wildcard match ('*' and '?'); `pattern` should be lowercase.
bool GlobMatchI(std::wstring_view pattern, std::wstring_view text);
// Case-insensitive substring test; `needleLower` must already be lowercase.
bool ContainsI(std::wstring_view hay, std::wstring_view needleLower);
bool HasWildcards(std::wstring_view s);

// Parses sizes like "512", "10k", "1.5G", "100MB", "2GiB". Binary multipliers.
bool ParseSize(std::wstring_view s, uint64_t& out);

std::wstring Win32ErrorMessage(unsigned long err);
int64_t NowFileTime();
int64_t UnixToFileTime(int64_t unixSeconds);
int64_t FileTimeToUnix(int64_t filetime);

// Starts a native thread with a large stack (deep recursion over trees is fine).
HANDLE StartThread(std::function<void()> fn, size_t stackSize = 16u << 20);

}  // namespace util
