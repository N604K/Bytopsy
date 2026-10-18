# Releasing WinNCDU

Releases are built by GitHub Actions ([.github/workflows/build.yml](../.github/workflows/build.yml)),
never on a developer's PC, so every published zip comes from a tagged commit that passed the tests.

## What the workflow does

On every push and pull request (`windows-latest`, MSVC x64, Ninja, Release):

1. builds `winncdu.exe`, `winncdu-cli.exe` and the self-tests (`cmake --build build`);
2. runs the self-tests (`ctest --test-dir build --output-on-failure`);
3. on tags, signs both programs if code signing is set up (see below);
4. packs both programs and `README.md` (and `LICENSE`, once there is one) into
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

## Code signing (prepared, not active)

Unsigned programs that delete files get SmartScreen warnings and antivirus false positives.
[SignPath Foundation](https://signpath.org) signs open-source projects for free; the workflow is
ready for it but signs nothing until it is configured. Until then releases are unsigned.

What the workflow does once configured, for tags only: it uploads `winncdu.exe` and
`winncdu-cli.exe` as a GitHub artifact, submits it with
[signpath/github-action-submit-signing-request](https://github.com/signpath/github-action-submit-signing-request),
waits for the signed files, checks that both carry a valid Authenticode signature, and zips those
instead of the unsigned ones. If signing fails, the release is not created.

To turn it on:

1. Add a LICENSE: SignPath Foundation only accepts projects under an OSI-approved license.
2. Apply to SignPath Foundation (see <https://signpath.org>) and meet their conditions, among
   them: releases built by this public GitHub workflow, a code signing policy linked from the
   README, and multi-factor authentication for the people who approve signing requests.
3. In SignPath: create the project (slug `WinNCDU`), a signing policy (slug `release-signing`),
   and an artifact configuration from
   [packaging/signpath/artifact-configuration.xml](../packaging/signpath/artifact-configuration.xml);
   link the GitHub repository as a trusted build system.
4. In the GitHub repository settings (*Secrets and variables → Actions*):
   - secret `SIGNPATH_API_TOKEN`: an API token of a SignPath CI user with submitter rights;
   - variable `SIGNPATH_ORGANIZATION_ID`: the SignPath organization id. Setting it is what turns
     signing on;
   - optional variables `SIGNPATH_PROJECT_SLUG`, `SIGNPATH_SIGNING_POLICY_SLUG` and
     `SIGNPATH_ARTIFACT_CONFIGURATION_SLUG` if the slugs differ from `WinNCDU`, `release-signing`
     and `initial`.
5. Push a tag and approve the signing request in SignPath (the workflow waits for it).

Check a signed program with `Get-AuthenticodeSignature winncdu.exe` (status `Valid`) or in the
file's *Properties → Digital Signatures*.

## Package managers (prepared, not published)

Templates live in `packaging/`: a winget multi-file manifest (schema 1.6, `N604K.WinNCDU`, a zip
with both programs as portable commands `winncdu` and `winncdu-cli`), a Scoop manifest (with
`checkver` and `autoupdate` from the GitHub releases; it installs in portable mode and persists
`WinNCDU-data`), and a Chocolatey package (`winncdu`, downloading the release zip). They use the
repository URL `https://github.com/N604K/WinNCDU` (from `git remote`) and contain
`LICENSE-PLACEHOLDER` where the license goes: set it once there is a LICENSE.

`packaging/update-manifests.ps1` fills in the version, URL and SHA-256 and writes the results to
`packaging/out/`. The release workflow runs it too and keeps the output as the workflow artifact
`manifests-<version>`.

```powershell
packaging\update-manifests.ps1 -Version 1.0.0        # SHA-256 from the release's .sha256 file
packaging\update-manifests.ps1 -Version 1.0.0 -ZipPath dist\winncdu-1.0.0-x64.zip
```

Then, per release:

- **winget**: copy `out\winget\manifests\n\N604K\WinNCDU\<version>` into a fork of
  [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs), test with
  `winget validate --manifest <folder>` and `winget install --manifest <folder>`, and open a pull
  request (or use `wingetcreate submit <folder>`).
- **Scoop**: put `out\scoop\winncdu.json` in a bucket repository (your own, or a pull request to
  an existing one such as *extras*); test with `scoop install .\winncdu.json`. Afterwards the
  bucket's autoupdate picks up new releases by itself.
- **Chocolatey**: in `out\chocolatey`, run `choco pack`, test with
  `choco install winncdu -s . -y`, then `choco push winncdu.<version>.nupkg --source https://push.chocolatey.org/`
  with an API key from community.chocolatey.org. New packages go through moderation.

## Checking a download

```powershell
Get-FileHash -Algorithm SHA256 winncdu-1.0.0-x64.zip
Get-Content winncdu-1.0.0-x64.zip.sha256
```

The two hashes must be the same. On Linux or Git Bash: `sha256sum -c winncdu-1.0.0-x64.zip.sha256`.
