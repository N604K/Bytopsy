#pragma once

// Disk health: each physical disk's model, type and Windows' own verdict (MSFT_PhysicalDisk), its
// wear, temperature, power-on hours and error counts (MSFT_StorageReliabilityCounter, which needs
// administrator rights; NVMe drives also answer their SMART / health log without them), which drive
// letters live on it, whether TRIM is on, and how used space changed over time.
// Read-only: nothing is ever sent to a disk but queries.

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "fsops.h"

enum class DiskHealthLevel { Unknown, Healthy, Warning, Unhealthy };

struct PhysicalDisk {
    int number = -1;  // DeviceId: \\.\PhysicalDriveN
    std::wstring model, firmware;
    int busType = 0;    // STORAGE_BUS_TYPE: 17 NVMe, 11 SATA, 7 USB, ...
    int mediaType = 0;  // 3 HDD, 4 SSD, 5 SCM, 0 unspecified
    uint64_t size = 0;
    int healthStatus = 5;  // MSFT_PhysicalDisk.HealthStatus: 0 healthy, 1 warning, 2 unhealthy, 5 unknown
    std::vector<int> opStatus;  // OperationalStatus codes (2 = OK)

    // Reliability counters (-1 = unknown).
    int wear = -1;              // % of the rated endurance used up
    int temperature = -1;       // °C now
    int temperatureMax = -1;    // °C the drive is rated for (warning threshold)
    int64_t powerOnHours = -1;
    int64_t readErrors = -1, readErrorsUncorrected = -1;
    int64_t writeErrors = -1, writeErrorsUncorrected = -1;
    int64_t startStopCycles = -1;
    // NVMe health log only.
    int criticalWarning = -1;   // bit 0 spare low, 1 temperature, 2 reliability degraded, 3 read-only, 4 backup failed
    int availableSpare = -1, spareThreshold = -1;  // %
    int64_t mediaErrors = -1;   // media and data integrity errors
    int64_t unsafeShutdowns = -1;
    uint64_t bytesWritten = 0, bytesRead = 0;  // 0 = unknown

    bool countersRead = false;    // MSFT_StorageReliabilityCounter answered
    bool nvmeLogRead = false;     // the NVMe health log answered
    bool countersDenied = false;  // the reliability counters need administrator rights
    std::vector<std::wstring> drives;  // "C:", "D:" (volumes with an extent on this disk)
    std::vector<std::wstring> fileSystems;  // their file systems, same order

    bool Ssd() const { return mediaType == 4 || mediaType == 5 || busType == 17; }
    bool HasCounters() const { return countersRead || nvmeLogRead; }
};

std::wstring BusName(int busType);         // "NVMe", "SATA", "USB", ...
std::wstring MediaName(const PhysicalDisk& d);  // "NVMe SSD", "SATA SSD", "Hard disk", ...
std::wstring OpStatusName(int code);       // "OK", "Predictive Failure", ...

// What the numbers mean: the worst problem decides the level; notes are worth knowing but don't
// change it (e.g. a few logged media errors on a drive Windows still rates healthy).
struct DiskVerdict {
    DiskHealthLevel level = DiskHealthLevel::Unknown;
    std::vector<std::wstring> problems;
    std::vector<std::wstring> notes;
};
DiskVerdict AssessDisk(const PhysicalDisk& d);
const wchar_t* HealthLevelName(DiskHealthLevel l);

// ---- Parsing (separate from the queries, so tests can feed sample output)

// A WMI object's properties as text, keys in lower case. Arrays are "2,53264".
using WmiProps = std::map<std::wstring, std::wstring>;
// `Get-CimInstance ... | Format-List *` output: "Key : Value" lines, blank lines between objects,
// arrays as "{2, 53264}", long values continued on indented lines.
std::vector<WmiProps> ParseFormatList(const std::wstring& text);
PhysicalDisk DiskFromWmi(const WmiProps& p);               // MSFT_PhysicalDisk
void ApplyReliabilityCounter(PhysicalDisk& d, const WmiProps& p);  // MSFT_StorageReliabilityCounter
// The NVMe SMART / Health Information log page (log 02h, 512 bytes).
bool ApplyNvmeHealthLog(PhysicalDisk& d, const uint8_t* log, size_t size);

// Which disks each volume has extents on.
struct VolumeDisks {
    std::wstring drive;  // "C:"
    std::wstring fs;     // "NTFS"
    std::vector<int> disks;
};
void AssignDrives(std::vector<PhysicalDisk>& disks, const std::vector<VolumeDisks>& volumes);

// ---- TRIM (fsutil behavior query DisableDeleteNotify): 1 on, 0 off, -1 unknown / not set.
struct TrimStatus {
    int ntfs = -1, refs = -1;
    std::wstring error;
};
TrimStatus ParseFsutilTrim(const std::wstring& out);
// TRIM for a file system ("NTFS", "ReFS"); -1 if unknown or the file system doesn't use it.
int TrimFor(const TrimStatus& t, const std::wstring& fs);

// ---- Used space over time

struct UsagePoint {
    int64_t time = 0;  // FILETIME
    uint64_t used = 0;
};
struct UsageTrend {
    bool ok = false;        // at least two points a day or more apart
    int64_t delta = 0;      // bytes, last - baseline
    double days = 0;        // between the baseline and the last point
    bool recent = false;    // the last point is from the last two days (so "in the last N days")
    int64_t daysToFull = -1;  // at this rate (growing, over at least a week), -1 = not meaningful
    std::vector<UsagePoint> points;  // baseline .. last, for a sparkline
};
// The change over the last `windowDays` (or over everything there is, if less).
UsageTrend ComputeTrend(std::vector<UsagePoint> pts, int64_t now, uint64_t freeNow, int windowDays = 30);
// "Used space grew 12.4 GiB in the last 30 days" (`what`: "Used space", "Scanned size").
std::wstring TrendText(const UsageTrend& t, const std::wstring& what, uint64_t total);

// WinNCDU records each drive's free space when Disk health opens or a scan finishes:
// %LOCALAPPDATA%\WinNCDU\freespace.log (WINNCDU_FREESPACE_LOG overrides it, for tests).
struct FreeSample {
    int64_t time = 0;  // FILETIME
    std::wstring drive;  // "C:"
    uint32_t serial = 0;  // volume serial number: another volume under the same letter is another series
    uint64_t total = 0, free = 0;
};
std::wstring FreeSpaceLogPath();
std::vector<FreeSample> ParseFreeSpaceLog(const std::string& text);
std::string FormatFreeSample(const FreeSample& s);
std::vector<FreeSample> LoadFreeSpaceLog();
// Appends a sample for each fixed drive given (roots like "C:\") unless one is younger than minGapSeconds.
void RecordFreeSpace(const std::vector<std::wstring>& roots, int64_t minGapSeconds = 6 * 3600);

struct DriveTrend {
    std::wstring drive, label, fs;  // "C:"
    uint64_t total = 0, free = 0;
    UsageTrend trend;
    bool fromScans = false;  // no free-space history yet: sizes of earlier scans of the drive root
    size_t samples = 0;      // points the trend is built from
    std::wstring text;       // TrendText, or how to get a trend
};
// `scans`: (time, bytes on disk) of earlier scans of this drive's root, any order.
DriveTrend BuildDriveTrend(const DriveInfo& d, uint32_t serial, const std::vector<FreeSample>& log,
                           const std::vector<UsagePoint>& scans, int64_t now);
uint32_t VolumeSerial(const std::wstring& root);

// ---- The queries

struct DiskHealthReport {
    std::vector<PhysicalDisk> disks;
    TrimStatus trim;
    bool elevated = false;
    std::wstring error;  // the storage WMI provider couldn't be asked
};
// WMI (root\Microsoft\Windows\Storage), the NVMe health log and temperature through
// IOCTL_STORAGE_QUERY_PROPERTY, volume extents, and fsutil. Takes a few seconds.
DiskHealthReport QueryDiskHealth();
// Drive letters and file systems of the volumes on each disk (IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS).
std::vector<VolumeDisks> ListVolumeDisks(const std::vector<DriveInfo>& drives);
