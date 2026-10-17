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
- [ ] **Restore point** before system-level cleanups (`SRSetRestorePoint`), optional.
- [x] **Handle-based deletion**: permanent deletes open everything relative to the parent's handle
      (`NtCreateFile` with `FILE_OPEN_REPARSE_POINT`) and delete via `FileDispositionInfoEx`; as
      administrator, a target whose path now leads elsewhere is refused. (Recycle Bin moves still go
      through the shell's `IFileOperation`.)

## 2. Cleaning coverage

- [ ] **Data-driven cleaners**: move cleanup categories out of C++ into rule files (JSON/TOML) shipped
      in the repo, so contributors add apps without touching code.
- [ ] **winapp2.ini import**: the community-maintained database with thousands of app cleaners
      (also used by BleachBit). Run every imported entry through the protection rules.
- [ ] **Developer junk finder** (like npkill): `node_modules`, `target`, `bin/obj`, `.gradle`, Unity
      `Library`, `__pycache__`, `.venv`, grouped by project with the project's last change date.
- [ ] More package caches: uv, Poetry, conda `pkgs`, Maven `.m2`, Bun, Deno, Composer, vcpkg, Conan.
- [ ] **AI cleanup that is actually safe**: Hugging Face cache by unused revision
      (`huggingface-cli scan-cache` logic), Ollama models with sizes and last use, LM Studio model
      list; delete single models, never the history.
- [ ] **WSL and Docker**: show `ext4.vhdx` / `docker_data.vhdx` sizes vs. used space and offer
      compaction (`wsl --shutdown` + `Optimize-VHD` / `diskpart compact vdisk`), `docker system prune`.
- [ ] Windows: component store cleanup (`DISM /StartComponentCleanup`), old drivers in DriverStore
      (`pnputil`), shadow storage limit (`vssadmin`), hibernation toggle, Compact OS, delivery
      optimization via its API, event logs, `$Windows.~BT`.
- [ ] Game launchers: Steam shader and download caches, Epic, Battle.net, EA app.
- [ ] Chat apps: Teams, Discord, Slack caches; WhatsApp/Telegram media folders (show, don't preselect).
- [ ] Browsers: cookies and history as opt-in categories, with a "keep me signed in to" list.

## 3. Maintenance beyond disk space

- [ ] **Startup manager**: Run keys, Startup folders, scheduled tasks and services, with impact
      estimates; disable (reversible) rather than delete.
- [ ] **Uninstaller with leftovers**: list apps from the Uninstall registry keys, run their uninstaller,
      then offer leftover folders (checked against the protection rules).
- [ ] **Disk health**: SMART/wear via `MSFT_PhysicalDisk` and `MSFT_StorageReliabilityCounter`, TRIM
      status, free-space trend from scan history.
- [ ] **Scheduled cleanup**: `winncdu-cli --clean temp,browser,dev --yes` plus a Task Scheduler entry
      from the GUI.
- [ ] Duplicate photos with a perceptual hash (not only byte-identical files).
- [ ] **No registry cleaner.** Say so in the README: it frees nothing measurable and breaks things.
      This stance is a selling point.

## 4. Open-source project health

- [ ] Add a LICENSE file (MIT or GPL-3.0); without one, nobody may legally reuse or contribute.
- [ ] GitHub Actions on `windows-latest`: build, `ctest`, upload release zips with SHA-256.
- [ ] Code signing for releases (SignPath offers free signing for open-source projects), so
      SmartScreen and antivirus don't flag a tool that deletes files.
- [ ] Distribution: winget, Scoop and Chocolatey manifests; portable mode (settings next to the exe).
- [ ] CONTRIBUTING.md with a short guide "add a protected location / add a cleaner", and issue
      templates for "false alarm" and "missing protected folder".
- [ ] Accessibility: the GUI is custom-drawn with Direct2D, so screen readers see nothing. Add a
      UI Automation provider for the list, buttons and dialogs.
- [ ] Localization: move strings to a table; start with the languages of the first contributors.
- [ ] Tests: analysis (search syntax, duplicates), export/import round trips, fuzzing the ncdu JSON
      importer.
- [ ] Privacy statement: no telemetry, no network access (except an optional update check).
