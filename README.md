# BG3 VR

First-person, full-body VR for Baldur's Gate 3: IK body, 6DOF hands, physical interaction, wrist UI, physical combat and dice, with D&D rules still deciding outcomes.

- Design document and handoff document: kept privately by the maintainer (ask in an issue if you need them).
- **How to play** (setup, controls, settings): [docs/playing.md](docs/playing.md). First headset test plan: [docs/headset-test-2026-10-20.md](docs/headset-test-2026-10-20.md).
- Current phase: **Phase 1, Stereo viewer**. See [docs/phase-1.md](docs/phase-1.md) (Phase 0 is done: [docs/phase-0.md](docs/phase-0.md)).

## Reverse-engineering notes

What the game's functions and structures are for, with addresses for **bg3_dx11.exe 4.1.1.7631656** (RVAs relative to
0x140000000): [re-local/ghidra/agents/](re-local/ghidra/agents/) (one report per topic: rendering, split screen,
animation, physics, items, UI, camera, LOD, picking, locomotion, weapons, stereo lighting, render resolution, UI
characters, ragdolls, outlines, decals), plus [docs/re-notes/](docs/re-notes/) and [docs/offsets.md](docs/offsets.md).
Raw decompiler output, Ghidra projects and extracted game files are not in the repository.

## Layout

```
config/        default bg3vr.ini and signatures.txt (copied to the game on first deploy)
docs/          phase plans, decisions (ADRs), RE notes, offset table
lua/Mods/BG3VR Script Extender mod: gameplay bridge (Lua)
native/
  proxy/       version.dll proxy: forwards to the system DLL, loads the core
  core/        bg3vr_core.dll: logging, config, module info, signature scanner
  xr/ render/ body/ interact/ bridge/   later phases (see each README)
third_party/   vendored libraries (SafetyHook + Zydis), pinned and digest-checked
data/          toolkit assets (statuses, spells, XAML UI), later phases
tools/         deploy script, RE helpers
tests/         native unit tests, scenario saves
```

## Build

Requires Visual Studio 2026 (C++ desktop workload, MASM; C++23) and CMake 3.28+.

```bash
cmake --preset vs2026
```

```bash
cmake --build --preset debug
```

```bash
ctest --preset debug
```

## Deploy and remove

```bash
powershell -File tools/deploy.ps1 -Config Debug
```

```bash
powershell -File tools/deploy.ps1 -Uninstall
```

Pack the Lua mod into the profile's Mods folder, then enable **BG3 VR** in the in-game Mod Manager (the game owns the load order and rewrites manual edits):

```bash
powershell -File tools/pack-lua.ps1
```

The core writes `<game>\bin\bg3vr\bg3vr.log`. To disable the mod without uninstalling, create an empty file `<game>\bin\bg3vr\disabled`.

## Rules

- Never commit game files or extracted assets (`.gitignore` blocks binaries and paks).
- Every confirmed address goes in [docs/offsets.md](docs/offsets.md) with the game version and its signature.
- Engine-specific code stays behind the adapter layer so a game patch breaks one module, not the mod.

## License

MIT, see [LICENSE](LICENSE). Vendored libraries keep their own licenses ([docs/third-party.md](docs/third-party.md)).

Baldur's Gate 3 is a trademark of Larian Studios. This is an unofficial fan project, not affiliated with or endorsed by
Larian Studios; it contains no game files, and you need your own copy of the game.
