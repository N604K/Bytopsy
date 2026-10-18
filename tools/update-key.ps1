<#
.SYNOPSIS
  Keys and signed update information (update.json) for Bytopsy's self-update. See docs/RELEASING.md.

.DESCRIPTION
  update-key.ps1 new -Out <file>
      Makes an ECDSA P-256 key pair. Writes the private key (PKCS#8, base64) to <file>, which must
      not exist yet, and never prints it. Prints the public key as the C++ initializer for
      src/updatekey.h, and as base64 (what BYTOPSY_UPDATE_TEST_KEY takes in the local test mode).
      Store the file's content as the GitHub secret BYTOPSY_UPDATE_KEY, then delete the file or
      keep it somewhere offline.

  update-key.ps1 manifest -Version 1.2.0 -Zip dist\bytopsy-1.2.0-x64.zip -Out dist\update-manifest.json
                 [-MinimumVersion 1.1.0] [-Released 2026-10-18] [-Repo N604K/Bytopsy]
      Writes the inner manifest: version, release date, release notes URL, the zip's download URL
      in the release of tag v<Version>, its SHA-256 and size, and optionally the oldest version
      that is still advised.

  update-key.ps1 sign -In dist\update-manifest.json -Out dist\update.json
      Signs the inner manifest's exact bytes with the private key in the environment variable
      BYTOPSY_UPDATE_KEY (base64 PKCS#8) and writes
      {"format":1,"manifest":"<base64>","signature":"<base64 r||s>"}.

  update-key.ps1 verify -In dist\update.json [-Header src\updatekey.h | -PublicKey <base64 X||Y>]
      Checks the signature with the public key that Bytopsy has built in (or the one given), and
      prints the manifest. Fails when it doesn't verify; warns when the header still holds the
      placeholder key.

  Works in PowerShell 7 (as in the release workflow) and Windows PowerShell 5.1.
#>
param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('new', 'manifest', 'sign', 'verify')][string]$Command,
    [string]$Out,
    [string]$In,
    [string]$Version,
    [string]$Zip,
    [string]$MinimumVersion,
    [string]$Released,
    [string]$Repo = 'N604K/Bytopsy',
    [string]$Header,
    [string]$PublicKey
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
Add-Type -AssemblyName System.Security
$Utf8 = New-Object System.Text.UTF8Encoding($false)

# A path relative to the current folder (not the process's, which .NET uses) made absolute.
function Resolve-Full([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path (Get-Location).ProviderPath $Path))
}

function Write-Bytes([string]$Path, [byte[]]$Bytes) { [IO.File]::WriteAllBytes((Resolve-Full $Path), $Bytes) }

function Read-Bytes([string]$Path) { [IO.File]::ReadAllBytes((Resolve-Full $Path)) }

# PKCS#8 bytes -> ECDsa (PowerShell 7: ImportPkcs8PrivateKey; 5.1: CNG).
function Import-PrivateKey([byte[]]$Pkcs8) {
    $k = [System.Security.Cryptography.ECDsa]::Create()
    if ($k.GetType().GetMethod('ImportPkcs8PrivateKey')) {
        $read = 0
        $k.ImportPkcs8PrivateKey($Pkcs8, [ref]$read)
        return $k
    }
    $cng = [System.Security.Cryptography.CngKey]::Import($Pkcs8, [System.Security.Cryptography.CngKeyBlobFormat]::Pkcs8PrivateBlob)
    return New-Object System.Security.Cryptography.ECDsaCng($cng)
}

function Import-PublicKey([byte[]]$XY) {
    if ($XY.Length -ne 64) { throw "A public key is 64 bytes (X then Y), not $($XY.Length)." }
    $p = New-Object System.Security.Cryptography.ECParameters
    $p.Curve = [System.Security.Cryptography.ECCurve+NamedCurves]::nistP256
    $q = New-Object System.Security.Cryptography.ECPoint
    $q.X = [byte[]]$XY[0..31]
    $q.Y = [byte[]]$XY[32..63]
    $p.Q = $q
    try { return [System.Security.Cryptography.ECDsa]::Create($p) } catch { throw 'Not a valid P-256 public key (the point is not on the curve).' }
}

# The 64 bytes of kUpdatePublicKey in src/updatekey.h.
function Read-HeaderKey([string]$Path) {
    $text = [IO.File]::ReadAllText((Resolve-Full $Path))
    $m = [regex]::Match($text, 'kUpdatePublicKey\[64\]\s*=\s*\{([^}]*)\}')
    if (-not $m.Success) { throw "No kUpdatePublicKey[64] = { ... } in $Path." }
    $bytes = [regex]::Matches($m.Groups[1].Value, '0x([0-9A-Fa-f]{2})') | ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) }
    if (@($bytes).Count -ne 64) { throw "kUpdatePublicKey in $Path has $(@($bytes).Count) bytes, not 64." }
    return [byte[]]$bytes
}

switch ($Command) {
    'new' {
        if (-not $Out) { throw 'new needs -Out <file> for the private key.' }
        $full = Resolve-Full $Out
        if (Test-Path -LiteralPath $full) { throw "$full exists already; a key file is never overwritten." }
        $probe = [System.Security.Cryptography.ECDsa]::Create()
        if ($probe.GetType().GetMethod('ExportPkcs8PrivateKey')) {
            $k = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
            $pkcs8 = $k.ExportPkcs8PrivateKey()
        } else {
            $cp = New-Object System.Security.Cryptography.CngKeyCreationParameters
            $cp.ExportPolicy = [System.Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
            # A real null name ($null would become "" here): an ephemeral key, nothing stored in Windows' key storage.
            $cng = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::ECDsaP256, [NullString]::Value, $cp)
            if (-not $cng.IsEphemeral) { $cng.Delete(); throw 'Windows stored the new key instead of keeping it in memory; nothing was written.' }
            $pkcs8 = $cng.Export([System.Security.Cryptography.CngKeyBlobFormat]::Pkcs8PrivateBlob)
            $k = New-Object System.Security.Cryptography.ECDsaCng($cng)
        }
        $q = $k.ExportParameters($false).Q
        $xy = [byte[]]($q.X + $q.Y)
        [IO.File]::WriteAllText($full, [Convert]::ToBase64String($pkcs8), [Text.Encoding]::ASCII)
        "Private key written to $full (keep it secret: GitHub secret BYTOPSY_UPDATE_KEY)."
        ''
        'Public key for src/updatekey.h:'
        'inline constexpr unsigned char kUpdatePublicKey[64] = {'
        for ($i = 0; $i -lt 64; $i += 16) {
            '    ' + ((0..15 | ForEach-Object { '0x{0:x2},' -f $xy[$i + $_] }) -join ' ')
        }
        '};'
        ''
        "Public key, base64 (BYTOPSY_UPDATE_TEST_KEY, verify -PublicKey): $([Convert]::ToBase64String($xy))"
    }
    'manifest' {
        if (-not $Version -or -not $Zip -or -not $Out) { throw 'manifest needs -Version, -Zip and -Out.' }
        if ($Version -notmatch '^\d+(\.\d+){0,3}(-[A-Za-z0-9.]+)?$') { throw "Not a version: $Version" }
        if ($MinimumVersion -and $MinimumVersion -notmatch '^\d+(\.\d+){0,3}(-[A-Za-z0-9.]+)?$') { throw "Not a version: $MinimumVersion" }
        $zipItem = Get-Item -LiteralPath $Zip
        $name = $zipItem.Name
        if ($name -notmatch '^[A-Za-z0-9._+-]+-x64\.zip$') { throw "Unexpected zip name: $name" }
        $sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipItem.FullName).Hash.ToLowerInvariant()
        if (-not $Released) { $Released = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd') }
        $m = [ordered]@{
            version   = $Version
            released  = $Released
            notes_url = "https://github.com/$Repo/releases/tag/v$Version"
        }
        if ($MinimumVersion) { $m.minimum_version = $MinimumVersion }
        $m.assets = @([ordered]@{
                name   = $name
                url    = "https://github.com/$Repo/releases/download/v$Version/$name"
                sha256 = $sha
                size   = [int64]$zipItem.Length
            })
        $json = ConvertTo-Json -InputObject $m -Depth 5
        Write-Bytes $Out $Utf8.GetBytes($json + "`n")
        "Wrote $Out"
        $json
    }
    'sign' {
        if (-not $In -or -not $Out) { throw 'sign needs -In and -Out.' }
        if (-not $env:BYTOPSY_UPDATE_KEY) { throw 'The environment variable BYTOPSY_UPDATE_KEY (the private key, base64 PKCS#8) is not set.' }
        $inner = Read-Bytes $In
        if ($inner.Length -eq 0 -or $inner.Length -gt 60000) { throw "The manifest has $($inner.Length) bytes; Bytopsy reads update.json up to 64 KB." }
        $k = Import-PrivateKey ([Convert]::FromBase64String($env:BYTOPSY_UPDATE_KEY.Trim()))
        # ECDsa.SignData with SHA-256: IEEE P1363 r||s, what BCryptVerifySignature takes.
        $sig = $k.SignData($inner, [System.Security.Cryptography.HashAlgorithmName]::SHA256)
        if ($sig.Length -ne 64) { throw "Unexpected signature length $($sig.Length)." }
        $outer = '{"format":1,"manifest":"' + [Convert]::ToBase64String($inner) + '","signature":"' + [Convert]::ToBase64String($sig) + '"}'
        Write-Bytes $Out $Utf8.GetBytes($outer + "`n")
        "Signed $In -> $Out"
    }
    'verify' {
        if (-not $In) { throw 'verify needs -In.' }
        $doc = [IO.File]::ReadAllText((Resolve-Full $In)) | ConvertFrom-Json
        if ($doc.format -ne 1) { throw "format is $($doc.format), not 1." }
        $inner = [Convert]::FromBase64String($doc.manifest)
        $sig = [Convert]::FromBase64String($doc.signature)
        if ($PublicKey) {
            $xy = [Convert]::FromBase64String($PublicKey)
        } else {
            if (-not $Header) { $Header = Join-Path $PSScriptRoot '..\src\updatekey.h' }
            $xy = Read-HeaderKey $Header
            if (-not ($xy | Where-Object { $_ -ne 0 })) {
                Write-Warning "$Header still holds the placeholder key (all zeros): Bytopsy can't use this update.json until the real public key is there."
                $Utf8.GetString($inner)
                return
            }
        }
        $k = Import-PublicKey $xy
        if (-not $k.VerifyData($inner, $sig, [System.Security.Cryptography.HashAlgorithmName]::SHA256)) {
            throw 'The signature does not verify with this public key: is BYTOPSY_UPDATE_KEY the private half of the key in src/updatekey.h?'
        }
        'Signature OK.'
        $Utf8.GetString($inner)
    }
}
