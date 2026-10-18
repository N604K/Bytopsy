// Self-check of the startup manager: bytopsy-startup-test.exe
// StartupApproved values, command line -> program, service image paths, impact estimates, boot log
// events, journal notes and the admin commands a change would run. Nothing is ever turned off or
// on: the only real-system part lists what is found, read-only.
#include <windows.h>

#include <cstdio>
#include <string>

#include "startup.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static bool EndsWith(const std::wstring& s, const std::wstring& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

static void Prog(const std::wstring& cmd, const std::wstring& want, const std::wstring& wantArgs = L"\x1") {
    std::wstring args;
    std::wstring got = ProgramFromCommand(cmd, &args);
    bool ok = got == want && (wantArgs == L"\x1" || args == wantArgs);
    Check(ok, L"program of [" + cmd + L"] = [" + got + L"]" + (wantArgs == L"\x1" ? L"" : L" args [" + args + L"]"));
}

static StartupEntry Est(uint64_t bytes, bool found = true) {
    StartupEntry e;
    e.program = L"C:\\Apps\\thing.exe";
    e.programFound = found;
    e.programBytes = bytes;
    return e;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    // Never read (or write) the real journal.
    wchar_t tmp[MAX_PATH];
    std::wstring journal = std::wstring(tmp, GetTempPathW(MAX_PATH, tmp)) + L"bytopsy-startup-test-journal.log";
    DeleteFileW(journal.c_str());
    SetEnvironmentVariableW(L"BYTOPSY_JOURNAL", journal.c_str());

    // ---- StartupApproved values
    {
        Check(ApprovedEnabled(nullptr, 0, nullptr), L"no StartupApproved value = enabled");
        uint8_t on2[12] = {0x02}, on6[12] = {0x06};
        int64_t when = 123;
        Check(ApprovedEnabled(on2, 12, &when) && when == 0, L"02 00 00 00 ... = enabled");
        Check(ApprovedEnabled(on6, 12, &when), L"06 ... = enabled");
        uint8_t off3[12] = {0x03, 0, 0, 0, 0x00, 0x80, 0x3E, 0xD5, 0xDE, 0xB1, 0x9D, 0x01};  // 1970-01-01 as a FILETIME
        Check(!ApprovedEnabled(off3, 12, &when) && when == 0x019DB1DED53E8000LL, L"03 ... + FILETIME = disabled, with the time");
        uint8_t off7[12] = {0x07};
        Check(!ApprovedEnabled(off7, 12, &when), L"07 ... = disabled");
        Check(!ApprovedEnabled(off3, 1, &when) && when == 0, L"a short disabled value has no time");
        auto v = MakeApproved(false, 0x0102030405060708LL);
        Check(v.size() == 12 && v[0] == 3 && v[1] == 0 && v[4] == 8 && v[11] == 1, L"MakeApproved(off) = 03 00 00 00 + FILETIME (little-endian)");
        int64_t back = 0;
        Check(!ApprovedEnabled(v.data(), v.size(), &back) && back == 0x0102030405060708LL, L"MakeApproved(off) reads back");
        auto on = MakeApproved(true, 999);
        bool zeros = true;
        for (size_t i = 1; i < on.size(); ++i) zeros = zeros && on[i] == 0;
        Check(on.size() == 12 && on[0] == 2 && zeros && ApprovedEnabled(on.data(), on.size(), nullptr), L"MakeApproved(on) = 02 + zeros");

        StartupEntry e;
        e.kind = StartupKind::Run;
        Check(EndsWith(ApprovedKey(e), L"StartupApproved\\Run"), L"HKCU Run -> StartupApproved\\Run");
        e.machine = e.wow32 = true;
        Check(EndsWith(ApprovedKey(e), L"StartupApproved\\Run32"), L"HKLM WOW6432Node Run -> StartupApproved\\Run32");
        e.kind = StartupKind::Folder;
        Check(EndsWith(ApprovedKey(e), L"StartupApproved\\StartupFolder"), L"Startup folder -> StartupApproved\\StartupFolder");
        e.kind = StartupKind::Task;
        Check(ApprovedKey(e).empty(), L"tasks have no StartupApproved key");
        e.kind = StartupKind::RunOnce;
        Check(ApprovedKey(e).empty(), L"RunOnce has no StartupApproved key");
    }

    // ---- Command line -> program
    Prog(L"\"C:\\Program Files\\App\\app.exe\" --minimized", L"C:\\Program Files\\App\\app.exe", L"--minimized");
    Prog(L"C:\\Program Files\\App\\app.exe /background", L"C:\\Program Files\\App\\app.exe", L"/background");
    Prog(L"C:\\Program Files (x86)\\Some Vendor\\tray.EXE", L"C:\\Program Files (x86)\\Some Vendor\\tray.EXE", L"");
    Prog(L"C:\\Tools\\app -config C:\\x\\run.cmd", L"C:\\Tools\\app");
    Prog(L"  \"C:\\a\\b.exe\"", L"C:\\a\\b.exe", L"");
    Prog(L"ctfmon", L"ctfmon");
    Prog(L"SecurityHealthSystray.exe", L"SecurityHealthSystray.exe");
    Prog(L"", L"");
    Prog(L"rundll32.exe C:\\Windows\\System32\\foo.dll,Entry arg", L"C:\\Windows\\System32\\foo.dll");
    Prog(L"C:\\WINDOWS\\system32\\rundll32.exe \"C:\\Program Files\\X Y\\z.dll\",Start", L"C:\\Program Files\\X Y\\z.dll");
    Prog(L"rundll32 C:\\Program Files\\X\\z.dll,Start -q", L"C:\\Program Files\\X\\z.dll");
    Prog(L"RUNDLL32.EXE shell32.dll,Control_RunDLL", L"shell32.dll");
    Prog(L"wscript.exe //B \"C:\\Users\\u\\run me.vbs\" /x", L"C:\\Users\\u\\run me.vbs", L"/x");
    Prog(L"C:\\Windows\\System32\\cscript.exe //nologo C:\\scripts\\s.js", L"C:\\scripts\\s.js");
    {
        std::wstring args;
        std::wstring p = ProgramFromCommand(L"C:\\Users\\u\\AppData\\Local\\Discord\\Update.exe --processStart Discord.exe", &args);
        Check(p == L"C:\\Users\\u\\AppData\\Local\\Discord\\Update.exe" && SquirrelTarget(args) == L"Discord.exe",
              L"Squirrel: Update.exe --processStart Discord.exe -> Discord.exe");
        Check(SquirrelTarget(L"--processStart \"Slack.exe\" --process-start-args \"--startup\"") == L"Slack.exe", L"Squirrel: quoted target");
        Check(SquirrelTarget(L"--processStart ..\\..\\evil.exe").empty(), L"Squirrel: a target with a path is ignored");
        Check(SquirrelTarget(L"--update https://x").empty(), L"Squirrel: no --processStart");
    }
    {
        wchar_t win[MAX_PATH];
        std::wstring w(win, GetEnvironmentVariableW(L"SystemRoot", win, MAX_PATH));
        Check(!w.empty() && ExpandEnv(L"%SystemRoot%\\x.exe") == w + L"\\x.exe", L"%SystemRoot% is expanded");
        Check(ExpandEnv(L"C:\\no vars") == L"C:\\no vars", L"no variables: unchanged");
        Check(ExpandEnv(L"%BYTOPSY_NOT_A_VAR_42%\\a") == L"%BYTOPSY_NOT_A_VAR_42%\\a", L"unknown variable: left as is");
        Prog(ExpandEnv(L"\"%ProgramFiles%\\App\\a.exe\" -s"), ExpandEnv(L"%ProgramFiles%\\App\\a.exe"));
    }

    // ---- Service image paths
    Check(ServiceImagePath(L"\\SystemRoot\\System32\\svc.exe", L"C:\\Windows") == L"C:\\Windows\\System32\\svc.exe", L"\\SystemRoot\\ form");
    Check(ServiceImagePath(L"\\??\\C:\\x\\y.exe -k", L"C:\\Windows") == L"C:\\x\\y.exe -k", L"\\??\\ form");
    Check(ServiceImagePath(L"system32\\svchost.exe -k netsvcs", L"C:\\Windows") == L"C:\\Windows\\system32\\svchost.exe -k netsvcs",
          L"relative system32\\ form");
    Check(ServiceImagePath(L"\"C:\\P F\\s.exe\" -run", L"C:\\Windows") == L"\"C:\\P F\\s.exe\" -run", L"quoted path unchanged");
    Prog(ServiceImagePath(L"\"\\??\\C:\\P F\\s.exe\" -run", L"C:\\Windows"), L"C:\\P F\\s.exe", L"-run");

    // ---- Publishers
    Check(IsMicrosoftPublisher(L"Microsoft Corporation"), L"Microsoft Corporation is Microsoft");
    Check(IsMicrosoftPublisher(L"MICROSOFT CORP."), L"case doesn't matter");
    Check(!IsMicrosoftPublisher(L"Valve Corporation") && !IsMicrosoftPublisher(L""), L"others are not");

    // ---- Impact
    {
        StartupEntry e = Est(1 << 20);
        e.measured = true;
        e.measuredMs = 4200;
        EstimateImpact(e);
        Check(e.impact == StartupImpact::High && util::ContainsI(e.impactWhy, L"measured"), L"measured 4.2 s = high (measured)");
        e.measuredMs = 1500;
        EstimateImpact(e);
        Check(e.impact == StartupImpact::Medium, L"measured 1.5 s = medium");
        e = Est(0, false);
        EstimateImpact(e);
        Check(e.impact == StartupImpact::Unknown, L"program not found = unknown, not invented");
        e = Est(0);
        EstimateImpact(e);
        Check(e.impact == StartupImpact::Unknown, L"0-byte app alias = unknown, not low");
        e = Est(2 << 20);
        e.chromium = true;
        EstimateImpact(e);
        Check(e.impact == StartupImpact::High && util::ContainsI(e.impactWhy, L"estimate"), L"Electron/Chromium app = high (estimate)");
        e = Est(150ull << 20);
        EstimateImpact(e);
        Check(e.impact == StartupImpact::High, L"150 MB program = high");
        e = Est(30ull << 20);
        EstimateImpact(e);
        Check(e.impact == StartupImpact::Medium, L"30 MB program = medium");
        e = Est(1 << 20);
        EstimateImpact(e);
        Check(e.impact == StartupImpact::Low, L"1 MB program = low");
        e = Est(30ull << 20);
        e.program = L"C:\\Apps\\GoogleUpdate.exe";
        EstimateImpact(e);
        Check(e.impact == StartupImpact::Medium, L"a 30 MB 'updater' is not assumed small");
        e = Est(3ull << 20);
        e.program = L"C:\\Apps\\GoogleUpdate.exe";
        EstimateImpact(e);
        Check(e.impact == StartupImpact::Low && util::ContainsI(e.impactWhy, L"updater"), L"small updater = low");
        e = Est(200ull << 20);
        e.kind = StartupKind::Service;
        e.delayed = true;
        EstimateImpact(e);
        Check(e.impact == StartupImpact::Low, L"delayed-start service = low");
    }

    // ---- Boot performance events
    {
        BootDelay d;
        std::wstring x101 =
            L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='Microsoft-Windows-"
            L"Diagnostics-Performance'/><EventID>101</EventID></System><EventData><Data Name='StartTime'>2026-01-01T08:00:00Z</Data>"
            L"<Data Name='Name'>Spotify.exe</Data><Data Name='FriendlyName'>Spotify &amp; Co</Data><Data Name='Version'/>"
            L"<Data Name='TotalTime'>4321</Data><Data Name='DegradationTime'>1200</Data></EventData></Event>";
        Check(ParseBootEvent(x101, d) && d.eventId == 101 && d.name == L"Spotify.exe" && d.friendlyName == L"Spotify & Co" &&
                  d.totalMs == 4321 && d.degradationMs == 1200,
              L"event 101 (slow app) is parsed");
        std::wstring x103 = L"<Event><System><EventID Qualifiers=\"\">103</EventID></System><EventData><Data Name=\"Name\">FooSvc</Data>"
                            L"<Data Name=\"TotalTime\">900</Data></EventData></Event>";
        Check(ParseBootEvent(x103, d) && d.eventId == 103 && d.name == L"FooSvc" && d.totalMs == 900, L"event 103 (slow service), double quotes");
        Check(!ParseBootEvent(L"<Event><System><EventID>100</EventID></System></Event>", d), L"event 100 (boot summary) is not an entry");
        Check(!ParseBootEvent(L"garbage", d), L"garbage is rejected");
    }

    // ---- Journal notes for services
    {
        std::wstring n = ServiceJournalNote(L"Foo Svc", true, L"delayed-auto");
        std::wstring name, word;
        bool off = false;
        Check(ParseServiceJournalNote(n, name, off, word) && name == L"Foo Svc" && off && word == L"delayed-auto",
              L"turn-off note keeps the previous start type: " + n);
        n = ServiceJournalNote(L"Foo Svc", false, L"auto");
        Check(ParseServiceJournalNote(n, name, off, word) && !off && word == L"auto", L"turn-on note: " + n);
        Check(!ParseServiceJournalNote(L"12 items in use were skipped", name, off, word), L"other notes are not service records");

        std::vector<JournalEntry> j(4);  // newest first
        j[0].note = ServiceJournalNote(L"A", true, L"auto");
        j[1].note = ServiceJournalNote(L"B", false, L"auto");
        j[2].note = ServiceJournalNote(L"B", true, L"delayed-auto");
        j[3].note = ServiceJournalNote(L"c", true, L"delayed-auto");
        auto m = ServicesTurnedOff(j);
        Check(m.size() == 2 && m[L"a"] == L"auto" && m[L"c"] == L"delayed-auto" && !m.count(L"b"),
              L"services turned off: the newest record of each counts (B was turned back on)");
    }

    // ---- Commands a change would run (built, never run)
    {
        StartupEntry e;
        e.kind = StartupKind::Run;
        e.name = L"Vendor Tray";
        Check(StartupAdminCommand(e, false, 0).empty(), L"this user's Run entries need no admin command");
        e.machine = e.wow32 = true;
        std::wstring c = StartupAdminCommand(e, false, 0x0102030405060708LL);
        Check(util::ContainsI(c, L"reg.exe\" add \"hklm\\software\\microsoft\\windows\\currentversion\\explorer\\startupapproved\\run32\" /v \"vendor tray\"") &&
                  EndsWith(c, L"/t REG_BINARY /d 030000000807060504030201 /f /reg:64"),
              L"HKLM Run32 off: " + c);
        c = StartupAdminCommand(e, true, 0x0102030405060708LL);
        Check(EndsWith(c, L"/d 020000000000000000000000 /f /reg:64"), L"HKLM Run32 on: " + c);
        e.kind = StartupKind::Folder;
        e.wow32 = false;
        e.name = L"Tool.lnk";
        Check(util::ContainsI(StartupAdminCommand(e, false, 1), L"startupapproved\\startupfolder\" /v \"tool.lnk\""), L"common Startup folder item");
        e.name = L"100%done";
        Check(StartupAdminCommand(e, false, 1).empty(), L"names with % are refused (cmd.exe would expand them)");
        e.name = L"say \"hi\"";
        Check(StartupAdminCommand(e, false, 1).empty(), L"names with quotes are refused");
        e.kind = StartupKind::Task;
        e.name = L"\\Vendor\\Update at logon";
        Check(EndsWith(StartupAdminCommand(e, false, 1), L"schtasks.exe\" /change /tn \"\\Vendor\\Update at logon\" /disable"), L"task off");
        Check(EndsWith(StartupAdminCommand(e, true, 1), L"/enable"), L"task on");
        e.kind = StartupKind::Service;
        e.name = L"VendorSvc";
        Check(EndsWith(StartupAdminCommand(e, false, 1), L"sc.exe\" config \"VendorSvc\" start= demand"), L"service off = Manual");
        Check(EndsWith(StartupAdminCommand(e, true, 1), L"start= auto"), L"service on = Automatic");
        e.restoreStart = L"delayed-auto";
        Check(EndsWith(StartupAdminCommand(e, true, 1), L"start= delayed-auto"), L"service on restores Automatic (delayed start)");
        StartupEntry once;
        once.kind = StartupKind::RunOnce;
        once.name = L"x";
        Check(StartupAdminCommand(once, false, 1).empty(), L"RunOnce has no command");
    }

    // ---- This PC, read-only
    {
        StartupList list = FindStartupEntries();
        size_t kinds[5] = {}, off = 0, ms = 0;
        bool named = true;
        for (auto& e : list.entries) {
            ++kinds[static_cast<int>(e.kind)];
            off += !e.enabled;
            ms += e.microsoft;
            named = named && !e.name.empty() && !e.display.empty() && !e.impactWhy.empty();
            if (e.kind == StartupKind::Service && e.microsoft) named = named && !e.canToggle;
            std::string u = util::WideToUtf8(L"     " + std::wstring(e.enabled ? L"on  " : L"off ") + StartupKindName(e.kind) + L" | " +
                                             e.display + L" | " + e.publisher + L" | " + StartupImpactName(e.impact) + L" | " +
                                             e.program + L"\n");
            fputs(u.c_str(), stdout);
        }
        wchar_t b[256];
        swprintf(b, 256, L"this PC: %zu Run, %zu RunOnce, %zu folder, %zu task, %zu service entries; %zu off, %zu Microsoft; %zu tasks in \\Microsoft\\Windows",
                 kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], off, ms, list.hiddenTasks);
        Check(named, b);
        Check(true, L"boot log: " + std::wstring(list.bootLogRead ? L"read" : list.bootLogNote.c_str()));
    }

    DeleteFileW(journal.c_str());
    printf(g_fail ? "\n%d FAILED\n" : "\nALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
