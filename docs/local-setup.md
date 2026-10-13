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

## RE tools

| Tool | Location | Notes |
| --- | --- | --- |
| x64dbg (snapshot 2026-05-27) | `E:\Modding\x64dbg\x64dbg-snapshot_2026-05-27_12-11\release\x64\x64dbg.exe` | Portable; settings stay in its folder |
| RenderDoc 1.46 | `C:\Program Files\RenderDoc` (user's choice) | Set its temporary capture directory to `E:\Modding\Baulders Gate 3 VR\re-local\captures`; its settings file lives in `%APPDATA%\qrenderdoc` |

## Endpoint protection (Sophos Intercept X)

This machine runs Sophos Intercept X; Windows Defender is off. Its HitmanPro.Alert component logs an **HWBGuard** mitigation whenever a process sets CPU debug registers (hardware breakpoints). Our unit test for the write watch did that on every run (15 events on 2026-10-12; nothing was blocked), so that test now runs only with `BG3VR_TEST_HWBP=1`. The in-game `!vr_cam_probe` uses the same technique; it wasn't flagged in the game, but treat it as an occasional tool, not something to leave running. On a managed machine these events may reach the security team.

## Launching the game directly

Starting `bin\bg3_dx11.exe` outside Steam makes the game exit and ask Steam to relaunch it; with arguments (`--skip-launcher`) Steam then shows a confirmation dialog. So an unattended launch needs either Steam's own launch (`steam://run/1086940`) with the launch options set in Steam, or a `steam_appid.txt` (containing `1086940`) in `bin\`, which skips the restart check. The latter is not set up; ask before adding it.
