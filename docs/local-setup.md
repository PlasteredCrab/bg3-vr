# Local machine setup

How this dev machine is arranged so BG3 VR work stays on E:. Not needed by other contributors, but every change here can be undone.

| What | Location | How it got there |
| --- | --- | --- |
| Game | `E:\SteamLibrary\steamapps\common\Baldurs Gate 3` | Steam library on E: |
| Repo | `E:\Modding\Baulders Gate 3 VR` | |
| Script Extender payload and logs | `E:\Modding\BG3ScriptExtender\{Cache,Logs}` | `CachePath` / `LogDirectory` in `bin\ScriptExtenderUpdaterConfig.json` and `bin\ScriptExtenderSettings.json`, written by `tools/deploy.ps1 -InstallScriptExtender` |
| BG3 profile data (saves, settings, `modsettings.lsx`) | `E:\Modding\LarianData\Baldur's Gate 3` | Directory junction from `%LOCALAPPDATA%\Larian Studios\Baldur's Gate 3` (created 2026-10-12) |
| Temp and scratch files | `E:\Modding\Baulders Gate 3 VR\scratch` | Git-ignored |

## The profile junction

On 2026-10-12 the profile folder (231 files, about 1.8 GB) was copied to E:, checked file by file with SHA-256, and replaced on C: by a junction. The original was kept as `%LOCALAPPDATA%\Larian Studios\Baldur's Gate 3.bak-20261012`.

To undo, with the game closed:

1. Delete the junction only: `cmd /c rmdir "%LOCALAPPDATA%\Larian Studios\Baldur's Gate 3"` (`rmdir` on a junction removes the link, not the E: data).
2. Copy `E:\Modding\LarianData\Baldur's Gate 3` back to that path, or rename the `.bak-20261012` folder back if nothing new needs keeping.

## Load order

`modsettings.lsx` has the BG3VR entry (UUID `434bc015-dcba-4e13-a8a5-ff2eeee08d8e`) after GustavX. The unedited file is next to it as `modsettings.lsx.orig`. If the game drops the entry because it doesn't recognise a loose-file mod, the fallback is to pack a `.pak`.

## Still on C:

- `%LOCALAPPDATA%\Larian Studios\Launcher`: the Larian Launcher's cache and logs. Launching with `--skip-launcher` stops new writes.
- Steam and Windows themselves.

## Script Extender settings location

The extender reads `ScriptExtenderSettings.json` relative to the working directory: `Launcher\` when the Larian launcher starts the game (`..\bin\bg3_dx11.exe`), `bin\` when the exe is run directly. `tools/deploy.ps1` writes it to both. The updater config is always read from `bin\`.

Loose-file mods under `Data\Mods` are dropped from `modsettings.lsx` at startup (seen 2026-10-12); the Lua mod has to ship as a `.pak`.

The mod's `meta.lsx` must follow the game's current module schema (format 4.8.0.500; compare `Mods/PhotoMode/meta.lsx` in `Data\GustavX.pak`). The first version used the old 4.0.9 schema (`Name` as `FixedString`, `LSWString` fields, `MainMenuLevelName`) and the game ignored the pak entirely: it never appeared in the in-game Mod Manager.
