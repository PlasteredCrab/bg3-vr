# ADR 0002: Native ↔ Lua bridge

**Status:** Accepted for Phases 1–5. Revisit at Phase 6 (combat), when gameplay intents need low latency.

## Context

The native core (camera, rendering, XR, hooks) and the Script Extender Lua mod (game rules, entities, actions) must talk to each other. Phase 0 used a key=value file that Lua writes with `Ext.IO.SaveFile` and the core polls every 250 ms. That's enough for debug commands, but it's one-way and slow.

What each direction carries:

| Direction | Data | Rate | Latency need |
| --- | --- | --- | --- |
| Lua → native | Object addresses (camera controller, visuals), settings, mode changes | Rare | ~1 frame is fine |
| Native → Lua | Gameplay intents (swing hit X, cast at point P, pick up item Y) | A few per second at most | 1–2 frames |
| Native only | Head and hand poses, per-eye matrices, frame submission | Every frame | Must never go through Lua |

Constraints found:
- The Script Extender DLL exports nothing but `DWriteCreateFactory`: no Lua C API, no plugin API (its `UpdaterAPI` is internal to its own updater).
- **The Script Extender auto-updates itself** at game start. Anything that hooks or signature-scans its internals breaks silently on its next update.
- Its Lua can read and write files under the profile's `Script Extender` folder (`Ext.IO.LoadFile` / `SaveFile`), print, and read and write game objects. It can't open shared memory, sockets or pipes.

## Options

| Option | Pros | Cons |
| --- | --- | --- |
| **A. File mailbox** (two files, sequence numbers) | Works today; no dependency on Script Extender internals; survives its updates | Disk API per message (OS-cached, so tens of microseconds); text parsing |
| B. Fork the Script Extender, add a native API module | Direct calls, fastest, typed | Must disable its auto-update and rebase on every release; Commons Clause licence fine for a free mod but adds maintenance |
| C. Hook Script Extender internals (find its `lua_State`, register C functions) | Direct calls without a fork | Breaks on every Script Extender auto-update |
| D. Side channel through game memory (Lua writes a game object field the core watches) | No files | Fragile, obscure, and limited to fields Lua can write |

## Decision

**A: a file mailbox**, upgraded from the Phase 0 polling file:

1. Two files under `Script Extender\BG3VR\`: `to_native.txt` (Lua writes) and `to_lua.txt` (core writes). Each starts with a sequence number; the reader ignores messages it has already seen.
2. The core watches the folder with `ReadDirectoryChangesW` instead of sleeping, so messages arrive within a frame.
3. Lua reads `to_lua.txt` on its `Tick` only while a session is active, with `Ext.IO.LoadFile` (cached by the OS).
4. Per-frame VR data never crosses the bridge.

Keep the message vocabulary small and versioned (`v=1` line) so either side can detect a mismatch.

## Revisit when

- Phase 6 measurements show intent latency or cost is noticeable, or
- the Script Extender gains a public plugin API (watch its release notes).

At that point option B (fork) is the fallback, with auto-update disabled in `ScriptExtenderUpdaterConfig.json` (`DisableUpdates`) and pinned to a tested release.
