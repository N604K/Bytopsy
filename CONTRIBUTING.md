# Contributing to Bytopsy

Bytopsy deletes files, so the bar is: every action is explained, reversible where possible, and
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

`build.cmd` configures CMake with Ninja (Release) and builds `build\bytopsy.exe`,
`build\bytopsy-cli.exe` and the self-tests. `ctest` needs CMake on PATH (a *Developer Command
Prompt* has it); otherwise run the test programs directly, each prints `ok`/`FAIL` per check and
exits non-zero on a failure:

```bat
for %t in (build\bytopsy-*-test.exe) do %t
```

| Test | Checks |
| --- | --- |
| `bytopsy-protect-test` | protection levels of well-known paths, shield badges, `protect.txt` rules |
| `bytopsy-cleanup-test` | `res/cleaners.ini` parses, Cleanup deletes exactly the reviewed list, winapp2.ini import |
| `bytopsy-journal-test` | journal format, a real Recycle Bin move and restore, where the data folder is (portable mode) |
| `bytopsy-delete-test` | permanent delete: links, read-only and locked files, long paths |
| `bytopsy-devjunk-test` | developer junk: project markers, grouping, app folders left alone |
| `bytopsy-aimodels-test` | Hugging Face / Ollama / LM Studio: revisions, shared layers, nothing outside model folders |
| `bytopsy-vdisks-test` | WSL/Docker: ext4 usage read from a VHDX, tool output parsing |
| `bytopsy-wintools-test` | DISM report parsing, which driver packages count as old |
| `bytopsy-games-test` | Steam libraries and leftovers, Epic manifests, launcher uninstallers |
| `bytopsy-analysis-test` | search syntax, largest files, file types, duplicates on real files |
| `bytopsy-export-test` | ncdu JSON round trips with odd names and flags, Linux ncdu input, CSV |
| `bytopsy-importfuzz-test` | 5000 mutated exports (fixed seed) fed to the ncdu JSON importer |
| `bytopsy-diskhealth-test` | WMI / fsutil / NVMe health log parsing, verdicts, used-space trends |
| `bytopsy-schedule-test` | `--clean` ids and arguments, schedule triggers, `--clean --yes` on a fixture |
| `bytopsy-startup-test` | `StartupApproved` values, command lines, impact; changes nothing |
| `bytopsy-apps-test` | which Uninstall entries are listed, which folders count as leftovers |
| `bytopsy-phash-test` | similar photos: hashes of re-encoded and rotated images, grouping, cache |
| `bytopsy-uia-test` | accessibility tree: element identity across frames, reading order, names, patterns |
| `bytopsy-i18n-test` | translations: `.lang` parsing, placeholders, plural rules, pseudo-language, every `res/lang` file |

The devjunk test builds its fixture next to the test program, so it finds nothing (and fails) when
the build folder sits inside a dot folder such as `.claude\worktrees\...`.

The tests work in `%TEMP%` and in a folder next to the test program, and remove what they create.
Two of them move a test folder to the Recycle Bin and take it out again; the protect test rewrites
`protect.txt` for a moment and restores it.

Before a pull request: the build is warning-free at `/W4`, all tests pass, the translation template
is current (`python -I tools/extract_strings.py`, see [below](#texts-in-the-code)), and the README
describes what changed for users. Match the style of the code around your change.

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

Build, run `build\bytopsy-protect-test.exe`, and `build\bytopsy-cli.exe --protected` to see which
protected locations exist on your PC. If you add an AI assistant, add it to the list in the README.

To fix a **false alarm**, narrow the rule rather than remove it: an `Allow` for the cache folder, a
`Self` instead of a `Tree`, or `fileOnly` for a name rule. Add an `Expect(..., Protect::None)` for the
path that was wrongly flagged.

You can try any rule without building: a line in `%LOCALAPPDATA%\Bytopsy\protect.txt` (format in the
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

Try it first without building: put the section in `%LOCALAPPDATA%\Bytopsy\cleaners.ini` (menu →
*Cleanup rules…*), click *Measure*, then *Review* to see exactly what it would delete.

`tests/cleanup_test.cpp` checks that the shipped file parses without errors and lists every category
id in order; add your id to that list (and update the count, and any index-based checks after it).
Add the app to the Cleanup paragraph of the README.

## Translate Bytopsy

A translation is one UTF-8 file, `res/lang/<code>.lang`, named after the language code Windows uses
(`de`, `fr`, `pt-BR`, `zh-Hans`, ...). The format is explained at the top of
[res/lang/template.lang](res/lang/template.lang), which lists every text the windowed app shows, each
with the source lines that use it.

1. Start the file: `python -I tools/extract_strings.py --new de --name Deutsch` (Python 3, nothing
   to install). Check `@plural`: left empty, the usual rule for the code is used.
2. Translate line by line; the right side of `=` is yours. Keep every `{0}`, `{1}`, ... (they are
   sizes, names and counts) and move them where your sentence needs them. Leave a line empty to
   show the English text. Keep it short where the English is short (buttons, column headers,
   chips): about a third longer still fits, much more gets cut off with "…".
3. Try it without building: copy the file into a `lang` folder next to `bytopsy.exe`, restart
   Bytopsy and choose it under `⋯` > *Language* (or start it with `BYTOPSY_LANG=de`). Mistakes in
   the file show up as a message at the bottom of the window.
4. Check it: `python -I tools/extract_strings.py` reports placeholders that don't match the English,
   and `build\bytopsy-i18n-test.exe` loads every file in `res/lang`.
5. Send a pull request with the file. It is built into `bytopsy.exe` from then on, and appears in
   the *Language* menu; Windows' display language picks it automatically.

When the code changes, `python -I tools/extract_strings.py --update res/lang/de.lang` brings your
file into the template's order: new texts are added empty, texts the code no longer has move to the
end as comments.

Not translated, on purpose: the terminal app, the journal (`journal.log`), `cleanup.log`, paths,
registry keys, command lines and cleanup rule ids. Numbers and dates already follow the Windows
region settings, so translations don't change them.

### Texts in the code

Every text the windowed app shows goes through `src/i18n.h`:

- `Tr(L"Scan a folder")` for a fixed text; the English text is the key.
- `TrF(L"Moved {0} to the Recycle Bin", size)` with numbered values: never build a sentence by
  adding pieces together, as other languages need a different order.
- `TrN(count, L"{0} item", L"{0} items")` for counts; `{0}` is the formatted count, `{1}`... the
  further arguments.
- `N_(L"...")` marks a text that is stored or compared in English (a name kept in a struct, a
  journal note) and translated with `Tr` where it is shown. The journal and logs are written inside
  an `i18n::EnglishScope`, which keeps them English whatever the language.

Names from the system or from data (paths, file and app names, distro and model names) stay as they
are. Cleaner names and descriptions from `res/cleaners.ini` are translated when shown. After
changing texts, run `python -I tools/extract_strings.py` to regenerate `res/lang/template.lang`;
CI fails when it is out of date, or when a `TrF`/`TrN` call passes fewer values than its text uses.
Check new layouts with `⋯` > *Language* > *Pseudo-language*, which makes every text about 40%
longer and adds accents.

## Releases

Releases are built, tested and (once set up) signed by GitHub Actions; see
[docs/RELEASING.md](docs/RELEASING.md).

## Issues

- **False alarm**: Bytopsy warns about, or blocks, something that is safe to delete.
- **Missing protected folder**: something that holds data you can't get back is not protected.
- **Bug report**: anything else that doesn't work.

`bytopsy-cli --protected` and the shield's tooltip show which rule matched; please include that.
Please don't paste file lists or paths that reveal more about you than the issue needs.
