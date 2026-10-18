# WinNCDU roadmap

Where WinNCDU can go from "disk usage analyzer with cleanup" to a PC maintenance tool people trust.
The guiding rule: **every action is explained, reversible where possible, and never touches data
the user can't get back without saying so.** That is the gap CCleaner-style tools leave open.

## 1. Safety and trust (do these first)

- [x] Protected locations: system folders blocked; AI sessions, keys, wallets, mail, VMs, synced
      folders and Git repositories need an extra confirmation (`src/protect.cpp`).
- [x] **Shield badges**: protected rows and treemap tiles are marked after a scan, so people see the
      warning before they select anything (`ProtectBadges` in `src/protect.cpp`).
- [x] **Deletion journal**: every delete and cleanup goes to `%LOCALAPPDATA%\WinNCDU\journal.log`
      (time, path, size, Recycle Bin or permanent); "Recently deleted" view with *Restore* for items
      still in the Recycle Bin, and `winncdu-cli --journal` (`src/journal.cpp`).
- [x] **Dry run / preview**: Cleanup's *Review* lists exactly what each category deletes, with
      per-item opt-out; cleaning deletes only that list.
- [x] **Who is locking this?** The Restart Manager (`RmGetList`) names the app holding files ("Close
      Google Chrome, then try again") for failed deletes and skipped Cleanup items (`AppsUsing` in
      `src/fsops.cpp`).
- [x] **Restore point** before system-level cleanups (`SRSetRestorePoint`), optional: offered as
      administrator when Windows temp / update categories are chosen; a failure cancels the cleanup.
      (Not yet exercised on a real elevated run.)
- [x] **Handle-based deletion**: permanent deletes open everything relative to the parent's handle
      (`NtCreateFile` with `FILE_OPEN_REPARSE_POINT`) and delete via `FileDispositionInfoEx`; as
      administrator, a target whose path now leads elsewhere is refused. (Recycle Bin moves still go
      through the shell's `IFileOperation`.)

## 2. Cleaning coverage

- [x] **Data-driven cleaners**: move cleanup categories out of C++ into rule files (JSON/TOML) shipped
      in the repo, so contributors add apps without touching code. *Done: `res/cleaners.ini` (INI,
      embedded in the exe) plus a user `cleaners.ini` that adds, replaces or disables categories;
      protected items are left out of every category.*
- [x] **winapp2.ini import**: the community-maintained database with thousands of app cleaners
      (also used by BleachBit). Run every imported entry through the protection rules. *Done:
      menu → Import winapp2.ini; file rules only, privacy entries and over-broad rules skipped,
      stricter protection threshold, overlap with built-in categories removed.*
- [x] **Developer junk finder** (like npkill): `node_modules`, `target`, `bin/obj`, `.gradle`, Unity
      `Library`, `__pycache__`, `.venv`, grouped by project with the project's last change date.
      *Done: "Developer junk" view (`src/devjunk.cpp`), ~20 ecosystems, every folder confirmed by
      its project's marker file; app and system folders never searched.*
- [x] More package caches: uv, Poetry, conda `pkgs`, Maven `.m2`, Bun, Deno, Composer, vcpkg, Conan.
      *Done: in `res/cleaners.ini` (dev, jvm, conda, conan); new `kind = command` runs a tool's own
      clean command (Conan), reusable for Docker and DISM.*
- [x] **AI cleanup that is actually safe**: Hugging Face cache by unused revision
      (`huggingface-cli scan-cache` logic), Ollama models with sizes and last use, LM Studio model
      list; delete single models, never the history.
      *Done: AI models view (no scan needed) and `winncdu-cli --ai-models`; deletes stay inside the
      model folders, and the journal keeps the download command.*
- [x] **WSL and Docker**: show `ext4.vhdx` / `docker_data.vhdx` sizes vs. used space and offer
      compaction (`wsl --shutdown` + `Optimize-VHD` / `diskpart compact vdisk`), `docker system prune`.
      *Done: WSL & Docker view and `winncdu-cli --wsl`. Compaction uses diskpart (works on Home, no
      Hyper-V module needed). Instead of `docker system prune`, only the build cache and unused images
      are pruned: stopped containers and volumes can hold data, so they are shown, not offered.*
- [x] Windows: component store cleanup (`DISM /StartComponentCleanup`), old drivers in DriverStore
      (`pnputil`), shadow storage limit (`vssadmin`), hibernation toggle, Compact OS, delivery
      optimization via its API, event logs, `$Windows.~BT`.
      *Done: Windows view and `winncdu-cli --windows`; Delivery Optimization and `$Windows.~BT` /
      `~WS` are Cleanup categories. The Security event log is never cleared.*
- [x] **Games**: every installed game across Steam, Epic, GOG, EA app, Ubisoft Connect, Battle.net
      and Xbox, with its size and when it was last played, uninstalled through its own launcher
      (so saves in the cloud and the launcher's library stay right); plus the launchers' shader
      and download caches.
      *Done: Games view and `winncdu-cli --games`, with leftovers of uninstalled games; a "Game
      launcher caches" Cleanup category. Only Steam could be checked on a real PC so far.*
- [x] Chat apps: Teams, Discord, Slack caches; WhatsApp/Telegram media folders (show, don't preselect).
      *Done: "Chat app caches" (new and classic Teams, Discord, Slack incl. Store versions; web caches
      only) and "WhatsApp & Telegram media" (received files and Telegram's media cache, never
      preselected; WhatsApp's `transfers` folder lowered to Caution so Cleanup may list it).*
- [x] Browsers: cookies and history as opt-in categories, with a "keep me signed in to" list.
      *Done: "Browser cookies" and "Browser history" (`kind = cookies` / `history`, `src/gui/browserdata.cpp`)
      edit the browsers' SQLite databases with a vendored SQLite: cookies of listed sites stay (the
      list starts with the most visited sites), Firefox bookmarks stay; open databases are skipped
      with the browser named. Checked on fixture profiles only, not yet on real browser data.*

## 3. Maintenance beyond disk space

- [x] **Startup manager**: Run keys, Startup folders, scheduled tasks and services, with impact
      estimates; disable (reversible) rather than delete.
      *Done: "Startup" view and `--startup` (`src/startup.cpp`); off = StartupApproved (as Task
      Manager), Task Scheduler's switch, or Manual start for services (previous start type kept in
      the journal); impact measured from the Diagnostics-Performance log when readable, else an
      estimate from program size and type, else "unknown"; Microsoft's services never changed.*
- [x] **Uninstaller with leftovers**: list apps from the Uninstall registry keys, run their uninstaller,
      then offer leftover folders (checked against the protection rules).
      *Done: Apps view and `winncdu-cli --apps` (`src/apps.cpp`, shared with Games). The app's own
      uninstaller runs; WinNCDU waits for its entry to go, then offers its former install folder and
      folders named after it, never ones another installed app may use, never protected data, never
      the registry. MSIX/Store apps are not listed. No real uninstall has been run in testing.*
- [x] **Disk health**: SMART/wear via `MSFT_PhysicalDisk` and `MSFT_StorageReliabilityCounter`, TRIM
      status, free-space trend from scan history.
      *Done: Disk health view and `winncdu-cli --disks` (`src/diskhealth.cpp`), read-only. NVMe
      drives' own health log fills in wear and temperature without administrator rights; the trend
      uses free space noted on each view load and scan (`freespace.log`), else earlier scans of the
      drive. The administrator path of the reliability counters has not been run on a real PC yet.*
- [x] **Scheduled cleanup**: `winncdu-cli --clean temp,browser,dev --yes` plus a Task Scheduler entry
      from the GUI. *Done: `--clean` (dry run unless `--yes`, `all-safe`, exit codes, cleanup.log)
      runs the GUI's own clean-and-journal path; Cleanup → Schedule… creates a per-user task
      (`src/schedule.cpp`, ITaskService): not elevated, not on battery, missed runs caught up.*
- [x] Duplicate photos with a perceptual hash (not only byte-identical files).
      *Done: "Similar photos" tab of Duplicates (`src/phash.cpp`): WIC decode at reduced size with
      EXIF orientation, dHash + pHash, exact multi-index lookup (fast for tens of thousands of
      photos), groups built around a suggested keeper, previews, hash cache; `--similar-photos`.*
- [x] **No registry cleaner.** Say so in the README: it frees nothing measurable and breaks things.
      This stance is a selling point.
      *Done: "No registry cleaner" section in the README, listing the only registry writes WinNCDU
      makes (the Explorer menu entry, startup entries), both on request and undoable.*

## 4. Open-source project health

- [ ] Add a LICENSE file (MIT or GPL-3.0); without one, nobody may legally reuse or contribute.
- [x] GitHub Actions on `windows-latest`: build, `ctest`, upload release zips with SHA-256.
      *Done: `.github/workflows/build.yml`; tags `v*` must match `src/version.h` and create a
      release with the zip and its `.sha256` (docs/RELEASING.md). Not yet run on GitHub: the tests
      were checked locally with `%TEMP%` set to an 8.3 short path like the runners'.*
- [ ] Code signing for releases (SignPath offers free signing for open-source projects), so
      SmartScreen and antivirus don't flag a tool that deletes files.
      *Prepared, not done: the release workflow signs through SignPath once the
      `SIGNPATH_ORGANIZATION_ID` variable and `SIGNPATH_API_TOKEN` secret are set (docs/RELEASING.md,
      `packaging/signpath/artifact-configuration.xml`). Left: a LICENSE, applying to SignPath
      Foundation, and the SignPath project setup. Releases are unsigned until then.*
- [x] Distribution: winget, Scoop and Chocolatey manifests; portable mode (settings next to the exe).
      *Done: portable mode (a `winncdu.portable` file next to the exe puts all data in
      `WinNCDU-data` beside it; one place decides, `util::DataDir()`). Manifest templates in
      `packaging/`, filled per release by `packaging/update-manifests.ps1` (docs/RELEASING.md).
      Left: choosing the license for the manifests and submitting them to winget-pkgs, a Scoop
      bucket and Chocolatey.*
- [x] CONTRIBUTING.md with a short guide "add a protected location / add a cleaner", and issue
      templates for "false alarm" and "missing protected folder".
      *Done: CONTRIBUTING.md (build, self-tests, both guides) and issue forms in
      `.github/ISSUE_TEMPLATE` (false alarm, missing protected folder, bug report).*
- [x] Accessibility: the GUI is custom-drawn with Direct2D, so screen readers see nothing. Add a
      UI Automation provider for the list, buttons and dialogs.
      *Done: a UI Automation provider (`src/gui/uia.cpp`, `uia_app.cpp`, `accmodel.cpp`). Each
      frame, the click regions, the text drawn and the checkboxes drawn become an element tree:
      toolbar, navigation list, the view (its list with headers and rows), status bar, and dialogs
      and the progress window as dialog windows. Invoke, Toggle, SelectionItem, Selection,
      ExpandCollapse and ScrollItem run the same code a click runs; focus, structure, property,
      window-opened and notification (toasts) events are raised; elements no longer on screen fail
      with UIA_E_ELEMENTNOTAVAILABLE. Dialogs gained Tab / arrow-key focus with a focus ring.
      Verified with a native IUIAutomation client and screenshots: tree dumps of Home, a scanned
      %TEMP% fixture, Cleanup, Startup, AI models, Apps, a running scan and the delete, schedule and
      About dialogs; switching views, selecting, multi-selecting and expanding rows, toggling a
      Cleanup checkbox and the delete dialog's checkbox, choosing a schedule option, cancelling
      dialogs; focus, selection and expand events while arrowing. `tests/uia_test.cpp` covers
      identity, reading order, naming and patterns. Not verified: Narrator, NVDA or JAWS
      themselves (nobody can listen here). Not done: rows off screen are not exposed (no
      virtualized list), the treemap is one image.*
- [x] Localization: move strings to a table; start with the languages of the first contributors.
      *Done: the mechanism, not a translation; none ships yet (the languages wait for their first
      contributors). `src/i18n.cpp`: `Tr`, `TrF` with numbered `{0}` values, `TrN` plurals with a
      per-language rule (one, zero-one, none, East Slavic, Polish, Czech, Romanian, Lithuanian,
      Slovenian, Arabic), English as the key and the fallback. Every text of the windowed app goes
      through it, including cleaner names and descriptions, dialogs, toasts and accessibility
      names; the journal, logs, paths, command lines and the terminal app stay English. Languages
      are UTF-8 `res/lang/<code>.lang` files built into the exe, or loaded from a `lang` folder next
      to it; Windows' display language picks one, `⋯` > *Language* overrides it. Numbers and dates
      follow the Windows region settings. A pseudo-language (menu, or `WINNCDU_LANG=pseudo`)
      lengthens every text by about 40%; with it, Home, Folders, Largest files, File types,
      Duplicates, Similar photos, Old files, Developer junk, Cleanup, AI models, WSL, Windows, Games,
      Disk health, Startup, Apps, History, Recently deleted and the delete dialog were checked in
      screenshots, and the layouts were made to measure their texts (buttons, chips, labels,
      dialog columns) rather than assume English widths. `tools/extract_strings.py` writes
      `res/lang/template.lang` (about 1400 texts), starts and updates translations, and its
      `--check` runs in CI. `tests/i18n_test.cpp`. Not done: data already on screen (startup impact
      notes, AI model details) changes language only when its view is refreshed.*
- [x] Tests: analysis (search syntax, duplicates), export/import round trips, fuzzing the ncdu JSON
      importer.
      *Done: `tests/analysis_test.cpp`, `tests/export_test.cpp`, `tests/importfuzz_test.cpp` (5000
      mutated files, fixed seed). Fixed what they found: deep nesting crashed the importer (now
      capped at 16384 levels), `>`/`<` were inclusive, a sub-folder export lost its full path.*
- [x] Privacy statement: no telemetry, no network access (except an optional update check).
      *Done: "Privacy" in the README, checked against the source (no networking APIs or libraries).
      There is no update check, so the statement has no exception.*
