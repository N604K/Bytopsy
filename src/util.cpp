#include "util.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace util {

bool g_si = false;

namespace {

// Lowercase mapping table for the whole BMP, built once with the invariant locale.
struct LowerTable {
    wchar_t map[65536];
    LowerTable() {
        for (int i = 0; i < 65536; ++i) map[i] = static_cast<wchar_t>(i);
        auto mapRange = [this](int from, int to) {
            int n = to - from + 1;
            std::vector<wchar_t> src(n), dst(n);
            for (int i = 0; i < n; ++i) src[i] = static_cast<wchar_t>(from + i);
            if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, src.data(), n, dst.data(), n,
                              nullptr, nullptr, 0) == n) {
                for (int i = 0; i < n; ++i) map[from + i] = dst[i];
            } else {
                for (int i = from; i <= to; ++i)
                    if (i >= 'A' && i <= 'Z') map[i] = static_cast<wchar_t>(i + 32);
            }
        };
        mapRange(1, 0xD7FF);
        mapRange(0xE000, 0xFFFF);
    }
};

const LowerTable& Lower() {
    static const LowerTable* table = new LowerTable();  // intentionally leaked
    return *table;
}

const wchar_t* kIecUnits[] = {L"  B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB", L"EiB"};
const wchar_t* kSiUnits[] = {L"B  ", L"kB ", L"MB ", L"GB ", L"TB ", L"PB ", L"EB "};

std::wstring Trim(std::wstring s) {
    size_t b = s.find_first_not_of(L' ');
    size_t e = s.find_last_not_of(L' ');
    if (b == std::wstring::npos) return L"";
    return s.substr(b, e - b + 1);
}

}  // namespace

std::wstring FormatSize(uint64_t bytes) {
    const wchar_t** units = g_si ? kSiUnits : kIecUnits;
    double base = g_si ? 1000.0 : 1024.0;
    wchar_t buf[32];
    if (bytes < 1000) {
        swprintf(buf, 32, L"%5llu %s", static_cast<unsigned long long>(bytes), units[0]);
        return buf;
    }
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 999.95 && i < 6) {
        v /= base;
        ++i;
    }
    swprintf(buf, 32, L"%5.1f %s", v, units[i]);
    return buf;
}

std::wstring FormatSizeShort(uint64_t bytes) {
    if (bytes < 1000) return std::to_wstring(bytes) + L" B";
    return Trim(FormatSize(bytes));
}

std::wstring FormatCount(uint64_t n) {
    std::wstring digits = std::to_wstring(n);
    std::wstring out;
    int cnt = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (cnt && cnt % 3 == 0) out.insert(out.begin(), L',');
        out.insert(out.begin(), *it);
        ++cnt;
    }
    return out;
}

std::wstring FormatCountShort(uint64_t n) {
    wchar_t buf[32];
    if (n < 1000000)
        swprintf(buf, 32, L"%6llu", static_cast<unsigned long long>(n));
    else if (n < 1000000000ull)
        swprintf(buf, 32, L"%5.1fM", n / 1e6);
    else
        swprintf(buf, 32, L"%5.1fG", n / 1e9);
    return buf;
}

std::wstring FormatTime(int64_t ft) {
    if (ft <= 0) return L"                ";
    FILETIME f;
    f.dwLowDateTime = static_cast<DWORD>(ft & 0xFFFFFFFF);
    f.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(ft) >> 32);
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&f, &utc)) return L"????-??-?? ??:??";
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;
    wchar_t buf[32];
    swprintf(buf, 32, L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour,
             local.wMinute);
    return buf;
}

std::wstring FormatDuration(double seconds) {
    wchar_t buf[64];
    if (seconds < 60)
        swprintf(buf, 64, L"%.1fs", seconds);
    else if (seconds < 3600)
        swprintf(buf, 64, L"%dm %02ds", static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60);
    else
        swprintf(buf, 64, L"%dh %02dm", static_cast<int>(seconds) / 3600, (static_cast<int>(seconds) / 60) % 60);
    return buf;
}

std::wstring FormatPercent(double pct) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%5.1f%%", pct);
    return buf;
}

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

wchar_t LowerChar(wchar_t c) {
    if (c < 128) return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    return Lower().map[c];
}

std::wstring ToLower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out) c = LowerChar(c);
    return out;
}

bool GlobMatchI(std::wstring_view p, std::wstring_view s) {
    size_t pi = 0, si = 0, star = std::wstring_view::npos, mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && p[pi] == L'*') {
            star = pi++;
            mark = si;
        } else if (pi < p.size() && (p[pi] == L'?' || p[pi] == LowerChar(s[si]))) {
            ++pi;
            ++si;
        } else if (star != std::wstring_view::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == L'*') ++pi;
    return pi == p.size();
}

bool ContainsI(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && LowerChar(hay[i + j]) == needle[j]) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

bool HasWildcards(std::wstring_view s) { return s.find_first_of(L"*?") != std::wstring_view::npos; }

bool ParseSize(std::wstring_view s, uint64_t& out) {
    std::wstring t = ToLower(s);
    while (!t.empty() && t.back() == L' ') t.pop_back();
    size_t i = 0;
    while (i < t.size() && (iswdigit(t[i]) || t[i] == L'.')) ++i;
    if (i == 0) return false;
    double v = _wtof(t.substr(0, i).c_str());
    std::wstring unit = t.substr(i);
    while (!unit.empty() && unit.front() == L' ') unit.erase(unit.begin());
    if (!unit.empty() && unit.back() == L'b') unit.pop_back();
    if (!unit.empty() && unit.back() == L'i') unit.pop_back();
    double mul = 1;
    if (unit.empty()) mul = 1;
    else if (unit == L"k") mul = 1024.0;
    else if (unit == L"m") mul = 1024.0 * 1024;
    else if (unit == L"g") mul = 1024.0 * 1024 * 1024;
    else if (unit == L"t") mul = 1024.0 * 1024 * 1024 * 1024;
    else if (unit == L"p") mul = 1024.0 * 1024 * 1024 * 1024 * 1024;
    else return false;
    out = static_cast<uint64_t>(v * mul);
    return true;
}

std::wstring Win32ErrorMessage(unsigned long err) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring msg;
    if (n && buf) {
        msg.assign(buf, n);
        LocalFree(buf);
        while (!msg.empty() && (msg.back() == L'\n' || msg.back() == L'\r' || msg.back() == L' ' || msg.back() == L'.'))
            msg.pop_back();
    } else {
        wchar_t tmp[64];
        swprintf(tmp, 64, L"Error 0x%08lX", err);
        msg = tmp;
    }
    return msg;
}

int64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

int64_t UnixToFileTime(int64_t unixSeconds) { return unixSeconds * 10000000LL + 116444736000000000LL; }

int64_t FileTimeToUnix(int64_t filetime) { return (filetime - 116444736000000000LL) / 10000000LL; }

namespace {
DWORD WINAPI ThreadProc(LPVOID param) {
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(param));
    (*fn)();
    return 0;
}
}  // namespace

HANDLE StartThread(std::function<void()> fn, size_t stackSize) {
    auto* heapFn = new std::function<void()>(std::move(fn));
    HANDLE h = CreateThread(nullptr, stackSize, ThreadProc, heapFn, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) {
        // Fall back to running synchronously so the work is never silently lost.
        std::unique_ptr<std::function<void()>> owned(heapFn);
        (*owned)();
    }
    return h;
}

}  // namespace util
