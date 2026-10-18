# Contributing to WinNCDU

WinNCDU deletes files, so the bar is: every action is explained, reversible where possible, and
never touches data people can't get back without saying so. Most useful contributions need no deep
knowledge of the code: a protected location that is missing, a false alarm, or a cleaner for an app.

- Found a false alarm or a folder that should be protected? Open an issue with the matching
  template, or send a pull request using the guides below.
- Bigger changes: open an issue first, so we can agree on the approach.

## Build and test

Requires Visual Studio 2022 (or Build Tools) with the *Desktop development with C++* workload.

```bat
build.cmd
ctest --test-dir build --output-on-failure
```

`build.cmd` configures CMake with Ninja (Release) and builds `build\winncdu.exe`,
`build\winncdu-cli.exe` and the self-tests. `ctest` needs CMake on PATH (a *Developer Command
Prompt* has it); otherwise run the test programs directly, each prints `ok`/`FAIL` per check and
exits non-zero on a failure:

```bat
for %t in (build\winncdu-*-test.exe) do %t
```

| Test | Checks |
| --- | --- |
| `winncdu-protect-test` | protection levels of well-known paths, shield badges, `protect.txt` rules |
| `winncdu-cleanup-test` | `res/cleaners.ini` parses, Cleanup deletes exactly the reviewed list, winapp2.ini import |
| `winncdu-journal-test` | journal format, a real Recycle Bin move and restore |
| `winncdu-delete-test` | permanent delete: links, read-only and locked files, long paths |
| `winncdu-devjunk-test` | developer junk: project markers, grouping, app folders left alone |
| `winncdu-aimodels-test` | Hugging Face / Ollama / LM Studio: revisions, shared layers, nothing outside model folders |
| `winncdu-vdisks-test` | WSL/Docker: ext4 usage read from a VHDX, tool output parsing |
| `winncdu-wintools-test` | DISM report parsing, which driver packages count as old |
| `winncdu-games-test` | Steam libraries and leftovers, Epic manifests, launcher uninstallers |

The tests work in `%TEMP%` and in a folder next to the test program, and remove what they create.
Two of them move a test folder to the Recycle Bin and take it out again; the protect test rewrites
`protect.txt` for a moment and restores it.

Before a pull request: the build is warning-free at `/W4`, all tests pass, and the README describes
what changed for users. Match the style of the code around your change.

## Add a protected location

Built-in rules are plain data in `BuiltinRules()` in [src/protect.cpp](src/protect.cpp), grouped by
section (Windows and boot, program folders, user profiles, personal folders, cloud sync roots, AI
assistants, keys, wallets, browsers/mail/messengers, virtual machines, and "anywhere: by name").
Add your line to the right section:

```cpp
b.Tree(L"{ROAMING}\\NewApp", P::Important, kAI, L"NewApp (chats, settings)", kAIR);
b.Allow(L"{ROAMING}\\NewApp\\Cache");   // its cache stays deletable
```

| Helper | Protects |
| --- | --- |
| `b.Tree(pattern, level, group, name, reason)` | the folder and everything inside it |
| `b.Self(pattern, level, group, name, reason)` | only the item itself (files inside are fine to delete) |
| `b.Path(pattern, self, inside, group, name, reason)` | the item and its contents at different levels |
| `b.Name(pattern, self, inside, contains, group, name, reason, fileOnly)` | any file or folder with that name, anywhere (`.git`, `*.kdbx`) |
| `b.Allow(pattern)` | a carve-out inside a protected folder, such as a cache |
| `b.EditorFamily(dir, ...)`, `b.Chromium(userData, name)` | VS Code forks and Chromium browsers, with their caches allowed |

Patterns take `{HOME}`, `{ROAMING}`, `{LOCAL}`, `{LOCALLOW}` (current user and every profile under
`C:\Users`), `%ENV%` variables and `*` segments. A rule whose variable isn't set is skipped. The most
specific path wins, so an `Allow` for a subfolder beats the `Tree` around it.

Choosing the level:

- **Blocked**: only when deleting breaks Windows or a program, or Windows has a proper tool for it
  (the reason names that tool).
- **Important**: data that can't be downloaded again: sessions, chat history, keys, wallets, saves,
  mail, synced folders.
- **Caution**: large but re-downloadable data, or a folder people often delete by mistake.

The reason is one sentence: what is lost, and what to do instead if there is a better way.

Then add a check to [tests/protect_test.cpp](tests/protect_test.cpp) (the folders need not exist):

```cpp
Expect(roaming + L"\\NewApp", Protect::Important);
Expect(roaming + L"\\NewApp\\Cache", Protect::None);
```

Build, run `build\winncdu-protect-test.exe`, and `build\winncdu-cli.exe --protected` to see which
protected locations exist on your PC. If you add an AI assistant, add it to the list in the README.

To fix a **false alarm**, narrow the rule rather than remove it: an `Allow` for the cache folder, a
`Self` instead of a `Tree`, or `fileOnly` for a name rule. Add an `Expect(..., Protect::None)` for the
path that was wrongly flagged.

You can try any rule without building: a line in `%LOCALAPPDATA%\WinNCDU\protect.txt` (format in the
README) works the same way.

## Add a cleaner

Cleanup categories are rules in [res/cleaners.ini](res/cleaners.ini); the format is documented at
the top of the file. A new app is one section:

```ini
[newapp]
name = NewApp cache
desc = Downloaded previews; NewApp fetches them again when needed.
icon = E7C3
path = %LOCALAPPDATA%\NewApp\Cache
path = %LOCALAPPDATA%\NewApp\Logs | files=*.log | age=7d
```

- Only list folders the app re-creates by itself. Name the cache subfolder, never the app's own
  folder or a profile.
- `safe = true` preselects the category: only for things that are always re-created or downloaded
  again and hold nothing personal.
- `desc` says honestly what is lost or what happens next ("games may stutter briefly").
- A cache with a database (deleting files would corrupt it) uses `kind = command` with the tool's
  own clean command; the paths are then only measured.
- Protection always wins: an item the protection rules mark Important or Blocked is left out of
  every category. If your cache sits inside a protected folder (a browser profile, an editor's
  settings folder), add a `b.Allow(...)` for it in `src/protect.cpp` as well.

Try it first without building: put the section in `%LOCALAPPDATA%\WinNCDU\cleaners.ini` (menu →
*Cleanup rules…*), click *Measure*, then *Review* to see exactly what it would delete.

`tests/cleanup_test.cpp` checks that the shipped file parses without errors and lists every category
id in order; add your id to that list (and update the count, and any index-based checks after it).
Add the app to the Cleanup paragraph of the README.

## Releases

Releases are built, tested and (once set up) signed by GitHub Actions; see
[docs/RELEASING.md](docs/RELEASING.md).

## Issues

- **False alarm**: WinNCDU warns about, or blocks, something that is safe to delete.
- **Missing protected folder**: something that holds data you can't get back is not protected.
- **Bug report**: anything else that doesn't work.

`winncdu-cli --protected` and the shield's tooltip show which rule matched; please include that.
Please don't paste file lists or paths that reveal more about you than the issue needs.
