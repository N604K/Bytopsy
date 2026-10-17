#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct CleanTarget {
    std::wstring path;         // may contain one or more '*' segments and %ENV% variables
    std::wstring filePattern;  // if set: only files in `path` matching this pattern (';' separates several)
    int minAgeHours = 0;       // skip entries modified more recently
    bool recurse = false;      // with filePattern: matching files in subfolders too
    bool removeSelf = false;   // the folder itself is the entry, not what is in it
};

// Files a category leaves alone (winapp2.ini ExcludeKey).
struct CleanExclude {
    std::wstring folder;   // may contain '*' segments and %ENV% variables
    std::wstring pattern;  // file names (';' separates several)
    bool tree = false;     // also in subfolders of `folder`
};

// One thing a category deletes: a top-level file or folder of a target, or one Recycle Bin item.
// The preview lists exactly these, and cleaning deletes exactly these (minus the kept ones).
struct CleanEntry {
    std::wstring path;              // Recycle Bin items: where the item was deleted from
    std::wstring binData, binInfo;  // Recycle Bin items: the $R and $I files
    uint64_t bytes = 0, files = 0;
    int64_t mtime = 0;              // Recycle Bin items: when it was deleted
    int target = -1;                // index into CleanCategory::targets
    bool dir = false;
    bool keep = false;  // unchecked in the preview: never deleted
};

struct CleanCategory {
    std::wstring id, name, desc;
    wchar_t icon = 0;
    std::vector<CleanTarget> targets;
    bool needsAdmin = false;
    bool recycleBin = false;
    bool external = false;  // handled by an external tool (Windows Disk Cleanup)
    // kind = command: measured from its paths, cleaned by running this (the tool's own clean
    // command, e.g. `conan remove "*" -c`), because deleting the files would corrupt its state.
    std::wstring command;
    bool safe = true;       // preselected
    bool enabled = true;    // "enabled = false" in cleaners.ini turns a built-in category off
    bool user = false;      // from the user's cleaners.ini
    bool imported = false;  // from an imported winapp2.ini (stricter protection, shown only when non-empty)
    std::vector<CleanExclude> excludes;
    // Imported categories: what the built-in and user categories already clean (with their age
    // limits), left to them so nothing is counted twice or cleaned sooner than they would.
    std::shared_ptr<const std::vector<CleanExclude>> coveredBy;
    // Registry keys ("HKCU\Software\App") or paths ('*' allowed); if any is given, the category
    // only applies when one of them exists (the app is installed).
    std::vector<std::wstring> detect;

    // Measurement results.
    uint64_t bytes = 0, files = 0;
    uint64_t guarded = 0;  // items left out because the protection rules cover them
    bool measured = false, denied = false, selected = false;
    bool present = true;  // detection found the app
    bool toolMissing = false;  // kind = command: the program isn't on PATH
    std::vector<CleanEntry> entries;  // largest first

    uint64_t KeptBytes() const;
    size_t KeptCount() const;
    uint64_t ToClean() const;  // what cleaning would free now
};

struct CleanResult {
    uint64_t freed = 0, files = 0, failed = 0;
    std::vector<std::wstring> lockedBy;  // apps that held skipped items open
    std::wstring error;                  // kind = command: why it didn't run or failed
};

// The full path of a command's program (first word, quotes allowed) found on PATH (.exe, .cmd,
// .bat, .com; the current folder is never searched), or empty.
std::wstring FindTool(const std::wstring& command);
// Runs a command without a window and waits (cancel stops it). Returns the exit code, or -1 with
// `err` set when it couldn't be started.
int RunCommand(const std::wstring& command, std::atomic<bool>& cancel, std::wstring& err);

// Categories come from res/cleaners.ini (built into the exe) merged with the user's
// %LOCALAPPDATA%\WinNCDU\cleaners.ini, then the imported winapp2.ini; problems go to `errors`.
std::vector<CleanCategory> BuildCleanupCategories(std::vector<std::wstring>* errors = nullptr);
std::vector<CleanCategory> ParseCleaners(const std::wstring& text, std::vector<std::wstring>& errors);
// The user's cleaners.ini; `create` writes a commented template if it doesn't exist yet.
std::wstring UserCleanersFile(bool create);

// winapp2.ini (https://github.com/MoscaDotTo/Winapp2): thousands of community app cleaners.
// Only file rules are used (no registry cleaning). Entries about personal data (cookies, history,
// passwords, sessions, settings, backups, saved games) are skipped, as are rules that would sweep a
// whole profile, drive or Windows folder; what remains still goes through the protection rules.
struct Winapp2Stats {
    size_t entries = 0;   // sections in the file
    size_t usable = 0;    // categories made from them
    size_t personal = 0;  // skipped: personal data
    size_t broad = 0;     // file rules skipped: too broad
    size_t regOnly = 0;   // skipped: registry only
};
std::vector<CleanCategory> ParseWinapp2(const std::wstring& text, Winapp2Stats& stats);
// %LOCALAPPDATA%\WinNCDU\winapp2.ini, where an import is kept.
std::wstring Winapp2File();
// Checks and copies a winapp2.ini into place.
bool ImportWinapp2(const std::wstring& source, Winapp2Stats& stats, std::wstring& err);
// Whether a registry key or a path (with '*') named by a detect rule exists.
bool DetectPresent(const std::wstring& what);
void MeasureCategory(CleanCategory& c, std::atomic<bool>& cancel);
// Deletes the measured entries that aren't kept, permanently. Entries that changed since they
// were measured and no longer qualify (too new now) are left alone.
CleanResult CleanCategoryNow(const CleanCategory& c, std::atomic<bool>& cancel,
                             const std::function<void(const std::wstring&)>& onItem);
std::vector<std::wstring> ExpandedPaths(const CleanCategory& c);
bool IsElevated();
// A System Restore point (needs administrator rights and System Protection on the system drive).
// Windows makes at most one per 24 hours by default; within that window it keeps the recent one.
bool CreateRestorePoint(const std::wstring& description, std::wstring& err);
