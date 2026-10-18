// Self-check of the scheduled cleanup: bytopsy-schedule-test.exe
// The pure helpers (ids, --clean arguments, triggers, results), then bytopsy-cli --clean against a
// fixture in %TEMP% (BYTOPSY_CLEANERS replaces every rule, so nothing else can be cleaned).
// Task Scheduler itself is only touched with BYTOPSY_TEST_TASKSCHED=1, and then only a harmless
// test task under \Bytopsy-test that is removed again.
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <string>
#include <vector>

#include "fsops.h"
#include "journal.h"
#include "schedule.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static void Put(const std::wstring& path, const char* text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        fputs(text, f);
        fclose(f);
    }
}

static void PutW(const std::wstring& path, const std::wstring& text) {
    std::string u = util::WideToUtf8(text);
    Put(path, u.c_str());
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

static void Age(const std::wstring& path, int hours) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    int64_t t = util::NowFileTime() - static_cast<int64_t>(hours) * 36000000000LL;
    FILETIME ft{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
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

static void RemoveTree(const std::wstring& path) {
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> removed{0};
    std::wstring err;
    if (PathExists(path)) DeletePermanently(path, cancel, removed, err);
}

static bool IsElevatedNow() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

int main() {
    // ---- Id lists
    {
        auto v = ParseIdList(L" temp, Browser,,dev,temp ,");
        Check(v.size() == 3 && v[0] == L"temp" && v[1] == L"browser" && v[2] == L"dev", L"id list: trimmed, lowercase, no duplicates");
        Check(JoinIds(v) == L"temp,browser,dev", L"ids joined with commas");
        Check(ParseIdList(L" , ,").empty(), L"an empty list stays empty");
    }

    // ---- --clean on the command line
    {
        CleanCommand c;
        std::wstring err;
        Check(ParseCleanCommandLine({L"-o", L"x.json", L"C:\\"}, c, err) && !c.clean, L"no --clean: left to the normal options");
        Check(ParseCleanCommandLine({L"--clean", L"temp,browser", L"--yes", L"--quiet"}, c, err) && c.clean && c.yes && c.quiet &&
                  c.ids.size() == 2,
              L"--clean temp,browser --yes --quiet");
        Check(ParseCleanCommandLine({L"--clean=Temp"}, c, err) && c.clean && !c.yes && c.ids.size() == 1 && c.ids[0] == L"temp",
              L"--clean=Temp is a dry run of temp");
        Check(ParseCleanCommandLine({L"-y", L"--clean", L"all-safe", L"-q", L"--si"}, c, err) && c.yes && c.quiet && c.si,
              L"short flags, in any order");
        Check(!ParseCleanCommandLine({L"--clean"}, c, err) && err.find(L"needs category ids") != std::wstring::npos, L"--clean without ids");
        Check(!ParseCleanCommandLine({L"--clean", L"--yes"}, c, err), L"--clean --yes: ids missing");
        Check(!ParseCleanCommandLine({L"--clean", L",,"}, c, err), L"--clean with an empty list");
        Check(!ParseCleanCommandLine({L"--clean", L"temp", L"-o", L"x"}, c, err) && err.find(L"-o") != std::wstring::npos,
              L"--clean refuses other options");
        Check(!ParseCleanCommandLine({L"--clean", L"temp", L"--clean", L"dev"}, c, err), L"--clean twice");
        Check(!ParseCleanCommandLine({L"--clean", L"temp", L"C:\\"}, c, err), L"--clean with a path");
    }

    // ---- Ids against the categories
    {
        std::vector<CleanerInfo> known = {{L"recycle", true, false, false, false}, {L"temp", true, false, false, false},
                                          {L"wintemp", true, false, true, false},  {L"dev", false, false, false, false},
                                          {L"winold", false, true, true, false},   {L"w2:app cache", false, false, false, true}};
        std::vector<std::wstring> out;
        std::wstring err;
        Check(ResolveCleanIds({L"all-safe"}, known, out, err) && JoinIds(out) == L"recycle,temp,wintemp",
              L"all-safe = the preselected ones: " + JoinIds(out));
        Check(ResolveCleanIds({L"dev", L"all-safe", L"temp"}, known, out, err) && JoinIds(out) == L"dev,recycle,temp,wintemp",
              L"opt-in ones only when named, no duplicates");
        Check(ResolveCleanIds({L"winold", L"w2:app cache"}, known, out, err) && out.size() == 2, L"external and imported ids can be named");
        Check(!ResolveCleanIds({L"temp", L"nope", L"bogus"}, known, out, err) && out.empty() && err.find(L"nope,bogus") != std::wstring::npos &&
                  err.find(L"Valid: recycle, temp, wintemp, dev, winold, or all-safe") != std::wstring::npos &&
                  err.find(L"w2:app") == std::wstring::npos && err.find(L"1 imported") != std::wstring::npos,
              L"unknown ids are named with the valid ones: " + err);
        Check(CleanExitCode(true, 0, 0) == 0 && CleanExitCode(true, 3, 0) == 1 && CleanExitCode(true, 0, 1) == 1 && CleanExitCode(false, 0, 0) == 3,
              L"exit codes");
    }

    // ---- The task's arguments
    {
        Check(CleanArguments({L"temp", L"browser"}) == L"--clean temp,browser --yes --quiet", L"arguments of the scheduled run");
        std::vector<std::wstring> ids;
        Check(ParseCleanArguments(L"--clean temp,browser --yes --quiet", ids) && JoinIds(ids) == L"temp,browser", L"ids read back from them");
        std::wstring q = CleanArguments({L"w2:app cache", L"temp"});
        Check(q == L"--clean \"w2:app cache,temp\" --yes --quiet" && ParseCleanArguments(q, ids) && ids.size() == 2 && ids[0] == L"w2:app cache",
              L"ids with spaces are quoted and read back: " + q);
        Check(!ParseCleanArguments(L"/c rem", ids) && !ParseCleanArguments(L"", ids), L"other arguments aren't a scheduled cleanup");
        Check(ParseCleanArguments(L"/c rem --clean temp,dev --yes --quiet", ids) && JoinIds(ids) == L"temp,dev", L"a wrapper's arguments before --clean are skipped");
        Check(ScheduleFolder() == L"\\Bytopsy", L"the task folder");
        SetEnvironmentVariableW(L"BYTOPSY_TASK_FOLDER", L"\\Microsoft\\Windows");
        Check(ScheduleFolder() == L"\\Bytopsy", L"the test override can't point anywhere else");
        SetEnvironmentVariableW(L"BYTOPSY_TASK_FOLDER", L"\\Bytopsy-test");
        Check(ScheduleFolder() == L"\\Bytopsy-test", L"... only at a \\Bytopsy-test folder");
        SetEnvironmentVariableW(L"BYTOPSY_TASK_FOLDER", nullptr);
    }

    // ---- Trigger text and first start
    {
        ScheduleSpec s;
        s.freq = ScheduleFreq::Daily;
        s.hour = 3;
        s.minute = 0;
        Check(DescribeSchedule(s) == L"Daily at 03:00", DescribeSchedule(s));
        s.freq = ScheduleFreq::Weekly;
        s.weekday = 1;
        s.hour = 12;
        s.minute = 30;
        Check(DescribeSchedule(s) == L"Weekly on Monday at 12:30", DescribeSchedule(s));
        s.freq = ScheduleFreq::Monthly;
        s.monthDay = 15;
        Check(DescribeSchedule(s) == L"Monthly on day 15 at 12:30", DescribeSchedule(s));
        s.freq = ScheduleFreq::Logon;
        Check(DescribeSchedule(s) == L"At logon", DescribeSchedule(s));

        ScheduleSpec d;
        d.freq = ScheduleFreq::Daily;
        d.hour = 3;
        d.minute = 0;
        // 2026-10-18 is a Sunday.
        Check(ScheduleStartBoundary(d, 2026, 10, 18, 0, 2, 59) == L"2026-10-18T03:00:00", L"daily, later today");
        Check(ScheduleStartBoundary(d, 2026, 10, 18, 0, 3, 0) == L"2026-10-19T03:00:00", L"daily, already past: tomorrow");
        Check(ScheduleStartBoundary(d, 2026, 12, 31, 4, 23, 0) == L"2027-01-01T03:00:00", L"daily over the new year");
        Check(ScheduleStartBoundary(d, 2028, 2, 28, 1, 9, 0) == L"2028-02-29T03:00:00", L"daily into a leap day");
        ScheduleSpec w;
        w.freq = ScheduleFreq::Weekly;
        w.weekday = 3;  // Wednesday
        w.hour = 12;
        w.minute = 0;
        Check(ScheduleStartBoundary(w, 2026, 10, 18, 0, 10, 0) == L"2026-10-21T12:00:00", L"weekly: the coming Wednesday");
        Check(ScheduleStartBoundary(w, 2026, 10, 21, 3, 13, 0) == L"2026-10-28T12:00:00", L"weekly: today but past, next week");
        Check(ScheduleStartBoundary(w, 2026, 10, 21, 3, 11, 0) == L"2026-10-21T12:00:00", L"weekly: later today");
        Check(ScheduleStartBoundary(w, 2026, 10, 29, 4, 11, 0) == L"2026-11-04T12:00:00", L"weekly over a month end");
        ScheduleSpec m;
        m.freq = ScheduleFreq::Monthly;
        m.monthDay = 10;
        m.hour = 9;
        m.minute = 15;
        Check(ScheduleStartBoundary(m, 2026, 10, 5, 1, 0, 0) == L"2026-10-10T09:15:00", L"monthly: later this month");
        Check(ScheduleStartBoundary(m, 2026, 10, 18, 0, 0, 0) == L"2026-11-10T09:15:00", L"monthly: next month");
        Check(ScheduleStartBoundary(m, 2026, 12, 10, 4, 10, 0) == L"2027-01-10T09:15:00", L"monthly: past today in December");
        m.monthDay = 31;
        Check(ScheduleStartBoundary(m, 2026, 10, 1, 4, 0, 0) == L"2026-10-28T09:15:00", L"monthly: day clamped to 28");
        int h = -1, mi = -1;
        Check(ParseBoundaryTime(L"2026-10-21T12:05:00", h, mi) && h == 12 && mi == 5, L"time read back from a start boundary");
        Check(ParseBoundaryTime(L"2026-10-21T07:30:00+02:00", h, mi) && h == 7 && mi == 30, L"... with a time zone");
        Check(!ParseBoundaryTime(L"2026-10-21", h, mi), L"... none without a time");
        Check(WeekdayFromMask(1) == 0 && WeekdayFromMask(1 << 6) == 6 && WeekdayFromMask(0) == -1, L"weekday masks");
        Check(MonthDayFromMask(1) == 1 && MonthDayFromMask(1u << 27) == 28 && MonthDayFromMask(0) == -1, L"month day masks");
        Check(DescribeTaskResult(0) == L"OK" && DescribeTaskResult(0x41303) == L"not run yet" && DescribeTaskResult(3).find(L"administrator") != std::wstring::npos &&
                  DescribeTaskResult(0x80041234) == L"failed (0x80041234)",
              L"last results in words");
    }

    // ---- bytopsy-cli --clean against a fixture
    wchar_t b[MAX_PATH];
    std::wstring tmp(b, GetTempPathW(MAX_PATH, b));
    std::wstring dir = tmp + L"bytopsy-schedule-test-" + std::to_wstring(GetCurrentProcessId());
    std::wstring data = dir + L"\\data";  // journal.log and cleanup.log
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW(data.c_str(), nullptr);
    SetEnvironmentVariableW(L"BYTOPSY_JOURNAL", (data + L"\\journal.log").c_str());
    std::wstring cache = dir + L"\\cache", admin = dir + L"\\admin", opt = dir + L"\\opt";
    for (auto& d : {cache, admin, opt, cache + L"\\old-folder", cache + L"\\repo", cache + L"\\repo\\.git"}) CreateDirectoryW(d.c_str(), nullptr);
    Put(cache + L"\\old.tmp", "old!");
    Put(cache + L"\\old-folder\\a.tmp", "aaaaaa");
    Put(cache + L"\\new.tmp", "new");
    Put(cache + L"\\repo\\.git\\HEAD", "ref");
    Put(cache + L"\\passwords.kdbx", "secret");
    Put(admin + L"\\a.tmp", "admin");
    Put(opt + L"\\o.tmp", "optional");
    for (auto& f : {cache + L"\\old.tmp", cache + L"\\old-folder\\a.tmp", cache + L"\\old-folder", cache + L"\\repo", cache + L"\\passwords.kdbx"})
        Age(f, 48);
    PutW(dir + L"\\cleaners.ini", L"[fixture]\nname = Test fixture\nsafe = true\npath = " + cache + L" | age=24h\n"
                                  L"[fixadmin]\nname = Admin fixture\nadmin = true\nsafe = true\npath = " + admin + L"\n"
                                  L"[fixopt]\nname = Optional fixture\npath = " + opt + L"\n");
    SetEnvironmentVariableW(L"BYTOPSY_CLEANERS", (dir + L"\\cleaners.ini").c_str());
    bool elevated = IsElevatedNow();
    {
        std::string out;
        int code = RunCli(L"--clean nope", out);
        Check(code == 2 && out.find("unknown cleanup category nope") != std::string::npos && out.find("fixture, fixadmin, fixopt") != std::string::npos,
              L"unknown id: exit 2 and the valid ids (" + std::to_wstring(code) + L")");
        code = RunCli(L"--clean all-safe", out);
        Check(code == 0 && out.find("Dry run") != std::string::npos && out.find("Would free 10 B") != std::string::npos &&
                  out.find("[fixture]") != std::string::npos && out.find("[fixopt]") == std::string::npos,
              L"dry run: exit 0, what would be freed (" + util::Utf8ToWide(out.substr(0, 400)) + L")");
        Check(PathExists(cache + L"\\old.tmp") && PathExists(admin + L"\\a.tmp") && !PathExists(data + L"\\cleanup.log"),
              L"dry run deletes nothing and logs nothing");
        if (!elevated)
            Check(out.find("needs administrator rights") != std::string::npos, L"dry run: admin category skipped with a note");

        code = RunCli(L"--clean all-safe --yes --quiet", out);
        Check(code == 0 && out.empty(), L"--yes --quiet: exit 0, no output (" + std::to_wstring(code) + L")");
        Check(!PathExists(cache + L"\\old.tmp") && !PathExists(cache + L"\\old-folder"), L"old entries deleted");
        Check(PathExists(cache + L"\\new.tmp"), L"entry newer than the age limit kept");
        Check(PathExists(cache + L"\\repo\\.git\\HEAD") && PathExists(cache + L"\\passwords.kdbx"), L"protected items (repository, KeePass) kept");
        Check(PathExists(opt + L"\\o.tmp"), L"opt-in category not named: untouched");
        Check(PathExists(admin + L"\\a.tmp") == !elevated, L"admin category skipped unless elevated");
        std::string journal = Slurp(data + L"\\journal.log");
        Check(journal.find("\tclean\t10\t2\tTest fixture\t") != std::string::npos && journal.find("command line") != std::string::npos,
              L"journaled like the windowed app");
        std::string log = Slurp(data + L"\\cleanup.log");
        Check(log.find("clean fixture,fixadmin: freed 10 B") != std::string::npos && (elevated || log.find("skipped fixadmin (needs administrator rights)") != std::string::npos) &&
                  log.find("exit 0") != std::string::npos,
              L"a line in cleanup.log: " + util::Utf8ToWide(log));
        if (!elevated) {
            code = RunCli(L"--clean fixadmin --yes", out);
            Check(code == 3 && PathExists(admin + L"\\a.tmp"), L"only admin categories, not elevated: exit 3, nothing deleted");
        }
        code = RunCli(L"--clean fixopt --yes", out);
        Check(code == 0 && !PathExists(opt + L"\\o.tmp") && out.find("Freed 8 B") != std::string::npos, L"opt-in category when named");
    }
    SetEnvironmentVariableW(L"BYTOPSY_CLEANERS", nullptr);
    RemoveTree(dir);
    Check(!PathExists(dir), L"fixture removed");

    // ---- Task Scheduler (only when asked): a harmless task under \Bytopsy-test, removed again
    wchar_t ts[8];
    if (GetEnvironmentVariableW(L"BYTOPSY_TEST_TASKSCHED", ts, 8) && ts[0] == L'1') {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const std::wstring folder = L"\\Bytopsy-test", name = L"Scheduled cleanup (test)";
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring exe = std::wstring(sys) + L"\\cmd.exe";
        ScheduleSpec w;
        w.freq = ScheduleFreq::Weekly;
        w.weekday = 3;
        w.hour = 4;
        w.minute = 30;
        std::wstring err;
        Check(TaskSave(folder, name, w, exe, L"/c rem bytopsy-test", L"Bytopsy self-test (harmless)", err), L"test task saved: " + err);
        TaskInfo info;
        Check(TaskQuery(folder, name, info, err) && info.exists && info.enabled && info.specKnown && info.spec.freq == ScheduleFreq::Weekly &&
                  info.spec.weekday == 3 && info.spec.hour == 4 && info.spec.minute == 30 && _wcsicmp(info.path.c_str(), exe.c_str()) == 0 &&
                  info.arguments == L"/c rem bytopsy-test" && info.nextRun > util::NowFileTime() && info.lastResult == 0x41303,
              L"read back: weekly Wednesday 04:30, next " + util::FormatTime(info.nextRun) + L", " + DescribeTaskResult(info.lastResult) + L" " + err);
        ScheduleSpec m;
        m.freq = ScheduleFreq::Monthly;
        m.monthDay = 7;
        m.hour = 22;
        Check(TaskSave(folder, name, m, exe, L"/c rem bytopsy-test", L"Bytopsy self-test (harmless)", err) && TaskQuery(folder, name, info, err) &&
                  info.spec.freq == ScheduleFreq::Monthly && info.spec.monthDay == 7 && info.spec.hour == 22,
              L"updated in place to monthly");
        ScheduleSpec l;
        l.freq = ScheduleFreq::Logon;
        Check(TaskSave(folder, name, l, exe, L"/c rem bytopsy-test", L"Bytopsy self-test (harmless)", err) && TaskQuery(folder, name, info, err) &&
                  info.spec.freq == ScheduleFreq::Logon,
              L"at logon (this user's): " + err);
        ScheduleSpec d;
        d.freq = ScheduleFreq::Daily;
        d.hour = 5;
        Check(TaskSave(folder, name, d, exe, L"/c rem bytopsy-test", L"Bytopsy self-test (harmless)", err) && TaskQuery(folder, name, info, err) &&
                  info.spec.freq == ScheduleFreq::Daily && info.spec.hour == 5,
              L"daily: " + err);
        Check(TaskRemove(folder, name, err), L"test task removed: " + err);
        Check(TaskQuery(folder, name, info, err) && !info.exists, L"and gone");
        Check(TaskRemove(folder, name, err), L"removing it again is fine");
        CoUninitialize();
    }

    printf(g_fail ? "%d FAILED\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
