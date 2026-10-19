# Skeleton, bones and the head bone (4.1.1.7631656)

Source: agent C's static RE (`re-local/ghidra/agents/C_animation_systems.md`, sections 1.3-1.7 and 2), with
the parts the native code depends on re-checked with `bg3vr_disasm`. Code: `native/core/src/skeleton.cpp`.

## Confirmed by disassembly (re-checked 2026-10-19)

| What | Where | Evidence |
|---|---|---|
| Skeleton vtable | exe+0x588D4A8 | agent C (ctor 0x3A1EDD0) |
| Slot 10 `bool GetBoneWorldMatrix(Skeleton*, int16 bone, float out[16])` | 0x3A1F170 | `test dx,dx; js fail` (negative bone fails), reads `[rcx+0xE0]` else `[rcx+0xF8]`, `GetWorldPose4x4` 0x4A11C40, copies 64 bytes to `r8`, `mov al,1` |
| Slot 18 bone name | 0x3A1FB90 | `[rcx+0xD8]` = granny skeleton; `cmp dx,[r8+8]` (bone count); `imul 0xA4`; `mov rax,[r8+0xC]` (bones); returns `[bone+0]` (char*) |
| Slot 19 parent index | 0x3A1FBC0 | same addressing, `movzx eax, word [bone+8]` |
| Slot 1 pose fresh | 0x3A207A0 | `[0x614FB88] - 1 <= [rcx+0x124]`: the game loop's frame counter vs. the skeleton's pose stamp |
| Slot 29 swap | 0x3A1FBE0 | swaps `+0xE0` and `+0xE8` |

Matrix layout of world-pose entries: row-major, rows 0-2 are the bone's axes, translation in elements 12-14
(agent C, 0x3A1F4A0 / 0x3A1FFB0). Poses are in model space (relative to the skeleton root): every
BuildWorldPose call site passes a NULL offset.

## From the Script Extender's layout (checked at run time by the logs)

- `Visual+0x88` SkeletonSlot* array, `Visual+0x91` count; slot `{+0 FixedString remapper slot (-1 =
  default), +8 Skeleton*}`. Agent C saw 0x3C52E10 / 0x3C6B900 / 0x3C6CCC0 use them this way. The code accepts
  a slot only if its object has the Skeleton vtable.
- `Visual+0x08` world Transform (quat, translate, scale). Not disassembly-checked. The skeleton log prints it
  next to the character position from the follow log, so the two can be compared.

## The probe

`!vr_skel` (or `!vr_go`, which sends it too) hands the controlled character's root visual to the core. On the
next game frame it:

1. checks that Skeleton vtable slot 10 still points to 0x3A1F170 (refuses otherwise);
2. finds the default skeleton slot, logs every bone as `index:name<parent`, then the ones whose names
   suggest an IK chain (head, neck, spine, clavicle, arm, hand, finger...);
3. every game frame reads `Head_M` through slot 10 and composes it with the visual's world transform;
   every 300 reads it logs model and world position, rotation, the visual transform and the 3x3 rows.

Things to check in the first log:
- the bone list and the real names of the arm chain (they are data, not in the exe);
- whether `Head_M` model Y is about the character's eye height (game units are metres);
- whether the world head position sits above the follow log's character position;
- `pose frame` vs `frame`: whether the head is read after this frame's animation (the frame callback is the
  camera's view update; ordering against the animation jobs is unconfirmed).

## Discrepancy to remember

Agent C reads the main-scene gather masks 0x220 / 0x420 as excluding visuals with view bit 0x200 / 0x400.
Our live test (cull.cpp, 2026-10-12) found the 0x600 bits act as an inclusion mask: clearing them hides a
visual from the main view while its shadow stays. The live result stands. The gather probably selects
objects that *have* the view bit.
