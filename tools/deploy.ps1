<#
.SYNOPSIS
    Copies the built BG3 VR files into the game install, or removes them with -Uninstall.

.DESCRIPTION
    Native:  build\vs2026\bin\<Config>\version.dll       -> <game>\bin\version.dll
             build\vs2026\bin\<Config>\bg3vr_core.dll    -> <game>\bin\bg3vr\bg3vr_core.dll (+ .pdb)
             config\bg3vr.ini, config\signatures.txt     -> <game>\bin\bg3vr\  (first deploy only)
    Lua:     lua\Mods\BG3VR                              -> <game>\Data\Mods\BG3VR  (loose files)
    SE:      with -InstallScriptExtender, the verified DWrite.dll from downloads\ -> <game>\bin\DWrite.dll

    Refuses to overwrite a version.dll that is not ours (another mod's proxy) unless -Force.
    -Uninstall never touches the Script Extender.
    To disable the mod without uninstalling, create an empty file <game>\bin\bg3vr\disabled.
#>
param(
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\Baldurs Gate 3",
    [ValidateSet("Debug", "RelWithDebInfo")]
    [string]$Config = "RelWithDebInfo",
    [switch]$Uninstall,
    [switch]$SkipLua,
    [switch]$InstallScriptExtender,
    [string]$ScriptExtenderDll = (Join-Path (Split-Path -Parent $PSScriptRoot) "downloads\bg3se-v32\extracted\DWrite.dll"),
    # Where the Script Extender keeps its downloaded payload and logs (its default is %LOCALAPPDATA% on C:).
    [string]$ScriptExtenderDataDir = "E:\Modding\BG3ScriptExtender",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$gameBin = Join-Path $GameDir "bin"
$vrDir = Join-Path $gameBin "bg3vr"
$proxyTarget = Join-Path $gameBin "version.dll"
$luaSource = Join-Path $repo "lua\Mods\BG3VR"
$luaTarget = Join-Path $GameDir "Data\Mods\BG3VR"

if (-not (Test-Path (Join-Path $gameBin "bg3_dx11.exe")) -and -not (Test-Path (Join-Path $gameBin "bg3.exe"))) {
    throw "No bg3.exe or bg3_dx11.exe in '$gameBin'. Is the game installed? Pass -GameDir if it lives elsewhere."
}

function Test-OurProxy([string]$path) {
    if (-not (Test-Path $path)) { return $true }
    return (Get-Item $path).VersionInfo.ProductName -eq "BG3 VR"
}

if ($InstallScriptExtender -and -not $Uninstall) {
    if (-not (Test-Path $ScriptExtenderDll)) { throw "Script Extender DLL not found at '$ScriptExtenderDll'." }
    if ((Get-Item $ScriptExtenderDll).VersionInfo.ProductName -ne "BG3 Script Extender") {
        throw "'$ScriptExtenderDll' is not the BG3 Script Extender updater."
    }
    $seTarget = Join-Path $gameBin "DWrite.dll"
    if ((Test-Path $seTarget) -and (Get-Item $seTarget).VersionInfo.ProductName -ne "BG3 Script Extender" -and -not $Force) {
        throw "'$seTarget' exists and is not the Script Extender; pass -Force to replace it."
    }
    Copy-Item $ScriptExtenderDll $seTarget -Force
    Write-Host "installed Script Extender updater to $seTarget (it downloads the extender itself on first launch)"

    # Both files are read from the game's bin folder. Written once; later edits are kept.
    $seCache = Join-Path $ScriptExtenderDataDir "Cache"
    $seLogs = Join-Path $ScriptExtenderDataDir "Logs"
    New-Item -ItemType Directory -Force $seCache, $seLogs | Out-Null
    $seConfigs = [ordered]@{
        "ScriptExtenderUpdaterConfig.json" = [ordered]@{ CachePath = $seCache }
        "ScriptExtenderSettings.json"      = [ordered]@{
            CreateConsole    = $true
            DeveloperMode    = $true
            EnableLogging    = $true
            LogDirectory     = $seLogs
            SendCrashReports = $false
        }
    }
    foreach ($name in $seConfigs.Keys) {
        $path = Join-Path $gameBin $name
        if (Test-Path $path) {
            Write-Host "kept existing $path"
        } else {
            # No BOM, and works on Windows PowerShell 5.1 as well as 7.
            [IO.File]::WriteAllText($path, ($seConfigs[$name] | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
            Write-Host "created $path"
        }
    }
}

if ($Uninstall) {
    if (Test-Path $proxyTarget) {
        if (-not (Test-OurProxy $proxyTarget) -and -not $Force) {
            throw "'$proxyTarget' is not the BG3 VR proxy; leaving it alone (use -Force to remove anyway)."
        }
        Remove-Item $proxyTarget
        Write-Host "removed $proxyTarget"
    }
    foreach ($name in "bg3vr_core.dll", "bg3vr_core.pdb", "version.pdb") {
        $path = Join-Path $vrDir $name
        if (Test-Path $path) { Remove-Item $path; Write-Host "removed $path" }
    }
    Write-Host "kept $vrDir (config, signatures, logs); delete it by hand if you want a clean slate"
    if ((Test-Path $luaTarget) -and -not $SkipLua) {
        Remove-Item $luaTarget -Recurse
        Write-Host "removed $luaTarget"
    }
    return
}

$buildBin = Join-Path $repo "build\vs2026\bin\$Config"
$proxySource = Join-Path $buildBin "version.dll"
$coreSource = Join-Path $buildBin "bg3vr_core.dll"
foreach ($file in $proxySource, $coreSource) {
    if (-not (Test-Path $file)) { throw "Missing '$file'. Build first: cmake --build --preset $(if ($Config -eq 'Debug') { 'debug' } else { 'release' })" }
}

if (-not (Test-OurProxy $proxyTarget) -and -not $Force) {
    throw "'$proxyTarget' belongs to another mod. Remove it or pass -Force to replace it."
}

New-Item -ItemType Directory -Force $vrDir | Out-Null
Copy-Item $proxySource $proxyTarget -Force
Copy-Item $coreSource $vrDir -Force
foreach ($pdb in "bg3vr_core.pdb", "version.pdb") {
    $path = Join-Path $buildBin $pdb
    if (Test-Path $path) { Copy-Item $path $vrDir -Force }
}
Write-Host "deployed native ($Config) to $gameBin"

foreach ($name in "bg3vr.ini", "signatures.txt") {
    $target = Join-Path $vrDir $name
    if (-not (Test-Path $target)) {
        Copy-Item (Join-Path $repo "config\$name") $target
        Write-Host "created $target"
    }
}

if (-not $SkipLua) {
    if (Test-Path $luaTarget) { Remove-Item $luaTarget -Recurse }
    New-Item -ItemType Directory -Force (Split-Path $luaTarget) | Out-Null
    Copy-Item $luaSource $luaTarget -Recurse
    Write-Host "deployed Lua mod to $luaTarget"
}
