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
| 0x588CB30 | `rf::Visual` vtable: slot 0 GetRTTI (returns 0x1001), 2-11 transform setters, **13 GetCullFlags** (`word [rcx+0xF4]`), 14 SetSomeCullFlag, **15 ToggleCullFlag600(bool set)**. The Script Extender lists 13/15 one slot later. |
| 0x3C6B810 | Visual cull-flag setter (this, mask, set): ORs the mask into the flags at +0xF4 when `set`, otherwise clears it; notifies the scene and recurses into attachments (array at +0xB8, stride 0x30, count +0xC4) |
