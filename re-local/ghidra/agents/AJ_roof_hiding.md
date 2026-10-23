# Roof / ceiling / wall hiding in first person - agent AJ, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_G, read-only, no analysis pass, request server
`agents\AJ\AJServer.java`; disassembly with bg3vr_disasm; two small byte scanners `agents\AJ\ripscan.py`,
`qdump.py`). RVAs are relative to 0x140000000. Raw output: `re-local\ghidra\agents\AJ\` (aj1..aj34). Query and
component names come from the exe's own `GetTypeName<...>` strings (script `AJ\QueryName.java`, `AJ\SpecRefs.java`).
"Confirmed" = read in code at the cited address. "Unconfirmed" = not settled by the code read; section 5 settles it.

## Fix table

| # | mechanism | what hides the geometry | how to turn it off in first person | risk | confidence |
|---|-----------|------------------------|------------------------------------|------|------------|
| 1 | **Obstruction fading** (camera-to-character raycast; covers `FadeableObstruction` objects, their `FadeGroup`/`FadeChildren`, FadeTrigger volumes; roofs, ceilings, walls, construction tiles) | per-entity `ls::FadingView0/1Component` opacity. CullStage drops the renderable when the opacity rounds to 0 (0x3E21C70), draws it faded otherwise. | **Detour 0x18CF450** (ecl::obstruction::RaycastSystem body, args rcx = sys, rdx = ctx). While FP: `gs = *(u8**)0x14604B1B8; old = gs[0x1CE]; gs[0x1CE] = 0; call original; gs[0x1CE] = old;`. GlobalSwitches+0x1CE is the switch **"Fading"**. With it 0 the game itself sends a fade-in for every active obstruction fade (0x18D62D0 else-branch) and starts no new ones. Third person: just stop zeroing. | Low. No config write (the byte is restored inside the call; the saver 0x5D22A0 could only see 0 if it ran on another thread during that call). Picking then hits roofs/ceilings again (picking skips objects with opacity <= 0.49, 0x3CFFCA0), which is right for a laser in first person. | Switch, gate and revert path Confirmed. That this is what hides the nautiloid ceiling: Unconfirmed (5.2). |
| 2 | **See-through capsule** (main view only) | shader parameter `SeeThroughEnabled` on materials of visuals with VisualFlags.SeeThrough (0x20000), cut inside a capsule given by the focus CB b7 (`view+0x2C00..`). | **Detour 0x17F2B80** (focus CB build, rcx = view, rdx = recorder). While FP: before the original, write `view[0x2CF8] = 0` and `view[0x2D30] = 0` (the two "active" bytes). Then b7 gets radius 0.0001 at the player position (code path at 0x17F2B80). | Low. The eye view already has zero focus data (R 2), so this only changes the main view. Picking also ignores hits inside this capsule (0x1BBF020); that comes from the ECS state, not the view, so it stays. | CB math Confirmed. What the shader does with b7: Unconfirmed. |
| 3 | **Room-trigger character fade** (NPCs/characters outside the player's room) | `RoomTriggerCharacterFadeView0/1` -> FadingView. | Only if 5.4 shows it: after the producer **0x18BF860** returns, move every key of the add maps of the `RoomTriggerCharacterFadeRequestsSingletonComponent` (view 0 at +0x00, view 1 at +0x40) into its remove sets (+0x80, +0xB0) with the game's set insert 0x140720010, then clear the add maps the way 0x18C0CD0 does. | Medium (writes ECS hash containers). | Producer and layout Confirmed; effect in FP Unconfirmed. |
| 4 | **Room-trigger fade** (the room trigger entities themselves; room mask stage 0x33) | `RoomTriggerFadeView0/1` on `ecl::RoomTrigger` entities. Read by stage 0x33's job 0x1816520 (calls 0x3CFFEC0). | Nothing for now. It fades room trigger entities, not level geometry (5.4 checks). | - | Entities Confirmed (query 0x60A71B0); visual effect Unconfirmed. |
| 5 | Fallback if 1 is not enough | same opacity | Detour **0x3CFFEC0** and **0x3D000A0** (opacity for view 0 / view 1; rcx = visual): while FP, if `visual[+0xF0] & 0x420` (IsWall 0x20 or IsScenery 0x400) return 1.0f. Detour **0x3CFFCA0** (picking "is faded") the same way: return false. | Medium. Also drops scripted/timeline fades of scenery while FP. Shadows and MOC occluders still use the faded value through other readers (0x3CFFCA0 callers 0x3D43D00, 0x189D8C0, 0x3D00280, 0x3D005E0 not decompiled). | Readers Confirmed. |

Not needed: the room filter 0x17E3320 / 0x17E32D0 (R) is not a geometry cull (callers: outline draws 0x17DC550/0x17DC960,
picking 0x1C43E60/0x1C43EE0, gameplay 0x17E32D0 users). Cluster visibility 0x3E647A0 is visual streaming by the camera
frustum, which is our eye camera. FloorTrigger/IsRoof has no link to rendering found (5.5).

## 1. How fading hides geometry (render side, Confirmed)

* CullStage sub-job **0x3E21C70** (called twice from CullStage job 0x3E24970) reads per renderable
  `f = 0x3CFFEC0(visual, fadeSet, 0)` (view index 0) or `0x3D000A0(...)` (index 1; byte `job+0x40`). `fadeSet` is the
  per-player block of `ls::FadingEntitiesForRenderingSingletonComponent` (0x1817500, AC 1.1). **If `f <= 0` or
  `floor(f*16+0.5)` is 0, the renderable is skipped** (not drawn). Else `f` goes into the draw entry (+0x28) for faded
  drawing.
* 0x3CFFEC0 / 0x3D000A0: entity = `visual+0x108`. VisualFlags at `visual+0xF0` (layout matches SE `VisualFlags`;
  0x3B98860 sets bit 0x20000 = SeeThrough from load flag 0x400):
  * bit 0x20 (IsWall): looks up `ls::ConstructionTile` (type 0x60A797C, 0x18D3950) and uses the tile's `+0x30` entity
    (the construction) for the lookup. Roofs, walls and floors built from tile sets fade as one construction.
  * bit 0x100 (IsEffect): effect fade `[0x19F7480(...)+0x48+i*4]`.
  * else: the entity or any parent visual (`+0xC8` chain) that is in the set.
  * value = `ls::FadingView0Component` (type 0x6052FA4, 0x18A04C0) or View1 (0x18A0630). Not in set: 1.0.
* Same reader in stage 0x33 (room masks) job 0x1816520.
* Picking **0x1C43EE0** skips a hit when **0x3CFFCA0** says opacity <= 0.49 (so you pick through faded roofs), when the
  point is inside the see-through capsule (0x1BBF020), and when the room filter 0x17E3320 fails.

## 2. Where the opacity comes from: ecl::FadeResolveSystem (Confirmed)

Body **0x18DC350** (reg 0x723CC0). It takes 11 request singletons and keeps per-type `*FadeView0/1` components, then
combines them into `ls::FadingView0/1Component` (0x18DEB20 / 0x18DFAC0 with query `FadingView0` + all type views;
combine rule not decompiled). Per-type functions (view 0, view 1) and their request getters:

| type | view0 / view1 fn | getter | request writers (producers) |
|------|------------------|--------|-----------------------------|
| BasicGOVisual | 0x18E28C0 / 0x18E2EB0 | 0x18E1BB0 | ecl::FadeLegacyRequestsSystem 0x18DA4C0, 0x18DAD30 |
| CharacterAttachToScene | 0x18E34A0 / 0x18E3A40 | 0x18E1CE0 | 0x18DA7A0, 0x18DAB20 |
| CharacterCreation | 0x18E4030 / 0x18E4620 | 0x18E1E10 | 0x2037670, 0x2039920 |
| CharacterOutOfSight | 0x18E4C10 / 0x18E5200 | 0x18E1F40 | 0x1CA2F30, 0x1CAD270 |
| Crowd | 0x18E57F0 / 0x18E5DE0 | 0x18E2070 | 0x1A8F180, 0x1A8FA60 |
| Invisibility | 0x18E63D0 / 0x18E69C0 | 0x18E21A0 | 0x1A44690, 0x1A49E20 |
| ItemPreview | 0x18E6FB0 / 0x18E75A0 | 0x18E22D0 | 0x1ED7A70, 0x1B50830, 0x1EE84D0 |
| **Obstruction** | 0x18E7B90 / 0x18E8180 | 0x18E2400 | **0x18D62D0** (obstruction FadeSystem) |
| RoomTrigger | 0x18E8770 / 0x18E8D60 | 0x18E2530 | 0x18BED80 (RoomPortalFadeSystem) |
| RoomTriggerCharacter | 0x18E9350 / 0x18E9940 | 0x18E2660 | 0x18BF860 (RoomPortalFadeSystem) |
| Timeline | 0x18E9F30 / 0x18EA520 | 0x18E2790 | 0x1D41CA0, 0x1D6EFB0 |

(Pairing by which getter each function calls first, disassembly; writers from the references to each request
query global, aj9.) Request singleton layout: add map view 0 at +0x00 (keys +0x20, count +0x2C, 16-byte values +0x30),
add map view 1 at +0x40, remove set view 0 at +0x80 (keys +0xA0, count +0xAC), remove set view 1 at +0xB0 (count
+0xDC) (0x18E28C0, 0x18C0CD0, 0x18D62D0). A remove entry makes the type fade back to 1.0 and then drops it
(0x18DD180).

## 3. The obstruction path in detail (Confirmed unless marked)

* `ecl::obstruction::RaycastSystem` (reg 0x723350, cb 0x7566C0 -> **0x18CF450**). System `+0x20` = GlobalSwitches
  (registrar 0x70EC90: `r13 = [0x14604B1B8]` at 0x70F2EF, passed as r9 at 0x70F4DC; not rewritten before).
  0x18CF450: `results+0x81 = (GS[0x1CE] == 0)`, `results+0x80 = GS[0x1CE] && gamestate running`; if `+0x80`, raycasts
  0x18CF7D0 (per split-screen player, 0x18CFAF0 needs GetPlayerView 0x3BAAE20 and the player's character 0x17F7230) and
  0x18D04C0.
* GlobalSwitches+0x1CE is **"Fading"**: the loader 0x5D0360 fills +0x1CE from FixedString global 0x5EED3B0, and the
  static table at exe 0x54F0728 pairs 0x5EED3B0 with the string "Fading" (and 0x5EED3B8 with "ShroudEnabled", which the
  loader puts at +0x1CB). This matches the SE GlobalSwitches order (ShroudEnabled, field_1AC, GodMode, Fading) shifted by
  0x20. Readers of `+0x1CE` in the client: only 0x18CF450 (displacement scan aj23; 0x48FB6B0/0x48FC720/0x3508110 use
  other objects).
* `ecl::obstruction::FadeSystem` (reg 0x7236B0, cb 0x756810 -> **0x18D4AA0**) merges the per-view hit lists only when
  `results+0x80`, then always calls **0x18D62D0**. 0x18D62D0 clears the ObstructionFadeRequests singleton (0x18C0CD0)
  and: Fading on -> new hits become fade-out requests, lost hits fade-in; **Fading off -> every entity with
  ObstructionFadeActiveView0/1 (queries 0x60A7AB4/AB8/ABC/AC0) is put into the remove sets** (+0x80/+0xB0) = fade back
  in. So zeroing the switch restores everything through the game's own path.
* FadeTrigger: `ecl::FadeTriggerSystem` (reg 0x722CE0, body 0x18C5770) maps FadeTrigger volumes to
  `FadeableObstruction` objects (queries 0x60A7598..0x60A75B4) and keeps a spatial set at `sys+0x58`. It writes no fade
  request singleton (none of the 11 request query globals is referenced by it). Whether the raycast uses its set:
  Unconfirmed (0x18CFAF0 too large to settle). Either way its fades can only reach the renderer through one of the 11
  request types above.
* Ray origin (camera vs. something else): Unconfirmed. With the camera at the eyes inside a building, a ray from the
  camera to the character body crossing a FadeTrigger volume would fade that trigger's roof/ceiling group. 5.2 checks.

## 4. See-through (main view only)

* `ecl::SeeThroughSystem` (reg 0x733970, cb 0x75BB80 -> 0x19C7850). Per player (0x19C8230): needs the player's view,
  `[GS+0x69B] != 0 || controller+0x80 != *[0x145FC50B8]` (GS = sys+0x28 = GlobalSwitches, same r13), positive view size,
  0x3CD2E40(). 0x1BBF200 returns a capsule and the hit objects; hit entities get `ls::IsSeeThroughComponent` (tag, type
  0x60AD4F8) and the capsule goes into `ls::SeeThroughStateComponent`. Meaning of GS+0x69B: Unconfirmed (no writer
  found).
* View slot 2 **0x17F2230** copies the capsule into `view+0x2CC8` (own player) / `+0x2D00` (other) only for the
  player's own view (R 2). Record: +0x00 entity, +0x08 radius, +0x0C vec3, +0x24 vec3, +0x30 active byte.
* **0x17F2B80** builds b7: if the active byte (+0x2CF8 / +0x2D30) is 0, b7 c0/c1 = position + 0.0001, radius 0.0001.
  It also uses fixed defaults when `view+0x2961` (cinematic) is set.
* Materials get the parameter "SeeThroughEnabled" (FixedString 0x5FCCE38, via 0x3CE8610) in 0x3B98860 when the
  template flag word has bit 0x400 (SE calls it VisualLoadFlags.SeeThrough); the same code sets visual+0xF0 |= 0x20000.
  What the material shader does with b7: Unconfirmed.

## 5. One-run logging plan

Session: start a game on the nautiloid in first person. Stand still 5 s looking up at the ceiling, 5 s looking ahead,
walk through one door. Then toggle fix 1 on (hotkey), repeat, then fix 2, then both off. Log every frame for 3 s after
each toggle, else every 30th frame. Log thread ids once per hook.

5.1 Switch. In the 0x18CF450 detour: `GS[0x1CE]` before the call, and after the call `res = 0x18CF5D0(rdx)`:
`res[0x80]`, `res[0x81]`, hit counts per view (`*(int*)(res+0x6C)`, `*(int*)(res+0x7C)`; arrays at +0x60/+0x70 of
0x18-byte records, entity at +8, byte +0x10). Expect `res[0x80] = 1` and hits > 0 while the ceiling is gone, and 0 with
fix 1.

5.2 Requests. Detour 0x18DC350 entry (rdx = ctx). For each getter 0x18E2400 (Obstruction), 0x18E2530 (RoomTrigger),
0x18E2660 (RoomTriggerCharacter), 0x18E1BB0 (BasicGOVisual), 0x18E2790 (Timeline): `s = getter(rdx)`; log add counts
`[s+0x2C]`, `[s+0x6C]`, remove counts `[s+0xAC]`, `[s+0xDC]`, and the first 4 add keys with their 16-byte values. A
ceiling entity appearing in the Obstruction adds settles section 3 (ray, FadeTrigger).

5.3 Render. Detour 0x3CFFEC0 and 0x3D000A0 (rcx = visual): call original; when the result < 1.0 log return address
(0x3E21D62/0x3E21D69 = CullStage, 0x1816980/0x1816A11 = room masks), `visual`, `[visual+0xF0]`, `[visual+0x108]`, for
bit 0x20 also `[0x18D3950([visual+0x110], [visual+0x108]) + 0x30]`, and the value. Per frame: number of calls, number
< 1.0, number <= 1/32 (dropped), split by IsWall / IsScenery / other. Expect the ceiling as IsWall or IsScenery with
value 0 before fix 1 and none after.

5.4 Room fades. Same counts as 5.3 but for return addresses in stage 0x33, plus 5.2's RoomTrigger and
RoomTriggerCharacter counts while walking through the door. Non-zero RoomTriggerCharacter adds with NPCs outside the
room = fix 3 is needed.

5.5 Floors and rooms. Per view in the 0x3CC6CB9 hook: `+0x2BF8` room id, `+0x2BFC`, `+0x2BFD`;
`[[[0x146007A28]+0x90]+0xB8]+0x250`. Log whether geometry is missing when the room id changes (stairs). No code links
FloorTrigger to culling (client users 0x1700970, 0x17013E0, 0x18698F0 only build per-trigger records); this checks it.

5.6 See-through. In the 0x17F2B80 detour, per view: `+0x2CC8` (8 B), `+0x2CD0` radius, `+0x2CD4` vec3, `+0x2CEC`
vec3, `+0x2CF8`, `+0x2D30`, and after the original `+0x2C00..+0x2C2C`. If something is missing in only one eye (main),
see-through is the cause.

5.7 Picking. Detour 0x3CFFCA0: per frame count of `true` (hit skipped as faded). Expect 0 on ceilings with fix 1.

## Open leads

* "FadeCharacters" (FixedString init 0x40A80) and "CombatCameraFadeDistance" (0x53BD40): not traced. They may fade
  characters near the camera (own body in first person).
* The combine rule into FadingView (0x18DEB20 / 0x18DFAC0) and the other FadingView readers (0x3D43D00, 0x189D8C0,
  0x3D00280, 0x3D005E0; shadows or occluders?) were not decompiled.
