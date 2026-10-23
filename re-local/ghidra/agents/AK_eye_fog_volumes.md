# Right eye: no distance haze, dark far terrain (and the earlier "missing far geometry") - agent AK, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_A, read-only, -noanalysis, request server
`agents\AK\AKServer.java`, stopped), bg3vr_disasm, the mod's source and its 15:08 log
(`E:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\bg3vr\bg3vr.log`). RVAs are relative to 0x140000000.
Raw output: `re-local\ghidra\agents\AK\` (ak1..ak16). "main" = left eye view, "eye" = the CreatePlayerView view
(right eye). Confirmed = read in code or in the log at the cited place. Unconfirmed = says what settles it.

The brief changed twice while I worked: first "far geometry missing", then "it is lighting / shadow", then "it is the
haze". The newest evidence (geometry drawn in both eyes, haze and lifted blacks missing in the right eye from about
20 m out, darker far terrain, stronger cast shadows) is what the fix table answers. The culling findings are short
(section 5).

## Fix table

| # | what | RVA / where | how | thread | confidence |
|---|------|-------------|-----|--------|------------|
| 1 | **Decide whether the distant-shadow cache is the cause, with no code** | `[0x604AA10]+0x48` (graphics option "distant shadow cache"; 0x3E1A490 copies it into every stage 0x02's +0x241B each frame) | Hotkey: write 0 (both eyes lose the cache), look at the same view, write it back. If the right eye's haze and far-terrain light then match the left eye, the cache is the cause and fix 2 applies. If they still differ, the cache is not the cause: go to the GPU check (section 4). | any (plain byte, read each frame) | Mechanism Confirmed (AE 1.2, 0x3E1C940 gate `cache+0x27B`). The visual result needs the run. |
| 2 | **Pick the cache mode by the GPU check G1, not by guess** | modes in `shadows.cpp` (`[views] eye_distant_cache`) | Both **own** and **left** are self-consistent by code (2.1, 2.2). **off** is the only inconsistent mode (main's textures with the eye's records). **own** is also the only mode whose texture reads match the eye's declared resource bindings (2.3). Run G1: if the eye's index-1 slices hold depth data, use **own**. If they are empty or cleared, use **left** and log G1 for the index-0 textures in the eye's draws. | render thread | Code Confirmed; which mode renders right: Unconfirmed (G1) |
| 3 | If **left** stays the default: give the eye its own parity copy | `shadows.cpp on_execute` | The copy of main's `stage02+0x21D8` into the eye also changes the slice the eye's own tile renderer draws into (0x3E18CE0 reads `stage02+0x21D8` at render time to pick the render target). Harmless while the eye reads index 0, but switching left -> own at run time then reads a half-refreshed cache. Either keep left/own fixed per session, or on a switch to own write `stage02+0x2454` (`cache+0x2B4`, pending tiles) = 0 so the eye's next 0x3E1C940 starts a fresh refresh. | render thread | Confirmed (code) |
| 4 | Fog volumes, DeferredFog gate, other per-view fog inputs | - | **No change.** No per-view or per-player fog-volume gather exists in the density pass. The DeferredFog gate is equal in both views. See section 3. | - | Confirmed (code + 15:10 log) |
| 5 | GPU diagnostic (one frame, both eyes) | `gpu_trace.cpp` | Section 4: G1 cache contents, G2 froxel columns (density, in-scatter, integrated), G3 apply. This settles the cause in one run whatever fix 1 shows. | render thread (D3D11 hooks) | - |

## 1. Why the distant cache matches the symptom (Confirmed mechanism)

* The cache covers everything past the last sun-cascade split. Two readers use it:
  * fog lighting job **0x3E3CCE0** (VolumetricFogLighting): the sun term of every froxel past `cb1[0].w` (last split)
    is multiplied by the cache shadow (AE 1.1). A fully shadowed cache = no sun in-scatter past the last split =
    no warm haze, and the haze also drops on the sky band (DeferredFog adds the integrated last slice to the sky);
  * sun-shadow resolve **0x3E1F520** (stage 0x03): far opaque terrain takes its sun shadow from the cache.
    A fully shadowed cache = dark far terrain with deep black shadows.
* So a bad cache in the eye gives exactly: near objects equal, haze and far light gone from the last split
  outward, darker far terrain (the 14:48 "far wall dark") and a darker horizon band. The split distance comes from
  the atmosphere (`[view+0x28A0]+0x54/+0x58`, T 2.1); "about 20 m" fits a short shadow distance (log it, plan 1).

## 2. Distant cache: what the code does per eye (answers a, b)

### 2.1 (a) Player 2's cache textures ARE rendered by the eye's own stage 0x02 (Confirmed)

* Creation: stage init 0x3E19910 calls **0x3E1C230** (at 0x3E1A0AA). It calls **0x3E1DB10**, which creates and binds
  the keys by `cache+0x27A` (= `stage02+0x241A`, set by the ctor 0x3E194E0 from `[view+0x2268]`, which was 1 when the
  eye was built): 0 -> 0xC0F3544B (2048x2048) / 0x447C4203 (1024x1024), 1 -> 0xB7F464DD / 0x337B7295. The bind is
  `0x40955F0(mgr, stage, key, 0, desc, 0x5230002, 0x5410002, 1, 1, 0)`, i.e. the eye's stage 0x02 owns them. 0x3E1C230
  then makes the render-target sets at `cache+0x18..` (`stage02+0x21B8..`) with 0x4096520 for those keys and the
  tile renderer object `*cache` (0x3E18530(..., 1, 0, 1): a 0x32 renderer with `+0x2150` set).
* Refresh: **0x3E1C940** (called from 0x3E1B0C0 when `[[view+0x10]+0x40] == 0`), gate `cache+0x27B`. When its tile queue
  (`cache+0x2B4`) is empty it queues 252 + 12 + 16 tiles around the camera entity (`[[view+0x21D8]+0x318]`, the same
  entity for both proxies) offset by the view's sun direction `view+0x2A40`, flips the parity `cache+0x38`
  (`stage02+0x21D8`), and writes the records of the new parity. It then always submits the tile renderer.
* Tile cull: job **0x3F7E2F0** (object `stage02+0x2460`, submitted by 0x3E1B0C0 at 0x3E1B8CD when
  `[[view+0x10]+0x40] == 0` and `+0x241B`) pops one tile, culls with 0x3F7DFD0 (mask 0x8200 from the live index 0),
  and appends it to the object's list `+0x598`.
* Tile render: **0x3E18CE0** (the `+0x2150` branch) pops the list (0x3F7DD10) and draws each tile with 0x3E187D0 into
  the render target `[stage02 + 0x21B8 + (parity + level*2)*8]`, parity read **at render time** from `stage02+0x21D8`.
  0x3E187D0 first clears that slice when `stage02+0x2419` is set (keys picked by `+0x241A`).
* Nothing on this path tests the player manager, view registration or the split index, except the cull mask
  (0x8200 = the view-0 bit, the same as main's). So in code the eye's index-1 cache is created, refreshed and
  drawn exactly like main's index-0 cache. Whether its slices hold data at run time: Unconfirmed (G1).

### 2.2 (b) The cache records are world-space (Confirmed)

* Records per parity p and level l: light view at `cache + (2l+1+p)*0x40`, projection at `cache + (2l+5+p)*0x40`,
  origin at `cache + 0x240 + l*0x18 + p*0xC` (writes in 0x3E1C940, reads in 0x3E1D830).
* **0x3E1D830** (lookup) subtracts the camera position passed in (the view's slot 9) from the stored origin and
  rebuilds the translation. The stored data does not depend on which eye wrote it. The only per-view input is
  the sun direction `view+0x2A40`; the 15:10:53 field diff shows main and eye equal to 1 ulp
  (`3EB77BFFBF5FB339` vs `...338`). So copying main's `+0x21E0..+0x240F` into the eye (left mode) is valid.
* Lookup parity: `(parity - 1) & 1`, i.e. the last completed refresh. The tile renderer writes the current parity.

### 2.3 Declared bindings per index (Confirmed; effect Unconfirmed)

The eye's readers declared **index-1** keys when they were set up (the eye's `[view+0x2268]` was 1 then):
* stage 0x03 init (vtable slot 2) **0x3E1EAC0**: at 0x3E1ED69 `movzx eax, [view+0x2268]`, then
  `0x40956C0(mgr, stage, 0xC0F3544B / 0x447C4203)` for 0, the index-1 pair for 1. Unbind 0x3E1F150 the same way.
* fog lighting sub-stage: **0x3E3C740** (called by its init 0x3E3C330 at 0x3E3C5F5, also a vtable slot at 0x58C6558)
  binds by the live index at 0x3E3C82F; 0x3E3CA20 unbinds (0x3E3CA4B).
* At render time the two readers pick textures by the **live** index (0x3E3D11A, 0x3E1F683), which the mod keeps at 0.
  So in **left** and **off** the eye samples index-0 keys its own stages never declared. The resource manager
  re-assigns physical textures to keys every frame (0x4098470; agent AG). Whether an undeclared read can see
  another key's memory: Unconfirmed (0x4098470 not decompiled). **own** has no such mismatch.
* Unconfirmed: whether 0x3E3C740 is ever re-run after setup (slot at 0x58C6558, caller unknown). If it is, the eye's
  fog lighting then declares index 0 instead. Plan 2 logs it.

### 2.4 (c) Other per-index or per-view inputs of the resolve and the lighting

Readers of `[view+0x2268]` in this area (FindDisplacement, R r16): 0x3E1EAC0 / 0x3E1F150 (0x03 bind/unbind, above),
0x3E1F683 (resolve cache keys), 0x3E1C1A6 (0x3E1BB10: two pure key lookups, results unused), 0x3E3C82F / 0x3E3C904 /
0x3E3CA4B (fog lighting bind/unbind), 0x3E3D11A (fog cache keys), 0x3F3CF00 (cluster CB tile window, index added to x),
0x40816E0 (cb9 z), 0x3F7A150 / 0x3F7D7FF (shadow caster cull: per-player fade block `+index*0x30`, index 0 for both
eyes, same as main), 0x3F7DFD0 / 0x3F7D370 / 0x3E00310 (cull masks, 0x200 bit for both). None of the others gives the
eye a different value from main.

Ruled out from the 15:10:53 field diff (Confirmed):
* Froxel distribution `+0x22C0..+0x22EF`: equal (it was zero only in the "after scene" diff, before the eye's first
  camera; 0x3CC70B0 fills it in begin-recording).
* Lighting blend `+0x28E0/+0x28E4`: main 2.0/2.0, eye 0/0. 0x3CC6DFA and 0x3CC8A46 treat duration <= 0 as a finished
  blend (factor 1.0 from 0x591CDE8). Not a cause.
* Fog layers: both `[id 847249953 variant 0 weight 1.000]` (15:10:25).
* DeferredFog gate `+0x2BD1`: byte 1 in both (`+0x2BD0` qword ends `..0101` in both).

## 3. Fog volumes and the density pass (Confirmed)

* Density job **0x3E3ABB0** (sub-stage 0x14). Inputs: the view's fog-layer records (uploaded from sub-stage
  `+0x90..`, count `+0x6C`, only when the dirty byte `job+0x48` is set, 0x3E3ABB0 lines 321-344, set by 0x3E3A650
  when a layer record changed), the ping-pong history 0xE66299BB / 0x9165A92D, two **global** textures
  `DAT_146049EF0 / DAT_146049EF8` and a global `DAT_145EDC298` slot.
* `DAT_146049EF0/EF8` are created once by the sub-stage init 0x3E39C40 through **0x407D350** (texture from file data)
  and freed by 0x3E3A4E0 when the last user goes (refcount `DAT_145FC306A`). They are static textures (noise), shared
  by every view. They are not fog volumes.
* No fog-volume entity list, CullStage bucket, player-manager lookup or split-index branch exists in 0x3E3A650 or
  0x3E3ABB0. Agent Z's "t5 / t17 / t18 = fog volumes" is not supported by this code: the per-view density input is
  the layer list only. Placed mist entities, if any, would be ordinary drawn effects (CullStage), which reach both
  eyes like other geometry.
* DeferredFog (0x3E42130) binds t24 = 0xDA734765 only if sub-stage `+0x58` (copied from `[view+0x2BD1]` at init,
  z13 line 61) is set: 1 in both views.

## 4. GPU diagnostic for gpu_trace.cpp (one armed frame, both views)

The trace already sees PS/CS SRVs, CS UAVs, render targets, viewports, draws, dispatches and the mod's per-view /
per-stage markers. Add three read-backs. Use a 1x1 (2D) or 1x1xN (3D) staging copy with CopySubresourceRegion
through the copy hook's trampoline, Map 2 frames later with DO_NOT_WAIT, log with the view marker.

**G1 - distant cache contents.** Identify the cache textures as the two 2048x2048 and two 1024x1024 2-slice
Texture2D arrays that appear as **render targets** during stage 0x02 of each view (the tile draws of 0x3E18CE0) and as
SRVs in the 0x03 resolve draw and the fog lighting dispatch. Log per view:
* the render-target resource pointers and slice of every tile draw (count draws per view per frame);
* the SRV resource pointers bound at the fog lighting dispatch (CS whose UAV 0 is the 3D lighting volume) and at
  the resolve draw (render target = the screen-sized shadow mask 0x94009344 region);
* once per second, the centre texel and the texel at (0.25, 0.25) of both slices of each cache texture.
Expected if healthy: the eye's tile draws target the same pointers its SRVs read (own) or main's (left), and the
texels are depths (not the clear value). If the eye's index-1 slices hold only the clear value while main's hold
depths, the eye's cache is not filled: use left (fix 2).

**G2 - froxel columns.** At the end of each view's stage 0x1B (after its Gather dispatch), copy one froxel column
from each 3D volume: the density volume (0xE66299BB / 0x9165A92D, UAV of the Initialize dispatch), the lighting
volume (0xBCE29AFA / 0xCBE5AA6C, UAV of the Lighting dispatch) and the integrated volume (0xDA734765, UAV of the
Gather dispatch). Column: x = (viewport.x + viewport.w/2) / 9 (+ split index, 0), y = viewport.h / 2 / 9, all z.
Log z = 0, 5, 10, 20, 30, 44 for both eyes. Reading it:
* density differs -> layers (section 3);
* density equal, in-scatter rgb lower in the eye from some z onward -> the sun term (cache or cascades); compare the
  z where it drops with `stage02+0x2470[count]` (last split) mapped through cb6;
* in-scatter equal, integrated differs -> Gather;
* all equal -> the apply (G3).

**G3 - apply.** At the DeferredFog draw of each view (PS reading t24 = the integrated 3D volume), read the scene
colour texel at the viewport centre before the draw (RT content) and after it. Equal input but different output ->
the apply constants (cb6 rows 23..25, depth); different input -> the lit scene (ClusteredLighting / aerial
perspective, AE 1.1).

## 5. Culling (original brief; the new evidence shows geometry is drawn, so only what was found)

* Each eye has its own Masked Occlusion Culling object `[view+0x48]` (created because the player-view config byte
  `+0x2BD0` is 1; log shows it in both). Camera Enter 0x3CF0080 registers a per-view cull entry (0x3F06CC0) with its
  occluder raster job **0x3F38A40**, which waits on nothing per eye but feeds that view's ctx+0x40 (Confirmed, ak1).
* 0x3F38A40 resizes the MOC every run to the view's own render viewport (`MOC->vf[1](w = [view+0x2180],
  h = [view+0x2184], [frame+0x4308])`, ak1 line 128), then rasterises the occluders with the 4x4 matrix at job
  `+0x40..+0x7F`, the camera position `+0x80` and the planes `+0xC0..`. The writer of those job fields was not found
  (Unconfirmed). If it is the real controller's matrix while CullStage tests with the proxy's view, occlusion would
  differ per eye. Log: in a detour of 0x3F38A40, the job's view `[job+0x18]`, `+0x40..+0x8B`, and the eye's
  PerCamera +0x80 / +0xD0 for comparison.
* CullStage 0x3E246B0 takes the MOC from the camera entity's entry list (0x3B88E40, matched by view), planes from the
  camera entity's component (0x17F3A50 + 8, via 0x3CEE3D0) and the position from the view controller's slot 9 (proxy:
  the eye) (Confirmed, ak1 lines 851-993). LOD bands use the same position and `[[view+0x21D8]+0x14]` (H 2.2), equal
  FOV in both proxies. Texture residency is global (the better level wins, H 4.1), so skipping the eye's stage 0x3E
  cannot drop geometry.

## 6. One-run logging plan

Session: the 15:03 spot (valley, distant mountains), 10 s each step, a log line at each key press.

1. Every 60 frames, render thread, in the existing 0x3CC6CB9 hook, for main and eye: `[0x604AA10]+0x48`, `+0x43`;
   each view's stage 0x02 (`find_stage(view, 2)`): `+0x18`, `+0x241A`, `+0x241B`, `+0x2419`, `+0x21D8`, `+0x2454`
   (pending tiles), `+0x2688`, `+0x2470..+0x248F` (splits); atmosphere `[view+0x28A0]+0x54`, `+0x58`; the shadows.cpp
   mode. Count per view per frame: calls of 0x3E18CE0 with `+0x2150` set and their popped tiles (detour or count
   0x3F7DD10 returns true), and calls of 0x3F7E2F0.
2. Count calls of 0x3E3C740 and 0x3E1EAC0 per view with the live `[view+0x2268]` at entry (once at setup expected).
3. Key A: `[0x604AA10]+0x48` 0 / back (fix 1). Key B: cycle `eye_distant_cache` off / own / left (with fix 3's reset
   when entering own).
4. Arm the gpu_trace G1-G3 once per key state.

Prediction if the cache is the cause: with key A both eyes look the same; G2 shows equal density and the eye's
in-scatter dropping at the last split; G1 shows which cache (index 0 or 1) is empty for the eye.

## Raw files (agents\AK\)

ak1 0x3F38A40 / 0x3F06CC0 / 0x3CF0080 / 0x3E246B0; ak2 0x3D656B0 / 0x3B88E40 / 0x3CEE3D0; ak3 displacement scan
(+0x2454, +0x2B4, +0x27A, +0x241A, +0x2A40); ak4 0x3F7E2F0 / 0x3E1C7D0 / 0x3E1DB10 / 0x3E187D0 / 0x3E1C230; ak5
disassembly of the two cache reads; ak6 tail of 0x3E1BB10; ak7 0x40979D0 / 0x4097AB0; ak8 density job 0x3E3ABB0; ak9 /
ak10 users of the fog globals; ak11 cache list displacements; ak12 0x3E18CE0 / 0x3F7DD10; ak13 other live-index
readers; ak14 callers of the bind functions; ak15 / ak16 lighting blend `+0x28E0`.
