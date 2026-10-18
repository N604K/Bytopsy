#pragma once

// What starts with Windows: Run keys, the Startup folders, scheduled tasks that run at sign-in or
// boot, and automatic services from other publishers. Turning an entry off never deletes it:
// Run keys and Startup folder items are marked disabled in StartupApproved exactly as Task Manager
// does, tasks are disabled in Task Scheduler, and services are set to Manual start (the previous
// start type is kept in the journal so it can be set back). Every change is journaled.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "journal.h"

enum class StartupKind : uint8_t {
    Run,      // HKCU/HKLM ...\CurrentVersion\Run (and WOW6432Node)
    RunOnce,  // ...\RunOnce: runs at the next sign-in, then Windows removes it (listed, not changed)
    Folder,   // the user's or everyone's Startup folder
    Task,     // a scheduled task with an "at log on" or "at startup" trigger
    Service,  // a service that starts automatically
};
const wchar_t* StartupKindName(StartupKind k);  // "Run key", "Startup folder", ...

enum class StartupImpact : uint8_t { Unknown, Low, Medium, High };
const wchar_t* StartupImpactName(StartupImpact i);  // "unknown", "low", ...

struct StartupEntry {
    StartupKind kind = StartupKind::Run;
    bool machine = false;   // all users (HKLM, the common Startup folder, services): changing it needs administrator rights
    bool wow32 = false;     // HKLM\SOFTWARE\WOW6432Node\...\Run (StartupApproved\Run32)
    std::wstring name;      // registry value name, file name, task path ("\Folder\Task") or service name
    std::wstring display;   // the program's description, the service's display name, or `name`
    std::wstring where;     // the key, folder or task folder it is registered in
    std::wstring command;   // as registered (shortcut: target and arguments)
    std::wstring program;   // the file that actually runs (environment expanded, rundll32/script hosts looked through)
    bool programFound = false;
    uint64_t programBytes = 0;
    bool chromium = false;  // Electron/Chromium app (a browser engine ships next to it)
    std::wstring publisher; // CompanyName of the program's version info
    bool microsoft = false; // Microsoft's own (publisher, or an unlabelled file in the Windows folder)
    bool enabled = true;
    int64_t disabledAt = 0; // FILETIME ticks from StartupApproved, 0 = unknown
    bool canToggle = true;
    std::wstring cantToggle;  // why not
    std::wstring trigger;     // tasks: "at log on", "at startup"
    uint32_t startType = 0;   // services: SERVICE_AUTO_START (2) or SERVICE_DEMAND_START (3)
    bool delayed = false;     // services: "Automatic (Delayed Start)"
    std::wstring restoreStart;  // services WinNCDU set to Manual: "auto" or "delayed-auto" (from the journal)
    StartupImpact impact = StartupImpact::Unknown;
    bool measured = false;    // from Windows' boot performance log, not an estimate
    uint32_t measuredMs = 0;
    std::wstring impactWhy;
};

struct StartupList {
    std::vector<StartupEntry> entries;
    size_t hiddenTasks = 0;      // tasks in \Microsoft\Windows\ (not listed)
    bool bootLogRead = false;    // the Diagnostics-Performance log could be read
    std::wstring bootLogNote;    // why not, e.g. "needs administrator rights"
};

// Everything on this PC (read-only). Takes a second or two (tasks, services, version info).
StartupList FindStartupEntries();

// Turning an entry off/on asks for administrator rights (machine-wide Run keys and Startup folder
// items, services; tasks only if Task Scheduler refuses the change for this user).
bool StartupNeedsAdmin(const StartupEntry& e);

struct StartupChange {
    bool ok = false;
    bool declined = false;  // the UAC prompt was declined
    std::wstring error;
};
// Turns an entry off or on (reversible, never deletes it) and journals the change.
StartupChange SetStartupEnabled(const StartupEntry& e, bool enable);

// ---- Exposed for tests

// StartupApproved\{Run,Run32,StartupFolder} values: the first byte is even (02, 06) when enabled and
// odd (03, 07) when disabled, followed by 3 zero bytes and the FILETIME it was disabled. No value =
// enabled.
bool ApprovedEnabled(const uint8_t* data, size_t size, int64_t* disabledAt);
std::vector<uint8_t> MakeApproved(bool enabled, int64_t when);  // what Task Manager writes (12 bytes)
// "HKCU\Software\...\StartupApproved\Run" style key (no hive) for an entry, or "" (tasks, services, RunOnce).
std::wstring ApprovedKey(const StartupEntry& e);

std::wstring ExpandEnv(const std::wstring& s);
// The program a command line runs, without looking at the disk: quoted or unquoted paths (spaces
// before the extension are part of the path), rundll32's DLL, wscript/cscript's script. `args`
// gets the rest of the command line.
std::wstring ProgramFromCommand(const std::wstring& command, std::wstring* args = nullptr);
// A service's ImagePath as a Win32 path: "\SystemRoot\...", "\??\C:\...", "system32\..." forms.
std::wstring ServiceImagePath(const std::wstring& imagePath, const std::wstring& windowsDir);
// Squirrel launchers (Discord, Slack, ...): "Update.exe --processStart App.exe" -> "App.exe", or "".
std::wstring SquirrelTarget(const std::wstring& args);
bool IsMicrosoftPublisher(const std::wstring& company);

// Fills impact/impactWhy from what is known about the entry (measured first, else an estimate).
void EstimateImpact(StartupEntry& e);

// One event of Microsoft-Windows-Diagnostics-Performance/Operational: 101 = an application slowed
// the boot, 103 = a service did.
struct BootDelay {
    int eventId = 0;
    std::wstring name, fileName, friendlyName;
    uint32_t totalMs = 0, degradationMs = 0;
};
bool ParseBootEvent(const std::wstring& xml, BootDelay& out);

// Journal note of a service change: "... [startup service=NAME was=auto]" when turned off,
// "[startup service=NAME now=auto]" when turned back on.
std::wstring ServiceJournalNote(const std::wstring& service, bool turnedOff, const std::wstring& startWord);
bool ParseServiceJournalNote(const std::wstring& note, std::wstring& service, bool& turnedOff, std::wstring& startWord);
// Services whose newest journal record says WinNCDU set them to Manual: name (lowercase) -> the
// start type to restore ("auto" / "delayed-auto"). `journal` is newest first (JournalRead).
std::map<std::wstring, std::wstring> ServicesTurnedOff(const std::vector<JournalEntry>& journal);

// The command run as administrator for a change (reg / schtasks / sc), or "" if the entry's name
// can't be passed safely through cmd.exe (quotes or percent signs).
std::wstring StartupAdminCommand(const StartupEntry& e, bool enable, int64_t now);
