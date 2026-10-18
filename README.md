# WinNCDU

A fast, modern disk usage analyzer for Windows, inspired by [ncdu](https://dev.yorhel.nl/ncdu).
Native C++20 with Direct2D and no runtime dependencies: two self-contained `.exe` files.

- **`winncdu.exe`**: the windowed app, with a folder browser, treemap, cleanup, scan history and a dark/light theme.
- **`winncdu-cli.exe`**: the ncdu-style terminal app, for consoles, SSH sessions and scripting.

Both share the same scanner, so results, exports and imports are interchangeable.

## The windowed app (`winncdu.exe`)

Start it, pick a drive (or drop a folder onto the window) and see what takes up the space.

- **Folders**: an expandable tree sorted by size, with usage bars, share of the parent folder,
  item counts and modified times. Breadcrumbs, back/forward, multi-select (Ctrl/Shift-click).
- **Treemap** below the list: every file drawn to scale and colored by kind (video, images, audio,
  documents, archives, code, programs, system). Hover for details, click to select the file in the
  list, double-click to zoom into a folder. Drag the splitter to resize it, or hide it.
- **Find**: the search box understands `*.iso`, `>1G`, `<10M`, `older:1y`, `newer:7d`, `type:d`,
  `ext:mp4,mkv`, `empty`, and combinations such as `*.mp4 >500M older:1y`.
- **Largest files**, **File types** (with a category breakdown; double-click a type to list its
  files), **Old files** (filter by age and size) and **Empty folders**.
- **Duplicates**: compares size, then a quick hash, then the full SHA-256. "Select all but oldest"
  selects every redundant copy in one click.
- **Developer junk** (like npkill, for every ecosystem): build output and dependency folders your
  projects recreate on the next build or install, grouped by project with the date the project
  itself last changed (its own files, not the build output or `.git`). Covers `node_modules`,
  `.next`/`.nuxt`/`.svelte-kit`/`.angular`/`.parcel-cache`/`.turbo`, Rust and Maven `target`,
  .NET `bin`/`obj`, Gradle `.gradle`/`build`, CMake `build`/`out`/`cmake-build-*`, Flutter/Dart,
  Unity `Library`/`Temp`/`obj`, Unreal `Intermediate`/`DerivedDataCache`, Python `__pycache__`,
  `.venv`, `.tox`, `.pytest_cache`/`.mypy_cache`/`.ruff_cache`, Terraform, Zig, Elixir and Haskell.
  Each folder must be confirmed by its project (`node_modules` next to a `package.json`, `target`
  next to `Cargo.toml` or `pom.xml`, `build` with a `CMakeCache.txt` inside, ...), and AppData,
  Program Files, Windows and dot-folders such as `.vscode` are never searched, so dependencies of
  installed programs and editor extensions don't show up. Click a project to select all its
  folders, sort by size or by least recently changed, or "Select inactive 3+ months"; deleting
  goes through the usual dialog, protection checks and journal.
- **AI models** (works without a scan): every model in the Hugging Face cache, Ollama and LM
  Studio, with what deleting that model *alone* frees, when it was downloaded and when it was last
  used (if Windows records file access). Hugging Face repositories list their revisions with the
  branch, tag or PR pointing to each; "Select unused revisions" picks the ones nothing points to
  any more (like `huggingface-cli scan-cache`). Ollama models count only the layers no other model
  shares. Deleting works like `huggingface-cli delete-cache` and `ollama rm`: the revision's
  snapshot, refs and its own blobs; the Ollama manifest, then the layers no remaining model uses.
  Only model folders are ever touched (paths are resolved and checked against them), never chats,
  history, settings, sign-in tokens or Ollama's key. The dialog shows how to get each model back
  (`huggingface-cli download ...`, `ollama pull ...`), and the journal keeps that command.
  `HF_HOME`, `HF_HUB_CACHE`, `OLLAMA_MODELS` and LM Studio's downloads folder are honored.
- **WSL & Docker** (works without a scan): the virtual disks of WSL 2 distros (`ext4.vhdx`) and
  Docker Desktop (`docker_data.vhdx`) grow as Linux writes files but never shrink by themselves.
  The view shows each disk's size on the drive next to the space Linux uses inside: a running distro
  is asked (`df`), a stopped one is never started, its usage is read from the ext4 superblock in
  the .vhdx file. "Free up" compacts the chosen disks the way Microsoft documents it: free space is
  trimmed inside the running distros, Docker Desktop is stopped (and started again afterwards), WSL
  is shut down, then `diskpart` compacts each disk (Windows asks for administrator permission).
  Docker's own data is listed too (`docker system df`): the build cache and images no container
  uses can be removed with Docker's prune commands; stopped containers and unused volumes are only
  shown, since they can hold data.
- **Windows** (works without a scan): the space Windows keeps for itself, each measured and changed
  with Windows' own tool, as administrator (Windows asks, or restart as administrator once):
  - *Component store (WinSxS)*: DISM analyzes it (`/AnalyzeComponentStore`) and removes components
    that newer updates replaced (`/StartComponentCleanup`, never `/ResetBase`, so updates stay
    removable).
  - *Old driver packages*: third-party drivers in the DriverStore for which a newer version of the
    same driver is installed and that no device uses (connected or not); printer drivers are never
    offered. `pnputil /delete-driver` removes them, without `/force`.
  - *System Restore*: how much the restore points use, and a limit of 3, 5 or 10% of the drive
    (`vssadmin resize shadowstorage`).
  - *Hibernation file*: turn it off, reduce it to what Fast Startup needs, or turn it on again
    (`powercfg`).
  - *Compact OS*: compress Windows' own files, or undo it (`compact /compactos`).
  - *Event logs*: clear every log except Security (`wevtutil`).
- **Games** (works without a scan): installed games from Steam (every library), Epic Games, GOG,
  EA app, Ubisoft Connect, Battle.net and the Xbox app, with their size and when they were last
  played (from Steam itself, or from when the game's program was last opened). Sort by size or by
  least recently played. Uninstalling goes through the game's own launcher or uninstaller
  (`steam://uninstall/<id>`, `uplay://uninstall/<id>`, the registered uninstaller, Settings for
  Xbox games), so the launcher's library and shared runtimes stay right; WinNCDU deletes nothing
  itself there. Steam runtimes (redistributables, Proton) are not listed. *Leftovers* are what
  launchers leave behind: game folders, workshop items and shader caches of games that are no
  longer installed; they go to the Recycle Bin.
- **Delete** to the Recycle Bin (default) or permanently, for one item or a whole selection.
  Junctions and symlinks are never followed.
- **Locate**: right-click for Open, Open in Explorer, Show in Explorer, Copy path, Open terminal here,
  Properties and Rescan.
- **Cleanup** (works without a scan): Recycle Bin, temporary files, browser caches, chat app caches
  (new and classic Teams, Discord, Slack: only their web caches, never sign-ins or message stores),
  thumbnail cache, crash dumps, Windows Update downloads, Delivery Optimization, shader caches, game
  launcher caches (Steam, Epic, Battle.net) and package caches:
  npm, Yarn, pnpm, Bun, Deno, pip, uv, Poetry, NuGet, Go, Cargo, Composer and vcpkg; Maven and
  Gradle; conda archives; Conan. Where a tool keeps more than downloads in its cache folder, only the
  downloads are taken (Poetry's virtualenvs, Deno's localStorage data and conda's hard-linked
  packages stay), and a cache with a database (Conan) is cleaned by the tool's own command
  (`conan remove "*" -c`), with the space it freed measured before and after. The Delivery
  Optimization cache is emptied with Windows' own `Delete-DeliveryOptimizationCache`; previous
  Windows installations and upgrade leftovers (`Windows.old`, `$Windows.~BT`, `$Windows.~WS`) are
  measured and handed to Windows Disk Cleanup. Media you received in WhatsApp Desktop
  (`LocalState\shared	ransfers`) and Telegram Desktop (`Downloads\Telegram Desktop` and its media
  cache, never the rest of `tdata`, which holds the sign-in) is shown with its size but never
  preselected: apart from Telegram's cache, those files are gone from the PC for good.
  Each category is measured first, and only the safe ones are preselected. *Review* lists exactly
  what a category would delete (files, folders or Recycle Bin items) so you can uncheck anything to
  keep; cleaning deletes only that list, and skips entries that changed since they were measured.
  As administrator, system-level cleanups (Windows temp, update downloads) offer a System Restore
  point first; if it can't be made, nothing is cleaned.
  Categories are rules in [res/cleaners.ini](res/cleaners.ini) (built into the exe), so adding an
  app needs no code. Your own rules go in `%LOCALAPPDATA%\WinNCDU\cleaners.ini` (menu → *Cleanup
  rules…* creates it with examples): a section with a built-in's id replaces it, `enabled = false`
  turns it off, and `kind = command` with `command = ...` runs a tool's clean command instead of
  deleting files (the tool must be on PATH; the current folder is never searched). User rules are never preselected, can't name a whole drive, and anything the
  protection rules mark Important or Blocked is left out of every category, as is any folder with
  a repository, key, password database or virtual disk somewhere inside.
- **winapp2.ini import** (menu → *Import winapp2.ini…*): the community database of thousands of app
  cleaners ([Winapp2](https://github.com/MoscaDotTo/Winapp2), also used by BleachBit). Apps found
  on this PC that have something to clean appear below the built-in list, largest first, never
  preselected. Only disk-space rules are used: entries about cookies, history, passwords, sessions,
  settings, backups or saved games are skipped, no registry is touched, rules that would sweep a
  whole profile, drive or Windows folder are dropped, and what the built-in categories already
  clean is left to them. Every item still goes through the protection rules, with a stricter
  threshold (anything WinNCDU would warn about is left out), and folders holding browser-profile
  files (cookies, logins, history) are never taken whole.
- **Who is using this file?** When something can't be deleted because it is open, WinNCDU asks the
  Windows Restart Manager which app holds it and says so ("Close Google Chrome, then try again"),
  in the delete dialog, after Cleanup and in the journal.
- **Protected locations**: WinNCDU knows which folders must not be deleted and which hold data you
  can't get back, and checks every delete (and every Cleanup item) against them. See below.
- **Recently deleted**: every delete and cleanup is written to a journal
  (`%LOCALAPPDATA%\WinNCDU\journal.log`: time, path, size, Recycle Bin or permanent). The
  *Recently deleted* view lists it, and anything still in the Recycle Bin comes back with *Restore*
  (it is never restored over something new at the same path).
- **History & compare**: every completed scan stores its folder sizes in
  `%LOCALAPPDATA%\WinNCDU\history`. Scan the same drive later and compare to see which folders grew
  or shrank.
- **Explorer integration** (menu `⋯` → *Add "Analyze with WinNCDU" to Explorer*): right-click any
  folder, drive or folder background to analyze it. It is per-user and needs no admin rights; the
  same menu item removes it again.
- **Run as administrator** to read protected folders; disk usage vs. apparent size; JSON/CSV export
  and import of ncdu JSON (including scans made with Linux `ncdu -o`); per-monitor DPI; dark, light
  or system theme. Window size and preferences are remembered.

Command line: `winncdu.exe [folder]` scans a folder, `winncdu.exe -f scan.json` opens a saved scan.
The window opens as it was started (e.g. minimized from a shortcut set to "Run: Minimized").

| Key | Action |
| --- | --- |
| `Ctrl+F` | Search |
| `Enter` / double-click | Open a folder, or open a file with its app |
| `Backspace`, `Alt+←` / `Alt+→` / `Alt+↑` | Up / back / forward / up |
| `Del` / `Shift+Del` | Delete to the Recycle Bin / delete permanently |
| `Ctrl+E` / `Ctrl+C` | Show in Explorer / copy path |
| `Ctrl+A`, `Space`, `Shift+↑↓` | Select all / toggle selection / extend selection |
| `F5` | Rescan |
| `Ctrl+O` / `Ctrl+S` | Scan a folder / export as JSON |
| `Esc` | Clear the selection, then go back to the folder view |
| `F1` | About and shortcuts |

## Protected locations

Before anything is deleted, each item is checked against built-in rules and your own
(`src/protect.cpp`). The check covers the item itself, the folder it is in, and everything inside it,
so deleting `C:\Users\me` warns about `.claude` and `.ssh` too.

You see this before selecting anything: after a scan, protected items carry a shield badge in the
lists (red: Blocked, orange: Important, grey: Caution; hover it for the reason) and a shield on their
treemap tile. An outlined badge such as *Git inside* marks a folder that only contains protected
data. When the open folder is protected itself, a notice above the list says so once instead of
badging every row. The terminal app shows the same as tags like `[AI]` after the name, and in the
details panel (`i`).

| Level | What happens | Examples |
| --- | --- | --- |
| **Blocked** | Never deleted. The rest of a selection still goes ahead. | Drive roots, `C:\Windows` (except Temp, update downloads and dumps), `WinSxS`, `Windows\Installer`, Program Files roots, `WindowsApps`, user profile and `AppData` roots, registry hives, `pagefile.sys`, `hiberfil.sys`, `System Volume Information`, `$Recycle.Bin`, DPAPI keys |
| **Important** | An extra warning step that says what would be lost; the Recycle Bin is preselected. | AI assistant data, SSH/GPG/cloud credentials, crypto wallets, browser profiles, mail, Signal/WhatsApp, WSL/Docker/VM disks, OneDrive/Dropbox/Google Drive roots, game saves, folders inside Program Files, Git repositories, `.kdbx`, `.pst`, private keys |
| **Caution** | A heads-up in the delete dialog. | Local AI models (Ollama, LM Studio, Hugging Face), `.env`, `.pem`, virtual disks, project `.claude`/`.cursor` folders, Downloads, Windows Temp |

Caches inside protected folders stay deletable: browser `Cache`/`Code Cache`/`GPUCache`, the
caches of VS Code, Cursor, Windsurf and the Claude desktop app, and so on. WhatsApp's received
media (`transfers`) is only Caution, so Cleanup can offer it; the rest of WhatsApp stays Important.

**AI assistants.** Sessions, memory, settings and sign-ins of: Claude Code (`~\.claude`,
`~\.claude.json`), the Claude desktop app, OpenAI Codex CLI (`~\.codex`), ChatGPT desktop, Cursor,
Windsurf/Codeium, VS Code (Copilot Chat, Cline, Roo, Continue), GitHub Copilot and Copilot CLI,
Gemini CLI, Continue, Kiro, Trae, Zed, Aider, Augment, Qwen Code, opencode, Amp, Crush, Goose, Jan,
Msty, LM Studio, Perplexity and SpecStory.

**Your own rules** live in `%LOCALAPPDATA%\WinNCDU\protect.txt` (created with examples on first use;
menu `⋯` → *Protected locations…* → *Edit protect.txt*):

```text
block      D:\Backups              ; nightly backups
important  {HOME}\Work             ; client projects
caution    *.blend
allow      {HOME}\.cache\huggingface
```

A line with a backslash is a path (it covers everything inside); without one it matches a file or
folder name anywhere. `%ENV%` variables, `{HOME}`, `{ROAMING}`, `{LOCAL}` and `*` work. The most
specific path wins, so `allow` can switch off a built-in rule for a subfolder.

Permanent deletes work through file handles: everything inside the chosen folder is opened
relative to its parent, and links (junctions, symlinks) are removed as links, never followed. As
administrator, WinNCDU also refuses a path that no longer leads where it did (a folder above it was
swapped for a link), so nobody can redirect an elevated delete into, say, `C:\Windows`.

`winncdu-cli --protected` lists the protected locations found on this PC.
Missing an app or a false alarm? Rules are plain data in `BuiltinRules()`; pull requests welcome.

## Build

Requires Visual Studio 2022 (or Build Tools) with the *Desktop development with C++* workload.

```bat
build.cmd
```

The script finds Visual Studio, configures CMake with Ninja, and produces `build\winncdu.exe` and
`build\winncdu-cli.exe`. Both link the C runtime statically, so you can copy them anywhere.

Manual build:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build    :: runs the protect, journal, cleanup, delete, devjunk, aimodels, vdisks, wintools and games self-tests
```

## The terminal app (`winncdu-cli.exe`)

The keyboard-driven ncdu experience: the same browser, find, largest files, file types, duplicates,
delete and export features in a true-color terminal UI.

### Usage

```text
winncdu-cli [options] [path]

  path                  Folder or drive to scan (no path: choose from a list)
  -o FILE               Scan, write an ncdu-compatible JSON export to FILE and exit
  --csv FILE            Scan, write a CSV report to FILE and exit
  -f FILE               Browse a previously exported JSON file (read-only)
  --exclude PATTERN     Skip matching files/folders (wildcards, repeatable;
                        patterns containing '\' match the full path)
  -X, --exclude-from F  Read exclude patterns from a file
  -t, --threads N       Scanner threads (default: CPU count, 2-8)
  -H, --hardlinks       Count NTFS hard links once (slower; useful for C:\Windows)
  --apparent            Start with apparent sizes
  --si                  Base-10 units (kB, MB)
  --ascii               ASCII-only bars and borders
  --no-mouse            Disable mouse input
  --no-delete           Safe mode: deleting is disabled
  --protected           List the protected locations found on this PC and exit
  --journal             List what WinNCDU deleted recently (and what is still in the
                        Recycle Bin) and exit
  --ai-models           List local AI models (Hugging Face, Ollama, LM Studio) with
                        what each frees, and exit
  --wsl                 List WSL and Docker virtual disks (size on the drive vs. used
                        inside) and what Docker could prune, and exit
  --windows             List space Windows keeps (old drivers, hibernation, System
                        Restore, event logs) and exit
  --games               List installed games of every launcher (size, last played)
                        and leftovers of uninstalled ones, and exit
```

Examples:

```bat
winncdu-cli                     :: pick a drive
winncdu-cli C:\Users\me         :: scan a folder
winncdu-cli D:\ --exclude node_modules --exclude "*.tmp"
winncdu-cli -o scan.json C:\    :: scan now, browse later with: winncdu-cli -f scan.json
```

Run it as Administrator to read folders that are otherwise "access denied" (marked `!`).

### Keys

| Key | Action |
| --- | --- |
| `↑ ↓` `k j` `PgUp PgDn` `Home End` | Move |
| `Enter` `→` `l` | Open folder (on a file: show details) |
| `←` `Backspace` `h` `u` | Parent folder |
| `Space` / `Ctrl+A` / `U` | Mark item / mark all / clear marks |
| `d` `Del` | Delete marked or selected items (Recycle Bin or permanently) |
| `Shift+Del` | Delete, with "permanently" preselected |
| `o` / `O` / `p` / `b` | Show in Explorer / open / copy path / terminal here |
| `/` | Filter the current folder |
| `f` / `L` / `y` / `D` | Find / largest files / file types / duplicates |
| `Tab` | Switch between the browser and the last results list |
| `a` | Disk usage ↔ apparent size |
| `s` `n` `C` `M` | Sort by size / name / items / modified time (again to reverse) |
| `t` `g` `c` `m` `H` `i` | Folders first, graph style, items column, time column, hide hidden, details panel |
| `r` `F5` | Rescan current folder |
| `E` | Export (JSON / CSV) |
| `v` | Choose another drive or folder |
| `?` `F1` | Help |
| `q` | Quit |

Flags in the left column: `!` read error, `.` error somewhere inside, `@` link/junction (not
followed), `H` hard link already counted, `~` cloud placeholder (e.g. OneDrive online-only),
`<` excluded, `e` empty folder.

### Notes

- Use Windows Terminal for the best rendering. The classic console also works; if your font
  draws boxes or bars badly, use `--ascii`.
- Hard links: Windows uses many of them in `C:\Windows\WinSxS`. Without `-H` they are counted once
  per link, like in Explorer. With `-H` each file is counted once (the scan is slower).
- Imported scans are read-only: delete and rescan are disabled.

## Project layout

```text
src/                  core engine + terminal app
  scanner.cpp         multithreaded directory scanner
  tree.cpp            in-memory tree and size totals
  fsops.cpp           delete, Recycle Bin, Explorer, clipboard, drives
  analysis.cpp        search, largest files, file types, duplicates
  export.cpp          ncdu JSON export/import, CSV export
  protect.cpp         protected locations (system, AI assistants, keys, ...) and user rules
  journal.cpp         deletion journal, Recycle Bin lookup and restore
  devjunk.cpp         developer junk finder (build output and dependencies by project)
  aimodels.cpp        AI models: Hugging Face cache, Ollama, LM Studio (list and delete)
  vdisks.cpp          WSL and Docker virtual disks: usage, compaction, Docker prune
  wintools.cpp        Windows: component store, old drivers, System Restore, hibernation, ...
  games.cpp           games of every launcher, uninstall through the launcher, leftovers
  runtool.cpp         running console tools without a window, and as administrator
  main.cpp, ui.cpp, console.cpp   terminal app
src/gui/              windowed app
  app.cpp             window, input, layout
  actions.cpp         commands, background tasks, views
  draw.cpp            all drawing (Direct2D / DirectWrite)
  gfx.cpp             rendering helpers
  treemap.cpp         squarified treemap layout
  cleanup.cpp         cleanup rules: parsing, measuring, cleaning
  winapp2.cpp         winapp2.ini import
  history.cpp         scan snapshots and comparison
  platform.cpp        settings, theme, Explorer integration, file dialogs
  icons.cpp           shell icon cache
tests/protect_test.cpp  self-check of the protection rules (ctest)
tests/journal_test.cpp  journal format and a real recycle + restore round trip (ctest)
tests/cleanup_test.cpp  Cleanup deletes exactly the reviewed list (ctest)
tests/delete_test.cpp   permanent delete: links, read-only, locked files, long paths (ctest)
tests/devjunk_test.cpp  developer junk: project markers, grouping, app folders left alone (ctest)
tests/aimodels_test.cpp AI models: revisions, shared layers, nothing outside the model folders (ctest)
tests/vdisks_test.cpp   WSL/Docker: ext4 usage read from a VHDX, df/docker/wsl output parsing (ctest)
tests/wintools_test.cpp Windows: DISM report parsing, which driver packages count as old (ctest)
tests/games_test.cpp    games: Steam libraries and leftovers, Epic manifests, launcher uninstallers (ctest)
res/winncdu.ico       app icon
res/cleaners.ini      built-in Cleanup categories (format documented at the top)
```
