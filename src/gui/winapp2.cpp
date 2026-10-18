// winapp2.ini import: the community database of app cleaners (https://github.com/MoscaDotTo/Winapp2),
// turned into Cleanup categories. Only what frees disk space is taken; see ParseWinapp2 in cleanup.h.
#include <windows.h>
#include <shlobj.h>

#include <algorithm>

#include "../util.h"
#include "cleanup.h"

namespace {

std::wstring Trim(const std::wstring& v) {
    size_t a = v.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = v.find_last_not_of(L" \t\r\n");
    return v.substr(a, b - a + 1);
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (true) {
        size_t j = s.find(sep, i);
        out.push_back(Trim(s.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i)));
        if (j == std::wstring::npos) return out;
        i = j + 1;
    }
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring Env(const std::wstring& s) {
    wchar_t buf[32768];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 32768);
    return n && n <= 32768 ? std::wstring(buf) : L"";
}

std::wstring StripSlashes(std::wstring p) {
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

// winapp2 variables that aren't environment variables (or mean more than one folder), turned into
// paths ExpandEnvironmentStrings understands. %ProgramFiles% covers both Program Files folders.
std::vector<std::wstring> Translate(const std::wstring& path) {
    static const std::vector<std::pair<std::wstring, std::vector<std::wstring>>> kMap = {
        {L"%programfiles%", {L"%ProgramFiles%", L"%ProgramFiles(x86)%"}},
        {L"%commonprogramfiles%", {L"%CommonProgramFiles%", L"%CommonProgramFiles(x86)%"}},
        {L"%locallowappdata%", {L"%USERPROFILE%\\AppData\\LocalLow"}},
        {L"%documents%", {KnownFolder(FOLDERID_Documents)}},
        {L"%music%", {KnownFolder(FOLDERID_Music)}},
        {L"%pictures%", {KnownFolder(FOLDERID_Pictures)}},
        {L"%video%", {KnownFolder(FOLDERID_Videos)}},
        {L"%desktop%", {KnownFolder(FOLDERID_Desktop)}},
    };
    std::wstring lower = util::ToLower(path);
    for (auto& [var, repl] : kMap) {
        size_t at = lower.find(var);
        if (at == std::wstring::npos) continue;
        std::vector<std::wstring> out;
        for (auto& r : repl) {
            if (r.empty()) continue;
            std::wstring p = path.substr(0, at) + r + path.substr(at + var.size());
            if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
        }
        return out;
    }
    return {path};
}

// Folders a winapp2 rule may not sweep: only named files directly in them are allowed (a rule like
// "%UserProfile%|*|RECURSE" would take the whole profile).
struct BroadFolders {
    std::vector<std::wstring> lower;
    BroadFolders() {
        for (const wchar_t* v : {L"%SystemDrive%\\", L"%SystemRoot%", L"%SystemRoot%\\System32", L"%SystemRoot%\\SysWOW64",
                                 L"%USERPROFILE%", L"%USERPROFILE%\\AppData", L"%APPDATA%", L"%LOCALAPPDATA%",
                                 L"%USERPROFILE%\\AppData\\LocalLow", L"%ProgramData%", L"%ProgramFiles%",
                                 L"%ProgramFiles(x86)%", L"%CommonProgramFiles%", L"%CommonProgramFiles(x86)%", L"%PUBLIC%",
                                 L"%SystemDrive%\\Users"})
            Add(Env(v));
        for (REFKNOWNFOLDERID id : {FOLDERID_Documents, FOLDERID_Desktop, FOLDERID_Downloads, FOLDERID_Music,
                                    FOLDERID_Pictures, FOLDERID_Videos})
            Add(KnownFolder(id));
    }
    void Add(const std::wstring& p) {
        std::wstring s = util::ToLower(StripSlashes(p));
        if (!s.empty() && s.find(L'%') == std::wstring::npos) lower.push_back(s);
    }
    // `path` (variables translated, not expanded) is a broad folder, or a bare '*' right below one.
    bool Is(const std::wstring& path) const {
        std::wstring p = util::ToLower(StripSlashes(Env(path)));
        if (p.size() <= 2) return true;  // a drive, or nothing
        if (std::find(lower.begin(), lower.end(), p) != lower.end()) return true;
        size_t star = p.find(L"\\*");
        return star != std::wstring::npos && std::find(lower.begin(), lower.end(), p.substr(0, star)) != lower.end();
    }
};

bool WholePattern(const std::wstring& p) { return p.empty() || p == L"*" || p == L"*.*"; }

// Entries about personal data rather than disk space: cookies, history, passwords, sessions,
// settings, backups, saved games. Cleanup is for space; these stay out of it.
bool PersonalEntry(const std::wstring& name) {
    std::wstring n = L" " + util::ToLower(name) + L" ";
    for (auto& c : n)
        if (!iswalnum(c)) c = L' ';
    static const wchar_t* const kParts[] = {L"cookie", L"histor", L"password", L"login", L"session", L"autofill",
                                            L"auto fill", L"bookmark", L"form data", L"autocomplete", L"setting",
                                            L"preference", L"config", L"backup", L"favorite", L"favourite", L"playlist",
                                            L"credential", L"wallet", L"account", L"savegame", L"jump list", L"jumplist"};
    static const wchar_t* const kWords[] = {L" tab ", L" tabs ", L" mru ", L" recent ", L" saved ", L" saves ", L" save "};
    for (const wchar_t* p : kParts)
        if (n.find(p) != std::wstring::npos) return true;
    for (const wchar_t* w : kWords)
        if (n.find(w) != std::wstring::npos) return true;
    return false;
}

struct Raw {
    std::wstring name, section, warning;
    std::vector<std::wstring> detect, files, excludes;
    bool registry = false;
};

void Finish(Raw& r, const BroadFolders& broad, std::vector<CleanCategory>& out, Winapp2Stats& st) {
    if (r.name.empty()) return;
    ++st.entries;
    if (PersonalEntry(r.name)) {
        ++st.personal;
        return;
    }
    CleanCategory c;
    c.imported = true;
    c.safe = false;
    c.icon = 0xE71D;  // all apps
    c.name = r.name;
    c.id = L"w2:" + util::ToLower(r.name);
    for (auto& v : r.files) {
        std::vector<std::wstring> f = Split(v, L'|');
        CleanTarget t;
        std::wstring pattern = f.size() > 1 ? f[1] : L"*";
        for (size_t i = 2; i < f.size(); ++i) {
            if (_wcsicmp(f[i].c_str(), L"RECURSE") == 0) t.recurse = true;
            if (_wcsicmp(f[i].c_str(), L"REMOVESELF") == 0) t.removeSelf = true;
        }
        // "*" with RECURSE or REMOVESELF: everything in the folder, taken as whole entries (or the
        // folder itself); "*" alone: the files directly in it.
        bool whole = WholePattern(pattern);
        t.filePattern = whole && (t.recurse || t.removeSelf) ? L"" : (whole ? L"*" : pattern);
        if (whole && !t.removeSelf) t.recurse = false;
        for (auto& p : Translate(StripSlashes(f[0]))) {
            if (broad.Is(p) && (t.recurse || t.removeSelf || whole)) {
                ++st.broad;
                continue;
            }
            t.path = p;
            c.targets.push_back(t);
        }
    }
    if (c.targets.empty()) {
        if (r.files.empty() && r.registry) ++st.regOnly;
        return;
    }
    for (auto& v : r.excludes) {
        std::vector<std::wstring> f = Split(v, L'|');
        if (f.size() < 2) continue;
        bool tree = _wcsicmp(f[0].c_str(), L"PATH") == 0;
        if (!tree && _wcsicmp(f[0].c_str(), L"FILE") != 0) continue;  // REG: no registry cleaning here
        for (auto& p : Translate(StripSlashes(f[1]))) c.excludes.push_back({p, f.size() > 2 && !f[2].empty() ? f[2] : L"*", tree});
    }
    for (auto& d : r.detect)
        for (auto& p : Translate(d)) c.detect.push_back(p);
    std::wstring where = r.files.empty() ? L"" : StripSlashes(Split(r.files[0], L'|')[0]);
    if (r.files.size() > 1) where += L" and " + std::to_wstring(r.files.size() - 1) + L" more";
    c.desc = !r.warning.empty() ? L"Note: " + r.warning : (r.section.empty() ? where : r.section + L"  ·  " + where);
    out.push_back(std::move(c));
    ++st.usable;
}

}  // namespace

std::vector<CleanCategory> ParseWinapp2(const std::wstring& text, Winapp2Stats& st) {
    st = {};
    std::vector<CleanCategory> out;
    BroadFolders broad;
    Raw cur;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            Finish(cur, broad, out, st);
            cur = Raw();
            size_t close = line.rfind(L']');
            std::wstring name = Trim(line.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1));
            if (name.size() > 2 && name.compare(name.size() - 2, 2, L" *") == 0) name = Trim(name.substr(0, name.size() - 2));
            cur.name = name;
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || cur.name.empty()) continue;
        std::wstring key = util::ToLower(Trim(line.substr(0, eq))), val = Trim(line.substr(eq + 1));
        while (!key.empty() && iswdigit(key.back())) key.pop_back();  // FileKey12 -> filekey
        if (val.empty()) continue;
        if (key == L"filekey") cur.files.push_back(val);
        else if (key == L"excludekey") cur.excludes.push_back(val);
        else if (key == L"detect" || key == L"detectfile") cur.detect.push_back(val);
        else if (key == L"regkey") cur.registry = true;
        else if (key == L"section") cur.section = val;
        else if (key == L"warning") cur.warning = val;
        else if (key == L"specialdetect") {
            // Older files name the big browsers this way.
            std::wstring s = util::ToLower(val);
            if (s == L"det_chrome") cur.detect.push_back(L"%LOCALAPPDATA%\\Google\\Chrome*");
            else if (s == L"det_mozilla") cur.detect.push_back(L"%APPDATA%\\Mozilla\\Firefox");
            else if (s == L"det_thunderbird") cur.detect.push_back(L"%APPDATA%\\Thunderbird");
            else if (s == L"det_opera") cur.detect.push_back(L"%APPDATA%\\Opera Software");
        }
    }
    Finish(cur, broad, out, st);
    return out;
}

std::wstring Winapp2File() {
    std::wstring dir = util::DataDir();
    return dir.empty() ? L"" : dir + L"\\winapp2.ini";
}

bool ImportWinapp2(const std::wstring& source, Winapp2Stats& st, std::wstring& err) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, source.c_str(), L"rb") != 0 || !f) {
        err = L"Can't open " + source;
        return false;
    }
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF) data.erase(0, 3);
    ParseWinapp2(util::Utf8ToWide(data), st);
    if (st.entries == 0) {
        err = L"This doesn't look like a winapp2.ini: it has no [entries].";
        return false;
    }
    std::wstring dest = Winapp2File();
    if (dest.empty()) {
        err = L"Can't find the local app data folder.";
        return false;
    }
    if (_wcsicmp(dest.c_str(), source.c_str()) == 0) return true;
    CreateDirectoryW(dest.substr(0, dest.rfind(L'\\')).c_str(), nullptr);
    if (!CopyFileW(source.c_str(), dest.c_str(), FALSE)) {
        err = util::Win32ErrorMessage(GetLastError());
        return false;
    }
    // A fresh timestamp, so a copy of an older file is parsed again.
    HANDLE h = CreateFileW(dest.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        SetFileTime(h, nullptr, nullptr, &now);
        CloseHandle(h);
    }
    return true;
}
