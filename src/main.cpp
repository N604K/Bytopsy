#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include "export.h"
#include "fsops.h"
#include "aimodels.h"
#include "diskhealth.h"
#include "driverupdates.h"
#include "runtool.h"
#include "apps.h"
#include "games.h"
#include "appupdates.h"
#include "gui/cleanup.h"
#include "journal.h"
#include "phash.h"
#include "protect.h"
#include "schedule.h"
#include "scanner.h"
#include "startup.h"
#include "ui.h"
#include "update.h"
#include "util.h"
#include "vdisks.h"
#include "version.h"
#include "wintools.h"
#include "gui/history.h"

namespace {

void PrintUsage() {
    printf(
        "Bytopsy " BYTOPSY_VERSION_A " - NCurses Disk Usage style analyzer for Windows\n"
        "\n"
        "Usage: bytopsy-cli [options] [path]\n"
        "\n"
        "  path                  Folder or drive to scan (no path: choose from a list)\n"
        "\n"
        "Options:\n"
        "  -o FILE               Scan, write an ncdu-compatible JSON export to FILE and exit\n"
        "  --csv FILE            Scan, write a CSV report to FILE and exit\n"
        "  -f FILE               Browse a previously exported JSON file (read-only)\n"
        "  --exclude PATTERN     Skip files/folders matching PATTERN (wildcards; repeatable).\n"
        "                        Patterns containing '\\' are matched against the full path.\n"
        "  -X, --exclude-from F  Read exclude patterns from file F (one per line)\n"
        "  -t, --threads N       Number of scanner threads (default: %u)\n"
        "  -H, --hardlinks       Detect NTFS hard links and count them once (slower)\n"
        "  --apparent            Show apparent sizes instead of disk usage\n"
        "  --si                  Use base-10 units (kB, MB) instead of KiB, MiB\n"
        "  --ascii               Use only ASCII characters for bars and borders\n"
        "  --no-mouse            Disable mouse support\n"
        "  --no-delete           Disable deleting files (safe browsing mode)\n"
        "  --protected           List the protected locations found on this PC and exit\n"
        "  --journal             List what Bytopsy deleted recently (and what is still in the\n"
        "                        Recycle Bin) and exit\n"
        "  --ai-models           List local AI models (Hugging Face, Ollama, LM Studio) with\n"
        "                        what each frees, and exit\n"
        "  --wsl                 List WSL and Docker virtual disks (size on the drive vs. used\n"
        "                        inside) and what Docker could prune, and exit\n"
        "  --windows             List space Windows keeps (old drivers, hibernation, System\n"
        "                        Restore, event logs) and exit\n"
        "  --games               List installed games of every launcher (size, last played)\n"
        "                        and leftovers of uninstalled ones, and exit\n"
        "  --driver-updates      Ask Windows Update which driver updates it offers for this PC\n"
        "                        (read-only), next to each device's installed driver, and the\n"
        "                        GPU vendor's and PC maker's own driver tools, and exit\n"
        "  --install-driver-updates IDS\n"
        "                        As administrator: create a System Restore point, then have\n"
        "                        Windows Update download and install exactly these updates\n"
        "                        (comma list of update ids from --driver-updates). Nothing is\n"
        "                        installed if the restore point fails. Used by the windowed app\n"
        "    --no-restore-point  ...without the restore point\n"
        "  --disks               List physical disks (health, wear, temperature, TRIM, drive\n"
        "                        letters) and each drive's used-space trend, and exit\n"
        "  --clean IDS           Cleanup categories (comma list of ids, e.g. temp,browser,dev,\n"
        "                        or all-safe = the preselected ones): shows what would be\n"
        "                        freed and exits. Unknown ids list the valid ones.\n"
        "    --yes               ...really clean (permanently; protected items never), journal\n"
        "                        it and append a line to cleanup.log. Exit code 0 = done,\n"
        "                        1 = some items in use, 2 = bad ids, 3 = all skipped (admin)\n"
        "    --quiet             ...print nothing (used by the scheduled task)\n"
        "  --startup             List what starts with Windows (Run keys, Startup folders,\n"
        "                        logon/boot tasks, services) with impact estimates, and exit\n"
        "  --apps                List installed apps (size, install date, version, publisher)\n"
        "                        from the Uninstall registry keys, and exit\n"
        "  --app-updates         List app updates winget knows of (winget upgrade: it only\n"
        "                        lists, nothing is updated) matched to the installed apps\n"
        "  --similar-photos[=strict|normal|loose]\n"
        "                        Scan path, list groups of photos that look alike (resized,\n"
        "                        re-saved, lightly edited copies) with the one to keep, and exit\n"
        "  --check-update        Ask bytopsy.com (or GitHub) for the latest version, print it\n"
        "                        next to this one, and exit; never installs anything. Exit\n"
        "                        code 0 = up to date, 2 = a newer version exists, 1 = no answer\n"
        "  -h, --help            Show this help\n"
        "  -V, --version         Show version\n"
        "\n"
        "Press ? inside the program for all key bindings.\n",
        std::clamp(std::thread::hardware_concurrency(), 2u, 8u));
}

std::wstring NormalizePattern(std::wstring p) {
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    return util::ToLower(p);
}

int ListProtected() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    RefreshProtection();
    auto all = ProtectedLocationsOnThisPC();
    std::wstring group;
    std::wstring out = L"Protected locations on this PC (system folders, AI assistant data, keys, wallets, synced folders).\n"
                       L"Bytopsy never deletes Blocked items and asks twice for Important ones.\n";
    for (auto& h : all) {
        if (h.group != group) {
            group = h.group;
            out += L"\n[" + group + L"]\n";
        }
        wchar_t lvl[16];
        swprintf(lvl, 16, L"%-10ls", ProtectLevelName(h.level));
        out += L"  " + std::wstring(lvl) + L" " + h.path + L"  (" + h.name + L")\n";
    }
    out += L"\nAlso anywhere: Git/SVN/Mercurial repositories, KeePass databases, wallet.dat, .pst, private keys.\n";
    out += L"Your own rules: " + ProtectRulesFile() + L"\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

int ListJournal() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    auto entries = JournalRead(50);
    auto bin = ListRecycleBin();
    std::wstring out = L"Recently deleted by Bytopsy (newest first). Journal: " + JournalFile() + L"\n\n";
    if (entries.empty()) out += L"  Nothing yet.\n";
    for (auto& e : entries) {
        const wchar_t* state = L"";
        if (e.action == JournalAction::Recycle)
            state = FindRecycled(bin, e) ? L"in Recycle Bin" : (PathExists(e.path) ? L"restored" : L"bin emptied");
        wchar_t head[96];
        swprintf(head, 96, L"  %ls  %-8ls %10ls  %-15ls ", util::FormatTime(e.time).c_str(), JournalActionName(e.action),
                 util::FormatSizeShort(e.bytes).c_str(), state);
        out += head + e.path + (e.note.empty() ? L"" : L"  (" + e.note + L")") + L"\n";
    }
    out += L"\nRestore items that are still in the Recycle Bin in the windowed app: Recently deleted.\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

int ListAiModels() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    auto stores = FindAiModels();
    bool atime = LastAccessTracked();
    std::wstring out = L"Local AI models (Hugging Face, Ollama, LM Studio). \"Frees\" is what deleting that model alone frees.\n";
    if (stores.empty()) out += L"\n  None found.\n";
    for (auto& s : stores) {
        out += L"\n" + s.name + L"  " + s.folder + L"  (" + util::FormatSizeShort(s.bytes) + L")\n";
        for (auto& it : s.items) {
            wchar_t head[64];
            swprintf(head, 64, L"  %10ls  %ls  ", util::FormatSizeShort(it.bytes).c_str(), util::FormatTime(it.added).substr(0, 10).c_str());
            out += head + std::wstring(it.parent >= 0 ? L"  " : L"") + it.name + L"  (" + it.detail + L")" +
                   (atime && it.used ? L"  last used " + util::FormatTime(it.used).substr(0, 10) : L"") + L"\n";
        }
    }
    out += L"\nDelete single models in the windowed app: AI models.\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

int ListVDisks() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    auto disks = FindVDisks();
    std::wstring out = L"WSL and Docker virtual disks. They grow but never shrink by themselves: compacting gives the\n"
                       L"space that is free inside back to Windows.\n\n";
    if (disks.empty())
        out += L"  None found.\n";
    else
        out += L"     On drive  Used inside   Could free  Disk\n";
    for (auto& d : disks) {
        wchar_t head[96];
        swprintf(head, 96, L"  %11ls  %11ls  %11ls  ", util::FormatSizeShort(d.fileBytes).c_str(),
                 d.usedKnown ? util::FormatSizeShort(d.used).c_str() : L"?",
                 d.usedKnown ? util::FormatSizeShort(d.Reclaimable()).c_str() : L"?");
        out += head + d.name + (d.running ? L"  (running)" : L"") + L"  " + d.path + L"\n";
    }
    DockerInfo dk = QueryDocker();
    if (dk.cli) {
        out += L"\nDocker (inside its virtual disk):\n";
        if (!dk.running) out += L"  Not running" + (dk.error.empty() ? L"" : L": " + dk.error) + L"\n";
        for (auto& r : dk.rows) {
            wchar_t head[96];
            swprintf(head, 96, L"  %11ls  %11ls reclaimable  ", util::FormatSizeShort(r.size).c_str(),
                     util::FormatSizeShort(r.reclaimable).c_str());
            out += head + r.type + L"\n";
        }
    }
    out += L"\nCompact disks and prune Docker in the windowed app: WSL & Docker.\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

int ListWindowsTools() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    std::wstring out = L"Space Windows keeps for itself (read-only overview; change it in the windowed app: Windows).\n\n";
    auto drivers = ListDriverPackages();
    uint64_t oldBytes = 0;
    size_t old = 0;
    for (auto& d : drivers)
        if (d.old) {
            oldBytes += d.bytes;
            ++old;
        }
    out += L"  Driver packages      " + util::FormatCount(drivers.size()) + L" third-party, " + util::FormatCount(old) +
           L" old versions nothing uses (" + util::FormatSizeShort(oldBytes) + L")\n";
    for (auto& d : drivers)
        if (d.old)
            out += L"                         " + d.oemInf + L"  " + d.originalInf + L"  " + d.provider + L"  " + d.version + L"  " +
                   util::FormatSizeShort(d.bytes) + L"\n";
    Hibernation h = QueryHibernation();
    out += L"  Hibernation          " +
           (h.on ? std::wstring(h.reduced ? L"on (reduced, for Fast Startup)" : L"on") + L", hiberfil.sys " + util::FormatSizeShort(h.bytes)
                 : std::wstring(L"off")) +
           L"\n";
    out += L"  Compact OS           " + std::wstring(CompactOsOn() ? L"on" : L"off") + L"\n";
    EventLogs e = QueryEventLogs();
    out += L"  Event logs           " + (e.known ? util::FormatSizeShort(e.bytes) + L" in " + util::FormatCount(e.files) + L" logs" : L"unknown") + L"\n";
    std::wstring err;
    auto shadows = QueryShadowStorage(err);
    if (!err.empty()) out += L"  System Restore       " + err + L"\n";
    for (auto& s : shadows)
        out += L"  System Restore       " + s.forDrive + L" uses " + util::FormatSizeShort(s.used) + L" of " +
               (s.max == UINT64_MAX ? std::wstring(L"no limit") : util::FormatSizeShort(s.max)) + L"\n";
    out += L"  Component store      analyze it in the windowed app (DISM, needs administrator rights)\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

int ListGames() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    auto games = FindGames();
    std::wstring out = L"Installed games (Steam, Epic, GOG, EA app, Ubisoft Connect, Battle.net, Xbox), largest first.\n\n";
    if (games.empty()) out += L"  None found.\n";
    for (int leftovers = 0; leftovers < 2; ++leftovers) {
        bool header = false;
        for (auto& g : games) {
            if (g.leftover != (leftovers == 1)) continue;
            if (leftovers && !header) out += L"\nLeftovers of uninstalled games:\n";
            header = true;
            wchar_t head[64];
            swprintf(head, 64, L"  %10ls  %-16ls ", util::FormatSizeShort(g.bytes).c_str(), g.launcher.c_str());
            out += head + g.name +
                   (g.leftover ? L"  " + g.folder
                               : L"  (last played " +
                                     (g.lastPlayed ? util::FormatTime(g.lastPlayed).substr(0, 10) + (g.lastPlayedGuess ? L", approx." : L"")
                                                   : std::wstring(L"unknown")) +
                                     L")") +
                   L"\n";
        }
    }
    out += L"\nUninstall through the launcher, or remove leftovers, in the windowed app: Games.\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

// --check-update: the windowed app's "Check for updates now", without installing.
int CheckUpdate() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    UpdateSources src = DefaultUpdateSources();
    std::wstring out = L"Installed:  Bytopsy " BYTOPSY_VERSION L"\n";
    int rc = 1;
    UpdateCheck c = CheckForUpdate(src, BYTOPSY_VERSION);
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    PackageManager pm = PackageManagerFor(std::wstring(exe, n));
    switch (c.status) {
        case UpdateStatus::UpToDate:
            out += L"Latest:     " + c.manifest.version + L" - up to date\n";
            rc = 0;
            break;
        case UpdateStatus::Available:
            out += L"Latest:     " + c.manifest.version + (c.manifest.released.empty() ? L"" : L" (released " + c.manifest.released + L")") + L"\n";
            if (c.required) out += L"            Important: versions before " + c.manifest.minimumVersion + L" should be updated.\n";
            if (!c.manifest.notesUrl.empty()) out += L"What's new: " + c.manifest.notesUrl + L"\n";
            if (pm != PackageManager::None)
                out += std::wstring(L"Installed with ") + PackageManagerName(pm) + L"; update with:  " + PackageManagerCommand(pm) + L"\n";
            else
                out += L"Update from the windowed app (menu > Check for updates now), or download:\n  " + c.asset.url + L"\n";
            rc = 2;
            break;
        case UpdateStatus::NoKey: out += L"This build has no update key, so it can't check for updates.\n"; break;
        case UpdateStatus::Failed: out += L"Couldn't check for updates: " + c.error + L"\n"; break;
    }
    if (!c.source.empty()) out += L"(from " + c.source + L")\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), rc == 1 ? stderr : stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return rc;
}

int ListDisks() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    DiskHealthReport rep = QueryDiskHealth();
    std::wstring out = L"Physical disks (read-only: the disks are only queried, never written to).\n\n";
    if (!rep.error.empty()) out += L"  " + rep.error + L"\n";
    if (rep.disks.empty() && rep.error.empty()) out += L"  None found.\n";
    bool denied = false;
    auto num = [](int64_t n) { return util::FormatCount(static_cast<uint64_t>(n)); };
    for (auto& d : rep.disks) {
        DiskVerdict v = AssessDisk(d);
        out += L"  Disk " + std::to_wstring(d.number) + L"  " + d.model + L"  ·  " + MediaName(d) + L"  ·  " + util::FormatSizeShort(d.size) +
               L"  ·  " + HealthLevelName(v.level) + L"\n";
        std::wstring drives;
        for (size_t i = 0; i < d.drives.size(); ++i)
            drives += (i ? L", " : L"") + d.drives[i] + (d.fileSystems[i].empty() ? L"" : L" (" + d.fileSystems[i] + L")");
        out += L"      Drives       " + (drives.empty() ? std::wstring(L"none with a letter") : drives) + L"\n";
        std::wstring status;
        for (int s : d.opStatus) status += (status.empty() ? L"" : L", ") + OpStatusName(s);
        if (!status.empty()) out += L"      Status       " + status + L"\n";
        if (d.wear >= 0) out += L"      Wear         " + std::to_wstring(d.wear) + L"% of its rated endurance used\n";
        if (d.temperature > 0)
            out += L"      Temperature  " + std::to_wstring(d.temperature) + L" °C" +
                   (d.temperatureMax > 0 ? L" (rated for " + std::to_wstring(d.temperatureMax) + L" °C)" : L"") + L"\n";
        if (d.powerOnHours >= 0) out += L"      Powered on   " + num(d.powerOnHours) + L" hours\n";
        if (d.availableSpare >= 0)
            out += L"      Spare        " + std::to_wstring(d.availableSpare) + L"% (warns at " + std::to_wstring(d.spareThreshold) + L"%)\n";
        if (d.bytesWritten) out += L"      Written      " + util::FormatSizeShort(d.bytesWritten) + L" over its life\n";
        std::wstring errs;
        auto addErr = [&](int64_t n, const wchar_t* what) {
            if (n >= 0) errs += (errs.empty() ? L"" : L", ") + num(n) + what;
        };
        addErr(d.mediaErrors, L" media errors");
        addErr(d.readErrorsUncorrected, L" uncorrected read errors");
        addErr(d.writeErrorsUncorrected, L" uncorrected write errors");
        addErr(d.unsafeShutdowns, L" unsafe shutdowns");
        if (!errs.empty()) out += L"      Errors       " + errs + L"\n";
        if (d.Ssd()) {
            std::wstring trim;
            for (auto& fs : d.fileSystems) {
                int t = TrimFor(rep.trim, fs);
                if (t >= 0 && trim.find(fs) == std::wstring::npos) trim += (trim.empty() ? L"" : L", ") + fs + (t ? L" on" : L" OFF");
            }
            if (!trim.empty()) out += L"      TRIM         " + trim + L"\n";
        }
        if (!d.HasCounters() && d.countersDenied) denied = true;
        for (auto& p : v.problems) out += L"      !  " + p + L"\n";
        for (auto& n : v.notes) out += L"      i  " + n + L"\n";
        out += L"\n";
    }
    if (denied) out += L"Wear and temperature of some disks need administrator rights: run this from an elevated prompt.\n\n";
    out += L"TRIM (fsutil behavior query DisableDeleteNotify): NTFS " +
           std::wstring(rep.trim.ntfs < 0 ? L"unknown" : rep.trim.ntfs ? L"on" : L"off") + L", ReFS " +
           std::wstring(rep.trim.refs < 0 ? L"not set" : rep.trim.refs ? L"on" : L"off") + L"\n\n";

    out += L"Used space over time:\n";
    auto log = LoadFreeSpaceLog();
    auto snaps = ListSnapshots();
    int64_t now = util::NowFileTime();
    for (auto& d : ListDrives()) {
        if (d.type != DRIVE_FIXED || !d.ready) continue;
        std::vector<UsagePoint> scans;
        for (auto& s : snaps)
            if (s.root.size() >= 2 && s.root.size() <= 3 && s.root[1] == L':' && towupper(s.root[0]) == towupper(d.root[0]))
                scans.push_back({s.time, s.disk});
        DriveTrend t = BuildDriveTrend(d, VolumeSerial(d.root), log, scans, now);
        out += L"  " + t.drive + L"  " + util::FormatSizeShort(d.total - d.free) + L" of " + util::FormatSizeShort(d.total) + L" used  ·  " +
               t.text + (t.trend.ok ? L" (" + util::FormatCount(t.samples) + (t.fromScans ? L" scans)" : L" samples)") : L"") + L"\n";
    }
    out += L"\nThe windowed app notes each drive's free space when Disk health opens or a scan finishes.\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

int ListStartup() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    StartupList list = FindStartupEntries();
    std::wstring out = L"What starts with Windows (read-only; turn entries off or on in the windowed app: Startup).\n";
    const StartupKind kinds[] = {StartupKind::Run, StartupKind::RunOnce, StartupKind::Folder, StartupKind::Task, StartupKind::Service};
    const wchar_t* titles[] = {L"Run keys", L"Run once (next sign-in only)", L"Startup folders", L"Scheduled tasks (at log on / startup)",
                               L"Services (automatic start)"};
    size_t hiddenMs = 0;
    for (int ki = 0; ki < 5; ++ki) {
        StartupKind k = kinds[ki];
        bool header = false;
        for (const StartupEntry& e : list.entries) {
            if (e.kind != k) continue;
            // Microsoft's services and tasks are left out, as in the windowed app's default view.
            if (e.microsoft && (k == StartupKind::Service || k == StartupKind::Task)) {
                ++hiddenMs;
                continue;
            }
            if (!header) out += L"\n" + std::wstring(titles[ki]) + L":\n";
            header = true;
            wchar_t head[64];
            swprintf(head, 64, L"  %-4ls %-8ls ", e.enabled ? L"on" : L"off", StartupImpactName(e.impact));
            out += head + e.display + (e.publisher.empty() ? L"" : L"  (" + e.publisher + L")") + L"\n";
            out += L"                " + (e.command.empty() ? e.where : e.command) + L"\n";
            if (!e.restoreStart.empty())
                out += L"                set to Manual by Bytopsy; turning it on restores start type " + e.restoreStart + L"\n";
        }
    }
    if (list.entries.empty()) out += L"\n  Nothing found.\n";
    out += L"\nImpact: measured when Windows' boot performance log recorded a slow start, otherwise an estimate from the\n"
           L"program's size and type (Electron/Chromium apps, updaters, delayed-start services); \"unknown\" when the\n"
           L"program file isn't there. ";
    out += list.bootLogRead ? std::wstring(L"The boot performance log was read.\n") : list.bootLogNote + L", so these are estimates.\n";
    out += L"Not listed: " + util::FormatCount(hiddenMs) + L" Microsoft services and tasks, and " + util::FormatCount(list.hiddenTasks) +
           L" tasks in \\Microsoft\\Windows.\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

int ListApps() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    auto apps = FindInstalledApps();
    uint64_t total = 0;
    for (auto& a : apps) total += a.bytes;
    std::wstring out = L"Installed apps (Uninstall registry keys; system components, updates and patches left out), largest first.\n" +
                       util::FormatCount(apps.size()) + L" apps, " + util::FormatSizeShort(total) + L".\n\n";
    if (apps.empty()) out += L"  None found.\n";
    for (auto& a : apps) {
        wchar_t head[64];
        swprintf(head, 64, L"  %10ls%lc %-10ls  ", a.bytes ? util::FormatSizeShort(a.bytes).c_str() : L"?", a.measured ? L'*' : L' ',
                 a.e.installDate ? util::FormatTime(a.e.installDate).substr(0, 10).c_str() : L"");
        bool versionInName = a.e.displayVersion.empty() || a.e.displayName.find(a.e.displayVersion) != std::wstring::npos;
        out += head + a.e.displayName + (versionInName ? L"" : L"  " + a.e.displayVersion) +
               (a.e.publisher.empty() ? L"" : L"  (" + a.e.publisher + L")") + L"\n";
    }
    out += L"\n* measured folder size (the app registers none).\n"
           L"Uninstall through each app's own uninstaller, then review the folders it left behind, in the windowed app: Apps.\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

// --app-updates: `winget upgrade` (a read-only listing), matched to the Apps list.
int ListAppUpdates() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    std::wstring out;
    int rc = 0;
    std::wstring winget = FindWinget();
    if (winget.empty()) {
        out = L"winget isn't installed. It comes with App Installer from the Microsoft Store:\n  " + std::wstring(kAppInstallerStoreLink) + L"\n";
        rc = 1;
    } else {
        fputs("Asking winget for updates (winget contacts its sources) ...\n", stdout);
        fflush(stdout);
        WingetList l = ListWingetUpdates(winget);
        if (!l.ok) {
            out = L"winget failed: " + l.error + L"\n";
            rc = 1;
        } else {
            auto apps = FindInstalledApps(false);
            std::vector<int> m = MatchUpdatesToApps(l.updates, apps);
            out = L"\nUpdates winget knows of (" + winget + L"):\n\n";
            if (l.updates.empty()) out += L"  None: every app winget knows is up to date.\n";
            size_t unmatched = 0;
            for (size_t i = 0; i < l.updates.size(); ++i) {
                const WingetUpdate& u = l.updates[i];
                std::wstring name = m[i] >= 0 ? apps[m[i]].e.displayName : u.name;
                out += L"  " + name + L"  " + u.version + L" -> " + u.available + L"  [" + u.id + (u.source.empty() ? L"" : L", " + u.source) + L"]";
                if (m[i] < 0) {
                    out += L" +";
                    ++unmatched;
                }
                if (u.explicitOnly) out += L"  (pinned: updated only when chosen)";
                if (!u.Updatable()) out += L"  (id cut off: update it with winget in a terminal)";
                out += L"\n";
            }
            if (unmatched) out += L"\n+ not in the Apps list (a Store/MSIX app, or one Installed apps leaves out).\n";
            if (l.unknownCount)
                out += L"\n" + std::to_wstring(l.unknownCount) + L" more app(s) have versions winget can't tell; they aren't listed.\n";
            if (l.pinnedCount) out += std::to_wstring(l.pinnedCount) + L" app(s) are pinned in winget and aren't listed.\n";
            out += L"\nNothing was updated. Update apps, each through its own installer, in the windowed app: Apps > Updates.\n";
        }
    }
    std::string u8 = util::WideToUtf8(out);
    fwrite(u8.data(), 1, u8.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return rc;
}

int Fail(const std::wstring& msg) {
    fwprintf(stderr, L"bytopsy-cli: %s\n", msg.c_str());
    return 2;
}

void PrintUtf8(const std::wstring& s) {
    std::string u = util::WideToUtf8(s);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
}

std::wstring DateOnly(int64_t ft) { return ft ? util::FormatTime(ft).substr(0, 10) : L"unknown date"; }

const wchar_t* AgeWord(OfferAge a) {
    switch (a) {
        case OfferAge::Newer: return L"newer than installed";
        case OfferAge::Same: return L"same as installed";
        case OfferAge::Older: return L"OLDER than installed";
        default: return L"can't compare";
    }
}

// Read-only: asks Windows Update which driver updates it offers, and lists them next to the
// driver each device has now, plus the vendors' own driver tools.
int ListDriverUpdates() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    PrintUtf8(L"Asking Windows Update which driver updates it offers for this PC (read-only; can take a minute)...\n\n");
    DriverSearch s = SearchDriverUpdates();
    std::wstring out;
    if (!s.ok) {
        out += L"  Windows Update couldn't be asked: " + s.error + L"\n";
    } else if (s.updates.empty()) {
        out += L"  Windows Update offers no driver updates for this PC.\n";
    } else {
        out += L"Driver updates Windows Update offers (install them in the windowed app: Drivers):\n";
        for (auto& u : s.updates) {
            const DeviceDriver* d = u.device >= 0 ? &s.devices[u.device] : nullptr;
            out += L"\n  " + u.title + L"\n";
            out += L"      offered    " + (u.version.empty() ? std::wstring(L"version not given") : u.version) + L", " + DateOnly(u.date) + L", " +
                   u.provider + (u.driverClass.empty() ? L"" : L" (" + u.driverClass + L")") + L", " + util::FormatSizeShort(u.bytes) + L"\n";
            if (d)
                out += L"      installed  " + (d->version.empty() ? std::wstring(L"unknown version") : d->version) + L", " + DateOnly(d->date) + L", " +
                       d->provider + L"  on " + d->name + (d->present ? L"" : L" (not connected)") +
                       (u.devices > 1 ? L" and " + std::to_wstring(u.devices - 1) + L" more" : L"") + L"\n";
            else
                out += L"      installed  no device with hardware id " + u.hardwareId + L" found\n";
            std::wstring notes = AgeWord(u.age);
            if (u.display) notes += L"; graphics driver: the GPU vendor's own tool is recommended";
            if (u.reboot == 1) notes += L"; needs a restart";
            if (u.reboot == 2) notes += L"; may need a restart";
            if (u.optional) notes += L"; optional";
            if (u.hidden) notes += L"; hidden in Windows Update";
            if (!u.eulaAccepted) notes += L"; license terms not accepted yet";
            out += L"      note       " + notes + L"\n";
            out += L"      update id  " + u.updateId + (u.kbs.empty() ? L"" : L"  " + u.kbs[0]) + L"\n";
        }
    }
    DriverToolsReport tools = ReadDriverTools();
    out += L"\nDrivers from the vendors' own tools:\n";
    auto toolLine = [&](const DriverTool& t) {
        if (t.installed)
            return L"installed: " + t.name + (t.launch.empty() ? L"" : L"  (" + t.launch + L")");
        return (t.name.empty() ? std::wstring(L"support page") : t.name + L" not installed, get it from") + L" " + t.launch;
    };
    if (!tools.maker.name.empty()) {
        out += L"  PC maker    " + tools.maker.name + (tools.maker.model.empty() ? L"" : L", " + tools.maker.model) +
               (tools.maker.selfBuilt ? L" (motherboard: a self-built PC)" : L"") + L"\n";
        for (auto& t : tools.makerTools) out += L"              " + toolLine(t) + L"\n";
    }
    for (auto& g : tools.gpus) {
        const DeviceDriver& a = tools.devices[g.adapters[0]];
        out += L"  Graphics    " + a.name + L", driver " + a.version + L" (" + DateOnly(a.date) + L")\n";
        for (auto& t : g.tools) out += L"              " + toolLine(t) + L"\n";
    }
    PrintUtf8(out);
    SetConsoleOutputCP(oldCp);
    return s.ok ? 0 : 1;
}

// Elevated only (the windowed app starts it through UAC): a restore point, then Windows Update
// downloads and installs exactly these updates. Prints one RESULT line per update.
// Exit codes: 0 all installed, 1 some failed or skipped, 2 bad arguments, 3 not elevated,
// 4 no restore point (nothing installed), 5 Windows Update couldn't be asked.
int InstallDriverUpdatesCli(const std::wstring& idList, bool restorePoint) {
    std::vector<std::wstring> ids;
    if (!ParseUpdateIdList(idList, ids)) return Fail(L"--install-driver-updates needs update ids (GUIDs, comma-separated; see --driver-updates)");
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    auto done = [&](int code) {
        SetConsoleOutputCP(oldCp);
        return code;
    };
    if (!RunningElevated()) {
        PrintUtf8(L"Installing driver updates needs administrator rights: run this from an elevated terminal.\n");
        return done(3);
    }
    if (restorePoint) {
        PrintUtf8(L"Creating a System Restore point...\n");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring err;
        bool ok = CreateRestorePoint(L"Bytopsy driver updates", err);
        CoUninitialize();
        if (!ok) {
            PrintUtf8(L"RESTOREPOINT\tfailed\t" + err + L"\nNo restore point, so nothing was installed: " + err + L"\n");
            return done(4);
        }
    }
    DriverInstallReport rep = InstallDriverUpdates(ids, [](const std::wstring& s) { PrintUtf8(s + L"\n"); });
    if (!rep.ok) {
        PrintUtf8(L"Windows Update couldn't be asked: " + rep.error + L"\n");
        return done(5);
    }
    bool all = true;
    for (auto& r : rep.results) {
        all = all && r.status == InstallStatus::Installed;
        PrintUtf8(FormatInstallResultLine(r) + L"\n");
    }
    if (rep.rebootRequired) PrintUtf8(L"Restart Windows to finish installing.\n");
    return done(all ? 0 : 1);
}

std::atomic<bool> g_cleanCancel{false};

BOOL WINAPI OnCtrl(DWORD) {
    g_cleanCancel = true;  // stop after the current item
    return TRUE;
}

// --clean <ids> [--yes] [--quiet]: the windowed app's Cleanup from the command line (and from the
// scheduled task). Measures exactly like the app, deletes through the same code path (protection
// rules, per-category excludes, age limits, re-checks before each delete) and journals the same.
int RunClean(const CleanCommand& cmd) {
    if (cmd.si) util::g_si = true;
    // A scheduled run gets a console of its own: let it go, nothing is printed anyway.
    if (cmd.quiet) {
        DWORD procs[4];
        if (GetConsoleProcessList(procs, 4) == 1) FreeConsole();
    }
    UINT oldCp = GetConsoleOutputCP();
    if (!cmd.quiet) SetConsoleOutputCP(CP_UTF8);
    auto print = [&](const std::wstring& s) {
        if (cmd.quiet) return;
        std::string u = util::WideToUtf8(s);
        fwrite(u.data(), 1, u.size(), stdout);
        fflush(stdout);
    };
    auto done = [&](int code) {
        if (!cmd.quiet) SetConsoleOutputCP(oldCp);
        return code;
    };

    std::vector<std::wstring> ruleErrors;
    std::vector<CleanCategory> cats = BuildCleanupCategories(&ruleErrors);
    for (auto& e : ruleErrors) print(L"Note: " + e + L"\n");
    std::vector<CleanerInfo> infos;
    for (auto& c : cats) infos.push_back({c.id, c.safe, c.external, c.needsAdmin, c.imported});
    std::vector<std::wstring> ids;
    std::wstring err;
    if (!ResolveCleanIds(cmd.ids, infos, ids, err)) {
        if (cmd.yes) CleanLogAppend(L"--clean " + JoinIds(cmd.ids) + L": " + err);
        done(0);
        return Fail(err);
    }

    bool elevated = IsElevated();
    std::vector<CleanCategory*> run;
    std::vector<std::wstring> skipped;  // "id (reason)"
    std::wstring out = cmd.yes ? L"Cleaning up (deleted permanently; protected items are never touched).\n\n"
                               : L"Dry run: nothing is deleted. Add --yes to clean.\n\n";
    print(out);
    uint64_t total = 0;
    for (auto& id : ids) {
        CleanCategory& c = *std::find_if(cats.begin(), cats.end(), [&](const CleanCategory& x) { return x.id == id; });
        std::wstring why;
        if (c.external)
            why = L"removed with Windows Disk Cleanup, not from the command line";
        else if (c.needsAdmin && !elevated)
            why = L"needs administrator rights";
        if (why.empty()) {
            MeasureCategory(c, g_cleanCancel);
            if (c.toolMissing) why = c.command.substr(0, c.command.find(L' ')) + L" not found on PATH";
        }
        wchar_t head[64];
        if (!why.empty()) {
            skipped.push_back(c.id + L" (" + why + L")");
            swprintf(head, 64, L"  %10ls  %-14ls ", L"skipped", L"");
            print(head + c.name + L" [" + c.id + L"]: " + why + L"\n");
            continue;
        }
        run.push_back(&c);
        total += c.ToClean();
        std::wstring count = util::FormatCount(c.files) + (c.recycleBin ? L" items" : L" files");
        swprintf(head, 64, L"  %10ls  %-14ls ", util::FormatSizeShort(c.ToClean()).c_str(), count.c_str());
        print(head + c.name + L" [" + c.id + L"]" + (c.guarded ? L"  (" + util::FormatCount(c.guarded) + L" protected items left out)" : L"") +
              (c.command.empty() ? L"" : L"  (runs " + c.command + L")") + L"\n");
    }
    if (!cmd.yes) {
        print(L"\nWould free " + util::FormatSizeShort(total) + L". Run again with --yes to clean.\n");
        return done(kCleanOk);
    }

    SetConsoleCtrlHandler(OnCtrl, TRUE);
    uint64_t freed = 0, failed = 0;
    size_t toolErrors = 0;
    std::wstring parts;
    print(L"\n");
    for (CleanCategory* c : run) {
        if (g_cleanCancel) break;
        // Nothing to clean: the Recycle Bin isn't even touched.
        if (c->ToClean() == 0) continue;
        CleanResult r = CleanAndJournal(*c, g_cleanCancel, [](const std::wstring&) {}, L"command line");
        freed += r.freed;
        failed += r.failed;
        if (!r.error.empty()) ++toolErrors;
        std::wstring note;
        if (r.failed) note = L"  (" + util::FormatCount(r.failed) + L" items in use skipped" +
                             (r.lockedBy.empty() ? L"" : L", close " + AppNames(r.lockedBy)) + L")";
        if (!r.error.empty()) note += L"  (" + r.error + L")";
        wchar_t head[64];
        swprintf(head, 64, L"  freed %10ls  ", util::FormatSizeShort(r.freed).c_str());
        print(head + c->name + L" [" + c->id + L"]" + note + L"\n");
        parts += (parts.empty() ? L"" : L", ") + c->id + L" " + util::FormatSizeShort(r.freed) + (r.failed ? L" (" + util::FormatCount(r.failed) + L" in use)" : L"") +
                 (r.error.empty() ? L"" : L" (" + r.error + L")");
    }
    int code = g_cleanCancel ? kCleanPartial : CleanExitCode(!run.empty(), failed, toolErrors);
    print(L"\nFreed " + util::FormatSizeShort(freed) + L". See --journal for the record.\n");
    std::wstring line = L"clean " + JoinIds(ids) + L": freed " + util::FormatSizeShort(freed) + (parts.empty() ? L"" : L" (" + parts + L")");
    if (!skipped.empty()) line += L"; skipped " + JoinIds(skipped);
    if (g_cleanCancel) line += L"; cancelled";
    line += L"; exit " + std::to_wstring(code);
    CleanLogAppend(line);
    return done(code);
}

// Scans cfg.path with progress on stderr. Null (after a message) when the path can't be scanned.
std::unique_ptr<Node> HeadlessScan(const AppConfig& cfg) {
    std::wstring err;
    std::wstring path = NormalizeDirPath(cfg.path.empty() ? L"." : cfg.path, err);
    if (path.empty()) {
        Fail(L"cannot scan " + cfg.path + L": " + err);
        return nullptr;
    }
    auto root = std::make_unique<Node>();
    root->name = path;
    root->flags = NF_DIR;
    ReadNodeMeta(root.get(), path);
    ScanStats stats;
    std::atomic<bool> done{false};
    std::thread th([&] {
        ScanDirectory(root.get(), path, cfg.scan, stats);
        RecomputeTotals(root.get());
        done = true;
    });
    ULONGLONG start = GetTickCount64();
    while (!done) {
        Sleep(250);
        fwprintf(stderr, L"\rScanning: %ls files, %ls dirs, %ls   ", util::FormatCount(stats.files).c_str(),
                 util::FormatCount(stats.dirs).c_str(), util::FormatSizeShort(stats.bytes).c_str());
    }
    th.join();
    fwprintf(stderr, L"\rScanned %ls items (%ls) in %ls, %ls errors\n", util::FormatCount(root->items).c_str(),
             util::FormatSizeShort(root->disk).c_str(), util::FormatDuration((GetTickCount64() - start) / 1000.0).c_str(),
             util::FormatCount(stats.errors).c_str());
    return root;
}

int HeadlessExport(const AppConfig& cfg, const std::wstring& outJson, const std::wstring& outCsv) {
    auto root = HeadlessScan(cfg);
    if (!root) return 2;
    std::wstring err;
    std::atomic<uint64_t> count{0};
    if (!outJson.empty()) {
        if (!ExportNcduJson(root.get(), outJson, count, err)) return Fail(err);
        fwprintf(stderr, L"Wrote %ls\n", outJson.c_str());
    }
    if (!outCsv.empty()) {
        if (!ExportCsv(root.get(), outCsv, count, err)) return Fail(err);
        fwprintf(stderr, L"Wrote %ls\n", outCsv.c_str());
    }
    return 0;
}

// Read-only: images are decoded small to compare them; only the hash cache in the app's data folder is written.
int ListSimilarPhotos(const AppConfig& cfg, PhotoMatch match) {
    auto root = HeadlessScan(cfg);
    if (!root) return 2;
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    const uint64_t minSize = 100 << 10;
    PhotoHashCache cache(PhotoHashCache::DefaultFile());
    PhotoProgress prog;
    std::vector<PhotoGroup> groups;
    std::atomic<bool> done{false};
    std::thread th([&] {
        groups = FindSimilarPhotos(root.get(), minSize, match, cfg.scan.threads, prog, &cache);
        done = true;
    });
    ULONGLONG start = GetTickCount64();
    while (!done) {
        Sleep(250);
        fwprintf(stderr, L"\rReading images: %ls / %ls   ", util::FormatCount(prog.filesDone).c_str(), util::FormatCount(prog.filesTotal).c_str());
    }
    th.join();
    fwprintf(stderr, L"\rCompared %ls images in %ls                \n", util::FormatCount(prog.filesTotal).c_str(),
             util::FormatDuration((GetTickCount64() - start) / 1000.0).c_str());
    uint64_t waste = 0;
    for (auto& g : groups) waste += g.Reclaimable();
    std::wstring out = L"Similar photos in " + root->name + L" (" + PhotoMatchName(match) + L" match, images of " +
                       util::FormatSizeShort(minSize) + L" or more). Read-only: nothing was changed.\n" + util::FormatCount(prog.filesTotal) +
                       L" images compared";
    if (prog.failed) out += L" (" + util::FormatCount(prog.failed) + L" unreadable)";
    if (prog.cached) out += L", " + util::FormatCount(prog.cached) + L" remembered from an earlier search";
    out += L"; " + util::FormatCount(groups.size()) + L" groups, " + util::FormatSizeShort(waste) + L" in the photos not suggested to keep.\n";
    if (groups.empty()) out += L"\n  No similar photos found.\n";
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        auto& g = groups[gi];
        out += L"\n[" + util::FormatCount(gi + 1) + L"] " + util::FormatCount(g.photos.size()) +
               (g.maxDist == 0 ? L" photos that look the same" : L" similar photos") + L", " + util::FormatSizeShort(g.Reclaimable()) +
               L" in the others\n";
        for (size_t i = 0; i < g.photos.size(); ++i) {
            auto& p = g.photos[i];
            wchar_t head[128];
            swprintf(head, 128, L"  %-5ls %6u x %-6u %10ls  %ls  ", i == 0 ? L"keep" : L"", p.hash.width, p.hash.height,
                     util::FormatSizeShort(p.node->size).c_str(), util::FormatTime(p.node->mtime).c_str());
            out += head + NodePath(p.node) + L"\n";
        }
    }
    out += L"\nReview and delete (to the Recycle Bin) in the windowed app: Duplicates, Similar photos.\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    AppConfig cfg;
    cfg.scan.threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 8u));
    std::wstring outJson, outCsv;
    bool similar = false;
    PhotoMatch match = PhotoMatch::Normal;

    {
        std::vector<std::wstring> args(argv + 1, argv + argc);
        CleanCommand clean;
        std::wstring err;
        if (!ParseCleanCommandLine(args, clean, err)) return Fail(err);
        if (clean.clean) return RunClean(clean);
    }

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](std::wstring& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::wstring v;
        if (a == L"-h" || a == L"--help" || a == L"/?") {
            PrintUsage();
            return 0;
        } else if (a == L"-V" || a == L"--version") {
            printf("Bytopsy " BYTOPSY_VERSION_A "\n");
            return 0;
        } else if (a == L"-o") {
            if (!next(outJson)) return Fail(L"-o needs a file name");
        } else if (a == L"--csv") {
            if (!next(outCsv)) return Fail(L"--csv needs a file name");
        } else if (a == L"-f") {
            if (!next(cfg.importFile)) return Fail(L"-f needs a file name");
        } else if (a == L"--exclude") {
            if (!next(v)) return Fail(L"--exclude needs a pattern");
            cfg.scan.excludes.push_back(NormalizePattern(v));
        } else if (a.rfind(L"--exclude=", 0) == 0) {
            cfg.scan.excludes.push_back(NormalizePattern(a.substr(10)));
        } else if (a == L"-X" || a == L"--exclude-from") {
            if (!next(v)) return Fail(L"--exclude-from needs a file name");
            FILE* f = nullptr;
            if (_wfopen_s(&f, v.c_str(), L"rb") != 0 || !f) return Fail(L"cannot read " + v);
            std::string content;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
            fclose(f);
            std::wstring w = util::Utf8ToWide(content);
            size_t pos = 0;
            while (pos < w.size()) {
                size_t nl = w.find(L'\n', pos);
                std::wstring line = w.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
                while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
                if (!line.empty() && line[0] == 0xFEFF) line.erase(line.begin());
                if (!line.empty() && line[0] != L'#') cfg.scan.excludes.push_back(NormalizePattern(line));
                if (nl == std::wstring::npos) break;
                pos = nl + 1;
            }
        } else if (a == L"-t" || a == L"--threads") {
            if (!next(v)) return Fail(L"--threads needs a number");
            cfg.scan.threads = std::clamp(_wtoi(v.c_str()), 1, 64);
        } else if (a == L"-H" || a == L"--hardlinks") {
            cfg.scan.hardlinks = true;
        } else if (a == L"--apparent") {
            cfg.apparent = true;
        } else if (a == L"--si") {
            util::g_si = true;
        } else if (a == L"--ascii") {
            cfg.ascii = true;
        } else if (a == L"--no-mouse") {
            cfg.mouse = false;
        } else if (a == L"--no-delete") {
            cfg.noDelete = true;
        } else if (a == L"--protected") {
            return ListProtected();
        } else if (a == L"--journal") {
            return ListJournal();
        } else if (a == L"--ai-models") {
            return ListAiModels();
        } else if (a == L"--wsl") {
            return ListVDisks();
        } else if (a == L"--windows") {
            return ListWindowsTools();
        } else if (a == L"--games") {
            return ListGames();
        } else if (a == L"--driver-updates") {
            return ListDriverUpdates();
        } else if (a == L"--install-driver-updates") {
            if (!next(v)) return Fail(L"--install-driver-updates needs update ids");
            bool restorePoint = true;
            for (int k = 1; k < argc; ++k)
                if (std::wstring(argv[k]) == L"--no-restore-point") restorePoint = false;
            return InstallDriverUpdatesCli(v, restorePoint);
        } else if (a == L"--no-restore-point") {
            // only with --install-driver-updates
        } else if (a == L"--disks") {
            return ListDisks();
        } else if (a == L"--startup") {
            return ListStartup();
        } else if (a == L"--apps") {
            return ListApps();
        } else if (a == L"--app-updates") {
            return ListAppUpdates();
        } else if (a == L"--check-update") {
            return CheckUpdate();
        } else if (a == L"--similar-photos" || a.rfind(L"--similar-photos=", 0) == 0) {
            similar = true;
            std::wstring m = a.size() > 17 ? a.substr(17) : L"normal";
            if (m == L"strict") match = PhotoMatch::Strict;
            else if (m == L"loose") match = PhotoMatch::Loose;
            else if (m != L"normal") return Fail(L"--similar-photos takes strict, normal or loose");
        } else if (a == L"-x") {
            // ncdu compatibility: Bytopsy never crosses into other volumes (mount points are links).
        } else if (a.size() > 1 && a[0] == L'-') {
            return Fail(L"unknown option " + a + L" (see --help)");
        } else {
            cfg.path = a;
        }
    }

    if (similar) return ListSimilarPhotos(cfg, match);
    if (!outJson.empty() || !outCsv.empty()) return HeadlessExport(cfg, outJson, outCsv);
    return RunApp(cfg);
}
