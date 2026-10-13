<#
.SYNOPSIS
    Decompiles functions of the game exe with Ghidra, headless, into a text file.

.DESCRIPTION
    Uses the project re-local\ghidra\BG3quick: bg3_dx11.exe imported WITHOUT auto-analysis (the full
    analysis lives in project BG3 and takes hours). Each RVA's function is created from its entry and
    decompiled; output is rougher than after analysis but readable. The first run imports (minutes).

.EXAMPLE
    tools\ghidra\decompile.ps1 -Rva 0x4095F50,0x17DCB10 -Out re-local\ghidra\resource.c
#>
param(
    [Parameter(Mandatory)] [string[]]$Rva,
    [string]$Out = "re-local\ghidra\decompiled.c",
    [string]$Exe = "E:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\bg3_dx11.exe"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$headless = Join-Path $PSScriptRoot "headless.ps1"
$outPath = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $repo $Out }
# Unquoted 0x... arguments reach the script already converted to decimal by PowerShell: anything that
# isn't 0x-prefixed is taken as that decimal number. Pass hex with 0x (quoted or not).
$rvas = $Rva | ForEach-Object { $_ -split "," } | Where-Object { $_ } | ForEach-Object {
    if ($_ -match '^0x[0-9a-fA-F]+$') { $_.Substring(2) } else { '{0:X}' -f [uint64]$_ }
}
if (-not (Test-Path (Join-Path $repo "re-local\ghidra\BG3quick.gpr"))) {
    Write-Host "importing $(Split-Path -Leaf $Exe) without analysis (first run only)"
    & $headless -Project BG3quick -GhidraArgs "-import", $Exe, "-noanalysis"
}
& $headless -Project BG3quick -GhidraArgs (@("-process", (Split-Path -Leaf $Exe), "-noanalysis", "-readOnly",
    "-scriptPath", (Join-Path $repo "tools\ghidra"), "-postScript", "DecompileAt.java", $outPath) + $rvas)
Write-Host "wrote $outPath"
