// Self-check of Disk health: winncdu-diskhealth-test.exe
// Parses sample Get-PhysicalDisk / Get-StorageReliabilityCounter / fsutil output and NVMe health
// logs, checks the verdicts, and computes used-space trends from synthetic history. Writes only a
// temp free-space log (WINNCDU_FREESPACE_LOG). Set WINNCDU_DISKS=1 to also query this PC's disks
// (read-only).
#include <windows.h>

#include <cstdio>
#include <string>

#include "diskhealth.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static bool Has(const std::vector<std::wstring>& v, const wchar_t* part) {
    for (auto& s : v)
        if (s.find(part) != std::wstring::npos) return true;
    return false;
}

static void PutLe(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

static constexpr int64_t kDay = 864000000000LL;
static constexpr uint64_t GB = 1ull << 30;

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // ---- Get-PhysicalDisk | Format-List * (two NVMe SSDs, a long ObjectId wrapped onto the next line)
    std::wstring pd =
        L"\r\n"
        L"DeviceId          : 1\r\n"
        L"FriendlyName      : WD_BLACK SN850X 1000GB\r\n"
        L"MediaType         : 4\r\n"
        L"BusType           : 17\r\n"
        L"HealthStatus      : 0\r\n"
        L"OperationalStatus : {2}\r\n"
        L"Size              : 1000204886016\r\n"
        L"FirmwareVersion   : 620361WD\r\n"
        L"ObjectId          : {1}\\\\PC\\root/Microsoft/Windows/Storage/Providers_v2\\SPACES_PhysicalDisk.ObjectId=\"{8403b184-a0ca\r\n"
        L"                    -11f0-83e1-806e6f6e6963}:PD:{6e937eee-fca7-5184-68d6-788ebd725a18}\"\r\n"
        L"\r\n"
        L"DeviceId          : 0\r\n"
        L"FriendlyName      : Samsung SSD 980 1TB\r\n"
        L"MediaType         : 4\r\n"
        L"BusType           : 17\r\n"
        L"HealthStatus      : 0\r\n"
        L"OperationalStatus : {2}\r\n"
        L"Size              : 1000204886016\r\n"
        L"\r\n";
    auto objs = ParseFormatList(pd);
    Check(objs.size() == 2, L"two physical disks in the Format-List output");
    if (objs.size() == 2) {
        PhysicalDisk a = DiskFromWmi(objs[0]);
        Check(a.number == 1 && a.model == L"WD_BLACK SN850X 1000GB" && a.busType == 17 && a.mediaType == 4 && a.size == 1000204886016ull &&
                  a.healthStatus == 0 && a.opStatus.size() == 1 && a.opStatus[0] == 2 && a.firmware == L"620361WD",
              L"MSFT_PhysicalDisk fields: " + a.model + L", " + MediaName(a));
        Check(objs[0][L"objectid"].find(L"-11f0-83e1") != std::wstring::npos && objs[0][L"objectid"].back() == L'"',
              L"a wrapped value is joined");
        Check(a.Ssd() && MediaName(a) == L"NVMe SSD", L"NVMe disk is an SSD");
        DiskVerdict v = AssessDisk(a);
        Check(v.level == DiskHealthLevel::Healthy && v.problems.empty() && v.notes.empty(), L"healthy disk: no problems, no notes");
    }

    // ---- Warning / unhealthy disks as Windows reports them
    {
        auto w = ParseFormatList(
            L"DeviceId          : 2\nFriendlyName      : ST2000DM008-2FR102\nMediaType         : 3\nBusType           : 11\n"
            L"HealthStatus      : 1\nOperationalStatus : {5}\nSize              : 2000398934016\n");
        PhysicalDisk d = w.empty() ? PhysicalDisk{} : DiskFromWmi(w[0]);
        DiskVerdict v = AssessDisk(d);
        Check(!d.Ssd() && MediaName(d) == L"Hard disk", L"SATA hard disk: " + MediaName(d));
        Check(v.level == DiskHealthLevel::Warning && Has(v.problems, L"Predictive Failure") && Has(v.problems, L"warning"),
              L"warning + predictive failure: " + std::wstring(HealthLevelName(v.level)));
        auto u = ParseFormatList(L"DeviceId : 3\nHealthStatus : 2\nOperationalStatus : {6, 53267}\nMediaType : 4\nBusType : 11\n");
        PhysicalDisk e = u.empty() ? PhysicalDisk{} : DiskFromWmi(u[0]);
        DiskVerdict ve = AssessDisk(e);
        Check(e.opStatus.size() == 2 && e.opStatus[1] == 53267 && OpStatusName(53267) == L"Failed Media", L"array of statuses parsed");
        Check(ve.level == DiskHealthLevel::Unhealthy && Has(ve.problems, L"unhealthy") && Has(ve.problems, L"Failed Media"),
              L"unhealthy disk with failed media");
        PhysicalDisk unknown;
        unknown.healthStatus = 5;
        Check(AssessDisk(unknown).level == DiskHealthLevel::Unknown, L"health status 5 stays unknown");
    }

    // ---- Get-StorageReliabilityCounter | Format-List * (administrator)
    {
        auto rc = ParseFormatList(
            L"DeviceId               : 2\nFlushLatencyMax        : 0\nLoadUnloadCycleCount   : 4211\nManufactureDate        :\n"
            L"PowerOnHours           : 31877\nReadErrorsCorrected    : 0\nReadErrorsTotal        : 12\nReadErrorsUncorrected  : 3\n"
            L"StartStopCycleCount    : 2110\nTemperature            : 38\nTemperatureMax         : 60\nWear                   : 0\n"
            L"WriteErrorsTotal       :\nWriteErrorsUncorrected :\n");
        PhysicalDisk d;
        d.healthStatus = 0;
        d.mediaType = 3;
        if (!rc.empty()) ApplyReliabilityCounter(d, rc[0]);
        Check(d.countersRead && d.powerOnHours == 31877 && d.temperature == 38 && d.temperatureMax == 60 && d.wear == 0 &&
                  d.readErrorsUncorrected == 3 && d.writeErrors == -1 && d.startStopCycles == 2110,
              L"reliability counters applied, empty values stay unknown");
        DiskVerdict v = AssessDisk(d);
        Check(v.level == DiskHealthLevel::Healthy && Has(v.notes, L"3 read/write errors") && Has(v.notes, L"still rates it healthy"),
              L"uncorrected errors are a note on a healthy disk");

        // Counters a drive doesn't support: all empty, temperature 0.
        auto none = ParseFormatList(L"DeviceId : 4\nTemperature : 0\nTemperatureMax : 0\nWear :\nPowerOnHours :\n");
        PhysicalDisk n;
        if (!none.empty()) ApplyReliabilityCounter(n, none[0]);
        Check(!n.countersRead && n.temperature == -1 && n.wear == -1 && !n.HasCounters(), L"missing counters: nothing known");

        PhysicalDisk hot = d;
        hot.temperature = 61;
        Check(AssessDisk(hot).level == DiskHealthLevel::Warning && Has(AssessDisk(hot).problems, L"running hot"), L"over its rated temperature");
        PhysicalDisk worn;
        worn.healthStatus = 0;
        worn.wear = 100;
        Check(AssessDisk(worn).level == DiskHealthLevel::Warning && Has(AssessDisk(worn).problems, L"rated endurance"), L"100% wear");
        worn.wear = 93;
        Check(AssessDisk(worn).level == DiskHealthLevel::Healthy && Has(AssessDisk(worn).notes, L"93%"), L"93% wear is a note");
    }

    // ---- Not administrator: the counters are denied, the disk is still listed
    {
        PhysicalDisk d;
        d.healthStatus = 0;
        d.busType = 11;
        d.mediaType = 4;
        d.countersDenied = true;
        Check(!d.HasCounters() && AssessDisk(d).level == DiskHealthLevel::Healthy && MediaName(d) == L"SATA SSD",
              L"non-admin: health from Windows, no counters");
    }

    // ---- NVMe SMART / Health log (what an NVMe drive answers without administrator rights)
    {
        uint8_t log[512] = {};
        log[0] = 0;              // critical warning
        log[1] = 320 & 0xFF;     // 320 K = 47 °C
        log[2] = 320 >> 8;
        log[3] = 81;             // available spare
        log[4] = 10;             // threshold
        log[5] = 7;              // percentage used
        PutLe(log + 32, 100000);   // data units read
        PutLe(log + 48, 200000);   // data units written
        PutLe(log + 128, 19418);   // power-on hours
        PutLe(log + 144, 61);      // unsafe shutdowns
        PutLe(log + 160, 478);     // media errors
        PhysicalDisk d;
        d.healthStatus = 0;
        d.busType = 17;
        d.mediaType = 4;
        d.countersDenied = true;
        Check(ApplyNvmeHealthLog(d, log, sizeof(log)) && d.nvmeLogRead && d.HasCounters(), L"NVMe health log read");
        Check(d.temperature == 47 && d.wear == 7 && d.availableSpare == 81 && d.spareThreshold == 10 && d.powerOnHours == 19418 &&
                  d.mediaErrors == 478 && d.unsafeShutdowns == 61 && d.bytesWritten == 200000ull * 512000 && d.bytesRead == 100000ull * 512000,
              L"NVMe fields: 47 °C, 7% used, 19418 h, 478 media errors");
        DiskVerdict v = AssessDisk(d);
        Check(v.level == DiskHealthLevel::Healthy && Has(v.notes, L"478 media errors") && v.problems.empty(),
              L"media errors are a note, Windows' verdict stays");
        PhysicalDisk c = d;
        c.criticalWarning = 0x04;
        Check(AssessDisk(c).level == DiskHealthLevel::Unhealthy && Has(AssessDisk(c).problems, L"degraded reliability"),
              L"critical warning: reliability degraded");
        c.criticalWarning = 0x01;
        c.availableSpare = 8;
        Check(AssessDisk(c).level == DiskHealthLevel::Warning && Has(AssessDisk(c).problems, L"8% left"), L"critical warning: spare low");
        PhysicalDisk wmiFirst;
        wmiFirst.wear = 9;
        wmiFirst.temperature = 40;
        ApplyNvmeHealthLog(wmiFirst, log, sizeof(log));
        Check(wmiFirst.wear == 9 && wmiFirst.temperature == 40, L"counters from Windows win over the raw log");
        uint8_t zero[512] = {};
        PhysicalDisk z;
        Check(!ApplyNvmeHealthLog(z, zero, sizeof(zero)) && !z.nvmeLogRead && !ApplyNvmeHealthLog(z, log, 100), L"empty or short log refused");
    }

    // ---- Drive letters on disks
    {
        std::vector<PhysicalDisk> disks(2);
        disks[0].number = 0;
        disks[1].number = 1;
        AssignDrives(disks, {{L"C:", L"NTFS", {0}}, {L"D:", L"NTFS", {1}}, {L"E:", L"ReFS", {0, 1}}});
        Check(disks[0].drives == std::vector<std::wstring>{L"C:", L"E:"} && disks[1].drives == std::vector<std::wstring>{L"D:", L"E:"} &&
                  disks[0].fileSystems[1] == L"ReFS",
              L"C: and E: on disk 0, D: and E: on disk 1");
    }

    // ---- fsutil behavior query DisableDeleteNotify
    {
        TrimStatus t = ParseFsutilTrim(
            L"NTFS DisableDeleteNotify = 0  (Allows TRIM operations to be sent to the storage device)\r\n"
            L"ReFS DisableDeleteNotify = 0  (Allows TRIM operations to be sent to the storage device)\r\n");
        Check(t.ntfs == 1 && t.refs == 1 && t.error.empty() && TrimFor(t, L"NTFS") == 1 && TrimFor(t, L"refs") == 1 && TrimFor(t, L"FAT32") == -1,
              L"TRIM on for NTFS and ReFS");
        TrimStatus off = ParseFsutilTrim(
            L"NTFS DisableDeleteNotify = 1  (Disables TRIM operations from being sent to the storage device)\r\n"
            L"ReFS DisableDeleteNotify is not currently set\r\n");
        Check(off.ntfs == 0 && off.refs == -1, L"NTFS TRIM off, ReFS not set");
        TrimStatus old = ParseFsutilTrim(L"DisableDeleteNotify = 0\r\n");
        Check(old.ntfs == 1 && old.refs == -1, L"older Windows: one value for NTFS");
        TrimStatus bad = ParseFsutilTrim(L"The FSUTIL utility requires that you have administrative privileges.\r\n");
        Check(bad.ntfs == -1 && bad.error.find(L"administrative") != std::wstring::npos, L"fsutil error comes through");
    }

    // ---- Trends from synthetic history
    int64_t now = util::NowFileTime();
    {
        // 60 daily points growing 0.4 GiB a day.
        std::vector<UsagePoint> pts;
        for (int i = 60; i >= 0; --i) pts.push_back({now - i * kDay, 400 * GB + static_cast<uint64_t>((60 - i) * 0.4 * GB)});
        UsageTrend t = ComputeTrend(pts, now, 100 * GB);
        int64_t want = static_cast<int64_t>(30 * 0.4 * GB);
        Check(t.ok && t.recent && std::llabs(t.delta - want) < static_cast<int64_t>(GB / 2) && t.days > 29.5 && t.days < 30.5,
              L"last 30 days: grew " + util::FormatSizeShort(static_cast<uint64_t>(t.delta)));
        Check(t.daysToFull >= 240 && t.daysToFull <= 260, L"full in ~250 days at 0.4 GiB/day: " + std::to_wstring(t.daysToFull));
        std::wstring text = TrendText(t, L"Used space", 500 * GB);
        Check(text.find(L"Used space grew 12") == 0 && text.find(L"in the last 30 days") != std::wstring::npos &&
                  text.find(L"full in about 8 months") != std::wstring::npos,
              L"\"" + text + L"\"");
        Check(t.points.size() >= 2 && t.points.size() <= 90 && t.points.back().time == pts.back().time, L"sparkline points end at the last one");

        std::vector<UsagePoint> flat = {{now - 10 * kDay, 300 * GB}, {now - 5 * kDay, 300 * GB + (50ull << 20)}, {now, 300 * GB}};
        UsageTrend f = ComputeTrend(flat, now, 200 * GB);
        Check(f.ok && TrendText(f, L"Used space", 500 * GB) == L"Used space stayed about the same in the last 10 days", L"steady");

        std::vector<UsagePoint> shrink = {{now - 12 * kDay, 300 * GB}, {now - 2 * kDay - kDay / 2, 280 * GB}};
        UsageTrend s = ComputeTrend(shrink, now, 200 * GB);
        std::wstring st = TrendText(s, L"Scanned size", 500 * GB);
        Check(s.ok && s.delta < 0 && !s.recent && s.daysToFull == -1 && st.find(L"Scanned size shrank 20") == 0 &&
                  st.find(L"over 10 days (up to ") != std::wstring::npos,
              L"\"" + st + L"\"");

        Check(!ComputeTrend({{now, GB}}, now, GB).ok, L"one point: no trend");
        Check(!ComputeTrend({{now - kDay / 8, GB}, {now, 2 * GB}}, now, GB).ok, L"three hours apart: no trend");
        // All points in the window: the trend spans everything there is.
        UsageTrend w = ComputeTrend({{now - 3 * kDay, 10 * GB}, {now - kDay, 11 * GB}, {now, 13 * GB}}, now, GB);
        Check(w.ok && w.delta == static_cast<int64_t>(3 * GB) && w.daysToFull == -1, L"three days: +3 GiB, too short to predict");
    }

    // ---- Free-space log and drive trends
    {
        FreeSample s;
        s.time = util::UnixToFileTime(1760000000);
        s.drive = L"C:";
        s.serial = 0xA1B2C3D4;
        s.total = 1000 * GB;
        s.free = 250 * GB;
        std::string line = FormatFreeSample(s);
        auto back = ParseFreeSpaceLog(line + "garbage line\n1760000100\tD:\t00000001\t5\t9\n" + line);
        Check(back.size() == 2 && back[0].drive == L"C:" && back[0].serial == 0xA1B2C3D4 && back[0].total == 1000 * GB &&
                  back[0].free == 250 * GB && back[0].time == s.time,
              L"free-space log round trip, bad lines skipped");

        DriveInfo c;
        c.root = L"C:\\";
        c.ready = true;
        c.type = DRIVE_FIXED;
        c.total = 1000 * GB;
        c.free = 200 * GB;
        std::vector<FreeSample> log;
        for (int i = 20; i >= 1; --i) {
            FreeSample x = s;
            x.time = now - i * kDay;
            x.free = 200 * GB + i * GB / 2;  // used grows 0.5 GiB a day
            log.push_back(x);
        }
        FreeSample other = s;  // another volume that once had the letter
        other.serial = 7;
        other.time = now - 40 * kDay;
        other.free = 900 * GB;
        log.push_back(other);
        DriveTrend dt = BuildDriveTrend(c, s.serial, log, {}, now);
        Check(dt.trend.ok && !dt.fromScans && dt.samples == 21 && dt.text.find(L"Used space grew 10") == 0,
              L"C: from the free-space log: " + dt.text);
        DriveTrend scans = BuildDriveTrend(c, 99, log, {{now - 9 * kDay, 700 * GB}, {now - kDay, 705 * GB}}, now);
        Check(scans.trend.ok && scans.fromScans && scans.text.find(L"Scanned size grew 5") == 0, L"no log for this volume: scans: " + scans.text);
        DriveTrend none = BuildDriveTrend(c, 99, {}, {{now - kDay, 700 * GB}}, now);
        Check(!none.trend.ok && none.text == L"Scan this drive a few times to see a trend", L"no history: " + none.text);
    }

    // ---- Recording free space (temp log; reads the real C: free space, read-only)
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring path = std::wstring(tmp) + L"winncdu-freespace-test-" + std::to_wstring(GetCurrentProcessId()) + L".log";
        SetEnvironmentVariableW(L"WINNCDU_FREESPACE_LOG", path.c_str());
        Check(FreeSpaceLogPath() == path, L"WINNCDU_FREESPACE_LOG respected");
        wchar_t win[MAX_PATH];
        GetWindowsDirectoryW(win, MAX_PATH);
        std::wstring sys = std::wstring(win).substr(0, 3);
        RecordFreeSpace({sys});
        RecordFreeSpace({sys});  // within the gap: no second sample
        RecordFreeSpace({L"\\\\server\\share"});
        auto got = LoadFreeSpaceLog();
        Check(got.size() == 1 && got[0].drive == sys.substr(0, 2) && got[0].total > 0 && got[0].free <= got[0].total &&
                  got[0].serial == VolumeSerial(sys),
              L"one sample of " + sys + L" recorded: " + (got.empty() ? L"none" : util::FormatSizeShort(got[0].free) + L" free"));
        RecordFreeSpace({sys}, 0);
        Check(LoadFreeSpaceLog().size() == 2, L"no gap: another sample");
        DeleteFileW(path.c_str());
        SetEnvironmentVariableW(L"WINNCDU_FREESPACE_LOG", nullptr);
    }

    // ---- This PC (read-only), on request
    wchar_t flag[8];
    if (GetEnvironmentVariableW(L"WINNCDU_DISKS", flag, 8)) {
        DiskHealthReport r = QueryDiskHealth();
        Check(r.error.empty() && !r.disks.empty(), L"this PC: " + std::to_wstring(r.disks.size()) + L" disks" + (r.error.empty() ? L"" : L", " + r.error));
        for (auto& d : r.disks) {
            std::wstring drives;
            for (auto& x : d.drives) drives += x + L" ";
            DiskVerdict v = AssessDisk(d);
            Check(true, L"  disk " + std::to_wstring(d.number) + L": " + d.model + L" (" + MediaName(d) + L") " + HealthLevelName(v.level) +
                            L", wear " + std::to_wstring(d.wear) + L"%, " + std::to_wstring(d.temperature) + L" °C, drives " + drives +
                            (d.countersDenied ? L"(counters need admin)" : L""));
        }
        Check(r.trim.ntfs >= 0, L"this PC: TRIM for NTFS is " + std::wstring(r.trim.ntfs == 1 ? L"on" : r.trim.ntfs == 0 ? L"off" : L"unknown"));
    }

    printf(g_fail ? "%d FAILED\n" : "ALL PASSED (%d failed)\n", g_fail);
    return g_fail ? 1 : 0;
}
