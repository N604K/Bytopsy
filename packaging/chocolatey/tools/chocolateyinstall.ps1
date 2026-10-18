# Template: packaging/update-manifests.ps1 fills in the version, download URL and SHA-256 (see docs/RELEASING.md).
$ErrorActionPreference = 'Stop'
$toolsDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

$packageArgs = @{
    packageName    = $env:ChocolateyPackageName
    unzipLocation  = $toolsDir
    url64bit       = '__URL__'
    checksum64     = '__SHA256__'
    checksumType64 = 'sha256'
}
Install-ChocolateyZipPackage @packageArgs
# Chocolatey creates the shims bytopsy and bytopsy-cli; bytopsy.exe.gui (shipped next to this
# script) makes the bytopsy shim return at once instead of waiting for the window to close.
