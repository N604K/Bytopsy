# Releasing Bytopsy

Releases are built by GitHub Actions ([.github/workflows/build.yml](../.github/workflows/build.yml)),
never on a developer's PC, so every published zip comes from a tagged commit that passed the tests.

## What the workflow does

On every push and pull request (`windows-latest`, MSVC x64, Ninja, Release):

1. builds `bytopsy.exe`, `bytopsy-cli.exe` and the self-tests (`cmake --build build`);
2. runs the self-tests (`ctest --test-dir build --output-on-failure`);
3. on tags, signs both programs if code signing is set up (see below);
4. packs both programs, `README.md` and `LICENSE` into
   `bytopsy-<version>-x64.zip` with `bytopsy-<version>-x64.zip.sha256` next to it, and keeps both as
   a workflow artifact. Untagged builds are named `<version>-<commit>`;
5. on tags, writes the signed update information `update.json` for Bytopsy's self-update, if the
   signing key is set up (see [Self-update](#self-update-updatejson)).

On a tag `v*` it also creates a GitHub release with the zip, its `.sha256` file and `update.json`. The tag must
match `BYTOPSY_VERSION_A` in `src/version.h`, or the build fails before anything is published.

## Cutting a release

1. Set the version in `src/version.h` (all three numbers and both strings), in the
   `VERSIONINFO` blocks of `src/bytopsy.rc` and `src/gui/bytopsy-gui.rc`, and in
   `project(Bytopsy VERSION ...)` in `CMakeLists.txt`.
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

What the workflow does once configured, for tags only: it uploads `bytopsy.exe` and
`bytopsy-cli.exe` as a GitHub artifact, submits it with
[signpath/github-action-submit-signing-request](https://github.com/signpath/github-action-submit-signing-request),
waits for the signed files, checks that both carry a valid Authenticode signature, and zips those
instead of the unsigned ones. If signing fails, the release is not created.

To turn it on:

1. The project is MIT-licensed (`LICENSE`), which SignPath Foundation accepts (it requires an
   OSI-approved license).
2. Apply to SignPath Foundation (see <https://signpath.org>) and meet their conditions, among
   them: releases built by this public GitHub workflow, a code signing policy linked from the
   README, and multi-factor authentication for the people who approve signing requests.
3. In SignPath: create the project (slug `Bytopsy`), a signing policy (slug `release-signing`),
   and an artifact configuration from
   [packaging/signpath/artifact-configuration.xml](../packaging/signpath/artifact-configuration.xml);
   link the GitHub repository as a trusted build system.
4. In the GitHub repository settings (*Secrets and variables → Actions*):
   - secret `SIGNPATH_API_TOKEN`: an API token of a SignPath CI user with submitter rights;
   - variable `SIGNPATH_ORGANIZATION_ID`: the SignPath organization id. Setting it is what turns
     signing on;
   - optional variables `SIGNPATH_PROJECT_SLUG`, `SIGNPATH_SIGNING_POLICY_SLUG` and
     `SIGNPATH_ARTIFACT_CONFIGURATION_SLUG` if the slugs differ from `Bytopsy`, `release-signing`
     and `initial`.
5. Push a tag and approve the signing request in SignPath (the workflow waits for it).

Check a signed program with `Get-AuthenticodeSignature bytopsy.exe` (status `Valid`) or in the
file's *Properties → Digital Signatures*.

## Package managers (prepared, not published)

Templates live in `packaging/`: a winget multi-file manifest (schema 1.6, `N604K.Bytopsy`, a zip
with both programs as portable commands `bytopsy` and `bytopsy-cli`), a Scoop manifest (with
`checkver` and `autoupdate` from the GitHub releases; it installs in portable mode and persists
`Bytopsy-data`), and a Chocolatey package (`bytopsy`, downloading the release zip). They use the
repository URL `https://github.com/N604K/Bytopsy` (from `git remote`) and the MIT license.

`packaging/update-manifests.ps1` fills in the version, URL and SHA-256 and writes the results to
`packaging/out/`. The release workflow runs it too and keeps the output as the workflow artifact
`manifests-<version>`.

```powershell
packaging\update-manifests.ps1 -Version 1.0.0        # SHA-256 from the release's .sha256 file
packaging\update-manifests.ps1 -Version 1.0.0 -ZipPath dist\bytopsy-1.0.0-x64.zip
```

Then, per release:

- **winget**: copy `out\winget\manifests\n\N604K\Bytopsy\<version>` into a fork of
  [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs), test with
  `winget validate --manifest <folder>` and `winget install --manifest <folder>`, and open a pull
  request (or use `wingetcreate submit <folder>`).
- **Scoop**: put `out\scoop\bytopsy.json` in a bucket repository (your own, or a pull request to
  an existing one such as *extras*); test with `scoop install .\bytopsy.json`. Afterwards the
  bucket's autoupdate picks up new releases by itself.
- **Chocolatey**: in `out\chocolatey`, run `choco pack`, test with
  `choco install bytopsy -s . -y`, then `choco push bytopsy.<version>.nupkg --source https://push.chocolatey.org/`
  with an API key from community.chocolatey.org. New packages go through moderation.

## Self-update: update.json

Bytopsy can update itself (opt-in, see the README's *Updates* and *Privacy*). It learns about new
versions from `update.json`, which the release workflow attaches to every GitHub release:

```json
{"format":1,"manifest":"<base64 of the inner JSON>","signature":"<base64 of 64 bytes r||s>"}
```

The inner JSON (exact bytes, as signed):

```json
{
  "version": "1.2.0",
  "released": "2026-10-18",
  "notes_url": "https://github.com/N604K/Bytopsy/releases/tag/v1.2.0",
  "minimum_version": "1.1.0",
  "assets": [{"name": "bytopsy-1.2.0-x64.zip",
              "url": "https://github.com/N604K/Bytopsy/releases/download/v1.2.0/bytopsy-1.2.0-x64.zip",
              "sha256": "<64 hex digits>", "size": 3180493}]
}
```

`minimum_version` is optional: copies older than it show the update as important. The signature is
ECDSA P-256 over SHA-256 of the inner JSON's bytes, in IEEE P1363 form (r||s), which is what .NET's
`ECDsa.SignData` writes and what `BCryptVerifySignature` reads. Bytopsy checks it with the public
key in `src/updatekey.h` before it reads anything inside, ignores versions that aren't newer than
its own, and only downloads from `https://github.com/N604K/Bytopsy/releases/download/`. It asks
`https://bytopsy.com/update.json` first and falls back to
`https://github.com/N604K/Bytopsy/releases/latest/download/update.json` (GitHub's "latest release"
download, no API involved).

### The signing key (once)

`tools/update-key.ps1` makes and uses the key (PowerShell 7, or Windows PowerShell 5.1):

```powershell
tools\update-key.ps1 new -Out C:\safe\place\bytopsy-update-key.txt
```

It writes the private key (PKCS#8, base64) to that file, never to the screen, and prints the public
key as the initializer for `src/updatekey.h`. Then:

1. Paste the initializer into `src/updatekey.h` (replacing the all-zero placeholder, which makes
   every update check report "no update key") and commit it. Releases built before this commit
   can't update themselves.
2. In the GitHub repository settings (*Secrets and variables → Actions*), add the secret
   `BYTOPSY_UPDATE_KEY` with the file's content.
3. Keep the file offline (or delete it: a lost key means a new key pair, and copies with the old
   public key then have to be updated by hand once).
4. Optional: the repository variable `BYTOPSY_MINIMUM_VERSION` becomes `minimum_version`.

On each tag the workflow then runs `update-key.ps1 manifest` (version, date, notes URL, zip URL,
SHA-256 and size), `update-key.ps1 sign` (with the secret), and `update-key.ps1 verify` against
`src/updatekey.h`, which fails the build if the secret and the built-in public key don't belong
together. Without the secret the step only warns: the release is published without `update.json`
and installed copies just don't see it.

To sign by hand (the private key comes from the environment, never from the command line):

```powershell
$env:BYTOPSY_UPDATE_KEY = Get-Content C:\safe\place\bytopsy-update-key.txt -Raw
tools\update-key.ps1 manifest -Version 1.2.0 -Zip dist\bytopsy-1.2.0-x64.zip -Out dist\update-manifest.json
tools\update-key.ps1 sign -In dist\update-manifest.json -Out dist\update.json
tools\update-key.ps1 verify -In dist\update.json
Remove-Item Env:BYTOPSY_UPDATE_KEY
```

### Pulling a bad release

Mark the release as a *pre-release* on GitHub (or delete it). `releases/latest` then points to the
previous release, and so does bytopsy.com once its cache expires (5 minutes below). Copies that
already updated stay on the bad version (Bytopsy never installs an older one); publish a fixed,
higher version, with `BYTOPSY_MINIMUM_VERSION` set to it if the bad one must not stay in use.

### bytopsy.com

The site only passes GitHub's file on; it never signs anything, so a compromised site can't push
a download (the signature and the pinned GitHub URLs stop it). With Next.js (App Router),
`app/update.json/route.ts`:

```ts
// Serves the latest release's signed update.json, cached for 5 minutes.
const SOURCE = "https://github.com/N604K/Bytopsy/releases/latest/download/update.json";

export async function GET() {
  const res = await fetch(SOURCE, { next: { revalidate: 300 } });
  if (!res.ok) return new Response("update.json not available", { status: 502 });
  return new Response(await res.text(), {
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "public, max-age=300",
    },
  });
}
```

The body goes out unchanged (any change breaks the signature, and Bytopsy then falls back to
GitHub). On plain hosting, a redirect does the same, since Bytopsy follows HTTPS redirects:

```text
# Netlify _redirects
/update.json  https://github.com/N604K/Bytopsy/releases/latest/download/update.json  302

# Apache .htaccess
Redirect 302 /update.json https://github.com/N604K/Bytopsy/releases/latest/download/update.json
```

### Trying it without the network

`BYTOPSY_UPDATE_URL` set to a local address (`file:///C:/test/update.json`, or
`http://localhost:<port>/update.json`) replaces both addresses; only then does
`BYTOPSY_UPDATE_TEST_KEY` (a public key in base64, as `update-key.ps1 new` prints it) replace the
built-in key, and the zip must be local too (`file:///...`). Any other value is ignored, so the
variable can't point a normal copy somewhere else. Use a throwaway key from `update-key.ps1 new`,
a copy of the programs in a test folder (with `bytopsy.portable`), and
`bytopsy-cli --check-update` or the windowed app's `⋯` > *Check for updates now*.
`build\bytopsy-update-test.exe` does the same with a key it makes itself.

## Checking a download

```powershell
Get-FileHash -Algorithm SHA256 bytopsy-1.0.0-x64.zip
Get-Content bytopsy-1.0.0-x64.zip.sha256
```

The two hashes must be the same. On Linux or Git Bash: `sha256sum -c bytopsy-1.0.0-x64.zip.sha256`.
