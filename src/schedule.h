#pragma once

// Scheduled cleanup: `bytopsy-cli --clean <ids> --yes` run by a per-user Task Scheduler task
// (\Bytopsy\Scheduled cleanup). The pure helpers (id lists, command lines, trigger text, exit codes)
// are testable without touching Task Scheduler; the Task* functions use Task Scheduler 2.0
// (ITaskService) and need COM initialized on the calling thread.

#include <cstdint>
#include <string>
#include <vector>

// ---- `--clean` on the command line

// "temp, Browser,,dev,temp" -> {"temp", "browser", "dev"}: lowercase, trimmed, no duplicates, in order.
std::vector<std::wstring> ParseIdList(const std::wstring& s);
std::wstring JoinIds(const std::vector<std::wstring>& ids);  // "temp,browser,dev"

// What the CLI is asked to do with `--clean`.
struct CleanCommand {
    bool clean = false;              // --clean was given
    std::vector<std::wstring> ids;   // as given (may contain "all-safe")
    bool yes = false;                // --yes / -y: really clean (otherwise a dry run)
    bool quiet = false;              // --quiet / -q: no output (the log file still gets a summary)
    bool si = false;                 // --si
};
// Parses the arguments after the program name. Returns false with `err` set on a usage error; when
// --clean isn't among them, returns true with out.clean = false (the caller handles the rest).
// With --clean only --yes, --quiet and --si may accompany it.
bool ParseCleanCommandLine(const std::vector<std::wstring>& args, CleanCommand& out, std::wstring& err);

// A cleanup category as far as choosing ids is concerned.
struct CleanerInfo {
    std::wstring id;
    bool safe = false;        // preselected in the windowed app
    bool external = false;    // Windows Disk Cleanup: never run from the command line
    bool needsAdmin = false;
    bool imported = false;    // from winapp2.ini
};
// Turns the requested ids into category ids. "all-safe" stands for the categories the windowed app
// preselects (safe, not external, not imported); anything else must be named. Unknown ids fail with
// `err` naming them and listing the valid ones.
bool ResolveCleanIds(const std::vector<std::wstring>& requested, const std::vector<CleanerInfo>& known,
                     std::vector<std::wstring>& out, std::wstring& err);

// Exit codes of `--clean`.
enum CleanExit : int {
    kCleanOk = 0,        // done (or dry run): nothing failed
    kCleanPartial = 1,   // cleaned, but some items were in use or a tool's command failed
    kCleanUsage = 2,     // bad arguments or unknown ids: nothing was cleaned
    kCleanSkipped = 3,   // nothing could run: every chosen category was skipped (e.g. needs admin)
};
int CleanExitCode(bool anyRan, uint64_t failedItems, size_t toolErrors);

// "--clean temp,browser --yes --quiet"
std::wstring CleanArguments(const std::vector<std::wstring>& ids);
// The ids back from such arguments (false if they aren't a scheduled cleanup's).
bool ParseCleanArguments(const std::wstring& args, std::vector<std::wstring>& ids);

// ---- The schedule

enum class ScheduleFreq { Daily, Weekly, Monthly, Logon };

struct ScheduleSpec {
    ScheduleFreq freq = ScheduleFreq::Weekly;
    int hour = 12, minute = 0;  // Daily / Weekly / Monthly
    int weekday = 0;            // Weekly: 0 = Sunday .. 6 = Saturday
    int monthDay = 1;           // Monthly: 1..28 (every month has it)
    std::vector<std::wstring> ids;
};

// "Daily at 03:00", "Weekly on Sunday at 12:00", "Monthly on day 1 at 09:30", "At logon"
std::wstring DescribeSchedule(const ScheduleSpec& s);
const wchar_t* WeekdayName(int d);  // 0 = Sunday
// The trigger's first start, local time "YYYY-MM-DDTHH:MM:SS": the next matching moment after
// `now` (given as local year, month, day, hour, minute; dayOfWeek 0 = Sunday). Task Scheduler
// repeats from there.
std::wstring ScheduleStartBoundary(const ScheduleSpec& s, int year, int month, int day, int dayOfWeek, int hour, int minute);
// Reads "YYYY-MM-DDTHH:MM[:SS]..." back into hour and minute.
bool ParseBoundaryTime(const std::wstring& boundary, int& hour, int& minute);
// Task Scheduler's bit masks.
int WeekdayFromMask(unsigned mask);    // DaysOfWeek: bit 0 = Sunday; -1 if none
int MonthDayFromMask(unsigned mask);   // DaysOfMonth: bit 0 = day 1; -1 if none
// A task's last result in words: "not run yet", "running", "OK", "some items were in use", ...
std::wstring DescribeTaskResult(int64_t code);

// ---- Task Scheduler (per user, no administrator rights)

constexpr const wchar_t* kScheduleFolder = L"\\Bytopsy";
constexpr const wchar_t* kScheduleTask = L"Scheduled cleanup";
// kScheduleFolder, or for tests BYTOPSY_TASK_FOLDER when it names a \Bytopsy-test... folder.
std::wstring ScheduleFolder();

struct TaskInfo {
    bool exists = false;
    bool enabled = false;
    std::wstring path, arguments;  // the first action
    ScheduleSpec spec;             // from the trigger and the arguments
    bool specKnown = false;        // the trigger is one Bytopsy writes
    int64_t nextRun = 0, lastRun = 0;  // FILETIME ticks (UTC), 0 = none
    int64_t lastResult = 0;
};

// Creates or replaces the task: runs `exe args` as the current user, only while logged on, without
// administrator rights; starts as soon as possible after a missed start, never on battery, stops
// after 2 hours. `description` goes into the task's registration info.
bool TaskSave(const std::wstring& folder, const std::wstring& name, const ScheduleSpec& when, const std::wstring& exe,
              const std::wstring& args, const std::wstring& description, std::wstring& err);
// exists = false (and true returned) when there is no such task.
bool TaskQuery(const std::wstring& folder, const std::wstring& name, TaskInfo& info, std::wstring& err);
// Deletes that one task, then the folder if nothing else is in it. A missing task is fine.
bool TaskRemove(const std::wstring& folder, const std::wstring& name, std::wstring& err);

// %LOCALAPPDATA%\Bytopsy\cleanup.log (next to the journal), where command-line cleanups and
// schedule changes leave a line each.
std::wstring CleanLogFile();
void CleanLogAppend(const std::wstring& line);  // prefixed with the local time
