// Self-check of the low disk space alerts: bytopsy-alerts-test.exe
// Limits (% and GiB), the settings.ini lines, the once-then-again-after-a-day state, the toast XML
// and its escaping, the links a notification opens, and bytopsy-cli --check-space (a dry run, and
// a real run with limits no drive is under, so nothing is ever shown).
// Only with BYTOPSY_TEST_NOTIFY=1: registers the test names (Bytopsy.Test / bytopsy-test) and removes
// them again; =2 also shows ONE notification titled "Bytopsy test notification" in between.
// Only with BYTOPSY_TEST_TASKSCHED=1: a harmless repeating test task under \Bytopsy-test, removed again.
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <string>
#include <vector>

#include "alerts.h"
#include "fsops.h"
#include "notify.h"
#include "schedule.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static void PutW(const std::wstring& path, const std::wstring& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        std::string u = util::WideToUtf8(text);
        fwrite(u.data(), 1, u.size(), f);
        fclose(f);
    }
}

static std::string Slurp(const std::wstring& path) {
    std::string data;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
        fclose(f);
    }
    return data;
}

static bool Has(const std::wstring& s, const std::wstring& part) { return s.find(part) != std::wstring::npos; }

static bool KeyThere(const std::wstring& key) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

// Runs bytopsy-cli.exe (next to this exe) with `args`; returns the exit code, stdout in `out`.
static int RunCli(const std::wstring& args, std::string& out) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe(self);
    exe = exe.substr(0, exe.rfind(L'\\')) + L"\\bytopsy-cli.exe";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring line = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(0);
    out.clear();
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    char b[4096];
    DWORD n = 0;
    while (ReadFile(rd, b, sizeof(b), &n, nullptr) && n) out.append(b, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

constexpr uint64_t GiB = 1ull << 30;

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetEnvironmentVariableW(L"BYTOPSY_NOTIFY_TEST", L"1");  // never the real names

    // ---- Limits
    {
        SpaceLimit l;
        Check(ParseLimit(L"10%", l) && l.unit == LimitUnit::Percent && l.value == 10, L"10%");
        Check(ParseLimit(L" 2.5 % ", l) && l.unit == LimitUnit::Percent && l.value == 2.5, L"2.5 % with spaces");
        Check(ParseLimit(L"20G", l) && l.unit == LimitUnit::GiB && l.value == 20, L"20G");
        Check(ParseLimit(L"20 GB", l) && l.unit == LimitUnit::GiB && l.value == 20, L"20 GB counts as GiB");
        Check(ParseLimit(L"1.5gib", l) && l.unit == LimitUnit::GiB && l.value == 1.5, L"1.5gib");
        bool anyBad = false;
        for (const wchar_t* bad : {L"", L"%", L"0%", L"100%", L"150%", L"-5%", L"10", L"abc", L"1.2.3G", L"2000000G", L"10 %%", L"0G", L"G", L"10M"}) {
            SpaceLimit x;
            if (ParseLimit(bad, x)) {
                anyBad = true;
                Check(false, std::wstring(L"rejected: \"") + bad + L"\"");
            }
        }
        Check(!anyBad, L"bad limits rejected (0, 100%, no unit, MB, ...)");
        Check(FormatLimit({LimitUnit::Percent, 10}) == L"10%" && FormatLimit({LimitUnit::Percent, 2.5}) == L"2.5%" &&
                  FormatLimit({LimitUnit::GiB, 20}) == L"20G",
              L"FormatLimit: 10%, 2.5%, 20G");
        for (auto& c : LimitChoices()) {
            SpaceLimit back;
            if (!ParseLimit(FormatLimit(c), back) || !(back == c)) Check(false, L"round trip of " + FormatLimit(c));
        }
        Check(DescribeLimit({LimitUnit::Percent, 10}) == L"10%" && DescribeLimit({LimitUnit::GiB, 20}) == L"20 GiB" &&
                  DescribeLimit({LimitUnit::GiB, 1.5}) == L"1.5 GiB",
              L"DescribeLimit: 10%, 20 GiB, 1.5 GiB");
        // % vs GiB: on a 100 GiB drive 10% is 10 GiB; on a 2 TiB drive it is 204.8 GiB.
        Check(LimitBytes({LimitUnit::Percent, 10}, 100 * GiB) == 10 * GiB, L"10% of 100 GiB = 10 GiB");
        Check(LimitBytes({LimitUnit::Percent, 10}, 2048 * GiB) == static_cast<uint64_t>(204.8 * GiB), L"10% of 2 TiB = 204.8 GiB");
        Check(LimitBytes({LimitUnit::GiB, 20}, 100 * GiB) == 20 * GiB && LimitBytes({LimitUnit::GiB, 20}, 2048 * GiB) == 20 * GiB,
              L"20 GiB is 20 GiB whatever the drive's size");
        Check(LimitBytes({LimitUnit::GiB, 1.5}, 0) == GiB + GiB / 2, L"1.5 GiB");
        Check(LimitBytes({LimitUnit::Percent, 10}, 0) == 0, L"a drive of size 0: no limit");
        Check(RearmMargin(100 * GiB) == GiB && RearmMargin(GiB) == GiB / 20 && RearmMargin(0) == 0, L"rearm margin: min(1 GiB, limit / 20)");
        Check(CheckIntervals().front() == 1 && CheckIntervals().back() == 24, L"intervals 1 .. 24 hours");
        Check(RepetitionInterval(1) == L"PT1H" && RepetitionInterval(4) == L"PT4H" && RepetitionInterval(0) == L"PT1H" && RepetitionInterval(99) == L"PT24H",
              L"repetition interval PT1H .. PT24H, clamped");
    }

    // ---- Settings
    {
        AlertSettings d;
        Check(!d.enabled && d.everyHours == 1 && d.atLogon && d.all == SpaceLimit{LimitUnit::Percent, 10} && d.drives.empty(),
              L"defaults: off, hourly and at logon, 10% for every drive");
        std::wstring ini = L"theme=2\r\nalerts=1\r\nalertEvery=4\r\nalertLogon=0\r\nalertLimit=15%\r\nalertDrive=d:,20G\r\nalertDrive=E:,off\r\n"
                           L"alertDrive=F:,nonsense\r\nalertDrive=GG:,5%\r\nalertDrive=D:,25G\r\nlanguage=de\r\n";
        AlertSettings s = ParseAlertSettings(ini);
        Check(s.enabled && s.everyHours == 4 && !s.atLogon && s.all == SpaceLimit{LimitUnit::Percent, 15}, L"parsed: on, every 4 h, not at logon, 15%");
        Check(s.drives.size() == 2 && s.drives[0].drive == L"D:" && s.drives[0].watched && s.drives[0].limit == SpaceLimit{LimitUnit::GiB, 25} &&
                  s.drives[1].drive == L"E:" && !s.drives[1].watched,
              L"drive rules: d: normalized, the later D: wins, E: off, nonsense and GG: skipped");
        EffectiveRule c = RuleFor(s, L"C:"), dd = RuleFor(s, L"d:"), e = RuleFor(s, L"E:");
        Check(c.watched && !c.own && c.limit == s.all, L"C: follows the rule for all drives");
        Check(dd.watched && dd.own && dd.limit == SpaceLimit{LimitUnit::GiB, 25}, L"D: has its own limit");
        Check(!e.watched && e.own, L"E: not watched");
        AlertSettings bad = ParseAlertSettings(L"alerts=yes\nalertEvery=3\nalertLimit=0%\nalertLogon=1\n");
        Check(!bad.enabled && bad.everyHours == 1 && bad.all == SpaceLimit{} && bad.atLogon, L"unreadable values keep the defaults");
        AlertSettings back = ParseAlertSettings(FormatAlertSettings(s));
        Check(back.enabled == s.enabled && back.everyHours == s.everyHours && back.atLogon == s.atLogon && back.all == s.all &&
                  back.drives.size() == 2 && back.drives[0].limit == s.drives[0].limit && !back.drives[1].watched,
              L"FormatAlertSettings round trip");
        SetDriveRule(s, L"D:", true, true, {});
        Check(!RuleFor(s, L"D:").own && s.drives.size() == 1, L"\"like all drives\" removes the drive's rule");
        SetDriveRule(s, L"c:", false, true, {LimitUnit::GiB, 5});
        Check(s.drives.size() == 2 && s.drives[0].drive == L"C:" && RuleFor(s, L"C:").limit == SpaceLimit{LimitUnit::GiB, 5},
              L"a rule of its own, sorted by drive");
        std::wstring merged = MergeAlertSettings(ini, s);
        Check(Has(merged, L"theme=2\r\n") && Has(merged, L"language=de\r\n") && !Has(merged, L"nonsense") && !Has(merged, L"alertDrive=D:") &&
                  Has(merged, L"alertDrive=C:,5G\n") && Has(merged, L"alertDrive=E:,off\n") && Has(merged, L"alertEvery=4\n"),
              L"MergeAlertSettings: other lines kept as they were, alert lines replaced");
        std::wstring lines = AlertLinesOf(ini);
        Check(Has(lines, L"alerts=1\n") && Has(lines, L"alertDrive=E:,off\n") && !Has(lines, L"theme") && !Has(lines, L"language") && !Has(lines, L"\r"),
              L"AlertLinesOf: only the alert lines");
        Check(IsAlertKey(L"alerts") && IsAlertKey(L"alertLimit") && !IsAlertKey(L"alert") && !IsAlertKey(L"theme"), L"IsAlertKey");
        AlertSettings t;
        t.everyHours = 1;
        t.atLogon = true;
        std::wstring a = DescribeCheckInterval(t);
        t.everyHours = 6;
        t.atLogon = false;
        std::wstring b = DescribeCheckInterval(t);
        t.everyHours = 24;
        Check(a == L"every hour and at logon" && b == L"every 6 hours" && DescribeCheckInterval(t) == L"once a day", L"check interval in words: " + a);
    }

    // ---- Settings file (BYTOPSY_SETTINGS): the windowed app's lines survive
    wchar_t tb[MAX_PATH];
    std::wstring tmp(tb, GetTempPathW(MAX_PATH, tb));
    std::wstring dir = tmp + L"bytopsy-alerts-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring ini = dir + L"\\settings.ini", state = dir + L"\\space-alerts.txt", flog = dir + L"\\freespace.log";
    SetEnvironmentVariableW(L"BYTOPSY_SETTINGS", ini.c_str());
    SetEnvironmentVariableW(L"BYTOPSY_ALERT_STATE", state.c_str());
    SetEnvironmentVariableW(L"BYTOPSY_FREESPACE_LOG", flog.c_str());
    SetEnvironmentVariableW(L"BYTOPSY_JOURNAL", (dir + L"\\journal.log").c_str());  // cleanup.log goes next to it
    {
        PutW(ini, L"theme=1\nsi=1\nalerts=0\n");
        AlertSettings s = LoadAlertSettings();
        s.enabled = true;
        s.all = {LimitUnit::GiB, 7};
        Check(SaveAlertSettings(s), L"settings saved");
        std::wstring text = util::Utf8ToWide(Slurp(ini));
        AlertSettings back = LoadAlertSettings();
        Check(Has(text, L"theme=1\n") && Has(text, L"si=1\n") && back.enabled && back.all == SpaceLimit{LimitUnit::GiB, 7}, L"saved next to the other settings");
        Check(SettingsFilePath() == ini && AlertStatePath() == state, L"paths overridden for the test");
    }

    // ---- Once, then again after it was above the limit or a day later
    {
        const uint64_t limit = 10 * GiB;
        const int64_t t0 = 1'800'000'000;
        AlertStep s = StepAlert(nullptr, 9 * GiB, limit, t0);
        Check(s.notify && s.next.low && s.next.notified == t0 && !s.cleared, L"first time under the limit: notify");
        AlertStep s2 = StepAlert(&s.next, 8 * GiB, limit, t0 + 3600);
        Check(!s2.notify && s2.next.low && s2.next.notified == t0, L"an hour later, still under: not again");
        AlertStep s3 = StepAlert(&s2.next, 8 * GiB, limit, t0 + kRenotifySeconds - 1);
        Check(!s3.notify, L"just under a day later: not again");
        AlertStep s4 = StepAlert(&s3.next, 8 * GiB, limit, t0 + kRenotifySeconds);
        Check(s4.notify && s4.next.notified == t0 + kRenotifySeconds, L"a day later, still under: again");
        // Just above the limit (less than the margin): still counts as low, so a dip doesn't notify.
        AlertStep s5 = StepAlert(&s4.next, limit + RearmMargin(limit) / 2, limit, t0 + kRenotifySeconds + 3600);
        Check(!s5.notify && s5.next.low && !s5.cleared, L"barely above the limit: still latched");
        AlertStep s6 = StepAlert(&s5.next, limit - 1, limit, t0 + kRenotifySeconds + 7200);
        Check(!s6.notify, L"dipping under again within the day: not again");
        AlertStep s7 = StepAlert(&s6.next, limit + RearmMargin(limit), limit, t0 + kRenotifySeconds + 10800);
        Check(!s7.notify && !s7.next.low && s7.cleared, L"above the limit by the margin: rearmed, notification cleared");
        AlertStep s8 = StepAlert(&s7.next, limit - 1, limit, t0 + kRenotifySeconds + 14400);
        Check(s8.notify, L"under again after being above: notify at once");
        AlertStep s9 = StepAlert(&s8.next, limit, limit, t0 + kRenotifySeconds + 18000);
        Check(!s9.notify && s9.next.low, L"exactly at the limit isn't under it (and isn't above by the margin)");
        AlertState future{L"C:", 1, true, t0 + 100000};
        Check(StepAlert(&future, 1, limit, t0).notify, L"clock went back: notify");
        AlertStep fine = StepAlert(nullptr, 50 * GiB, limit, t0);
        Check(!fine.notify && !fine.cleared && !fine.next.low, L"plenty free: nothing");
    }

    // ---- EvaluateSpace over several drives
    {
        AlertSettings s;
        s.enabled = true;
        s.all = {LimitUnit::Percent, 10};
        SetDriveRule(s, L"D:", false, true, {LimitUnit::GiB, 50});
        SetDriveRule(s, L"E:", false, false, {});
        std::vector<DriveSpace> drives = {{L"C:", L"", 0x1111, 500 * GiB, 40 * GiB},   // 8% < 10%: low
                                          {L"D:", L"Data", 0x2222, 100 * GiB, 40 * GiB},  // 40 GiB < 50 GiB: low (as % it would be fine)
                                          {L"E:", L"", 0x3333, 100 * GiB, 1 * GiB},     // not watched
                                          {L"f:", L"", 0x4444, 100 * GiB, 60 * GiB}};   // fine
        std::vector<AlertState> prev = {{L"D:", 0x2222, true, 1'800'000'000},           // told an hour ago
                                        {L"E:", 0x3333, true, 1'800'000'000},           // was watched before
                                        {L"G:", 0x5555, true, 1'700'000'000}};          // not here now (unplugged)
        std::vector<AlertState> next;
        auto r = EvaluateSpace(s, drives, prev, 1'800'003'600, next);
        Check(r.size() == 4, L"every drive in the result");
        Check(r[0].watched && r[0].low && r[0].notify && r[0].limit == 50 * GiB, L"C: 8% free < 10%: notify");
        Check(r[1].watched && r[1].low && !r[1].notify && r[1].lastNotified == 1'800'000'000 && r[1].limit == 50 * GiB, L"D: under 50 GiB, told an hour ago: not again");
        Check(!r[2].watched && !r[2].low && !r[2].notify, L"E: not watched: never");
        Check(r[3].watched && !r[3].low && r[3].d.drive == L"F:", L"F: fine (drive letter normalized)");
        bool hasE = false, hasG = false;
        for (auto& n : next) {
            hasE |= n.drive == L"E:";
            hasG |= n.drive == L"G:";
        }
        Check(!hasE && hasG && next.size() == 4, L"state: E: dropped (not watched), G: kept (not plugged in), C: D: F: there");
        // Another volume under the letter D: starts afresh.
        drives[1].serial = 0x9999;
        auto r2 = EvaluateSpace(s, drives, prev, 1'800'003'600, next);
        Check(r2[1].notify, L"D: is another volume now: notify");
        // State file format
        std::string text = FormatAlertState(next);
        auto back = ParseAlertState(text + "garbage\nZZ:\t1\t1\t5\nC:\tx\n");
        Check(back.size() == next.size() && back[0].drive == next[0].drive && back[0].serial == next[0].serial && back[0].low == next[0].low &&
                  back[0].notified == next[0].notified,
              L"state file round trip, junk lines skipped");
        DriveCheck c = r[1];
        Check(AlertTitle(c) == L"D: (Data) is almost full" && AlertTitle(r[0]) == L"C: is almost full", L"title: " + AlertTitle(c));
        Check(AlertBody(r[0]) == L"40.0 GiB free of 500.0 GiB, below your limit of 10% (50.0 GiB)." &&
                  AlertBody(c) == L"40.0 GiB free of 100.0 GiB, below your limit of 50 GiB.",
              L"body: " + AlertBody(r[0]) + L" / " + AlertBody(c));
        ToastContent t = AlertToast(c, L"bytopsy");
        Check(t.link == L"bytopsy:disk-health?drive=D" && t.actions.size() == 2 && t.actions[1].link == L"bytopsy:scan?drive=D" &&
                  t.actions[1].label == L"Scan D:",
              L"toast: opens Disk health, a button scans the drive");
        std::wstring xml = BuildToastXml(t);
        Check(ToastXmlLoads(xml), L"Windows' XML parser takes the alert's toast: " + xml.substr(0, 120));
    }

    // ---- XML escaping
    {
        Check(EscapeXml(L"a&b<c>d\"e'f") == L"a&amp;b&lt;c&gt;d&quot;e&apos;f", L"the five entities");
        std::wstring ctl = L"x";
        ctl += wchar_t(1);
        ctl += L"y\tz";
        ctl += wchar_t(0xFFFF);
        Check(EscapeXml(ctl) == L"xy\tz", L"control characters and U+FFFF dropped, tab kept");
        std::wstring sur = L"a";
        sur += wchar_t(0xD83D);
        sur += wchar_t(0xDE00);  // 😀
        sur += wchar_t(0xD800);  // lone high
        sur += L"b";
        sur += wchar_t(0xDC00);  // lone low
        std::wstring want = L"a";
        want += wchar_t(0xD83D);
        want += wchar_t(0xDE00);
        want += L"b";
        Check(EscapeXml(sur) == want, L"surrogate pairs kept, lone halves dropped");
        ToastContent t;
        t.title = L"<script>alert('x')</script> & \"D:\"";
        t.body = L"Ünïcödé 😀 ok";
        t.footer = L"x>y";
        t.link = L"bytopsy:scan?drive=C&x=\"1\"";
        t.actions = {{L"A&B", L"bytopsy:disk-health"}};
        std::wstring x = BuildToastXml(t);
        Check(Has(x, L"&lt;script&gt;alert(&apos;x&apos;)&lt;/script&gt; &amp; &quot;D:&quot;") && !Has(x, L"<script>") &&
                  Has(x, L"launch=\"bytopsy:scan?drive=C&amp;x=&quot;1&quot;\" activationType=\"protocol\"") &&
                  Has(x, L"<action content=\"A&amp;B\" activationType=\"protocol\" arguments=\"bytopsy:disk-health\"/>") &&
                  Has(x, L"<text placement=\"attribution\">x&gt;y</text>"),
              L"toast XML escapes title, body, link and buttons");
        Check(ToastXmlLoads(x), L"and Windows' parser takes it");
        ToastContent plain;
        plain.title = L"Only a title";
        std::wstring px = BuildToastXml(plain);
        Check(!Has(px, L"launch=") && !Has(px, L"<actions>") && !Has(px, L"attribution") && ToastXmlLoads(px), L"no link, no buttons: plain toast");
        Check(!ToastXmlLoads(L"<toast><visual>"), L"broken XML is refused (the check itself works)");
    }

    // ---- Links
    {
        AlertLink l = ParseAlertLink(L"bytopsy:disk-health");
        Check(l.kind == AlertLink::DiskHealth && l.drive == 0, L"bytopsy:disk-health");
        l = ParseAlertLink(L"BYTOPSY:Disk-Health?Drive=c");
        Check(l.kind == AlertLink::DiskHealth && l.drive == L'C', L"case doesn't matter");
        l = ParseAlertLink(L"bytopsy://scan?drive=d/");
        Check(l.kind == AlertLink::Scan && l.drive == L'D', L"bytopsy://scan?drive=d/ (as some launchers pass it)");
        l = ParseAlertLink(L"bytopsy-test:disk-health/");
        Check(l.kind == AlertLink::DiskHealth, L"the test scheme, trailing slash");
        bool anyBad = false;
        for (const wchar_t* bad : {L"bytopsy:scan", L"bytopsy:scan?drive=CD", L"bytopsy:scan?drive=1", L"bytopsy:disk-health?path=C:\\",
                                   L"bytopsy:../../windows", L"http://example.com/disk-health", L"bytopsy:delete?drive=C", L"", L"disk-health",
                                   L"bytopsy:scan?drive=C&x=1"}) {
            if (ParseAlertLink(bad).kind != AlertLink::None) {
                anyBad = true;
                Check(false, std::wstring(L"refused: ") + bad);
            }
        }
        Check(!anyBad, L"anything else is refused (no paths, no other actions)");
        Check(MakeAlertLink(L"bytopsy", AlertLink::Scan, L'e') == L"bytopsy:scan?drive=E" &&
                  MakeAlertLink(L"bytopsy", AlertLink::DiskHealth, 0) == L"bytopsy:disk-health",
              L"MakeAlertLink");
        Check(ParseAlertLink(MakeAlertLink(L"bytopsy", AlertLink::Scan, L'Z')).drive == L'Z', L"made links parse back");
        Check(LinkCommand(L"C:\\Program Files\\Bytopsy\\bytopsy.exe") == L"\"C:\\Program Files\\Bytopsy\\bytopsy.exe\" --alert-link \"%1\"",
              L"the protocol's command quotes the exe and the link");
        NotifyNames n = DefaultNotifyNames();
        Check(n.aumid == L"Bytopsy.Test" && n.scheme == L"bytopsy-test" && AumidKey(n) == L"Software\\Classes\\AppUserModelId\\Bytopsy.Test" &&
                  SchemeKey(n) == L"Software\\Classes\\bytopsy-test",
              L"test names under BYTOPSY_NOTIFY_TEST=1");
        Check(DescribeCheckResult(0) == L"all drives above their limits" && DescribeCheckResult(1) == L"a drive was under its limit" &&
                  DescribeCheckResult(0x80070002) == L"failed: bytopsy.exe was not found",
              L"task results in words");
    }

    // ---- bytopsy-cli --check-space
    {
        std::vector<DriveSpace> fixed = FixedDriveSpace();
        std::string out;
        PutW(ini, L"theme=1\nalerts=1\nalertLimit=99%\n");  // every drive is under 99% free
        int code = RunCli(L"--check-space --dry-run", out);
        Check(fixed.empty() || (code == 1 && out.find("Dry run") != std::string::npos && out.find("would notify") != std::string::npos),
              L"dry run, 99%: exit 1, \"would notify\" (" + std::to_wstring(code) + L"): " + util::Utf8ToWide(out.substr(0, 300)));
        Check(!PathExists(state) && !PathExists(flog), L"dry run writes no state and no free-space sample");
        code = RunCli(L"--check-space --bogus", out);
        Check(code == 2 && out.find("--check-space can't be combined with --bogus") != std::string::npos, L"unknown companion option: exit 2");
        PutW(ini, L"alerts=0\nalertLimit=99%\n");
        code = RunCli(L"--check-space", out);
        Check(out.find("off") != std::string::npos && !PathExists(state) && !PathExists(flog), L"alerts off: nothing checked or written");
        // A real run with a limit no drive is under (10 MiB; drives with less than 1 GiB free are
        // left out): state and a free-space sample are written, nothing can be shown.
        std::wstring rules = L"alerts=1\nalertLimit=0.01G\n";
        for (auto& d : fixed)
            if (d.free < GiB) rules += L"alertDrive=" + d.drive + L",off\n";
        PutW(ini, rules);
        code = RunCli(L"--check-space --quiet", out);
        std::string st = Slurp(state);
        Check(code == 0 && out.empty(), L"real run, 10 MiB: exit 0, --quiet prints nothing (" + std::to_wstring(code) + L")");
        Check(fixed.empty() || (st.find(":\t") != std::string::npos && st.find("\t1\t") == std::string::npos && PathExists(flog)),
              L"state written (no drive low) and a free-space sample recorded: " + util::Utf8ToWide(st));
        Check(!PathExists(dir + L"\\cleanup.log"), L"nothing notified, nothing logged");
        code = RunCli(L"--check-space", out);
        Check(code == 0 && out.find("Low disk space alerts: on, every hour and at logon") != std::string::npos && out.find("limit 10.2 MiB") != std::string::npos,
              L"the summary: " + util::Utf8ToWide(out.substr(0, 400)));
    }

    // ---- Live: the registration and ONE test notification (BYTOPSY_TEST_NOTIFY=1 only)
    wchar_t live[8];
    // 1: only the registration; 2: also the notification.
    if (GetEnvironmentVariableW(L"BYTOPSY_TEST_NOTIFY", live, 8) && (live[0] == L'1' || live[0] == L'2')) {
        NotifyNames n = DefaultNotifyNames();
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring gui(self);
        gui = gui.substr(0, gui.rfind(L'\\')) + L"\\bytopsy.exe";  // its icon goes on the toast
        std::wstring err;
        Check(!KeyThere(AumidKey(n)) && !KeyThere(SchemeKey(n)) && !KeyThere(SettingsKey(n)), L"test names not registered before");
        Check(RegisterNotifications(n, gui, err), L"registered: " + err);
        Check(NotificationsRegistered(n) && RegisteredLinkCommand(n) == LinkCommand(gui), L"both keys there, the command runs bytopsy.exe");
        Check(PathExists(NotifyIconFile(n)), L"icon written: " + NotifyIconFile(n));
        int setting = NotificationSetting(n);
        Check(setting >= 0, L"Windows' notification setting for it: " + std::to_wstring(setting));
        if (live[0] == L'2') {
            Check(ShowTestNotification(err), L"\"Bytopsy test notification\" shown: " + err);
            Sleep(4000);
        }
        UnregisterNotifications(n);
        Check(!KeyThere(AumidKey(n)) && !KeyThere(SchemeKey(n)) && !KeyThere(SettingsKey(n)) && !NotificationsRegistered(n),
              L"registration removed (and Windows' record of the app)");
        Check(!PathExists(NotifyIconFile(n)), L"icon removed");
    }

    // ---- Live: a repeating test task (BYTOPSY_TEST_TASKSCHED=1 only)
    wchar_t ts[8];
    if (GetEnvironmentVariableW(L"BYTOPSY_TEST_TASKSCHED", ts, 8) && ts[0] == L'1') {
        SetEnvironmentVariableW(L"BYTOPSY_TASK_FOLDER", L"\\Bytopsy-test");
        std::wstring folder = ScheduleFolder(), name = L"Disk space check test", err;
        Check(folder == L"\\Bytopsy-test", L"test folder");
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring exe = std::wstring(sys) + L"\\cmd.exe";
        Check(TaskSaveRepeating(folder, name, 4, true, exe, L"/c rem bytopsy-test", L"Bytopsy self-test (harmless)", err), L"repeating test task saved: " + err);
        TaskInfo info;
        Check(TaskQuery(folder, name, info, err) && info.exists && info.enabled && info.nextRun > 0 && info.path == exe &&
                  info.arguments == L"/c rem bytopsy-test",
              L"there, enabled, with a next run");
        Check(TaskSaveRepeating(folder, name, 1, false, exe, L"/c rem bytopsy-test", L"Bytopsy self-test (harmless)", err), L"replaced: " + err);
        Check(TaskRemove(folder, name, err), L"test task removed: " + err);
        Check(TaskQuery(folder, name, info, err) && !info.exists, L"and gone");
        SetEnvironmentVariableW(L"BYTOPSY_TASK_FOLDER", nullptr);
    }

    for (auto& f : {ini, state, flog, ini + L".tmp", state + L".tmp", dir + L"\\journal.log", dir + L"\\cleanup.log"}) DeleteFileW(f.c_str());
    RemoveDirectoryW(dir.c_str());
    printf(g_fail ? "%d FAILED\n" : "ALL PASSED\n", g_fail);
    CoUninitialize();
    return g_fail ? 1 : 0;
}
