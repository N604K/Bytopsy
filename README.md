# Bytopsy

A byte autopsy for your Windows PC: a fast disk usage analyzer that finds out what really uses the
space, cleans it up safely, and looks after the rest: apps, startup, games, AI models, WSL and Docker
disks, and the drives' health. It started out inspired by [ncdu](https://dev.yorhel.nl/ncdu) (and
was called WinNCDU until October 2026). Native C++20 with Direct2D and no runtime dependencies: two
self-contained `.exe` files.

- **`bytopsy.exe`**: the windowed app, with a folder browser, treemap, cleanup, scan history and a dark/light theme.
- **`bytopsy-cli.exe`**: the ncdu-style terminal app, for consoles, SSH sessions and scripting.

Both share the same scanner, so results, exports and imports are interchangeable.

## The windowed app (`bytopsy.exe`)

Start it, pick a drive (or drop a folder onto the window) and see what takes up the space.

- **Folders**: an expandable tree sorted by size, with usage bars, share of the parent folder,
  item counts and modified times. Breadcrumbs, back/forward, multi-select (Ctrl/Shift-click).
- **Treemap** below the list: every file drawn to scale and colored by kind (video, images, audio,
  documents, archives, code, programs, system). Hover for details, click to select the file in the
  list, double-click to zoom into a folder. Drag the splitter to resize it, or hide it.
- **Find**: the search box understands `*.iso`, `>1G`, `<10M` (`>=`/`<=` include the limit),
  `older:1y`, `newer:7d`, `type:d`, `ext:mp4,mkv`, `empty`, and combinations such as
  `*.mp4 >500M older:1y`.
- **Largest files**, **File types** (with a category breakdown; double-click a type to list its
  files), **Old files** (filter by age and size) and **Empty folders**.
- **Duplicates**: compares size, then a quick hash, then the full SHA-256. "Select all but oldest"
  selects every redundant copy in one click.
- **Similar photos** (second tab of Duplicates): pictures that look alike rather than match byte
  for byte, such as a photo resized, re-saved as JPEG, slightly brightened or stored sideways with
  an EXIF rotation tag. Each image (JPEG, PNG, HEIC/WebP where Windows has the codec, TIFF, BMP,
  GIF) is decoded small through Windows Imaging Component, turned upright and reduced to two
  64-bit perceptual hashes (dHash and a DCT pHash); photos within the chosen distance of each
  other (Strict / Normal / Loose) form a group. Every group suggests one photo to keep (most
  pixels, then largest file, then oldest, then the name without "copy" or "(2)"), and everything
  else in it is close to that photo. Rows show a small preview, resolution, size and date. Nothing
  is selected for you; "Select all but suggested" does that on request, and deleting goes through
  the usual protection check, Recycle Bin and journal. Images are only read; hashes are kept in
  `%LOCALAPPDATA%\Bytopsy\photohash.cache` per path, size and modified time, so a second search
  only decodes new or changed files. Cloud placeholders are skipped (reading one would download
  it). Mirrored or cropped copies are not matched.
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
  Xbox games), so the launcher's library and shared runtimes stay right; Bytopsy deletes nothing
  itself there. Steam runtimes (redistributables, Proton) are not listed. *Leftovers* are what
  launchers leave behind: game folders, workshop items and shader caches of games that are no
  longer installed; they go to the Recycle Bin.
- **Disk health** (works without a scan, read-only): one card per physical disk with its model,
  type (NVMe / SATA SSD, hard disk, USB), Windows' own verdict (`MSFT_PhysicalDisk` health and
  operational status), wear, temperature against the drive's limit, power-on hours, error counts,
  spare blocks, data written, TRIM state, and which drive letters live on it. Wear and temperature
  come from `MSFT_StorageReliabilityCounter` (needs administrator rights) and, for NVMe drives,
  from the drive's own SMART / health log, which Windows hands out without them. TRIM comes from
  `fsutil behavior query DisableDeleteNotify`. *Used space over time* shows each drive's trend
  ("used space grew 12 GiB in the last 30 days", with a sparkline and when it would be full at
  that rate) from free-space measurements Bytopsy notes in `%LOCALAPPDATA%\Bytopsy\freespace.log`
  when the view opens or a scan finishes, or else from earlier scans of the drive. The disks are
  only queried (handles opened without read or write access); nothing is ever sent to them.
- **Startup** (works without a scan): what starts with Windows: the Run keys (this user, all users,
  32-bit), Run once entries, both Startup folders, scheduled tasks with an "at log on" or "at
  startup" trigger (Windows' own `\Microsoft\Windows\` tasks are left out), and services that start
  automatically. Each row shows the program's publisher, where it is registered, the command and an
  impact. Turning an entry off never deletes it: Run keys and Startup folder items are marked
  disabled in `StartupApproved` exactly as Task Manager does it (so Task Manager agrees), tasks are
  disabled in Task Scheduler, and services are set to Manual start, with the previous start type
  kept in the journal so turning it on sets it back. This user's Run and Startup folder entries flip
  at once; anything for all users, tasks and services are confirmed first and ask for
  administrator permission when needed. Microsoft's own services are only shown (behind "Show
  Microsoft's services and tasks"), never changed; Run once entries are only listed. *Impact* is
  honest about what it is: measured when Windows' boot performance log (Diagnostics-Performance,
  events 101/103) recorded the program slowing a boot, which needs administrator rights to read;
  otherwise an estimate from the program file's size and type (Electron/Chromium apps high, small
  updaters and delayed-start services low), and "unknown" when the program file or its size isn't
  there. Hover the impact to see why. Every change is journaled.
- **Apps** (works without a scan): installed programs from the Uninstall registry keys (HKLM 64- and
  32-bit, HKCU), as Settings lists them: system components, updates and patches are left out. Name,
  publisher, version, install date and size (the registered size, or the measured install folder);
  sort by any of them and filter by typing in the search box. *Uninstall* runs the app's own
  registered uninstaller, which asks what to do; Bytopsy waits for it (and for the app's entry to
  disappear, since many uninstallers hand over to a copy of themselves), then lists the folders the
  app left behind for review: its former install folder (the only one preselected, and only with
  nothing protected inside) and folders named after it, or after its publisher and it, in Program
  Files, ProgramData, AppData (Roaming, Local, LocalLow) and Local\Programs. Folders another installed
  app may use (same name, a name it begins with, a publisher folder holding other apps, or holding or
  inside another app's folder) are never offered; folders with protected data inside (wallets, keys,
  chat history, browser profiles) are shown with a shield and left alone. Checked folders go to the
  Recycle Bin and the journal. Registry entries are never touched (no registry cleaning).
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
  (`LocalState\shared\transfers`) and Telegram Desktop (`Downloads\Telegram Desktop` and its media
  cache, never the rest of `tdata`, which holds the sign-in) is shown with its size but never
  preselected: apart from Telegram's cache, those files are gone from the PC for good.
  Each category is measured first, and only the safe ones are preselected. *Review* lists exactly
  what a category would delete (files, folders or Recycle Bin items) so you can uncheck anything to
  keep; cleaning deletes only that list, and skips entries that changed since they were measured.
  As administrator, system-level cleanups (Windows temp, update downloads) offer a System Restore
  point first; if it can't be made, nothing is cleaned.
  Categories are rules in [res/cleaners.ini](res/cleaners.ini) (built into the exe), so adding an
  app needs no code. Your own rules go in `%LOCALAPPDATA%\Bytopsy\cleaners.ini` (menu → *Cleanup
  rules…* creates it with examples): a section with a built-in's id replaces it, `enabled = false`
  turns it off, `kind = cookies` / `kind = history` cleans the browser profile folders it names,
  and `kind = command` with `command = ...` runs a tool's clean command instead of
  deleting files (the tool must be on PATH; the current folder is never searched). User rules are never preselected, can't name a whole drive, and anything the
  protection rules mark Important or Blocked is left out of every category, as is any folder with
  a repository, key, password database or virtual disk somewhere inside.
- **Scheduled cleanup** (Cleanup → *Schedule…*): runs the checked categories daily, weekly, monthly
  or at logon through a Task Scheduler task of your own (`\Bytopsy\Scheduled cleanup`, no
  administrator rights needed). The task runs `bytopsy-cli --clean <ids> --yes --quiet` as you, only
  while you're signed in and never on battery; a missed run starts as soon as possible. Categories
  that need administrator rights and imported winapp2.ini rules are left out (the task never runs
  elevated). Each run deletes what its categories list at that moment, through the same code as the
  *Clean up* button: protected items are never touched, but items unchecked under *Review* are not
  remembered. Cleanup shows the schedule, its next run and how the last one went; *Remove schedule*
  deletes only that task. Runs are journaled and every run and schedule change adds a line to
  `%LOCALAPPDATA%\Bytopsy\cleanup.log`.
- **Browser cookies and history** (Cleanup, opt-in, never preselected): Chrome, Edge, Brave, Vivaldi,
  Chromium, Opera and Firefox, per profile. Cookies are deleted inside the browser's own cookie
  database (with SQLite), except those of the sites on your **keep me signed in to** list
  (`%LOCALAPPDATA%\Bytopsy\keep-signed-in.txt`, opened from the card or menu → *Keep me signed in
  to…*; a site covers its subdomains). The list starts with your most visited sites, so nothing
  you use daily signs you out unless you remove it. History: Chromium's `History` file goes with the
  files made from it (Visited Links, Top Sites, Shortcuts, Network Action Predictor); Firefox's
  visits are deleted in `places.sqlite` together with pages nothing else refers to, so bookmarks and
  keywords stay. Passwords, bookmarks and open tabs are never touched. Measuring reads the databases
  as read-only snapshots (no locks, nothing written); cleaning needs the browser closed, and a
  browser that still has its database open is named and skipped. A database whose layout isn't the
  expected one is left alone. Browser profiles are Important in the protection rules, which these
  two categories exist to clean; only Blocked folders are left out of them.
- **winapp2.ini import** (menu → *Import winapp2.ini…*): the community database of thousands of app
  cleaners ([Winapp2](https://github.com/MoscaDotTo/Winapp2), also used by BleachBit). Apps found
  on this PC that have something to clean appear below the built-in list, largest first, never
  preselected. Only disk-space rules are used: entries about cookies, history, passwords, sessions,
  settings, backups or saved games are skipped, no registry is touched, rules that would sweep a
  whole profile, drive or Windows folder are dropped, and what the built-in categories already
  clean is left to them. Every item still goes through the protection rules, with a stricter
  threshold (anything Bytopsy would warn about is left out), and folders holding browser-profile
  files (cookies, logins, history) are never taken whole.
- **Who is using this file?** When something can't be deleted because it is open, Bytopsy asks the
  Windows Restart Manager which app holds it and says so ("Close Google Chrome, then try again"),
  in the delete dialog, after Cleanup and in the journal.
- **Protected locations**: Bytopsy knows which folders must not be deleted and which hold data you
  can't get back, and checks every delete (and every Cleanup item) against them. See below.
- **Recently deleted**: every delete and cleanup is written to a journal
  (`%LOCALAPPDATA%\Bytopsy\journal.log`: time, path, size, Recycle Bin or permanent). The
  *Recently deleted* view lists it, and anything still in the Recycle Bin comes back with *Restore*
  (it is never restored over something new at the same path).
- **History & compare**: every completed scan stores its folder sizes in
  `%LOCALAPPDATA%\Bytopsy\history`. Scan the same drive later and compare to see which folders grew
  or shrank.
- **Explorer integration** (menu `⋯` → *Add "Analyze with Bytopsy" to Explorer*): right-click any
  folder, drive or folder background to analyze it. It is per-user and needs no admin rights; the
  same menu item removes it again.
- **Run as administrator** to read protected folders; disk usage vs. apparent size; JSON/CSV export
  and import of ncdu JSON (including scans made with Linux `ncdu -o`); per-monitor DPI; dark, light
  or system theme. Window size and preferences are remembered.
- **Accessibility**: the window is drawn with Direct2D, so it describes itself to screen readers
  (Narrator, NVDA, JAWS) and other UI Automation clients: the toolbar buttons (by name, also the
  icon-only ones), the sidebar as a list of views with the current one selected, each view's text,
  buttons, cards and checkboxes, the list with its column headers and rows (a row is read as its
  cells: "Photos, 3.8 MiB, 44%, 3, 2026-10-18 18:51"), the status bar, and dialogs as dialog
  windows with their title, message, checkbox, choices and buttons. Buttons and cards can be
  invoked, checkboxes, chips and the startup switches toggled, rows selected, opened, expanded and
  collapsed: each runs exactly what a click runs, so deleting still asks first. Moving through a
  list with the arrow keys moves the reader along (each row is read), changed checkboxes and
  selections are announced, a dialog is read when it opens, and messages at the bottom of the
  window are spoken. The search box is a regular edit control. In dialogs, Tab / Shift+Tab and the
  arrow keys move between the checkbox, the choices and the buttons (a focus ring shows where),
  Enter presses the focused button and Escape cancels. Not covered yet: only the rows on screen are
  exposed (a reader sees the list as far as it is scrolled, not its full length), the treemap is a
  single image (the list shows the same items), and the keyboard moves through lists and dialogs
  only, not between a view's buttons (a screen reader's own navigation reaches those). The right-click
  and `⋯` menus are standard Windows menus.
- **Updates** (opt-in): on the first start Bytopsy asks whether it may look for a new version once
  a day; the answer is *off* until you say yes (`⋯` > *Check for updates daily* changes it, and
  `⋯` > *Check for updates now* works either way). A newer version shows a banner: *What's new*,
  *Update*, *Not now*, *Skip this version*. *Update* downloads the release zip from GitHub and
  installs it only if it matches the signed update information (see [Privacy](#privacy)), then
  replaces `bytopsy.exe` and `bytopsy-cli.exe` next to it and restarts; the old programs stay as
  `.old` files until the next start, and if anything fails everything is put back. Settings,
  history, rules, `Bytopsy-data`, `lang` and the portable marker are never touched, and the
  update is written to the journal. Installed with winget, Scoop or Chocolatey, the banner shows
  that package manager's update command instead. In a folder Bytopsy can't write to (Program
  Files) it says so and offers *Restart as administrator*.
- **Languages**: every text of the windowed app comes from a translation table, so Bytopsy can be
  translated without touching the code ([how](#translations)). It starts in Windows' display
  language when a translation for it exists, otherwise in English; `⋯` > *Language* overrides that
  (remembered in `settings.ini`). Numbers and dates follow your Windows region settings in every
  language. No translation ships yet: the menu offers English and a *pseudo-language* that
  lengthens and accents every text, for checking that layouts cope with longer languages. The
  terminal app, the journal, logs and command lines stay English.

Command line: `bytopsy.exe [folder]` scans a folder, `bytopsy.exe -f scan.json` opens a saved scan.
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
| `Tab` / `Shift+Tab`, `←` `→` (in a dialog) | Move between the checkbox, the choices and the buttons; `Enter` presses the focused one |

## No registry cleaner

Bytopsy will not clean the registry, and that is on purpose. The whole registry is a few hundred
megabytes at most, so removing "unused" keys frees nothing you could measure; Windows doesn't get
faster from it either, because it only reads the keys it needs. What registry cleaners do achieve is
breakage: a key that looks orphaned is often read by an installer, a repair, a file association or
a COM component later, and when it's gone, that thing fails. Microsoft doesn't support registry
cleaners for this reason.

So the only registry changes Bytopsy makes are the ones you ask for, each undoable from the app: the
Explorer menu entry, and turning a startup entry off or on (the `StartupApproved` value Task Manager
writes, or a service's start type through `sc`). Settings such as hibernation or the System Restore
limit are changed by Windows' own tools (`powercfg`, `vssadmin`), never by editing keys. Uninstalling
an app runs that app's own uninstaller, and the leftovers offered afterwards are folders, never
registry keys.

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

**Your own rules** live in `%LOCALAPPDATA%\Bytopsy\protect.txt` (created with examples on first use;
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
administrator, Bytopsy also refuses a path that no longer leads where it did (a folder above it was
swapped for a link), so nobody can redirect an elevated delete into, say, `C:\Windows`.

`bytopsy-cli --protected` lists the protected locations found on this PC.
Missing an app or a false alarm? Rules are plain data in `BuiltinRules()`; open an issue with the
*False alarm* or *Missing protected folder* template, or see [CONTRIBUTING.md](CONTRIBUTING.md) to
add a rule (or a cleaner) yourself.

## Privacy

Bytopsy has no telemetry and sends nothing about you, your PC or your disk anywhere. What it
learns about your disk stays on your PC.

**Its one network connection: the update check**, and only when you turned on *Check for updates
daily* (asked once, off until you say yes), pressed `⋯` > *Check for updates now*, or ran
`bytopsy-cli --check-update`. Bytopsy then fetches `https://bytopsy.com/update.json` and, if that
doesn't answer, `https://github.com/N604K/Bytopsy/releases/latest/download/update.json`: a small
file naming the latest version. The request carries nothing but what any download does (your IP
address, and `Bytopsy/<version>` as the user agent); no cookies, no identifiers. Only if you then
choose *Update* does it download the release zip from
`https://github.com/N604K/Bytopsy/releases/download/...` (GitHub serves it from its file storage).
This is WinHTTP with your Windows proxy settings, HTTPS only. The update file is signed (ECDSA
P-256, checked with Windows' `bcrypt.dll` against a key built into Bytopsy) and names the zip's
SHA-256, so a hijacked website can't make Bytopsy install anything; older versions are never
installed, and nothing is downloaded from anywhere but the project's GitHub releases. The
publisher check of signed releases works offline (no revocation lookups). There is no other
networking code.

**What it reads.** Names, sizes, times and attributes of what you scan. File contents only where a
feature needs them: hashes for Duplicates, the ext4 superblock inside WSL disks, game launcher and
AI model manifests, scans you import, and its rule files; pixels of images for *Similar photos*; the
cookie and history databases of browsers for those two Cleanup categories (cookies are counted per
site, never read out). From Windows it reads the Uninstall, Run and service registry keys, scheduled
tasks, and disk health from WMI and the drives' own health logs.

**What it writes.** Everything lives in `%LOCALAPPDATA%\Bytopsy` (in
[portable mode](#portable-mode), in `Bytopsy-data` next to the exe):

| File | Contents |
| --- | --- |
| `settings.ini` | Window position, theme, view options and exclude patterns of the windowed app |
| `history\` | Folder sizes of each completed scan, for *History & compare* |
| `journal.log` (`journal.1.log` after 4 MB) | What was deleted or cleaned: time, path, size, Recycle Bin or permanent |
| `protect.txt` | Your protection rules (created with commented examples) |
| `cleaners.ini` | Your cleanup rules (created when you open *Cleanup rules…*) |
| `winapp2.ini` | A copy of the winapp2.ini you imported |
| `keep-signed-in.txt` | The sites whose cookies Cleanup keeps (created when cookies are first measured) |
| `cleanup.log` | One line per `bytopsy-cli --clean --yes` run and per schedule change |
| `freespace.log` | Each drive's free space, at most every 6 hours, for *Disk health* trends |
| `photohash.cache` | Image hashes by path, size and time, so *Similar photos* is fast the second time |
| `lang\` | Translations you add yourself (only read; see [Translations](#translations)) |

`settings.ini` also holds the update settings (`updates`, `updatesAsked`, the time of the last
automatic check, a skipped version).

Outside that folder: exports where you save them; the Explorer menu entry, if you add it (three keys
under `HKCU\Software\Classes`, removed by the same menu item); the scheduled cleanup task
(`\Bytopsy\Scheduled cleanup` in Task Scheduler), if you create one; when you turn a startup entry
off or on, the same `StartupApproved` value Task Manager writes, the task's enabled switch, or the
service's start type; when you install an update, the new `bytopsy.exe`, `bytopsy-cli.exe`,
`README.md` and `LICENSE` next to the running ones (the previous files as `<name>.old` until the
next start; nothing else in that folder is touched); and short-lived files in `%TEMP%` (a
`diskpart` script while compacting virtual disks, the output of tools run as administrator, an
update's download in `bytopsy-update-*`), deleted when the tool or update finishes. Deleting the `Bytopsy` folder removes everything else.

**Programs it starts**, only from the views and actions that need them: Windows' own tools (DISM,
`pnputil`, `vssadmin`, `powercfg`, `compact`, `wevtutil`, `diskpart`, PowerShell for System Restore
storage and the Delivery Optimization cache, Disk Cleanup, `fsutil` for TRIM, `reg`, `schtasks` and
`sc` for startup entries that need administrator rights, `tar.exe` to unpack an update),
`wsl.exe`, `docker`, the clean command of a
`kind = command` cleanup rule (such as `conan remove`), an app's own uninstaller, and a game
launcher's uninstall link (`steam://uninstall/...`), and after an update the new `bytopsy.exe`.
Bytopsy itself goes online for none of these,
but the programs behave as they always do: Steam or Docker Desktop, for example, may connect on
their own.

## Translations

Bytopsy is in English only so far, and translations are welcome. Each language is one UTF-8 text
file, `res/lang/<code>.lang` (e.g. `de.lang`, `pt-BR.lang`), with one line per text:

```text
@name = Deutsch
Largest files = Größte Dateien
Docker removed {0} = Docker hat {0} entfernt
{0} item|{0} items = {0} Element|{0} Elemente
```

The English text is the key; `{0}`, `{1}` are values (sizes, names, counts) that can move to wherever
the sentence needs them; plurals give one form per case of the language's plural rule. Untranslated
lines fall back to English. Start a file from
[res/lang/template.lang](res/lang/template.lang), which lists every text with where it is used:

```bat
python -I tools/extract_strings.py --new de --name Deutsch
```

**Try it without building**: put the file in a `lang` folder next to `bytopsy.exe` (or in
`%LOCALAPPDATA%\Bytopsy\lang`, `Bytopsy-data\lang` in portable mode), restart Bytopsy and pick it
under `⋯` > *Language*. Mistakes in the file (such as a `{1}` the English text doesn't have) are
shown as a message. Setting `BYTOPSY_LANG=de` (or `BYTOPSY_LANG=pseudo`) starts in that language
whatever the menu says. Files in `res/lang` are built into `bytopsy.exe`; a file in a `lang` folder
replaces the built-in one of the same code. Paths, registry keys, command lines, cleanup rule ids,
the journal and logs are never translated. See [CONTRIBUTING.md](CONTRIBUTING.md#translate-bytopsy)
for keeping a translation up to date.

## Install

Download `bytopsy-<version>-x64.zip` from the GitHub releases, check it against the `.sha256` file
next to it (see [docs/RELEASING.md](docs/RELEASING.md#checking-a-download)), and unpack it anywhere.
There is no installer; both programs run from any folder.

To keep it up to date, let it check for updates (asked on the first start; see the *Updates*
feature above), or run `bytopsy-cli --check-update` now and then.

Manifests for winget, Scoop and Chocolatey are prepared in [packaging/](packaging) but not yet
published in those repositories.

### Portable mode

Put an empty file named `bytopsy.portable` next to `bytopsy.exe` / `bytopsy-cli.exe`:

```bat
type nul > bytopsy.portable
```

Both programs then keep everything they write (settings, history, journal, `protect.txt`,
`cleaners.ini`, the imported `winapp2.ini`) in a `Bytopsy-data` folder next to the exe instead of
`%LOCALAPPDATA%\Bytopsy`, so the folder can live on a USB stick. The folder must be writable.
Nothing is moved over: copy the files from `%LOCALAPPDATA%\Bytopsy` into `Bytopsy-data` to keep them.
The Explorer menu entry, if you add it, still goes to the registry (it points at that exe).
The Scoop manifest installs in portable mode and keeps `Bytopsy-data` across updates.

### Coming from WinNCDU

Bytopsy was called WinNCDU before. On its first start it renames the old data folder
(`%LOCALAPPDATA%\WinNCDU`, or `WinNCDU-data` next to a portable exe, where `winncdu.portable` still
counts as the marker), so settings, history, the journal and your rules carry over. An "Analyze with
WinNCDU" Explorer entry is replaced by "Analyze with Bytopsy". A cleanup scheduled under the old name
(`\WinNCDU\Scheduled cleanup`) is not moved: remove it in Task Scheduler and schedule again.

## Build

Requires Visual Studio 2022 (or Build Tools) with the *Desktop development with C++* workload.

```bat
build.cmd
```

The script finds Visual Studio, configures CMake with Ninja, and produces `build\bytopsy.exe` and
`build\bytopsy-cli.exe`. Both link the C runtime statically, so you can copy them anywhere.

Manual build:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build    :: runs the protect, journal, cleanup, delete, devjunk, aimodels, vdisks, wintools, games, analysis, export, importfuzz, diskhealth, schedule, startup, apps, phash, uia, i18n and update self-tests
```

GitHub Actions builds and runs the self-tests on every push and pull request
([.github/workflows/build.yml](.github/workflows/build.yml)). A tag `v*` publishes a release with
`bytopsy-<version>-x64.zip` (both programs and this README) and its SHA-256 in a `.sha256` file; see
[docs/RELEASING.md](docs/RELEASING.md). Releases are not code-signed yet, so SmartScreen may warn
before the first start; the workflow is prepared for SignPath signing (same document).

## The terminal app (`bytopsy-cli.exe`)

The keyboard-driven ncdu experience: the same browser, find, largest files, file types, duplicates,
delete and export features in a true-color terminal UI.

### Usage

```text
bytopsy-cli [options] [path]

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
  --journal             List what Bytopsy deleted recently (and what is still in the
                        Recycle Bin) and exit
  --ai-models           List local AI models (Hugging Face, Ollama, LM Studio) with
                        what each frees, and exit
  --wsl                 List WSL and Docker virtual disks (size on the drive vs. used
                        inside) and what Docker could prune, and exit
  --windows             List space Windows keeps (old drivers, hibernation, System
                        Restore, event logs) and exit
  --games               List installed games of every launcher (size, last played)
                        and leftovers of uninstalled ones, and exit
  --disks               List physical disks (health, wear, temperature, TRIM, drive
                        letters) and each drive's used-space trend, and exit
  --clean IDS           Cleanup categories (comma list of ids, e.g. temp,browser,dev,
                        or all-safe = the preselected ones): shows what would be
                        freed and exits. Unknown ids list the valid ones.
    --yes               ...really clean (permanently; protected items never), journal
                        it and append a line to cleanup.log. Exit code 0 = done,
                        1 = some items in use, 2 = bad ids, 3 = all skipped (admin)
    --quiet             ...print nothing (used by the scheduled task)
  --startup             List what starts with Windows (Run keys, Startup folders,
                        logon/boot tasks, services) with impact estimates, and exit
  --apps                List installed apps (size, install date, version, publisher)
                        from the Uninstall registry keys, and exit
  --similar-photos[=strict|normal|loose]
                        Scan path, list groups of photos that look alike (resized,
                        re-saved, lightly edited copies) with the one to keep, and exit
  --check-update        Ask bytopsy.com (or GitHub) for the latest version, print it
                        next to this one, and exit; never installs anything. Exit
                        code 0 = up to date, 2 = a newer version exists, 1 = no answer
```

Examples:

```bat
bytopsy-cli                     :: pick a drive
bytopsy-cli C:\Users\me         :: scan a folder
bytopsy-cli D:\ --exclude node_modules --exclude "*.tmp"
bytopsy-cli -o scan.json C:\    :: scan now, browse later with: bytopsy-cli -f scan.json
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
  diskhealth.cpp      disk health: WMI storage, NVMe health log, TRIM, used-space trends
  schedule.cpp        scheduled cleanup: --clean arguments, Task Scheduler task, cleanup.log
  startup.cpp         startup manager: Run keys, Startup folders, tasks, services; reversible off/on
  apps.cpp            installed apps (Uninstall registry keys), uninstallers, leftover folders
  phash.cpp           similar photos: WIC decoding, dHash/pHash, hash index, grouping, cache
  i18n.cpp            translations: Tr/TrF/TrN, .lang files, plural rules, pseudo-language
  update.cpp          self-update: signed update.json, WinHTTP download, SHA-256, unpack, file swap
  updatekey.h         the public key update.json is checked with (placeholder until generated)
  runtool.cpp         running console tools without a window, and as administrator
  main.cpp, ui.cpp, console.cpp   terminal app
src/gui/              windowed app
  app.cpp             window, input, layout
  actions.cpp         commands, background tasks, views
  draw.cpp            all drawing (Direct2D / DirectWrite)
  gfx.cpp             rendering helpers
  treemap.cpp         squarified treemap layout
  cleanup.cpp         cleanup rules: parsing, measuring, cleaning
  browserdata.cpp     browser cookies (with the keep list) and history, through SQLite
  winapp2.cpp         winapp2.ini import
  apps_view.cpp       Apps view: list, uninstall and wait, leftovers review
  update_gui.cpp      update question, daily check, update banner, installing and restarting
  history.cpp         scan snapshots and comparison
  platform.cpp        settings, theme, Explorer integration, file dialogs
  icons.cpp           shell icon cache
  accmodel.cpp        accessible element tree built from each frame (identity, reading order, names)
  uia.cpp             UI Automation provider (COM): elements, patterns, events
  uia_app.cpp         what the window shows to UI Automation, and what acting on an element runs
tests/protect_test.cpp  self-check of the protection rules (ctest)
tests/journal_test.cpp  journal format, a real recycle + restore round trip, portable mode's data folder (ctest)
tests/cleanup_test.cpp  Cleanup deletes exactly the reviewed list; kept sites' cookies and Firefox bookmarks survive (ctest)
tests/delete_test.cpp   permanent delete: links, read-only, locked files, long paths (ctest)
tests/devjunk_test.cpp  developer junk: project markers, grouping, app folders left alone (ctest)
tests/aimodels_test.cpp AI models: revisions, shared layers, nothing outside the model folders (ctest)
tests/vdisks_test.cpp   WSL/Docker: ext4 usage read from a VHDX, df/docker/wsl output parsing (ctest)
tests/wintools_test.cpp Windows: DISM report parsing, which driver packages count as old (ctest)
tests/games_test.cpp    games: Steam libraries and leftovers, Epic manifests, launcher uninstallers (ctest)
tests/analysis_test.cpp search syntax, largest files, file types, duplicates on real files (ctest)
tests/export_test.cpp   ncdu JSON round trips with odd names and flags, Linux ncdu input, CSV (ctest)
tests/importfuzz_test.cpp fuzzing the ncdu JSON importer with mutated exports, fixed seed (ctest)
tests/diskhealth_test.cpp disk health: WMI/fsutil/NVMe log parsing, verdicts, used-space trends (ctest)
tests/schedule_test.cpp scheduled cleanup: ids, --clean arguments, triggers, --clean --yes on a fixture (ctest)
tests/startup_test.cpp  startup: StartupApproved values, command lines, impact, journal notes; changes nothing (ctest)
tests/apps_test.cpp     apps: which entries are listed, which folders count as leftovers and which never do (ctest)
tests/phash_test.cpp    similar photos: hashes of re-encoded/rotated images, hash index, grouping, cache (ctest)
tests/uia_test.cpp      accessibility tree: element identity across frames, reading order, names from text, patterns (ctest)
tests/i18n_test.cpp     translations: .lang parsing, placeholders, plural rules, pseudo-language, res/lang files (ctest)
tests/update_test.cpp   self-update: signatures (throwaway key), versions, pinned URLs, SHA-256, file swap and rollback, package managers; offline (ctest)
res/bytopsy.ico       app icon
res/cleaners.ini      built-in Cleanup categories (format documented at the top)
res/lang/             translations (<code>.lang, built in) and template.lang, the generated list of texts
tools/extract_strings.py  collects the texts into template.lang, checks and updates translations
tools/update-key.ps1  the update signing key: make it, write and sign update.json, verify it
third_party/sqlite/   SQLite 3.54.1 amalgamation (sqlite3.c, sqlite3.h), unmodified
CONTRIBUTING.md       building, testing, adding a protected location, a cleaner or a translation
.github/ISSUE_TEMPLATE  issue forms: false alarm, missing protected folder, bug report
.github/workflows/build.yml  CI: build, self-tests, translation template check; release zips with SHA-256 on tags
docs/RELEASING.md     how releases are built, signed (once set up) and published, update.json and bytopsy.com
packaging/signpath/   SignPath artifact configuration for code signing
packaging/winget/, scoop/, chocolatey/  package manager manifests (templates)
packaging/update-manifests.ps1  fills them in for a release
```

## License

MIT: see [LICENSE](LICENSE). You may use, change and share Bytopsy, also commercially, as long as
the copyright notice and the license text come along.

## Third-party code

[SQLite](https://www.sqlite.org) (`third_party/sqlite`, version 3.54.1, the amalgamation from
sqlite.org) is in the public domain. It is used only to read and edit browsers' cookie and history
databases.
