#pragma once

// Low disk space alerts. No program stays running: a per-user Task Scheduler task
// (\Bytopsy\Disk space check) starts `bytopsy.exe --check-space` every few hours (and at logon).
// That run reads the alert settings from settings.ini, notes each fixed drive's free space in
// freespace.log (for Disk health's trend), and shows a Windows notification for a drive whose free
// space is under its limit: once, then again only after it has been back above the limit, or a
// day later. What it last told is kept in space-alerts.txt next to the settings.
// `bytopsy-cli --check-space` does the same from a terminal (--dry-run: only says what it would do).

#include <cstdint>
#include <string>
#include <vector>

#include "notify.h"

// ---- Limits

enum class LimitUnit { Percent, GiB };

struct SpaceLimit {
    LimitUnit unit = LimitUnit::Percent;
    double value = 10;  // % of the drive's size, or GiB
    bool operator==(const SpaceLimit&) const = default;
};

// "10%", "2.5 %", "20G", "20 GB", "20 GiB" (GB counts as GiB: like every size Bytopsy parses,
// limits are binary). False for anything else, 0 or less, a percentage of 100 or more, or more
// than 1,000,000 GiB.
bool ParseLimit(const std::wstring& s, SpaceLimit& out);
std::wstring FormatLimit(const SpaceLimit& l);    // "10%", "20G" (settings.ini)
std::wstring DescribeLimit(const SpaceLimit& l);  // "10%", "20 GiB" (shown; regional decimal separator)
// The limit in bytes for a drive of `total` bytes (a percentage of it, or the GiB).
uint64_t LimitBytes(const SpaceLimit& l, uint64_t total);
// The limits the windowed app offers, smallest percentage first, then GiB.
std::vector<SpaceLimit> LimitChoices();
// How often the check can run: 1, 2, 4, 6, 12, 24 hours.
const std::vector<int>& CheckIntervals();

// ---- Settings (settings.ini, next to the windowed app's own settings)

struct DriveRule {
    std::wstring drive;  // "D:"
    bool watched = true;
    SpaceLimit limit;
};

struct AlertSettings {
    bool enabled = false;
    int everyHours = 1;  // one of CheckIntervals()
    bool atLogon = true;
    SpaceLimit all;      // every fixed drive without a rule of its own
    std::vector<DriveRule> drives;
};

struct EffectiveRule {
    bool watched = true;
    SpaceLimit limit;
    bool own = false;  // the drive has a rule of its own
};
EffectiveRule RuleFor(const AlertSettings& s, const std::wstring& drive);
// Sets (or, with `like all drives`, removes) a drive's own rule.
void SetDriveRule(AlertSettings& s, const std::wstring& drive, bool likeAll, bool watched, const SpaceLimit& limit);

// Keys: alerts=1, alertEvery=1, alertLogon=1, alertLimit=10%, alertDrive=D:,20G (repeatable;
// "D:,off" = not watched). Anything unreadable keeps its default.
bool IsAlertKey(const std::wstring& key);
AlertSettings ParseAlertSettings(const std::wstring& ini);
std::wstring FormatAlertSettings(const AlertSettings& s);  // those lines, "\n" after each
// `ini` with its alert lines replaced by FormatAlertSettings(s); every other line is kept as it was.
std::wstring MergeAlertSettings(const std::wstring& ini, const AlertSettings& s);
// The alert lines of `ini` (the windowed app keeps them when it rewrites its settings).
std::wstring AlertLinesOf(const std::wstring& ini);

std::wstring SettingsFilePath();  // util::DataDir()\settings.ini (BYTOPSY_SETTINGS overrides it, for tests)
std::wstring ReadSettingsText();  // "" when there is none
AlertSettings LoadAlertSettings();
bool SaveAlertSettings(const AlertSettings& s);

// "every hour and at logon", "every 4 hours", "once a day"
std::wstring DescribeCheckInterval(const AlertSettings& s);

// ---- What was last told (space-alerts.txt)

struct AlertState {
    std::wstring drive;    // "C:"
    uint32_t serial = 0;   // another volume under the same letter starts afresh
    bool low = false;      // under its limit at the last check (and not back above it since)
    int64_t notified = 0;  // Unix seconds of the last notification, 0 = none
};
std::vector<AlertState> ParseAlertState(const std::string& text);
std::string FormatAlertState(const std::vector<AlertState>& states);
std::wstring AlertStatePath();  // util::DataDir()\space-alerts.txt (BYTOPSY_ALERT_STATE overrides it)

constexpr int64_t kRenotifySeconds = 24 * 3600;
// How far above the limit free space must get before the drive counts as "above it again": a few
// megabytes up and down around the limit don't make a new notification. min(1 GiB, limit / 20).
uint64_t RearmMargin(uint64_t limitBytes);

struct AlertStep {
    AlertState next;
    bool notify = false;   // show a notification now
    bool cleared = false;  // back above the limit: its notification can go from the Action Center
};
// One drive, one check. `prev` null: nothing known about it yet.
AlertStep StepAlert(const AlertState* prev, uint64_t freeBytes, uint64_t limitBytes, int64_t nowUnix);

// ---- A check

struct DriveSpace {
    std::wstring drive, label;  // "C:"
    uint32_t serial = 0;
    uint64_t total = 0, free = 0;
};

struct DriveCheck {
    DriveSpace d;
    bool watched = false;
    SpaceLimit rule;
    uint64_t limit = 0;  // bytes
    bool low = false;    // free < limit
    bool notify = false, cleared = false;
    int64_t lastNotified = 0;  // Unix seconds, before this check
};
// Every drive given, watched or not; `next` gets the state to keep (drives not given keep theirs).
std::vector<DriveCheck> EvaluateSpace(const AlertSettings& s, const std::vector<DriveSpace>& drives, const std::vector<AlertState>& prev,
                                      int64_t nowUnix, std::vector<AlertState>& next);

// The notification's text.
std::wstring AlertTitle(const DriveCheck& c);  // "C: is almost full"
std::wstring AlertBody(const DriveCheck& c);   // "8.2 GiB free of 476 GiB, below your limit of 10% (47.6 GiB)."

// Title, body, links and buttons ("Open Disk health", "Scan C:") of a drive's notification.
ToastContent AlertToast(const DriveCheck& c, const std::wstring& scheme);

// Links a notification opens (the bytopsy: protocol, see notify.h).
struct AlertLink {
    enum Kind { None, DiskHealth, Scan } kind = None;
    wchar_t drive = 0;  // 'C', or 0
};
// "bytopsy:disk-health", "bytopsy:disk-health?drive=C", "bytopsy:scan?drive=C" (also with
// "bytopsy://", a trailing "/", or the self-tests' bytopsy-test: scheme). Anything else is None.
AlertLink ParseAlertLink(const std::wstring& uri);
std::wstring MakeAlertLink(const std::wstring& scheme, AlertLink::Kind kind, wchar_t drive);

struct SpaceCheck {
    AlertSettings settings;
    std::vector<DriveCheck> drives;
    size_t shown = 0;          // notifications shown
    bool turnedOff = false;    // Windows Settings turned Bytopsy's notifications off
    std::wstring error;        // a notification couldn't be shown
};
// The fixed drives that are ready, with their free space.
std::vector<DriveSpace> FixedDriveSpace();
// The check for real: with `dryRun` (or alerts turned off) nothing is written or shown.
SpaceCheck RunSpaceCheck(bool dryRun);
// Exit code: 0 every watched drive is above its limit, 1 one is under it.
int SpaceCheckExitCode(const SpaceCheck& c);

// A sample notification (the windowed app's "Test" button): "Bytopsy test notification".
bool ShowTestNotification(std::wstring& err);

// ---- The task

constexpr const wchar_t* kAlertTask = L"Disk space check";
constexpr const wchar_t* kAlertTaskArgs = L"--check-space";
// The task's last result in words: "all drives above their limits", "a drive was under its limit", ...
std::wstring DescribeCheckResult(int64_t code);
