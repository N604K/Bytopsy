#include "journal.h"

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <mutex>

#include "fsops.h"
#include "tree.h"
#include "util.h"

namespace {

std::mutex g_journalLock;
constexpr int64_t kRotateBytes = 4 << 20;  // journal.log -> journal.1.log

std::wstring FormatUtc(int64_t ticks) {
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(ticks);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool ParseUtc(const std::wstring& s, int64_t& ticks) {
    SYSTEMTIME st{};
    if (swscanf_s(s.c_str(), L"%4hu-%2hu-%2huT%2hu:%2hu:%2hu", &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute,
                  &st.wSecond) != 6)
        return false;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    ticks = (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return true;
}

// Tabs and line breaks would break the line format (Windows forbids them in names anyway).
std::wstring Field(std::wstring s) {
    for (auto& c : s)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return s;
}

std::vector<std::wstring> SplitTabs(const std::wstring& line) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (true) {
        size_t j = line.find(L'\t', i);
        out.push_back(line.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i));
        if (j == std::wstring::npos) break;
        i = j + 1;
    }
    return out;
}

bool ParseAction(const std::wstring& s, JournalAction& a) {
    for (JournalAction x : {JournalAction::Recycle, JournalAction::Delete, JournalAction::Clean, JournalAction::Restore})
        if (s == JournalActionName(x)) {
            a = x;
            return true;
        }
    return false;
}

std::wstring CurrentUserSid() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return L"";
    DWORD n = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> buf(n);
    std::wstring out;
    if (n && GetTokenInformation(tok, TokenUser, buf.data(), n, &n)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
            out = s;
            LocalFree(s);
        }
    }
    CloseHandle(tok);
    return out;
}

bool ReadSmallFile(const std::wstring& path, std::string& data, size_t limit) {
    HANDLE h = CreateFileW(ToLongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    data.resize(limit);
    DWORD got = 0;
    BOOL ok = ReadFile(h, data.data(), static_cast<DWORD>(limit), &got, nullptr);
    CloseHandle(h);
    data.resize(ok ? got : 0);
    return ok != FALSE;
}

// $I record: int64 version, int64 size, FILETIME deleted, then the original path:
// version 1 (Vista to 8.1): WCHAR[260]; version 2 (Windows 10+): uint32 length (with NUL), WCHAR[length].
bool ParseInfoRecord(const std::string& d, RecycledItem& it) {
    if (d.size() < 28) return false;
    int64_t version;
    memcpy(&version, d.data(), 8);
    memcpy(&it.size, d.data() + 8, 8);
    memcpy(&it.deleted, d.data() + 16, 8);
    size_t start, maxChars;
    if (version == 1) {
        start = 24;
        maxChars = std::min<size_t>(260, (d.size() - 24) / 2);
    } else if (version == 2) {
        uint32_t n;
        memcpy(&n, d.data() + 24, 4);
        start = 28;
        maxChars = std::min<size_t>(n, (d.size() - 28) / 2);
    } else {
        return false;
    }
    std::wstring p(maxChars, L'\0');
    memcpy(p.data(), d.data() + start, maxChars * 2);
    p.resize(wcsnlen(p.c_str(), p.size()));
    if (p.size() < 3) return false;
    it.original = p;
    return true;
}

bool CreateDirs(const std::wstring& dir) {
    if (PathExists(dir)) return true;
    size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 2 && !CreateDirs(dir.substr(0, slash))) return false;
    return CreateDirectoryW(ToLongPath(dir).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace

std::wstring JournalFile() {
    wchar_t over[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"WINNCDU_JOURNAL", over, MAX_PATH);  // tests
    if (n && n < MAX_PATH) return over;
    std::wstring dir = util::DataDir();
    return dir.empty() ? L"" : dir + L"\\journal.log";
}

const wchar_t* JournalActionName(JournalAction a) {
    switch (a) {
        case JournalAction::Recycle: return L"recycle";
        case JournalAction::Delete: return L"delete";
        case JournalAction::Clean: return L"clean";
        case JournalAction::Restore: return L"restore";
    }
    return L"";
}

void JournalAppend(const JournalEntry& e) {
    std::wstring file = JournalFile();
    if (file.empty()) return;
    std::lock_guard<std::mutex> lk(g_journalLock);
    CreateDirectoryW(util::DataDir().c_str(), nullptr);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool exists = GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa) != FALSE;
    if (exists && ((static_cast<int64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) > kRotateBytes) {
        std::wstring old = file.substr(0, file.size() - 4) + L".1.log";
        exists = !MoveFileExW(file.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"ab") != 0 || !f) return;
    std::wstring line;
    if (!exists) line = L"# WinNCDU deletion journal. Columns: time (UTC), action, bytes, items, path, note\r\n";
    line += FormatUtc(e.time ? e.time : util::NowFileTime()) + L"\t" + JournalActionName(e.action) + L"\t" +
            std::to_wstring(e.bytes) + L"\t" + std::to_wstring(e.items) + L"\t" + Field(e.path) + L"\t" + Field(e.note) + L"\r\n";
    std::string u = util::WideToUtf8(line);
    fwrite(u.data(), 1, u.size(), f);
    fclose(f);
}

std::vector<JournalEntry> JournalRead(size_t maxEntries) {
    std::vector<JournalEntry> out;
    std::wstring file = JournalFile();
    if (file.empty()) return out;
    std::string data;
    {
        std::lock_guard<std::mutex> lk(g_journalLock);
        if (!ReadSmallFile(file, data, static_cast<size_t>(kRotateBytes) * 2)) return out;
    }
    std::wstring text = util::Utf8ToWide(data);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        auto f = SplitTabs(line);
        JournalEntry e;
        if (f.size() < 5 || !ParseUtc(f[0], e.time) || !ParseAction(f[1], e.action)) continue;
        e.bytes = _wcstoui64(f[2].c_str(), nullptr, 10);
        e.items = _wcstoui64(f[3].c_str(), nullptr, 10);
        e.path = f[4];
        if (f.size() > 5) e.note = f[5];
        out.push_back(std::move(e));
    }
    std::reverse(out.begin(), out.end());
    if (out.size() > maxEntries) out.resize(maxEntries);
    return out;
}

std::vector<RecycledItem> ListRecycleBin() {
    std::vector<RecycledItem> out;
    std::wstring sid = CurrentUserSid();
    if (sid.empty()) return out;
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        UINT t = GetDriveTypeW(root.c_str());
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        std::wstring dir = root + L"$Recycle.Bin\\" + sid;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\$I*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring name = fd.cFileName;
            if (name.size() < 3) continue;
            RecycledItem it;
            it.infoFile = dir + L"\\" + name;
            it.dataFile = dir + L"\\$R" + name.substr(2);
            std::string data;
            if (!ReadSmallFile(it.infoFile, data, 64 * 1024) || !ParseInfoRecord(data, it)) continue;
            if (GetFileAttributesW(ToLongPath(it.dataFile).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            out.push_back(std::move(it));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetErrorMode(oldMode);
    return out;
}

const RecycledItem* FindRecycled(const std::vector<RecycledItem>& bin, const JournalEntry& e) {
    if (e.action != JournalAction::Recycle) return nullptr;
    // Same original path, deleted around the time of the entry (closest wins if it was deleted twice).
    const int64_t kWindow = 6LL * 3600 * 10000000;
    const RecycledItem* best = nullptr;
    int64_t bestDist = 0;
    for (auto& it : bin) {
        if (_wcsicmp(it.original.c_str(), e.path.c_str()) != 0) continue;
        int64_t d = it.deleted > e.time ? it.deleted - e.time : e.time - it.deleted;
        if (d > kWindow) continue;
        if (!best || d < bestDist) {
            best = &it;
            bestDist = d;
        }
    }
    return best;
}

bool RestoreRecycled(const RecycledItem& it, std::wstring& err) {
    if (PathExists(it.original)) {
        err = it.original + L": something with this name is there now. Rename or move it, then try again.";
        return false;
    }
    size_t slash = it.original.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 2 && !CreateDirs(it.original.substr(0, slash))) {
        err = it.original.substr(0, slash) + L": " + util::Win32ErrorMessage(GetLastError());
        return false;
    }
    DWORD attrs = GetFileAttributesW(ToLongPath(it.dataFile).c_str());
    if (!MoveFileExW(ToLongPath(it.dataFile).c_str(), ToLongPath(it.original).c_str(), 0)) {
        err = it.original + L": " + util::Win32ErrorMessage(GetLastError());
        return false;
    }
    DeleteFileW(ToLongPath(it.infoFile).c_str());
    bool dir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
    SHChangeNotify(dir ? SHCNE_MKDIR : SHCNE_CREATE, SHCNF_PATHW, it.original.c_str(), nullptr);
    SHChangeNotify(dir ? SHCNE_RMDIR : SHCNE_DELETE, SHCNF_PATHW, it.dataFile.c_str(), nullptr);
    JournalEntry e;
    e.action = JournalAction::Restore;
    e.bytes = it.size;
    e.path = it.original;
    JournalAppend(e);
    return true;
}
