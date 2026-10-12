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
| BG3 Script Extender (Norbyte) | Not installed |
| RenderDoc | Not installed |
| Ghidra or IDA | Not installed |
| x64dbg, Cheat Engine | Not installed |
| NVIDIA Nsight Graphics (optional) | Not installed |
| Hooking library (SafetyHook or MinHook) | Not added; needs a vcpkg fetch |

## Done

- [x] Repo, CMake build, presets, unit tests
- [x] `version.dll` proxy that forwards all 17 exports and loads the core ([ADR 0001](decisions/0001-loader.md))
- [x] Core: log file and console, INI config, game exe identification, section map, module-load watcher
- [x] Signature scanner driven by `signatures.txt` (no recompile needed to test an RE find)
- [x] Lua mod skeleton with a `!vr_probe` server console command
- [x] Deploy and uninstall script

## Next steps

1. [ ] Finish the game install. Launch once without mods and reach the main menu.
2. [ ] Confirm the game loads `version.dll` from `bin`: `dumpbin /imports bin\bg3_dx11.exe` and check its dependencies. If it doesn't, switch the proxy (ADR 0001 lists fallbacks).
3. [ ] Record the game build in [offsets.md](offsets.md).
4. [ ] Install the Script Extender from its official GitHub releases, and enable its console.
5. [ ] Deploy (`tools/deploy.ps1 -Config Debug`), launch `bg3_dx11.exe --skip-launcher`, and check `bin\bg3vr\bg3vr.log` for exe, version, sections and module-load order.
6. [ ] Get the Lua mod loading: confirm loose files under `Data\Mods\BG3VR` work, or pack a `.pak`; then run `!vr_probe`.
7. [ ] Capture a DX11 frame in RenderDoc. Map the scene pass, the camera constant buffer (view and projection), the post-FX passes and the UI pass in [re-notes/renderer.md](re-notes/renderer.md).
8. [ ] Find the camera: watch its position change while panning (Cheat Engine or x64dbg), find what writes it, and write a signature into `signatures.txt` until the log reports one unique hit. Notes in [re-notes/camera.md](re-notes/camera.md).
9. [ ] Read the Script Extender source for existing camera and render bindings; this feeds the fork-or-separate decision.
10. [ ] Add a hooking library, hook the camera update, and override its pose from `bg3vr.ini`. **This is the exit gate.**

## Decisions this phase must make

- DX11 or Vulkan first (DX11 assumed)
- Fork the Script Extender, or stay a separate DLL
- Native-to-Lua channel
