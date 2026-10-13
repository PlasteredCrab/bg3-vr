# Phase 0: Groundwork

**Status: complete (2026-10-12).** Exit gate: move the game camera to any pose from code. Met, then improved to take over the camera at its source.

## Environment

| Item | Status |
| --- | --- |
| Visual Studio 2026, MSVC 14.50, MASM | Installed |
| CMake 4.3, Git, Python 3.10, .NET 8 | Installed |
| SteamVR (active OpenXR runtime) | Installed |
| Baldur's Gate 3 4.1.1.7631656 (Steam build 25605617) | Installed on E:, run with DX11 |
| BG3 Script Extender | v32 updater, auto-updated to 33.0.0.0; cache and logs on E: |
| SafetyHook v0.7.0 (+ Zydis 4.0.0) | Vendored in `third_party/safetyhook` |
| LSLib v1.20.4 (Divine) | `downloads/lslib-v1.20.4`, used by `tools/pack-lua.ps1` |
| x64dbg, RenderDoc 1.46 | Installed (see [local-setup.md](local-setup.md)) |

## What was built

- Repo, CMake presets, unit tests (`ctest`), vendored SafetyHook.
- `version.dll` proxy that forwards all 17 exports and loads the core ([ADR 0001](decisions/0001-loader.md)); verified against both game exes.
- Core: log, INI config, game exe and version identification, module-load watcher, `signatures.txt` scanner, hooking helper.
- In-game reverse-engineering probes: vtable call counting and a hardware write watch with call stacks (`!vr_cam_probe`).
- `bg3vr_disasm`: offline disassembly, signatures, cross-references, string search, vtable listing.
- Lua mod shipped as `BG3VR.pak` (enabled in the in-game Mod Manager); loose Lua in `Data\Mods\BG3VR\ScriptExtender` overrides it, so `deploy.ps1 -LuaOnly` and `reset client` reload Lua without a repack.
- Deploy, uninstall and pack scripts; everything on E: ([local-setup.md](local-setup.md)).

## Steps

1. [x] Game installed; DX11 build at the main menu.
2. [x] Both exes import `VERSION.dll` directly; the proxy loads ([ADR 0001](decisions/0001-loader.md)).
3. [x] Game build recorded in [offsets.md](offsets.md).
4. [x] Script Extender installed with its cache, logs and settings on E: (settings needed in both `bin\` and `Launcher\`).
5. [x] First runtime log: exe, version, sections, module-load order.
6. [x] Lua mod loads from a `.pak` (after rewriting `meta.lsx` on the current module schema); `!vr_probe` reads the host transform; loose Lua overrides the pak.
7. [ ] RenderDoc capture: **moved to Phase 1** (step 6 there).
8. [x] Camera found through the Script Extender's layout plus in-game probes ([re-notes/camera.md](re-notes/camera.md)).
9. [x] Script Extender source read; it informed the bridge decision ([ADR 0002](decisions/0002-native-lua-bridge.md)).
10. [x] **Exit gate.** First by replacing the view matrix at the setter; then (approach B) by writing the pose into the camera entity's Transform at the start of the view update, so view, frustum, culling and every system that asks the camera where it is agree. The game rebuilds exactly our view (max error about 1e-4), and mouse drag and scroll no longer move the held camera.

## Extras done in Phase 0

- Characters face their rotation's −Z; `!vr_cam_set here` adds 180°.
- Head and hair hidden from the main view (shadow kept) by calling the game's own `ToggleCullFlag600` on those visuals (`!vr_head hide|show`). The Script Extender's vtable slot numbers were off by one in this build, and the flags turned out to be an inclusion mask; both are confirmed by disassembly.

## Decisions

| Question | Decision |
| --- | --- |
| DX11 or Vulkan first | DX11. Every signature so far is also unique in the Vulkan exe, so the port should be cheap. |
| Fork the Script Extender or stay separate | Separate. It auto-updates and has no plugin API; nothing so far has needed its internals. |
| Native ↔ Lua channel | File mailbox ([ADR 0002](decisions/0002-native-lua-bridge.md)), revisit at Phase 6. |

## Carried into Phase 1

- RenderDoc capture of a frame.
- Identify the four extra cameras the view update sees every frame (the core now logs their projection type and pose).
- Whether lighting still shifts with the orbit camera under the source-level override. Not reported since approach B; check.
- Small performance hit seen with the head hidden; the check is now throttled and timed. Measure with the optimized build.
