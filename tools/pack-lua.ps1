<#
.SYNOPSIS
    Packs lua\Mods\BG3VR into BG3VR.pak in the profile's Mods folder and, with -Activate,
    adds it to modsettings.lsx.

.DESCRIPTION
    The game drops loose-file mods from the load order, so the mod ships as a .pak. Loose files
    deployed by tools/deploy.ps1 under <game>\Data\Mods\BG3VR still override the packed copies,
    which keeps the Lua edit loop fast once the .pak is in place (to verify).

    -Activate refuses to run while the game is open, because the game rewrites modsettings.lsx.
#>
param(
    [string]$ProfileDir = "E:\Modding\LarianData\Baldur's Gate 3",
    [string]$Divine = (Join-Path (Split-Path -Parent $PSScriptRoot) "downloads\lslib-v1.20.4\extracted\Packed\Tools\Divine.exe"),
    [switch]$Activate
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo "lua"
$meta = Join-Path $source "Mods\BG3VR\meta.lsx"
$modsDir = Join-Path $ProfileDir "Mods"
$pak = Join-Path $modsDir "BG3VR.pak"
$modSettings = Join-Path $ProfileDir "PlayerProfiles\Public\modsettings.lsx"

if (-not (Test-Path $Divine)) { throw "Divine.exe not found at '$Divine'." }
if (-not (Test-Path $ProfileDir)) { throw "Profile folder '$ProfileDir' not found." }

New-Item -ItemType Directory -Force $modsDir | Out-Null
& $Divine --game bg3 --action create-package --source $source --destination $pak --compression-method lz4 --loglevel warn
if ($LASTEXITCODE -ne 0) { throw "Divine failed with exit code $LASTEXITCODE" }
Write-Host ("packed {0} ({1:N0} bytes)" -f $pak, (Get-Item $pak).Length)

if (-not $Activate) { return }

if (Get-Process | Where-Object { $_.Name -match '^bg3' }) {
    throw "BG3 is running; close it before -Activate (the game rewrites modsettings.lsx)."
}

# Take the module's identity from meta.lsx so the two can't drift apart.
[xml]$metaXml = Get-Content -LiteralPath $meta -Raw
$info = $metaXml.SelectSingleNode("//node[@id='ModuleInfo']")
function Get-MetaAttr([string]$id) { $info.SelectSingleNode("attribute[@id='$id']").value }
$uuid = Get-MetaAttr "UUID"

[xml]$settings = Get-Content -LiteralPath $modSettings -Raw
$mods = $settings.SelectSingleNode("//node[@id='Mods']/children")
if ($mods.SelectSingleNode("node[@id='ModuleShortDesc'][attribute[@id='UUID' and @value='$uuid']]")) {
    Write-Host "BG3VR already in $modSettings"
    return
}

$node = $settings.CreateElement("node")
$node.SetAttribute("id", "ModuleShortDesc")
$fields = [ordered]@{
    Folder         = @("LSString", (Get-MetaAttr "Folder"))
    MD5            = @("LSString", "")
    Name           = @("LSString", (Get-MetaAttr "Name"))
    PublishHandle  = @("uint64", "0")
    UUID           = @("guid", $uuid)
    Version64      = @("int64", (Get-MetaAttr "Version64"))
}
foreach ($id in $fields.Keys) {
    $attr = $settings.CreateElement("attribute")
    $attr.SetAttribute("id", $id)
    $attr.SetAttribute("type", $fields[$id][0])
    $attr.SetAttribute("value", $fields[$id][1])
    [void]$node.AppendChild($attr)
}
[void]$mods.AppendChild($node)

$writerSettings = [Xml.XmlWriterSettings]::new()
$writerSettings.Indent = $true
$writerSettings.IndentChars = "    "
$writerSettings.Encoding = [Text.UTF8Encoding]::new($false)

# Write to a temp file and swap it in only once it parses, so a failure never truncates the
# real file. PSBase: PowerShell exposes the <save> root element as a property that hides Save().
$temp = "$modSettings.tmp"
$writer = [Xml.XmlWriter]::Create($temp, $writerSettings)
try { $settings.PSBase.Save($writer) } finally { $writer.Dispose() }
[void][xml](Get-Content -LiteralPath $temp -Raw)
Copy-Item -LiteralPath $modSettings -Destination "$modSettings.bak" -Force
Move-Item -LiteralPath $temp -Destination $modSettings -Force
Write-Host "added BG3VR to $modSettings (previous version in modsettings.lsx.bak)"
