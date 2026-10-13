# Offsets and signatures

Every confirmed address, per game build. The signature is the source of truth; the RVA is only for cross-checking.

## Game builds

| Date checked | Exe | Product version | Steam build | Notes |
| --- | --- | --- | --- | --- |
| 2026-10-12 | bg3_dx11.exe (104,354,368 bytes) | 4.1.1.7631656 | 25605617 | FileVersion is always 1.0.0.0; use ProductVersion |
| 2026-10-12 | bg3.exe (107,018,304 bytes) | 4.1.1.7631656 | 25605617 | Vulkan build |

Script Extender in use: 33.0.0.0 (auto-updated by the v32 updater).

## bg3_dx11.exe 4.1.1.7631656 layout

| Section | RVA | Size |
| --- | --- | --- |
| .text | 0x00001000 | 0x5248EC7 |
| .rdata | 0x0524A000 | 0xC54986 |
| .data | 0x05E9F000 | 0x2B8230 |
| .pdata | 0x06158000 | 0x3BCE98 |
| _RDATA | 0x06515000 | 0x2208 |
| .rsrc | 0x06518000 | 0x23B62 |
| .reloc | 0x0653C000 | 0x78C80 |

## Signatures

| Name | Purpose | Signature (signatures.txt line) | RVA | Build | Found by |
| --- | --- | --- | --- | --- | --- |
| `camera_view_update` | Entry of the camera controller's view update (`rcx` = controller). Pose override writes the camera entity's Transform here. | `48 8B C4 53 48 81 EC A0 00 00 00 0F 57 C0 48 8D 50 A8 0F 11 40 B8 48 8B D9` | DX11 0x4085150; Vulkan 0x409A640 | 4.1.1.7631656 | Disassembly of the probe's call stack, 2026-10-12 |
| `dx11_present_call` | The game's `IDXGISwapChain::Present` call (vtable slot 8) in `rf::dx11::RenderFrame::FlipBuffers`; `rcx` = swapchain. Hook for getting the device and the finished frame without patching DXGI. `rdi` = the RenderFrame (`this`) at the call. | `48 8B 01 FF 50 40 8B F0 8D 88 FB FF 85 77 F7 C1 FD FF FF FF` | DX11 0x4090903 (not in the Vulkan exe) | 4.1.1.7631656 | Static: xref to the string "IDXGISwapChain::Present failed!", 2026-10-13 |
| `camera_set_projection_call` | Call in the projection update that stores the new projection (`rcx` = controller+0x90, `rdx` = matrix, `rbx` = controller). Phase 1 hook for per-eye projections; not hooked yet. | `48 8D 8B 90 00 00 00 48 8D 54 24 70 E8 ?? ?? ?? ?? 48 8B CB C6 43 11 00 \| offset 12` | DX11 0x408541F (match 0x4085413); Vulkan match 0x409A903 | 4.1.1.7631656 | Static: `if (ProjectionDirty) 0x40851F0(this)` pattern, 2026-10-12 |
| `camera_set_view_call` | Call in the camera controller's view update that stores the new view matrix (`rcx` = controller+0x90, `rdx` = matrix, `rbx` = controller). Hook site for camera overrides. | `48 8D 8B 90 00 00 00 48 8D 54 24 60 E8 ?? ?? ?? ?? 48 8B CB C6 43 10 00 \| offset 12` | DX11 0x40851CA (match 0x40851BE); Vulkan match 0x409A6AE | 4.1.1.7631656 | `!vr_cam_probe` write watch, 2026-10-12 |

Related functions in bg3_dx11.exe 4.1.1.7631656 (not hooked; for orientation):

| RVA | What it is |
| --- | --- |
| 0x4085150 | Camera controller view update: builds the view from helpers (0x4084CB0, 0x4084DC0, 0x4084ED0, look-at 0x4084FF0), stores it, clears `ViewDirty`, then calls 0x4086010 (frustum rebuild, probably) |
| 0x408BD60 | Matrix setter: copies the view to `[rcx]`, computes and stores the inverse at `[rcx+0x40]` |
| 0x3CF07C0 | Vtable slot 5 of the gameplay controller (vtable 0x58B6750). Not called during gameplay |
| 0x3CF0740 / 0x3CF0760 | Vtable slots 9 (`GetWorldTranslate`, returns +0x10) and 11/13 (`GetWorldRotate`/`GetWorldTransform`): look up the camera entity's Transform component via `0xE59570([controller+0x320], [controller+0x318])` |
| 0x4084CB0 / 0x4084DC0 / 0x4084ED0 | Rotate +X / +Y / +Z by the Transform's quaternion: the view's right / up / forward rows |
| 0x4084FF0 | Builds the view matrix from right, up, forward and position |
| 0xE59570 | ECS component lookup (world, entity handle) |
| 0x40851F0 | Camera controller projection update: ortho path (with off-centre bounds at +0x70..+0x7C when `OffCenter`) or perspective path (FOV from vtable slot 3, aspect from slot 4, builder 0x4084950); stores via 0x408C030, clears `ProjectionDirty`, calls 0x4086010 |
| 0x4084950 | Perspective builder (out, fov, aspect, near, far): symmetric, left-handed, reversed-Z with a finite far plane: col2 row2 = near/(near-far), col3 row2 = -far*near/(near-far), col2 row3 = 1 |
| 0x408C030 | Projection setter: copies the matrix to `[rcx+0x80]` (controller+0x110), computes the inverse |
| 0x58EDC60 | Base `rf::CameraController` vtable. Transform lives in the object: slot 9 `GetWorldTranslate` = `this+0x34`, slots 11/13 = `this+0x24`; slot 3 GetFov (+0x14), slot 4 GetAspectRatio (+0x20); slot 5 is a transform setter (the Script Extender's slot list doesn't match this build) |
| 0x3E01E00, 0x3E02440, 0x3E18CE0, 0x3E1C940 | Create base-class camera controllers (the 4 extra cameras seen per frame); purpose still open |
| 0x40908A0 | `rf::dx11::RenderFrame::FlipBuffers(this, bool)`: swapchain at `this+0x43A8`; sync interval from `this+0x40` / `+0x43B4`; tearing flag (0x200) unless `+0x43B6`/`+0x44` say otherwise; error path logs "IDXGISwapChain::Present failed!". `this` stays in `rdi` across the Present call. Vtable 0x58EE488 (FlipBuffers is slot 9, called only through it); its other methods include `CreateFrameBufferSurfaces` and `HandleDeviceRemoved`, so this is the per-window output object rather than a per-frame snapshot, and probably holds no view matrix (`headset.cpp` searches it anyway, then the render views `!vr_views` finds). |
| 0x15B6442 | Registers input event `ToggleForceSplitscreen` (id 0x5E0) |
| 0x40CE0 / 0x2137110 | Registers / looks up the named global `ForceSplitscreen` |
| 0x588CB30 | `rf::Visual` vtable: slot 0 GetRTTI (returns 0x1001), 2-11 transform setters, **13 GetCullFlags** (`word [rcx+0xF4]`), 14 SetSomeCullFlag, **15 ToggleCullFlag600(bool set)**. The Script Extender lists 13/15 one slot later. |
| 0x3C6B810 | Visual cull-flag setter (this, mask, set): ORs the mask into the flags at +0xF4 when `set`, otherwise clears it; notifies the scene and recurses into attachments (array at +0xB8, stride 0x30, count +0xC4) |
