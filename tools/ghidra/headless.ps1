<#
.SYNOPSIS
    Runs Ghidra's headless analyzer on the BG3 project with enough memory, temp files on E:.

.DESCRIPTION
    Calls Ghidra's launch.bat directly (analyzeHeadless.bat hard-codes 2 GB, too little for
    bg3_dx11.exe) with -MaxMem, and points TEMP/TMP at re-local\ghidra\temp. Ghidra's per-user
    settings stay in %APPDATA%\ghidra, where the user's Ghidra keeps them. -GhidraArgs go to AnalyzeHeadless
    after the project location.

.EXAMPLE
    # Import and auto-analyze (once; long):
    tools\ghidra\headless.ps1 -GhidraArgs "-import","E:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\bg3_dx11.exe"
    # Serve MCP for an analyzed program (stays running):
    tools\ghidra\headless.ps1 -GhidraArgs "-process","bg3_dx11.exe","-noanalysis","-preScript","GAMCPStartServerScript.java","host=127.0.0.1","port=8080","wait=true"
#>
param(
    [string]$Ghidra = "E:\Ghidra\ghidra_12.0.3_PUBLIC",
    [string]$MaxMem = "16G",
    [Parameter(Mandatory)] [string[]]$GhidraArgs
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$work = Join-Path $repo "re-local\ghidra"
$temp = Join-Path $work "temp"
New-Item -ItemType Directory -Force $work, $temp | Out-Null
$env:TEMP = $temp
$env:TMP = $temp
# Ghidra's caches go under %LOCALAPPDATA%\ghidra; keep them on E: as well.
$env:LOCALAPPDATA = Join-Path $work "localappdata"
New-Item -ItemType Directory -Force $env:LOCALAPPDATA | Out-Null
Remove-Item Env:_JAVA_OPTIONS -ErrorAction SilentlyContinue # its "Picked up" line breaks launch.bat's Java check

$launch = Join-Path $Ghidra "support\launch.bat"
$vmargs = "-XX:ParallelGCThreads=4 -XX:CICompilerCount=2"
& $launch fg jdk Ghidra-Headless $MaxMem $vmargs ghidra.app.util.headless.AnalyzeHeadless $work BG3 @GhidraArgs
exit $LASTEXITCODE
