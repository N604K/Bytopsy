<#
.SYNOPSIS
Fills the winget, Scoop and Chocolatey manifest templates in packaging\ for one release.

.DESCRIPTION
Replaces __VERSION__, __URL__ and __SHA256__ in the templates and writes the results to
packaging\out (not committed):

  out\winget\manifests\n\N604K\WinNCDU\<version>\   three files for a PR to microsoft/winget-pkgs
  out\scoop\winncdu.json                            for a Scoop bucket
  out\chocolatey\                                   run "choco pack" there, then "choco push"

The SHA-256 comes from -Sha256, from -ZipPath (a local copy of the release zip), or else from the
release's .sha256 file on GitHub (the only case that goes online). See docs\RELEASING.md.

.EXAMPLE
packaging\update-manifests.ps1 -Version 1.0.0
.EXAMPLE
packaging\update-manifests.ps1 -Version 1.0.0 -ZipPath dist\winncdu-1.0.0-x64.zip
#>
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ZipPath,
    [string]$Sha256,
    [string]$OutDir = (Join-Path $PSScriptRoot 'out')
)
$ErrorActionPreference = 'Stop'
# Absolute, so .NET file calls don't depend on the process's current folder.
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)

$repo = 'https://github.com/N604K/WinNCDU'
$Version = $Version.TrimStart('v')
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must look like 1.2.3, not '$Version'" }
$zipName = "winncdu-$Version-x64.zip"
$url = "$repo/releases/download/v$Version/$zipName"

if (-not $Sha256) {
    if ($ZipPath) {
        $Sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ZipPath).Hash
    } else {
        $tmp = [IO.Path]::GetTempFileName()
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "$url.sha256" -OutFile $tmp
            $Sha256 = ((Get-Content -Raw -LiteralPath $tmp).Trim() -split '\s+')[0]
        } finally {
            Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
        }
    }
}
$Sha256 = $Sha256.ToLowerInvariant()
if ($Sha256 -notmatch '^[0-9a-f]{64}$') { throw "Not a SHA-256: '$Sha256'" }

$tokens = [ordered]@{ '__VERSION__' = $Version; '__URL__' = $url; '__SHA256__' = $Sha256 }
$utf8 = New-Object System.Text.UTF8Encoding($false)
$placeholders = @()

function Fill([string]$from, [string]$to) {
    $text = [IO.File]::ReadAllText($from, $utf8)
    foreach ($k in $tokens.Keys) { $text = $text.Replace($k, $tokens[$k]) }
    $text = $text -replace '(?m)^# Template: .*\r?\n', ''
    if ($text -match '__[A-Z0-9]+__') { throw "$from still has a placeholder: $($Matches[0])" }
    if ($text.Contains('LICENSE-PLACEHOLDER')) { $script:placeholders += $to }
    New-Item -ItemType Directory -Force (Split-Path -Parent $to) | Out-Null
    [IO.File]::WriteAllText($to, $text, $utf8)
    $to
}

if (Test-Path -LiteralPath $OutDir) { Remove-Item -Recurse -Force -LiteralPath $OutDir }
$winget = Join-Path $OutDir "winget\manifests\n\N604K\WinNCDU\$Version"
foreach ($f in Get-ChildItem (Join-Path $PSScriptRoot 'winget') -Filter *.yaml) {
    Fill $f.FullName (Join-Path $winget $f.Name)
}
Fill (Join-Path $PSScriptRoot 'scoop\winncdu.json') (Join-Path $OutDir 'scoop\winncdu.json')
$choco = Join-Path $PSScriptRoot 'chocolatey'
foreach ($f in Get-ChildItem $choco -Recurse -File) {
    Fill $f.FullName (Join-Path (Join-Path $OutDir 'chocolatey') $f.FullName.Substring($choco.Length + 1))
}

"version $Version, $url"
"sha256  $Sha256"
if ($placeholders.Count) {
    Write-Warning "No license chosen yet: LICENSE-PLACEHOLDER is still in $($placeholders -join ', '). Set it in the templates before publishing."
}
