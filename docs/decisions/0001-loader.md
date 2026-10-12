# ADR 0001: Load through a version.dll proxy

**Status:** Accepted for Phase 0. Revisit after the Script Extender fork decision.

## Context

The native core must be inside the game process early, before the renderer is created. Options:

| Option | Pros | Cons |
| --- | --- | --- |
| Proxy DLL in `bin` | Standalone, no launcher, loads before most game code | Must pick a DLL the game actually loads |
| Built into a Script Extender fork | Reuses its loader and engine bindings | Couples us to its release cycle; licence and maintainer questions |
| External injector | No file in the game folder | Extra step every launch; timing races |

The Script Extender already uses a DLL proxy for its own loader (`bin\DWrite.dll`, confirmed from its v32 release), so we must not take the same name.

## Decision

Ship `version.dll`: it forwards all 17 exports to `System32\version.dll` through jump stubs (`native/proxy/version_stubs.asm`), and inside `bg3.exe` or `bg3_dx11.exe` it loads `bin\bg3vr\bg3vr_core.dll` on a worker thread.

- The proxy stays tiny and never changes; all logic lives in the core.
- An empty `bin\bg3vr\disabled` file turns the mod off without uninstalling.
- The proxy carries ProductName "BG3 VR" so the deploy script won't overwrite another mod's `version.dll`.

## Verification (game 4.1.1.7631656, checked 2026-10-12)

- [x] `bg3_dx11.exe` and `bg3.exe` both import `VERSION.dll` directly (`GetFileVersionInfoW`, `GetFileVersionInfoSizeW`, `VerQueryValueA`).
- [x] `version` is not in this machine's KnownDLLs, and both exes have Dependent Load Flag 0, so the loader searches the exe folder first and finds the proxy.
- [x] Both exes also import `DWrite.dll` directly, which is why the Script Extender uses that name. Ours does not collide.
- [x] `dxgi.dll` is delay-loaded; `d3d11.dll` is a direct import of the DX11 build only. The Vulkan build imports no Vulkan DLL, so it loads `vulkan-1.dll` at runtime.
- [x] Both exes use Control Flow Guard (guard flags 0x100). It doesn't affect the proxy's jump stubs; keep it in mind for hooks that redirect indirect calls.
- [x] Runtime: `bg3vr.log` appears after launching `bg3_dx11.exe`. The core initialises about 15 ms after start-up, by which point `dxgi.dll`, `d3d11.dll` and the Script Extender's `DWrite.dll` are already loaded. The proxy and the Script Extender coexist.
- [ ] No other popular BG3 mod already ships a `version.dll`.

Fallbacks if the runtime check fails: `dbghelp.dll` (also a direct import), then `winmm.dll` or `dinput8.dll`.
