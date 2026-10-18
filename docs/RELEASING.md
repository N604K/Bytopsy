# Releasing WinNCDU

Releases are built by GitHub Actions ([.github/workflows/build.yml](../.github/workflows/build.yml)),
never on a developer's PC, so every published zip comes from a tagged commit that passed the tests.

## What the workflow does

On every push and pull request (`windows-latest`, MSVC x64, Ninja, Release):

1. builds `winncdu.exe`, `winncdu-cli.exe` and the self-tests (`cmake --build build`);
2. runs the self-tests (`ctest --test-dir build --output-on-failure`);
3. packs both programs and `README.md` (and `LICENSE`, once there is one) into
   `winncdu-<version>-x64.zip` with `winncdu-<version>-x64.zip.sha256` next to it, and keeps both as
   a workflow artifact. Untagged builds are named `<version>-<commit>`.

On a tag `v*` it also creates a GitHub release with the zip and its `.sha256` file. The tag must
match `WINNCDU_VERSION_A` in `src/version.h`, or the build fails before anything is published.

## Cutting a release

1. Set the version in `src/version.h` (all three numbers and both strings), in the
   `VERSIONINFO` blocks of `src/winncdu.rc` and `src/gui/winncdu-gui.rc`, and in
   `project(WinNCDU VERSION ...)` in `CMakeLists.txt`.
2. Commit, then tag and push:

   ```bat
   git tag v1.0.0
   git push origin v1.0.0
   ```

3. Wait for the *Build* workflow; the release appears under *Releases* with generated notes. Edit
   the notes if needed.

## Checking a download

```powershell
Get-FileHash -Algorithm SHA256 winncdu-1.0.0-x64.zip
Get-Content winncdu-1.0.0-x64.zip.sha256
```

The two hashes must be the same. On Linux or Git Bash: `sha256sum -c winncdu-1.0.0-x64.zip.sha256`.
