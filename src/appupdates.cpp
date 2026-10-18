#include "appupdates.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <tuple>

#include "i18n.h"
#include "runtool.h"
#include "util.h"

const wchar_t* const kAppInstallerStoreLink = L"ms-windows-store://pdp/?productid=9NBLGGH4NNS1";

namespace {

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? std::wstring(buf, n) : L"";
}

bool Exists(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring Trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == 0xA0)) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == 0xA0)) --e;
    return std::wstring(s.substr(b, e - b));
}

bool EndsWithEllipsis(const std::wstring& s) { return !s.empty() && s.back() == L'…'; }

// Terminal cells a character takes (winget lays its table out in cells: East Asian wide
// characters take two).
int Cells(wchar_t c) {
    if (c >= 0xDC00 && c <= 0xDFFF) return 0;  // low surrogate: counted with its high half
    if (c >= 0xD800 && c <= 0xDBFF) return 2;  // emoji and other characters beyond the BMP
    if ((c >= 0x0300 && c <= 0x036F) || c == 0x200B || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F)) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
        (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6))
        return 2;
    return 1;
}

// The cell each character starts at.
std::vector<int> CellStarts(const std::wstring& s, int* total = nullptr) {
    std::vector<int> v(s.size());
    int x = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        v[i] = x;
        x += Cells(s[i]);
    }
    if (total) *total = x;
    return v;
}

bool IsDashLine(const std::wstring& l) {
    std::wstring t = Trim(l);
    if (t.size() < 10) return false;
    for (wchar_t c : t)
        if (c != L'-' && c != 0x2500) return false;
    return true;
}

bool IsBlank(const std::wstring& l) { return Trim(l).empty(); }

// The character covering cell `cell` (L'\0' past the end).
wchar_t CharAtCell(const std::wstring& s, const std::vector<int>& starts, int cell) {
    for (size_t i = 0; i < s.size(); ++i) {
        int w = Cells(s[i]);
        if (cell >= starts[i] && cell < starts[i] + std::max(w, 1)) return s[i];
        if (starts[i] > cell) break;
    }
    return 0;
}

std::wstring CellText(const std::wstring& s, const std::vector<int>& starts, int from, int to) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i)
        if (starts[i] >= from && (to < 0 || starts[i] < to)) out += s[i];
    return Trim(out);
}

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

// "12 package(s) have ..." -> 12 when the rest matches.
int CountNote(const std::wstring& line, const wchar_t* rest) {
    std::wstring t = Trim(line);
    size_t i = 0;
    while (i < t.size() && iswdigit(t[i])) ++i;
    if (!i || t.find(rest, i) == std::wstring::npos) return -1;
    return _wtoi(t.substr(0, i).c_str());
}

// Runs winget (or the test's .cmd stand-in).
int RunWinget(const std::wstring& winget, const std::wstring& args, std::string& out, unsigned long timeoutMs,
              std::atomic<bool>* cancel) {
    std::wstring ext = winget.size() > 4 ? util::ToLower(winget.substr(winget.size() - 4)) : L"";
    if (ext == L".cmd" || ext == L".bat")
        return RunTool(SystemTool(L"cmd.exe"), L"/d /s /c \"\"" + winget + L"\" " + args + L"\"", out, timeoutMs, cancel);
    return RunTool(winget, args, out, timeoutMs, cancel);
}

std::wstring NormName(const std::wstring& s) {
    std::wstring out;
    bool space = false;
    for (wchar_t c : util::ToLower(Trim(s))) {
        if (c == L' ' || c == L'\t' || c == 0xA0) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += L' ';
        space = false;
        out += c;
    }
    return out;
}

std::wstring Alnum(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : util::ToLower(s))
        if (iswalnum(c)) out += c;
    return out;
}

// "< 3.10.8" -> "3.10.8"; "1.2.0.0" -> "1.2".
std::wstring NormVersion(std::wstring v) {
    v = Trim(v);
    while (!v.empty() && (v[0] == L'<' || v[0] == L'>' || v[0] == L'=' || v[0] == L' ')) v.erase(v.begin());
    while (v.size() > 2 && v.compare(v.size() - 2, 2, L".0") == 0) v.resize(v.size() - 2);
    return util::ToLower(v);
}

}  // namespace

bool ValidWingetId(const std::wstring& id) {
    if (id.empty() || id.size() > 260 || EndsWithEllipsis(id)) return false;
    for (wchar_t c : id)
        if (c <= L' ' || c == L'"' || c == L'\'' || c == L'&' || c == L'|' || c == L'<' || c == L'>' || c == L'^' || c == L'%' ||
            c == L'!' || c == L'`' || c == L'(' || c == L')' || c == L'…' || c == 0x7F)
            return false;
    return true;
}

bool WingetUpdate::Updatable() const { return !idCut && ValidWingetId(id); }

std::wstring FindWinget() {
    std::wstring test = Env(L"BYTOPSY_WINGET");  // tests only
    if (!test.empty()) return Exists(test) ? test : L"";
    std::wstring p = FindOnPath(L"winget.exe");
    if (Exists(p)) return p;
    std::wstring local = Env(L"LOCALAPPDATA");
    if (!local.empty()) {
        p = local + L"\\Microsoft\\WindowsApps\\winget.exe";
        if (Exists(p)) return p;
    }
    return L"";
}

// ---- When winget can't be found

const wchar_t* const kAppInstallerFamily = L"Microsoft.DesktopAppInstaller_8wekyb3d8bbwe";
const wchar_t* const kWingetReleasesLink = L"https://github.com/microsoft/winget-cli/releases/latest";
const wchar_t* const kAppAliasesSettings = L"ms-settings:advanced-apps";

bool IsAppInstallerPackage(const std::wstring& fullName) {
    const std::wstring name = L"microsoft.desktopappinstaller_", publisher = L"_8wekyb3d8bbwe";
    std::wstring l = util::ToLower(fullName);
    return l.size() > name.size() + publisher.size() && l.compare(0, name.size(), name) == 0 &&
           l.compare(l.size() - publisher.size(), publisher.size(), publisher) == 0;
}

namespace {

// The App Installer packages among a registry key's subkeys (each named after a package full name).
void AppInstallerKeys(HKEY root, const wchar_t* path, std::vector<std::wstring>& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD n = 512;
        LONG rc = RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_MORE_DATA) continue;
        if (rc != ERROR_SUCCESS) break;
        std::wstring s(name, n);
        if (IsAppInstallerPackage(s) && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
    RegCloseKey(k);
}

// The package to name: an app package before its bundle ("..._neutral_~_...").
std::wstring PickPackage(const std::vector<std::wstring>& v) {
    for (auto& p : v)
        if (p.find(L"_~_") == std::wstring::npos) return p;
    return v.empty() ? L"" : v.front();
}

}  // namespace

WingetProbe ProbeWinget() {
    WingetProbe p;
    p.winget = FindWinget();
    std::wstring test = util::ToLower(Env(L"BYTOPSY_APPINSTALLER"));  // tests only
    if (!test.empty()) {
        std::wstring pkg = L"Microsoft.DesktopAppInstaller_1.29.379.0_x64__8wekyb3d8bbwe";
        if (test == L"user" || test == L"alias") p.userPackages.push_back(pkg);
        if (test != L"none") p.machinePackages.push_back(pkg);
        p.packageAlias = test == L"alias";
        return p;
    }
    AppInstallerKeys(HKEY_CURRENT_USER,
                     L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
                     p.userPackages);
    AppInstallerKeys(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\PackageRepository\\Packages",
                     p.machinePackages);
    AppInstallerKeys(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\Applications",
                     p.machinePackages);
    std::wstring local = Env(L"LOCALAPPDATA");
    if (!local.empty())
        p.packageAlias = Exists(local + L"\\Microsoft\\WindowsApps\\" + kAppInstallerFamily + L"\\winget.exe");
    return p;
}

WingetDiagnosis DiagnoseWinget(const WingetProbe& p) {
    WingetDiagnosis d;
    d.winget = p.winget;
    if (!p.winget.empty()) {
        d.state = WingetState::Found;
        d.package = PickPackage(p.userPackages);
    } else if (!p.userPackages.empty()) {
        d.state = p.packageAlias ? WingetState::AliasOff : WingetState::NotReachable;
        d.package = PickPackage(p.userPackages);
    } else if (!p.machinePackages.empty()) {
        d.state = WingetState::NotRegistered;
        d.package = PickPackage(p.machinePackages);
    } else {
        d.state = WingetState::NotInstalled;
    }
    return d;
}

std::wstring WingetRepairCommand() { return L"Add-AppxPackage -RegisterByFamilyName -MainPackage " + std::wstring(kAppInstallerFamily); }

std::wstring WingetRepairArgs() { return L"-NoProfile -NonInteractive -Command \"" + WingetRepairCommand() + L"\""; }

WingetRepairResult RepairWinget() {
    WingetRepairResult r;
    std::wstring standIn = Env(L"BYTOPSY_WINGET_REPAIR");  // tests only: a .cmd run instead
    bool simulated = !Env(L"BYTOPSY_WINGET").empty() || !Env(L"BYTOPSY_APPINSTALLER").empty();
    std::string raw;
    if (!standIn.empty())
        r.code = Exists(standIn) ? RunWinget(standIn, L"", raw, 5 * 60 * 1000, nullptr) : -1;
    else if (simulated) {
        r.outcome = WingetRepairOutcome::NotRun;  // never touch the real App Installer while winget is simulated
        r.after = DiagnoseWinget();
        return r;
    } else
        r.code = RunTool(SystemTool(L"WindowsPowerShell\\v1.0\\powershell.exe"), WingetRepairArgs(), raw, 5 * 60 * 1000, nullptr);
    r.after = DiagnoseWinget();
    for (auto& l : ConsoleLines(DecodeToolOutput(raw)))
        if (!Trim(l).empty()) {
            r.detail = Trim(l).substr(0, 300);
            break;
        }
    if (r.after.state == WingetState::Found)
        r.outcome = WingetRepairOutcome::Fixed;
    else if (r.code == -1)
        r.outcome = WingetRepairOutcome::NoPowerShell;
    else if (r.code == -2)
        r.outcome = WingetRepairOutcome::TimedOut;
    else if (r.code != 0)
        r.outcome = WingetRepairOutcome::Failed;
    else if (r.after.state == WingetState::AliasOff || r.after.state == WingetState::NotReachable)
        r.outcome = WingetRepairOutcome::StillAlias;
    else
        r.outcome = WingetRepairOutcome::StillMissing;
    if (r.outcome != WingetRepairOutcome::Failed) r.detail.clear();
    return r;
}

std::wstring WingetRepairJournalNote(const WingetRepairResult& r) {
    std::wstring what;
    switch (r.outcome) {
        case WingetRepairOutcome::Fixed: what = L"winget found again (" + r.after.winget + L")"; break;
        case WingetRepairOutcome::StillAlias: what = L"registered, but winget.exe still isn't reachable (App execution alias off?)"; break;
        case WingetRepairOutcome::Failed: what = L"failed: " + r.detail; break;
        case WingetRepairOutcome::StillMissing: what = L"PowerShell succeeded, but App Installer still isn't registered"; break;
        case WingetRepairOutcome::NoPowerShell: what = L"powershell.exe couldn't be started"; break;
        case WingetRepairOutcome::TimedOut: what = L"stopped after 5 minutes"; break;
        case WingetRepairOutcome::NotRun: what = L"not run (a test setting stands in for winget)"; break;
    }
    std::wstring s = L"powershell.exe " + WingetRepairArgs() + L": " + what;
    if (r.code > 0 || r.code < -2) s += L" (exit code " + std::to_wstring(r.code) + L")";
    return s;
}

std::wstring WingetMissingText(const WingetDiagnosis& d) {
    const std::wstring repair = L"  powershell.exe " + WingetRepairArgs() + L"\n";
    const std::wstring aliases = L"  Settings > Apps > Advanced app settings > App execution aliases: turn on App Installer (winget.exe)\n  " +
                                 std::wstring(kAppAliasesSettings) + L"\n";
    const std::wstring pkg = d.package.empty() ? L"" : L" (" + d.package + L")";
    switch (d.state) {
        case WingetState::Found: return L"winget: " + d.winget + L"\n";
        case WingetState::AliasOff:
            return L"winget can't be found: App Installer" + pkg + L" is registered for your account, but its winget.exe App execution alias is turned off.\n\n"
                   L"Turn the alias on:\n" + aliases +
                   L"\nIf that doesn't help, register App Installer again for your account (no admin, nothing is downloaded):\n" + repair;
        case WingetState::NotReachable:
            return L"winget can't be found: App Installer" + pkg + L" is registered for your account, but winget.exe isn't there "
                   L"(a new account before the Microsoft Store finished setting it up, or its alias is turned off).\n\n"
                   L"Register App Installer again for your account (no admin, nothing is downloaded):\n" + repair +
                   L"\nIf winget is still missing afterwards, turn its alias on:\n" + aliases;
        case WingetState::NotRegistered:
            return L"winget can't be found: App Installer" + pkg + L" is on this PC, but isn't registered for your account.\n\n"
                   L"Register it for your account (no admin, nothing is downloaded):\n" + repair;
        case WingetState::NotInstalled: break;
    }
    return L"winget isn't installed: App Installer isn't on this PC (Windows LTSC or Server, or the Microsoft Store was removed).\n\n"
           L"Get App Installer from the Microsoft Store (the easy way):\n  " + std::wstring(kAppInstallerStoreLink) +
           L"\nOn a PC without the Store, Microsoft's winget releases:\n  " + kWingetReleasesLink + L"\n";
}

std::vector<std::wstring> ConsoleLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstring cur;
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c == 0x1B) {  // escape sequence
            if (i + 1 < text.size() && text[i + 1] == L'[') {
                i += 2;
                while (i < text.size() && !(text[i] >= 0x40 && text[i] <= 0x7E)) ++i;
            } else if (i + 1 < text.size() && text[i + 1] == L']') {
                i += 2;
                while (i < text.size() && text[i] != 0x07 && !(text[i] == 0x1B && i + 1 < text.size() && text[i + 1] == L'\\')) ++i;
                if (i < text.size() && text[i] == 0x1B) ++i;
            } else {
                ++i;
            }
            continue;
        }
        if (c == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') continue;
            cur.clear();  // the spinner went back to the start of the line
            continue;
        }
        if (c == L'\n') {
            lines.push_back(cur);
            cur.clear();
            continue;
        }
        if (c == L'\b') {
            if (!cur.empty()) cur.pop_back();
            continue;
        }
        if (c == 0xFEFF) continue;
        if (c == L'\t') c = L' ';
        if (c < 0x20) continue;
        cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

WingetList ParseWingetUpgrade(const std::wstring& text) {
    WingetList r;
    std::vector<std::wstring> lines = ConsoleLines(text);
    int tables = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& l = lines[i];
        int n;
        if ((n = CountNote(l, L"have version numbers that cannot be determined")) >= 0) r.unknownCount += n;
        if ((n = CountNote(l, L"have pins that prevent upgrade")) >= 0) r.pinnedCount += n;
        if ((n = CountNote(l, L"have a pin that needs to be removed")) >= 0) r.pinnedCount += n;
        if (StartsWithI(Trim(l), L"No installed package found matching input criteria") ||
            StartsWithI(Trim(l), L"No applicable upgrade found"))
            r.ok = true;
        if (!IsDashLine(l) || i == 0 || IsBlank(lines[i - 1])) continue;

        // The header: column starts are where its labels start (in cells).
        std::wstring header = lines[i - 1];
        size_t lead = header.find_first_not_of(L' ');
        if (lead != std::wstring::npos && l.find_first_not_of(L' ') == 0) header.erase(0, lead);
        std::vector<int> hs = CellStarts(header);
        std::vector<int> cols;
        for (size_t k = 0; k < header.size(); ++k)
            if (header[k] != L' ' && (k == 0 || header[k - 1] == L' ')) cols.push_back(hs[k]);
        if (cols.size() < 4) continue;

        // The rows: up to a blank line, or the next table's header.
        size_t end = i + 1;
        while (end < lines.size() && !IsBlank(lines[end]) && !IsDashLine(lines[end]) &&
               !(end + 1 < lines.size() && IsDashLine(lines[end + 1])))
            ++end;
        // A label with a space in it ("Versión disponible") is one column: a start that most rows
        // run across isn't one.
        std::vector<std::vector<int>> starts;
        for (size_t j = i + 1; j < end; ++j) starts.push_back(CellStarts(lines[j]));
        for (size_t k = cols.size(); k-- > 1;) {
            int across = 0, seen = 0;
            for (size_t j = i + 1; j < end; ++j) {
                if (lines[j].find(L"  ") == std::wstring::npos) continue;  // a sentence, not a padded row
                const auto& st = starts[j - i - 1];
                wchar_t before = CharAtCell(lines[j], st, cols[k] - 1);
                if (!before) continue;
                ++seen;
                if (before != L' ') ++across;
            }
            if (seen && across * 2 >= seen) cols.erase(cols.begin() + k);
        }
        if (cols.size() < 4) continue;
        r.ok = true;
        for (size_t j = i + 1; j < end; ++j) {
            const std::wstring& row = lines[j];
            const auto& st = starts[j - i - 1];
            bool fits = true;
            for (size_t k = 1; k < cols.size() && fits; ++k) {
                wchar_t before = CharAtCell(row, st, cols[k] - 1);
                fits = !before || before == L' ';
            }
            if (!fits) continue;
            auto cell = [&](size_t k) { return k < cols.size() ? CellText(row, st, cols[k], k + 1 < cols.size() ? cols[k + 1] : -1) : L""; };
            WingetUpdate u;
            u.name = cell(0);
            u.id = cell(1);
            u.version = cell(2);
            u.available = cell(3);
            u.source = cols.size() > 4 ? cell(4) : L"";
            if (u.name.empty() || u.id.empty() || u.version.empty() || u.available.empty() || u.id.find(L' ') != std::wstring::npos)
                continue;  // "32 upgrades available." and other notes
            u.nameCut = EndsWithEllipsis(u.name);
            u.idCut = EndsWithEllipsis(u.id);
            u.unknownVersion = _wcsicmp(u.version.c_str(), L"Unknown") == 0;
            u.explicitOnly = tables > 0;
            r.updates.push_back(u);
        }
        ++tables;
        i = end - 1;
    }
    return r;
}

std::wstring WingetCodeText(int code) {
    switch (static_cast<uint32_t>(code)) {
        case 0: return N_(L"Updated");
        case 0x8A15002B: return N_(L"No applicable update found");
        case 0x8A150014: return N_(L"winget no longer finds this app");
        case 0x8A150015: return N_(L"More than one package matches this id");
        case 0x8A150008: return N_(L"The download failed");
        case 0x8A150011: return N_(L"The installer's hash doesn't match its manifest");
        case 0x8A150041: return N_(L"The package's agreements were not accepted");
        case 0x8A150046: return N_(L"winget's source agreements were not accepted");
        case 0x8A150068: return N_(L"A pin keeps this app from updating (see winget pin list)");
        case 0x8A150101: return N_(L"The app is running: close it and try again");
        case 0x8A150102: return N_(L"Another installation is in progress: try again later");
        case 0x8A150103: return N_(L"A file is in use by a running app: close it and try again");
        case 0x8A150104: return N_(L"A dependency is missing");
        case 0x8A150105: return N_(L"The disk is full");
        case 0x8A150106: return N_(L"Not enough memory");
        case 0x8A150107: return N_(L"No network connection");
        case 0x8A150109: return N_(L"Updated: restart Windows to finish");
        case 0x8A15010A: return N_(L"Restart Windows, then update again");
        case 0x8A15010B: return N_(L"Windows is restarting to finish the update");
        case 0x8A15010C: return N_(L"The installer was cancelled");
        case 0x8A15010E: return N_(L"A newer version is already installed");
        case 0x8A15010F: return N_(L"Blocked by policy");
        case 0x8A150114: return N_(L"Its installer can't update an installed version");
        default: return L"";
    }
}

std::wstring WingetUpgradeArgs(const WingetUpdate& u) {
    std::wstring a = L"upgrade --id " + u.id + L" --exact";
    if (!u.source.empty() && ValidWingetId(u.source)) a += L" --source " + u.source;
    if (u.unknownVersion) a += L" --include-unknown";
    return a + L" --disable-interactivity --accept-package-agreements --accept-source-agreements";
}

WingetList ListWingetUpdates(const std::wstring& winget, std::atomic<bool>* cancel) {
    std::string raw;
    int code = RunWinget(winget, L"upgrade --disable-interactivity", raw, 5 * 60 * 1000, cancel);
    WingetList r = ParseWingetUpgrade(DecodeToolOutput(raw));
    if (code == -1) {
        r.ok = false;
        r.error = N_(L"winget couldn't be started");
    } else if (code == -2) {
        r.ok = false;
        r.error = N_(L"Cancelled");
    } else if (!r.ok && (code == 0 || static_cast<uint32_t>(code) == 0x8A150014)) {
        r.ok = true;  // nothing to update, said in another language
    } else if (!r.ok) {
        std::wstring known = WingetCodeText(code);
        r.error = !known.empty() ? known : LastLine(raw);
        if (r.error.empty()) r.error = N_(L"winget failed");
    }
    return r;
}

WingetUpgradeResult RunWingetUpgrade(const std::wstring& winget, const WingetUpdate& u, std::atomic<bool>* cancel) {
    WingetUpgradeResult r;
    if (!u.Updatable()) {
        r.message = N_(L"winget shortened its id: update it with winget in a terminal");
        return r;
    }
    std::string raw;
    r.code = RunWinget(winget, WingetUpgradeArgs(u), raw, 30 * 60 * 1000, cancel);
    uint32_t c = static_cast<uint32_t>(r.code);
    r.ok = r.code == 0 || c == 0x8A150109 || c == 0x8A15010B;
    r.restart = c == 0x8A150109 || c == 0x8A15010B || c == 0x8A15010A;
    if (r.code == -1)
        r.message = N_(L"winget couldn't be started");
    else if (r.code == -2)
        r.message = N_(L"Stopped: it took longer than 30 minutes");
    else {
        r.message = WingetCodeText(r.code);
        if (r.message.empty()) {
            r.message = LastLine(raw);
            if (r.message.empty()) r.message = N_(L"winget failed");
        }
    }
    return r;
}

std::vector<int> MatchUpdatesToApps(const std::vector<WingetUpdate>& updates, const std::vector<InstalledApp>& apps) {
    std::vector<std::tuple<int, size_t, size_t>> pairs;  // score, update, app
    std::vector<std::wstring> appNames(apps.size()), appAlnum(apps.size()), appPub(apps.size());
    for (size_t a = 0; a < apps.size(); ++a) {
        appNames[a] = NormName(apps[a].e.displayName);
        appAlnum[a] = Alnum(apps[a].e.displayName);
        appPub[a] = Alnum(apps[a].e.publisher);
    }
    for (size_t u = 0; u < updates.size(); ++u) {
        const WingetUpdate& w = updates[u];
        std::wstring name = NormName(w.name);
        bool cut = EndsWithEllipsis(name);
        if (cut) name = Trim(name.substr(0, name.size() - 1));
        // Id fallback: "VideoLAN.VLC" -> publisher "videolan", product "vlc".
        std::wstring idPub, idProduct;
        size_t dot = w.id.find(L'.');
        if (dot != std::wstring::npos && !w.idCut) {
            idPub = Alnum(w.id.substr(0, dot));
            idProduct = Alnum(w.id.substr(w.id.rfind(L'.') + 1));
        }
        std::wstring ver = NormVersion(w.version);
        for (size_t a = 0; a < apps.size(); ++a) {
            int score = 0;
            if (!name.empty() && appNames[a] == name && !cut)
                score = 4;
            else if (cut && name.size() >= 4 && appNames[a].compare(0, name.size(), name) == 0)
                score = 3;
            else if (!idProduct.empty() && idProduct.size() >= 3 && appAlnum[a] == idProduct && !idPub.empty() && !appPub[a].empty() &&
                     (appPub[a].find(idPub) != std::wstring::npos || idPub.find(appPub[a]) != std::wstring::npos))
                score = 1;
            if (!score) continue;
            // "Microsoft.VCRedist.2012.x64" picks "... (x64) ..." over "... (x86) ..." when both are cut.
            if (score > 1 && idProduct.size() >= 2 && appAlnum[a].find(idProduct) != std::wstring::npos) ++score;
            std::wstring av = NormVersion(apps[a].e.displayVersion);
            if (!ver.empty() && !av.empty()) {
                if (av == ver)
                    score += 2;
                else if (av.compare(0, ver.size(), ver) == 0 && av.size() > ver.size() && av[ver.size()] == L'.')
                    score += 1;  // winget "8.0.21", registered "8.0.21.35325"
            }
            pairs.emplace_back(score, u, a);
        }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const auto& x, const auto& y) { return std::get<0>(x) > std::get<0>(y); });
    std::vector<int> out(updates.size(), -1);
    std::vector<bool> used(apps.size(), false);
    for (auto& [score, u, a] : pairs) {
        if (out[u] >= 0 || used[a]) continue;
        out[u] = static_cast<int>(a);
        used[a] = true;
    }
    return out;
}
