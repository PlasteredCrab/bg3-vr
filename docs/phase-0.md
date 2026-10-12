# Phase 0: Groundwork

**Exit gate:** move the game camera to any pose from code.

Phase 0 answers the design doc's first spike: can we control the render camera's view and projection every frame? Everything else waits on that answer.

## Environment (checked 2026-10-12)

| Item | Status |
| --- | --- |
| Visual Studio 2026, MSVC 14.50, MASM | Installed |
| CMake 4.3, Git, Python 3.10 | Installed |
| SteamVR (active OpenXR runtime) | Installed |
| Baldur's Gate 3 | Downloading in Steam |
| BG3 Script Extender (Norbyte) | v32 updater downloaded and SHA-256 verified; installs once the game is in place |
| SafetyHook v0.7.0 (+ Zydis 4.0.0) | Vendored in `third_party/safetyhook`, digest verified |
| RenderDoc | Not installed |
| Ghidra or IDA | Not installed |
| x64dbg, Cheat Engine | Not installed |
| NVIDIA Nsight Graphics (optional) | Not installed |

## Done

- [x] Repo, CMake build, presets, unit tests
- [x] `version.dll` proxy that forwards all 17 exports and loads the core ([ADR 0001](decisions/0001-loader.md))
- [x] Core: log file and console, INI config, game exe identification, section map, module-load watcher
- [x] Signature scanner driven by `signatures.txt` (no recompile needed to test an RE find)
- [x] Lua mod skeleton with a `!vr_probe` server console command
- [x] Deploy and uninstall script
- [x] SafetyHook vendored; `hooks::install(signature, detour)` helper with a passing unit test (C++23 now required)
- [x] Script Extender v32 updater downloaded to `downloads/bg3se-v32` (git-ignored)

## Next steps

1. [x] Finish the game install and reach the main menu (DX11).
2. [x] Confirm the game loads `version.dll` from `bin`. Both exes import it directly (see ADR 0001); runtime confirmation comes with step 5.
3. [x] Record the game build in [offsets.md](offsets.md): 4.1.1.7631656, Steam build 25605617.
4. [x] Install the Script Extender with `tools/deploy.ps1 -Config Debug -InstallScriptExtender`. This also writes `ScriptExtenderUpdaterConfig.json` and `ScriptExtenderSettings.json` into `bin`, so its payload cache and logs go to `E:\Modding\BG3ScriptExtender` instead of `%LOCALAPPDATA%`, with the console on and crash reports off.
5. [x] Deploy, launch `bg3_dx11.exe`, and check `bin\bg3vr\bg3vr.log`. First run (2026-10-12): game 4.1.1.7631656, image size 0x65B5000, `.text` 0x5248EC7 bytes; `dxgi`, `d3d11` and `DWrite` already loaded when the core starts; no separate Noesis DLL (UI is linked into the exe). Script Extender 33.0.0.0 downloaded to and ran from `E:\Modding\BG3ScriptExtender\Cache`.
6. [x] Get the Lua mod loading. It ships as `BG3VR.pak` in the profile's `Mods` folder (on E: via the junction, see [local-setup.md](local-setup.md)), enabled in the in-game Mod Manager. `!vr_probe` in the server console prints the host character's transform (first run 2026-10-12).
    - [ ] Still to confirm: loose Lua under `Data\Mods\BG3VR\ScriptExtender` overrides the packed copy (fast edit loop): confirm loose files under `Data\Mods\BG3VR` work, or pack a `.pak`; then run `!vr_probe`.
7. [ ] Capture a DX11 frame in RenderDoc. Map the scene pass, the camera constant buffer (view and projection), the post-FX passes and the UI pass in [re-notes/renderer.md](re-notes/renderer.md).
8. [x] Found via the Script Extender's layout plus in-game probes instead of manual debugging (see [re-notes/camera.md](re-notes/camera.md)). Original plan: Find the camera: watch its position change while panning (Cheat Engine or x64dbg), find what writes it, and write a signature into `signatures.txt` until the log reports one unique hit. Notes in [re-notes/camera.md](re-notes/camera.md).
9. [x] Read the Script Extender source for existing camera and render bindings; this feeds the fork-or-separate decision.
10. [x] **Exit gate met 2026-10-12.** `!vr_cam_set` moves the gameplay camera to any pose: a mid-hook on the view setter call (`camera_set_view_call`) swaps in our view matrix. Pitch sign confirmed (negative looks down); characters face their rotation's −Z, so `here` adds 180°.
    - Known gaps, carried into Phase 1: the game's orbit camera keeps running underneath, so mouse drag and scroll still change things, and **world lighting changes when the orbit camera moves**. Some systems (shadows, light clustering, fog or roof fading are suspects) follow the orbit camera, not our matrix. More than one controller passes through the setter each frame; the core now logs each one. **This is the exit gate.**

## Decisions this phase must make

- DX11 or Vulkan first (DX11 assumed)
- Fork the Script Extender, or stay a separate DLL
- Native-to-Lua channel
