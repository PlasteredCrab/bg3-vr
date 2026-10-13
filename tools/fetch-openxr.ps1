<#
.SYNOPSIS
    Downloads the official OpenXR loader package (KhronosGroup/OpenXR-SDK on GitHub) into
    downloads\openxr-<version>, checks its SHA-256 against the pinned value, and extracts it.

.DESCRIPTION
    CMake picks it up from downloads\openxr-<version>\extracted (BG3VR_OPENXR_SDK). The package is a
    NuGet zip with include\openxr\*.h and native\x64\release\{lib,bin}\openxr_loader.*.
    Needs the GitHub CLI (gh). Re-running is a no-op once the package is in place.
#>
param(
    [string]$Version = "1.1.63",
    [string]$Sha256 = "4e5a50a8807ef66f25180ff224e7d8150b594aa8ee4b07590f9ade55a8e98703",
    # Signing key named in the official release notes (Khronos OpenXR specification editor).
    [string]$SigningKey = "45207B2B1E53E1F2755FF63CC5A2D593A61DBC9D",
    [string]$Gpg = "$env:LOCALAPPDATA\Programs\Git\usr\bin\gpg.exe"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $repo "downloads\openxr-$Version"
$package = Join-Path $dir "OpenXR.Loader.$Version.nupkg"
$extracted = Join-Path $dir "extracted"

if (Test-Path (Join-Path $extracted "include\openxr\openxr.h")) {
    Write-Host "OpenXR $Version already present in $extracted"
    return
}

New-Item -ItemType Directory -Force $dir | Out-Null
if (-not (Test-Path $package)) {
    gh release download "release-$Version" -R KhronosGroup/OpenXR-SDK -p "OpenXR.Loader.$Version.nupkg" -D $dir
}
$actual = (Get-FileHash $package -Algorithm SHA256).Hash.ToLower()
if ($actual -ne $Sha256) {
    throw "SHA-256 mismatch for $package (got $actual)"
}
Write-Host "sha256 OK"

# PGP: the release's .asc must be a valid signature by a subkey of the pinned primary key.
# Uses a throwaway keyring under scratch\ (Git for Windows' gpg wants /e/... style paths).
if (Test-Path $Gpg) {
    $signature = "$package.asc"
    if (-not (Test-Path $signature)) {
        gh release download "release-$Version" -R KhronosGroup/OpenXR-SDK -p "OpenXR.Loader.$Version.nupkg.asc" -D $dir
    }
    $keyFile = Join-Path $dir "signing-key-$SigningKey.asc"
    if (-not (Test-Path $keyFile)) {
        Invoke-WebRequest -Uri "https://keys.openpgp.org/vks/v1/by-fingerprint/$SigningKey" -OutFile $keyFile -UseBasicParsing
    }
    $toMsys = { param($p) "/" + $p.Substring(0, 1).ToLower() + ($p.Substring(2) -replace '\\', '/') }
    $keyring = Join-Path $repo "scratch\gnupg-openxr"
    New-Item -ItemType Directory -Force $keyring | Out-Null
    # gpg reports progress on stderr; with "Stop", Windows PowerShell 5.1 would treat that as an error.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Gpg --homedir (& $toMsys $keyring) --batch --quiet --import (& $toMsys $keyFile) 2>$null
    $status = & $Gpg --homedir (& $toMsys $keyring) --batch --status-fd 1 --verify (& $toMsys $signature) (& $toMsys $package) 2>$null
    $ErrorActionPreference = $previous
    if (-not ($status -match "^\[GNUPG:\] VALIDSIG .* $SigningKey$")) {
        throw "PGP signature check failed for $package"
    }
    Write-Host "PGP signature OK (primary key $SigningKey)"
} else {
    Write-Warning "gpg not found at $Gpg; skipped the PGP signature check"
}

# A .nupkg is a zip; Expand-Archive only accepts the .zip extension.
$zip = Join-Path $dir "package.zip"
Copy-Item $package $zip -Force
Expand-Archive $zip -DestinationPath $extracted -Force
Remove-Item $zip
Write-Host "extracted to $extracted"
