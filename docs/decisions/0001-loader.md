# ADR 0001: Load through a version.dll proxy

**Status:** Accepted for Phase 0. Revisit after the Script Extender fork decision.

## Context

The native core must be inside the game process early, before the renderer is created. Options:

| Option | Pros | Cons |
| --- | --- | --- |
| Proxy DLL in `bin` | Standalone, no launcher, loads before most game code | Must pick a DLL the game actually loads |
| Built into a Script Extender fork | Reuses its loader and engine bindings | Couples us to its release cycle; licence and maintainer questions |
| External injector | No file in the game folder | Extra step every launch; timing races |

The Script Extender already uses a DLL proxy for its own loader, so we must not take the same name.

## Decision

Ship `version.dll`: it forwards all 17 exports to `System32\version.dll` through jump stubs (`native/proxy/version_stubs.asm`), and inside `bg3.exe` or `bg3_dx11.exe` it loads `bin\bg3vr\bg3vr_core.dll` on a worker thread.

- The proxy stays tiny and never changes; all logic lives in the core.
- An empty `bin\bg3vr\disabled` file turns the mod off without uninstalling.
- The proxy carries ProductName "BG3 VR" so the deploy script won't overwrite another mod's `version.dll`.

## To verify

- The game, or one of its early dependencies, loads `version.dll`. Fallbacks if not: `winmm.dll`, `dinput8.dll`, `dbghelp.dll`.
- No other popular BG3 mod already ships a `version.dll`.
