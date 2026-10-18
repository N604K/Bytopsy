#include "alerts.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "diskhealth.h"
#include "fsops.h"
#include "i18n.h"
#include "notify.h"
#include "schedule.h"
#include "util.h"

namespace {

constexpr uint64_t kGiB = 1ull << 30;

std::wstring Trim(const std::wstring& v) {
    size_t a = v.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = v.find_last_not_of(L" \t\r\n");
    return v.substr(a, b - a + 1);
}

std::string ReadFileText(const std::wstring& path) {
    std::string out;
    if (path.empty()) return out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char b[16384];
    DWORD n = 0;
    while (out.size() < (1u << 20) && ReadFile(h, b, sizeof(b), &n, nullptr) && n) out.append(b, n);
    CloseHandle(h);
    return out;
}

// Writes through a temporary file, so a crash never leaves half a file.
bool WriteFileText(const std::wstring& path, const std::string& data) {
    if (path.empty()) return false;
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &w, nullptr) && w == data.size();
    CloseHandle(h);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(tmp.c_str());
    return false;
}

std::wstring EnvPath(const wchar_t* name) {
    wchar_t over[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, over, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(over) : L"";
}

// "10", "2.5" (invariant: settings.ini is always written with a dot).
std::wstring Number(double v) {
    wchar_t b[32];
    swprintf(b, 32, L"%.2f", v);
    std::wstring s = b;
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    return s;
}

std::wstring NormDrive(const std::wstring& d) {
    if (d.size() < 2 || d[1] != L':' || !iswalpha(d[0])) return L"";
    return std::wstring(1, static_cast<wchar_t>(towupper(d[0]))) + L":";
}

}  // namespace

// =============================================================================================
// Limits

bool ParseLimit(const std::wstring& text, SpaceLimit& out) {
    std::wstring s = util::ToLower(Trim(text));
    LimitUnit unit;
    auto strip = [&](const wchar_t* suffix) {
        size_t n = wcslen(suffix);
        if (s.size() > n && s.compare(s.size() - n, n, suffix) == 0) {
            s = Trim(s.substr(0, s.size() - n));
            return true;
        }
        return false;
    };
    if (strip(L"%"))
        unit = LimitUnit::Percent;
    else if (strip(L"gib") || strip(L"gb") || strip(L"g"))
        unit = LimitUnit::GiB;
    else
        return false;
    if (s.empty() || s.find_first_not_of(L"0123456789.") != std::wstring::npos || std::count(s.begin(), s.end(), L'.') > 1) return false;
    double v = _wtof(s.c_str());
    if (!(v > 0) || (unit == LimitUnit::Percent && v >= 100) || (unit == LimitUnit::GiB && v > 1e6)) return false;
    out.unit = unit;
    out.value = v;
    return true;
}

std::wstring FormatLimit(const SpaceLimit& l) { return Number(l.value) + (l.unit == LimitUnit::Percent ? L"%" : L"G"); }

std::wstring DescribeLimit(const SpaceLimit& l) {
    bool whole = std::floor(l.value) == l.value;
    std::wstring n = util::FormatDecimal(l.value, whole ? 0 : (std::floor(l.value * 10) == l.value * 10 ? 1 : 2));
    return l.unit == LimitUnit::Percent ? n + L"%" : TrF(L"{0} GiB", n);
}

uint64_t LimitBytes(const SpaceLimit& l, uint64_t total) {
    if (l.unit == LimitUnit::Percent) return static_cast<uint64_t>(static_cast<double>(total) * l.value / 100.0);
    return static_cast<uint64_t>(l.value * static_cast<double>(kGiB));
}

std::vector<SpaceLimit> LimitChoices() {
    std::vector<SpaceLimit> v;
    for (double p : {1.0, 2.0, 3.0, 5.0, 10.0, 15.0, 20.0, 25.0}) v.push_back({LimitUnit::Percent, p});
    for (double g : {1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0}) v.push_back({LimitUnit::GiB, g});
    return v;
}

const std::vector<int>& CheckIntervals() {
    static const std::vector<int> v = {1, 2, 4, 6, 12, 24};
    return v;
}

// =============================================================================================
// Settings

EffectiveRule RuleFor(const AlertSettings& s, const std::wstring& drive) {
    EffectiveRule r;
    r.limit = s.all;
    std::wstring d = NormDrive(drive);
    for (auto& x : s.drives)
        if (x.drive == d) {
            r.own = true;
            r.watched = x.watched;
            r.limit = x.limit;
        }
    return r;
}

void SetDriveRule(AlertSettings& s, const std::wstring& drive, bool likeAll, bool watched, const SpaceLimit& limit) {
    std::wstring d = NormDrive(drive);
    if (d.empty()) return;
    s.drives.erase(std::remove_if(s.drives.begin(), s.drives.end(), [&](const DriveRule& r) { return r.drive == d; }), s.drives.end());
    if (likeAll) return;
    s.drives.push_back({d, watched, watched ? limit : s.all});
    std::sort(s.drives.begin(), s.drives.end(), [](const DriveRule& a, const DriveRule& b) { return a.drive < b.drive; });
}

bool IsAlertKey(const std::wstring& key) { return key == L"alerts" || (key.size() > 5 && key.compare(0, 5, L"alert") == 0); }

AlertSettings ParseAlertSettings(const std::wstring& ini) {
    AlertSettings s;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t nl = ini.find(L'\n', pos);
        std::wstring line = ini.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? ini.size() : nl + 1;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        if (k == L"alerts") {
            s.enabled = v == L"1";
        } else if (k == L"alertEvery") {
            int h = _wtoi(v.c_str());
            auto& iv = CheckIntervals();
            if (std::find(iv.begin(), iv.end(), h) != iv.end()) s.everyHours = h;
        } else if (k == L"alertLogon") {
            s.atLogon = v != L"0";
        } else if (k == L"alertLimit") {
            SpaceLimit l;
            if (ParseLimit(v, l)) s.all = l;
        } else if (k == L"alertDrive") {
            size_t comma = v.find(L',');
            if (comma == std::wstring::npos) continue;
            std::wstring d = NormDrive(Trim(v.substr(0, comma)));
            std::wstring rest = util::ToLower(Trim(v.substr(comma + 1)));
            if (d.empty() || Trim(v.substr(0, comma)).size() != 2) continue;
            SpaceLimit l;
            if (rest == L"off")
                SetDriveRule(s, d, false, false, s.all);
            else if (ParseLimit(rest, l))
                SetDriveRule(s, d, false, true, l);
        }
    }
    // A drive rule written before the rule for all drives keeps its own limit; "off" rules keep
    // whatever (it isn't used).
    return s;
}

std::wstring FormatAlertSettings(const AlertSettings& s) {
    std::wstring out;
    out += L"alerts=" + std::wstring(s.enabled ? L"1" : L"0") + L"\n";
    out += L"alertEvery=" + std::to_wstring(s.everyHours) + L"\n";
    out += L"alertLogon=" + std::wstring(s.atLogon ? L"1" : L"0") + L"\n";
    out += L"alertLimit=" + FormatLimit(s.all) + L"\n";
    for (auto& d : s.drives) out += L"alertDrive=" + d.drive + L"," + (d.watched ? FormatLimit(d.limit) : std::wstring(L"off")) + L"\n";
    return out;
}

namespace {
bool LineIsAlert(const std::wstring& line) {
    size_t eq = line.find(L'=');
    return eq != std::wstring::npos && IsAlertKey(Trim(line.substr(0, eq)));
}
}  // namespace

std::wstring AlertLinesOf(const std::wstring& ini) {
    std::wstring out;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t nl = ini.find(L'\n', pos);
        std::wstring line = ini.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? ini.size() : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (LineIsAlert(line)) out += line + L"\n";
    }
    return out;
}

std::wstring MergeAlertSettings(const std::wstring& ini, const AlertSettings& s) {
    std::wstring out;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t nl = ini.find(L'\n', pos);
        std::wstring line = ini.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? ini.size() : nl + 1;
        std::wstring bare = line;
        if (!bare.empty() && bare.back() == L'\r') bare.pop_back();
        if (LineIsAlert(bare)) continue;
        if (bare.empty() && nl == std::wstring::npos) continue;
        out += line + L"\n";
    }
    return out + FormatAlertSettings(s);
}

std::wstring SettingsFilePath() {
    std::wstring over = EnvPath(L"BYTOPSY_SETTINGS");  // tests
    if (!over.empty()) return over;
    std::wstring dir = util::DataDir();
    return dir.empty() ? L"" : dir + L"\\settings.ini";
}

std::wstring ReadSettingsText() { return util::Utf8ToWide(ReadFileText(SettingsFilePath())); }

AlertSettings LoadAlertSettings() { return ParseAlertSettings(ReadSettingsText()); }

bool SaveAlertSettings(const AlertSettings& s) {
    return WriteFileText(SettingsFilePath(), util::WideToUtf8(MergeAlertSettings(ReadSettingsText(), s)));
}

std::wstring DescribeCheckInterval(const AlertSettings& s) {
    std::wstring every = s.everyHours <= 1    ? Tr(L"every hour")
                         : s.everyHours >= 24 ? Tr(L"once a day")
                                              : TrF(L"every {0} hours", s.everyHours);
    return s.atLogon ? TrF(L"{0} and at logon", every) : every;
}

// =============================================================================================
// State

std::vector<AlertState> ParseAlertState(const std::string& text) {
    std::vector<AlertState> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        char drive[8] = {};
        unsigned serial = 0;
        int low = 0;
        long long notified = 0;
        if (sscanf_s(line.c_str(), "%7s\t%x\t%d\t%lld", drive, static_cast<unsigned>(sizeof(drive)), &serial, &low, &notified) != 4) continue;
        std::wstring d = NormDrive(util::Utf8ToWide(drive));
        if (d.empty() || strlen(drive) != 2 || notified < 0) continue;
        out.erase(std::remove_if(out.begin(), out.end(), [&](const AlertState& a) { return a.drive == d; }), out.end());
        out.push_back({d, serial, low != 0, notified});
    }
    return out;
}

std::string FormatAlertState(const std::vector<AlertState>& states) {
    std::string out;
    for (auto& s : states) {
        char b[96];
        snprintf(b, sizeof(b), "%s\t%08x\t%d\t%lld\n", util::WideToUtf8(s.drive).c_str(), s.serial, s.low ? 1 : 0, static_cast<long long>(s.notified));
        out += b;
    }
    return out;
}

std::wstring AlertStatePath() {
    std::wstring over = EnvPath(L"BYTOPSY_ALERT_STATE");  // tests
    if (!over.empty()) return over;
    std::wstring dir = util::DataDir();
    return dir.empty() ? L"" : dir + L"\\space-alerts.txt";
}

uint64_t RearmMargin(uint64_t limitBytes) { return std::min<uint64_t>(kGiB, limitBytes / 20); }

AlertStep StepAlert(const AlertState* prev, uint64_t freeBytes, uint64_t limitBytes, int64_t nowUnix) {
    AlertStep s;
    if (prev) s.next = *prev;
    if (freeBytes < limitBytes) {
        // Newly under the limit, or still under it a day after the last notification (or the clock
        // went back).
        if (!prev || !prev->low || prev->notified <= 0 || nowUnix - prev->notified >= kRenotifySeconds || nowUnix < prev->notified) {
            s.notify = true;
            s.next.notified = nowUnix;
        }
        s.next.low = true;
    } else if (freeBytes >= limitBytes + RearmMargin(limitBytes)) {
        s.cleared = prev && prev->low;
        s.next.low = false;
    }
    // In between (above the limit by less than the margin): as it was.
    return s;
}

// =============================================================================================
// A check

std::vector<DriveCheck> EvaluateSpace(const AlertSettings& s, const std::vector<DriveSpace>& drives, const std::vector<AlertState>& prev,
                                      int64_t nowUnix, std::vector<AlertState>& next) {
    std::vector<DriveCheck> out;
    next.clear();
    for (auto& p : prev)  // drives not there now (unplugged, not ready) keep what they had
        if (std::none_of(drives.begin(), drives.end(), [&](const DriveSpace& d) { return NormDrive(d.drive) == p.drive; })) next.push_back(p);
    for (auto& d : drives) {
        DriveCheck c;
        c.d = d;
        c.d.drive = NormDrive(d.drive);
        EffectiveRule r = RuleFor(s, c.d.drive);
        c.watched = r.watched && d.total > 0 && !c.d.drive.empty();
        c.rule = r.limit;
        c.limit = LimitBytes(r.limit, d.total);
        c.low = c.watched && d.free < c.limit;
        if (!c.watched) {
            out.push_back(c);
            continue;  // its state goes: watching it again starts afresh
        }
        const AlertState* p = nullptr;
        for (auto& x : prev)
            if (x.drive == c.d.drive && x.serial == d.serial) p = &x;
        c.lastNotified = p ? p->notified : 0;
        AlertStep st = StepAlert(p, d.free, c.limit, nowUnix);
        st.next.drive = c.d.drive;
        st.next.serial = d.serial;
        c.notify = st.notify;
        c.cleared = st.cleared;
        next.push_back(st.next);
        out.push_back(c);
    }
    std::sort(next.begin(), next.end(), [](const AlertState& a, const AlertState& b) { return a.drive < b.drive; });
    return out;
}

std::wstring AlertTitle(const DriveCheck& c) {
    return c.d.label.empty() ? TrF(L"{0} is almost full", c.d.drive) : TrF(L"{0} ({1}) is almost full", c.d.drive, c.d.label);
}

std::wstring AlertBody(const DriveCheck& c) {
    std::wstring limit = c.rule.unit == LimitUnit::Percent ? TrF(L"{0} ({1})", DescribeLimit(c.rule), util::FormatSizeShort(c.limit))
                                                           : DescribeLimit(c.rule);
    return TrF(L"{0} free of {1}, below your limit of {2}.", util::FormatSizeShort(c.d.free), util::FormatSizeShort(c.d.total), limit);
}

AlertLink ParseAlertLink(const std::wstring& uri) {
    AlertLink none;
    std::wstring u = util::ToLower(Trim(uri));
    size_t colon = u.find(L':');
    if (colon == std::wstring::npos) return none;
    std::wstring scheme = u.substr(0, colon);
    if (scheme != L"bytopsy" && scheme != L"bytopsy-test") return none;
    std::wstring rest = u.substr(colon + 1);
    if (rest.rfind(L"//", 0) == 0) rest.erase(0, 2);
    std::wstring query;
    size_t q = rest.find(L'?');
    if (q != std::wstring::npos) {
        query = rest.substr(q + 1);
        rest.erase(q);
    }
    while (!rest.empty() && rest.back() == L'/') rest.pop_back();
    while (!query.empty() && query.back() == L'/') query.pop_back();
    AlertLink l;
    if (rest == L"disk-health")
        l.kind = AlertLink::DiskHealth;
    else if (rest == L"scan")
        l.kind = AlertLink::Scan;
    else
        return none;
    if (!query.empty()) {
        if (query.size() != 7 || query.compare(0, 6, L"drive=") != 0 || query[6] < L'a' || query[6] > L'z') return none;
        l.drive = static_cast<wchar_t>(towupper(query[6]));
    }
    if (l.kind == AlertLink::Scan && !l.drive) return none;
    return l;
}

std::wstring MakeAlertLink(const std::wstring& scheme, AlertLink::Kind kind, wchar_t drive) {
    std::wstring s = scheme + L":" + (kind == AlertLink::Scan ? L"scan" : L"disk-health");
    if (drive) s += L"?drive=" + std::wstring(1, static_cast<wchar_t>(towupper(drive)));
    return s;
}

std::vector<DriveSpace> FixedDriveSpace() {
    std::vector<DriveSpace> out;
    for (auto& d : ListDrives()) {
        if (d.type != DRIVE_FIXED || !d.ready || d.root.size() < 2) continue;
        DriveSpace s;
        s.drive = NormDrive(d.root.substr(0, 2));
        if (s.drive.empty()) continue;
        s.label = d.label;
        s.serial = VolumeSerial(d.root);
        s.total = d.total;
        s.free = d.free;
        out.push_back(s);
    }
    return out;
}

namespace {

std::wstring GuiExe() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/") + 1) + L"bytopsy.exe";
}

}  // namespace

ToastContent AlertToast(const DriveCheck& c, const std::wstring& scheme) {
    ToastContent t;
    t.title = AlertTitle(c);
    t.body = AlertBody(c);
    t.footer = Tr(L"Bytopsy low disk space alert");
    t.link = MakeAlertLink(scheme, AlertLink::DiskHealth, c.d.drive[0]);
    t.actions = {{Tr(L"Open Disk health"), t.link}, {TrF(L"Scan {0}", c.d.drive), MakeAlertLink(scheme, AlertLink::Scan, c.d.drive[0])}};
    return t;
}

SpaceCheck RunSpaceCheck(bool dryRun) {
    SpaceCheck r;
    r.settings = LoadAlertSettings();
    std::vector<DriveSpace> drives = FixedDriveSpace();
    std::vector<AlertState> prev = ParseAlertState(ReadFileText(AlertStatePath())), next;
    int64_t now = util::FileTimeToUnix(util::NowFileTime());
    r.drives = EvaluateSpace(r.settings, drives, prev, now, next);
    if (dryRun || !r.settings.enabled) return r;

    std::vector<std::wstring> roots;
    for (auto& d : drives) roots.push_back(d.drive + L"\\");
    RecordFreeSpace(roots);  // Disk health's trend gets a point even when the app isn't opened

    NotifyNames names = DefaultNotifyNames();
    for (auto& c : r.drives) {
        std::wstring tag = c.d.drive.substr(0, 1);
        if (c.cleared) RemoveToast(names, tag, L"space");
        if (!c.notify) continue;
        ToastContent t = AlertToast(c, names.scheme);
        std::wstring err;
        ToastResult res = ShowToast(names, t, tag, L"space", 24, err);
        bool shown = res == ToastResult::Shown;
        if (res == ToastResult::Failed) {
            shown = ShowBalloon(t, GuiExe(), L"--alert-link \"" + t.link + L"\"", 30000);
            if (!shown) r.error = err;
        }
        if (res == ToastResult::TurnedOff) r.turnedOff = true;
        if (shown) ++r.shown;
        if (res == ToastResult::Failed && !shown) {
            // Not told: try again at the next check.
            for (auto& n : next)
                if (n.drive == c.d.drive) {
                    n.low = false;
                    n.notified = c.lastNotified;
                }
        }
        std::wstring line;
        {
            i18n::EnglishScope en;  // cleanup.log is kept in English
            line = L"space alert: " + AlertTitle(c) + L", " + AlertBody(c);
        }
        line += shown ? L" Notification shown." : res == ToastResult::TurnedOff ? L" Notifications are turned off in Windows Settings." : L" Notification failed: " + err;
        CleanLogAppend(line);
    }
    WriteFileText(AlertStatePath(), FormatAlertState(next));
    return r;
}

int SpaceCheckExitCode(const SpaceCheck& c) {
    for (auto& d : c.drives)
        if (d.watched && d.low) return 1;
    return 0;
}

bool ShowTestNotification(std::wstring& err) {
    NotifyNames names = DefaultNotifyNames();
    ToastContent t;
    t.title = Tr(L"Bytopsy test notification");
    t.body = Tr(L"This is how a low disk space alert looks. Clicking it opens Disk health.");
    t.footer = Tr(L"Bytopsy low disk space alert");
    t.link = MakeAlertLink(names.scheme, AlertLink::DiskHealth, 0);
    t.actions = {{Tr(L"Open Disk health"), t.link}};
    ToastResult res = ShowToast(names, t, L"test", L"test", 1, err);
    if (res == ToastResult::TurnedOff) err = Tr(L"Notifications from Bytopsy are turned off in Windows Settings (System, Notifications)");
    return res == ToastResult::Shown;
}

std::wstring DescribeCheckResult(int64_t code) {
    switch (static_cast<uint32_t>(code)) {
        case 0: return Tr(L"all drives above their limits");
        case 1: return Tr(L"a drive was under its limit");
        case 0x00041303: return Tr(L"not run yet");
        case 0x00041301: return Tr(L"running now");
        case 0x00041306: return Tr(L"stopped before it finished");
        case 0x800710E0: return Tr(L"didn't start (not signed in)");
        case 0x80070002: return Tr(L"failed: bytopsy.exe was not found");
    }
    wchar_t b[16];
    swprintf(b, 16, L"%08X", static_cast<uint32_t>(code));
    return TrF(L"failed (0x{0})", std::wstring(b));
}
