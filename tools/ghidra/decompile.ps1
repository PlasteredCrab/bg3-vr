<#
.SYNOPSIS
    Decompiles functions of the game exe with Ghidra, headless, into a text file.

.DESCRIPTION
    The first run imports bg3_dx11.exe into a Ghidra project under re-local\ghidra (no auto-analysis:
    far too slow for this exe; functions are created from the RVAs given). Later runs reuse it.
    Ghidra's home, settings and temp files are kept on E: too.

.EXAMPLE
    tools\ghidra\decompile.ps1 -Rva 0x4096520,0x407A440 -Out re-local\ghidra\fault.c
#>
param(
    [Parameter(Mandatory)] [string[]]$Rva,
    [string]$Out = "re-local\ghidra\decompiled.c",
    [string]$Ghidra = "E:\Ghidra\ghidra_12.0.3_PUBLIC",
    [string]$Exe = "E:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\bg3_dx11.exe"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$work = Join-Path $repo "re-local\ghidra"
$home_dir = Join-Path $work "home"
$temp = Join-Path $work "temp"
New-Item -ItemType Directory -Force $work, $home_dir, $temp | Out-Null

# Everything Ghidra writes goes to E: (user settings, caches, temp).
$env:TEMP = $temp
$env:TMP = $temp
$env:_JAVA_OPTIONS = "-Duser.home=$home_dir -Djava.io.tmpdir=$temp"

$headless = Join-Path $Ghidra "support\analyzeHeadless.bat"
$project = "bg3"
$program = Split-Path -Leaf $Exe
$outPath = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $repo $Out }
$rvas = $Rva | ForEach-Object { $_ -split "," } | Where-Object { $_ }

if (-not (Test-Path (Join-Path $work "$project.gpr"))) {
    Write-Host "importing $program (first run only, a few minutes)"
    & $headless $work $project -import $Exe -noanalysis 2>&1 | Select-Object -Last 5
}
& $headless $work $project -process $program -noanalysis -readOnly `
    -scriptPath (Join-Path $repo "tools\ghidra") -postScript DecompileAt.java $outPath @rvas 2>&1 |
    Select-String -Pattern "ERROR|Exception|DecompileAt" | Select-Object -Last 10
Write-Host "wrote $outPath"
