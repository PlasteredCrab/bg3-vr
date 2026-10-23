# Decals at the wrong depth in one eye - agent S, 2026-10-22

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_A, read-only, headless, request server
`agents\S\SServer.java`) plus the game's DX11 shader bytecode. RVAs are relative to 0x140000000. Raw output is in
`re-local\ghidra\agents\S\` (s1..s32). The shaders were extracted read-only with the project's LSLib Divine:
engine decals, LinearizeDepth and ExtractNormal from `Data\EngineShaders.pak` into `S\shaders\`, and the game's
decal and surface materials from `Data\Materials.pak` into `S\shaders_mat\`. They were disassembled with
d3dcompiler_47 D3DDisassemble (`S\dis_bshd.py`) into `S\shaders_asm\` and `S\shaders_mat_asm\`. As in N's report,
cbN[i] means constant buffer bN, 16-byte row i.

## Hook points for the VR mod

| need | RVA | how to use | confidence |
|------|-----|-----------|------------|
| **Fix A: make cb11[13] and the PerView translation match the camera-relative origin that every draw actually uses** | existing hook **0x3CC6CB9** (`on_camera_built`, render thread, before the upload) | For the eye views, call `rewrite_block` with the eye offset's **rotation only** (`offset.position = {0,0,0}`, after the IPD scaling). Then PerCamera +0x00 keeps the head's translation, +0xD0 stays the game's slot-9 position, `rewrite_view_block` derives PerView +0x60 = head, and the 0x1B and 0x40 hooks follow automatically. Equivalent minimal patch: after `rewrite_block`, restore +0xD0..+0xDB of both blocks to the game's value. | Confirmed (code and shaders) that this makes surfaces and world-mapped decals consistent with what is rasterised. That it is the observed artefact: Unconfirmed (log below) |
| Fix B: real per-eye stereo (proper fix, replaces A) | GameRenderView begin-recording entry **0x17F21E0** (slot 1, render thread, per view, after frame sync 0x4072CB0 and before 0x3CC6C00) | Put a per-eye proxy controller into ReadState `[view+0x21D8]`. The proxy is a copy of the gameplay controller with its own vtable copy, where slot 9 (+0x48) returns the eye position. Set its +0x90 view / +0xD0 inverse view to the eye's. Then every camera-relative CPU path and cb11[13] use the eye. Keep the mod's projection rewrite; drop its translation part. | Mechanism Confirmed (all readers go through `[view+0x21D8]` slot 9). Design and side effects Unconfirmed |
| Camera-relative origin per view (logging) | controller slot 9: `call [[ctrl]+0x48](ctrl)`, ctrl = `[view+0x21D8]`; gameplay controller = **0x3CF0740** (camera entity's ECS Transform +0x10) | Log the value per eye view at 0x3CC6CB9, next to PerCamera +0xD0 before and after the rewrite. Calling slot 9 here is safe: 0x3CC7490 does the same thing on the same thread just before. | Confirmed (code) |
| Decal draw (logging) | **0x3CEE9A0** (rdx = cull entry, `[rdx]` = decal object; r8 = render context: `[r8+0x10]` view, `[r8+0x30]` controller) | Count calls per view. The decal's camera-relative world translation is object position minus `[[r8+0x30]]` slot 9. | Confirmed |
| Decal per-object CB (logging) | **0x3CEEE20**(ctx, obj, world, axes, tiling, p6, p7, p8) | Not needed for the fix. Writes InvWorldOffsetMatrix = inverse(camera-relative world) and NormalMatrix. | Confirmed |
| Surface (blood/grease/water pool) draw (logging) | **0x1B7AA80** (rcx = render context, `[rcx+0x10]` view; r8 = surface render object) | Count calls per view. It builds its screen-covering box from controller +0x150 (inverse projection, game FOV) and +0xD0 (inverse view rotation). | Confirmed |
| LinearizeDepth | 0x3E2C960 (stage 0x0B, per view) | No change needed (near/far only, offset 0) | Confirmed |

## 1. Cause

### 1.1 The camera-relative origin is the controller's position (head), for every draw (Confirmed)

All per-draw positions are camera-relative. The origin is the **render context's controller slot 9**, read live in the
stage jobs (render worker threads), where the context's +0x30 = `[view+0x21D8]` (ReadState controller):

* Decals: 0x3CEE9A0. `world = 0x3C5E8B0(obj)` (object transform), axes times the decal size `obj+0x10C/+0x110/+0x114`,
  then `translation -= *(vec3*)([ctx+0x30]->slot9())` (s11 lines 67-90). It passes this to 0x3CEEE20.
* Every mesh draw: 0x3C5F050 -> 0x3C609E0 -> 0x3C60BD0: `obj->vtbl[+0xC0](obj, out, ctx[6]->slot9())`. 0x3C60AE0
  (+0xC0, shared by all renderable vtables) subtracts the position from the world translation (s24, s25, s11).
* Instance buffers: 0x3CD87B0 (A 1.4), 0x3CD8BB0, 0x3CD9120 (s19).
* Contexts: decal jobs 0x3EBBB50/0x3EBC0C0 `local_b8 = [view+0x21D8]` (s1). Opaque G-buffer job 0x3E2C3B0 `local_b8 =
  view[0x43B]` = +0x21D8 (s15). Stage 0x3B job 0x17F1B40 the same (s30).
* The gameplay controller's slot 9 = 0x3CF0740: `ECS_Transform(entity +0x318, world +0x320) + 0x10` (s27). The
  same object is used by both eye views (views.cpp assigns `g_controller` to the eye view via SetCamera), so **both
  eyes get the same origin, the head**.
* GPU side: all vertex shaders do `clip = cb11[8..11] * p_rel` and `view ray = cb11[0..2].xyz * p_rel` (decal VS,
  surface VS, outline VS; no cb11[3] term). cb11[8..11] = PerCamera +0x80 = P x view **rotation** (game 0x3CC7490
  and the mod's `rotation_view_projection` both drop the translation).

Consequence (Confirmed by the code above): no eye translation enters any vertex position. Both eye views rasterise
geometry, depth and decal boxes **from the head position**, each with its eye's rotation and asymmetric FOV. The
eye offset reaches the GPU only through:
* PerCamera +0x00 translation (cb11[3]; no vertex shader reads it);
* **PerCamera +0xD0 (cb11[13])**, which the mod sets to `camera_position(eye view)` (stereo.cpp `rewrite_block`);
* PerView +0x60 (cb12[6]), which `rewrite_view_block` sets to the same eye position;
* the stage 0x1B / 0x40 hooks (full P x V_eye, +0x22F0 = eye position).

In the unmodified game, +0xD0 is the same slot-9 value (0x3CC7490: `param_3[0x1a] = *slot9()`, N r1_begin.c 513),
so `cb11[13]` is exactly the camera-relative origin. The mod breaks that rule: cb11[13] = eye, origin = head.

### 1.2 The decal and surface shaders add cb11[13] to a head-relative position (Confirmed)

Deferred decal / surface pixel shaders (Engine_Default_Decal_Deferred_ST_DEF, Base_Decal_ST_DEF,
Surface_Default_ST_DEF, VFX_Decal_*):
```
uv   = SV_Position.xy * cb10[0].zw                 // b10 = pass CB of RT 0x4C0D1B61 (1/size)
mask = t37 (0x5BE0FB7A, extracted normal; discard where the surface doesn't receive decals)
lin  = t30 (0xFF193183, linear depth = viewZ / far)
rel  = cb12[3..5] (inverse view rotation) * ( v1 * cb11[12].y / v1.z * lin )   // v1 = cb11[0..2]*p_rel from the VS
```
`rel` is the surface point relative to the point the depth and the box were rasterised from (the head, 1.1). Then:
* **Box projection** (engine decals, Base_Decal): `decal = cb0[4..7] * rel`. cb0[4..7] = InvWorldOffsetMatrix =
  inverse of the head-relative decal matrix (0x3CEEE20, s18 lines 193-266). This is consistent, so **box-projected
  decals stay on the surface**.
* **Surfaces** (BG3's blood/grease/water/fire pools; Surface_Default_ST_DEF lines 104-125, stage 0x3B in stage
  0x46): `world = rel + cb11[13]`, then `cell = (world.xz - cb1[0].xz) / cellSize`. The grid texture t0 (gather) and
  the height texture t1 (`*1310.7 + cb1[0].y` vs world.y) decide coverage and discard. With cb11[13] = eye the
  whole pool shape is sampled at `world + (eye - head)`.
* **World-mapped VFX decals** (scorch marks `VFX_Decal_Deferred_AlphaBlend_Base_Burning*`: `r1.zyzx +
  cb11[13].xzxy`; `..._AlphaErosion_01`: `(rel + cb11[13]).xz / cb1[3].w` -> erosion texture;
  `GradientMap_SphereMask`, `Glow_Circle_WorldMask`, `PolarUV_*`, `Caustic`, `World_SCL_Env`, ...) and engine
  `..._WSTile_*`: texture coordinates from `rel + cb11[13]`, so the same offset.
* Base_Decal's focus fade (cb7 capsule) uses `rel + cb11[13]` too.
* The same applies outside decals: ClusteredLighting `world = ray*depth + cb11[13]` (N 1.4), so light and shadow
  positions get the same offset.

Effect (derived from the above): in each eye the pattern of a surface or world-mapped decal is shifted by `-(eye -
head)` = +/-IPD/2 (times world scale) along the head's right axis. The surface under it has no such shift. The two
eyes get opposite shifts, so the pattern gets a crossed disparity of about IPD/D relative to its surface and is seen
at a different depth. That matches "blood, scorch marks, ground decals" (R: the "shadow" artefact was blood
decals). Box-projected decals with a plain texture should not show it. That they look fine in game is Unconfirmed.

### 1.3 Inputs checked and found per-eye or identical (Confirmed)

| input | where computed | per-eye? |
|-------|----------------|----------|
| decal world / InvWorldOffsetMatrix / NormalMatrix / tiling / params (per-object CB) | 0x3CEE9A0 -> 0x3CEEE20, in each view's stage 0x46 job, from the object and the context's controller slot 9 | same for both views (shared controller); consistent with the geometry |
| engine param offsets (DecalWorld 0x5FCD888, InvWorldOffsetMatrix 0x5FCD8A8, NormalMatrix 0x5FCD8B0, +0x890/+0x898/+0x8A0) | shader param table, 0x3CD5910 (load) | static per shader |
| cb11 (PerCamera) rows 0-2, 8-12 | 0x3CC7490 + mod rewrite | eye rotation/FOV, consistent |
| cb12 rows 3-5 (inverse rotation) | mod `rewrite_view_block` | consistent |
| **cb11[13] (PerCamera +0xD0)** | mod `rewrite_block` = eye position | **eye, but the origin is the head: inconsistent** |
| t30 linear depth 0xFF193183 | stage 0x0B, 0x3E2C960: own viewport rect, CB = {near/far terms from controller +0x18/+0x1C, cb0[0].zw = 0, cb0[1].x = controller +0x12} | per view; the mod keeps the projection's z terms, so consistent |
| t37 extracted normal 0x5BE0FB7A | stage 0x0B, 0x3E2CC60 (DeferredDecal_ExtractNormal: G-buffer normal where bit 3 of the receive flags is set) | per view |
| b10 (1/RT size), b9 viewport | 0x3EBBB50 binds RT 0x4C0D1B61's CB; 0x3AECC70 viewport | per view |
| cached / first-frame state | stage 0x46 prepare 0x3AECC10, slots 0x3AECE90/0x3AECE20, jobs 0x3EBBB50/0x3EBC0C0, 0x3CEE9A0, 0x3CEEE20 write no stage fields | none |

Decal bucketing (0x3E220C0, s9): objects whose type (vtable slot 0) is 0xB (decal vtable **0x58B6420**,
draw slot +0xA0 = 0x3CEE9A0) with flag bit 16 go to `m_DeferredDecalObjects` (+0x2048) or, when byte `+0x108` is
set, to `m_DeferredGameDecalObjects` (+0x20B8). With bit 15 they go to the emissive buckets +0x1F68/+0x1FD8. All use
the same draw.

### 1.4 Secondary finding: surfaces are drawn in a game-FOV box (Confirmed code, effect Unconfirmed)

0x1B7AA80 builds the surface "decal world" from controller +0x150..+0x18F (inverse projection: the game's symmetric
FOV, not the eye's) and +0xD0..+0xF8 (inverse view rotation), at depth min(controller +0x18, 0.95). The box covers
only the game-FOV frustum. If the headset FOV is wider, surfaces can be cut off near the edges of each eye image.
The fix is N's item 4 (a controller FOV/aspect that encloses both eyes). It is unrelated to the depth error.

## 2. Fix

### Fix A (now; makes decals and surfaces sit on the surface as rendered)

In `on_camera_built` (render thread, 0x3CC6CB9, before the upload, both blocks), drop the translation from the eye
offset:
```
offset.position = {0.0f, 0.0f, 0.0f};   // after the IPD scaling; keep offset.orientation
rewrite_block(block0, offset, fov, false);
rewrite_block(block1, offset, fov, true);
rewrite_views_of(view);
```
Result: +0x00 = [R_eye | head translation], +0xD0 = head = slot 9 (what the game wrote), PerView +0x60 = head, stage
0x1B VP and stage 0x40 +0x22F0 = head. All of them match the origin of the vertex data. That fixes the surface
grid lookup, world-mapped decals, WSTile, focus fades, and lighting/fog positions. Put it behind a config switch
(e.g. `[views] eye_translation = false`) for the A/B test below.

This does not add stereo parallax, because geometry has none today (1.1).

### Fix B (proper stereo; then Fix A is reverted)

Do **not** put the eye translation into PerCamera +0x80 column 3. The decal and surface vertex shaders compute the
view ray as `cb11[0..2]*p_rel` without translation. With the depth rendered from the eye and the rays from the head,
`rel` would be wrong for every deferred decal and surface.

The origin itself must be the eye, per view. Every reader goes through the view's ReadState controller slot 9:
0x3C60BD0, 0x3CD87B0, 0x3CD8BB0, 0x3CD9120, 0x3CEE9A0, 0x3F4C7C0, 0x3E13520 (via ctx+0x30), 0x3CC7490, 0x3E24970
(cull), 0x3D961D0, 0x3E092E0, 0x3E1B0C0, 0x3E3CCE0, 0x3E54C00, 0x3DFC660, 0x1816520 (via `[view+0x21D8]`)
(s19/s28/s29).

Proposal (Unconfirmed):
1. Hook 0x17F21E0 entry (render thread, per view, after 0x4072CB0 copied WriteState +0x2260 to ReadState +0x21D8).
2. For an eye view, refresh a per-eye proxy: copy the real controller (at least +0x000..+0x327: fields used by the
   render code are +0x10..+0x20, +0x90..+0x18F and +0x318/+0x320; exact object size not decompiled).
3. Give the proxy a copied vtable (0x58B6750) whose slot 9 returns the proxy's stored eye position.
4. Write the eye view (+0x90) and its inverse (+0xD0) into the proxy, then store the proxy in `[view+0x21D8]`.
5. The next frame sync restores the real pointer.

The mod's 0x3CC6CB9 rewrite then only replaces the projection. The 0x1B/0x40 hooks become unnecessary, since they
read the proxy. Risks to check: other vtable slots called on the ReadState controller during rendering; code that
compares the controller pointer (the MOC lookup 0x3E246B0 is by view; Enter/Exit are not called).

## 3. One-run confirmation log

Every 60th frame (plus the first 4), for both eye views:
1. At 0x3CC6CB9: `ctrl = [view+0x21D8]`, `pos9 = *(vec3*)ctrl->vtbl[9](ctrl)`, PerCamera[0]/[1] +0xD0 before and
   after the rewrite, +0xB0..+0xBF (column 3 of +0x80) after, and the eye offset.
   Prediction: same `ctrl` and `pos9` for both views; game +0xD0 == pos9; after the rewrite, +0xD0 - pos9 = +/-IPD/2
   along the head right axis; column 3 has no eye term.
2. Detours (count per view, `[ctx+0x10]`): 0x3CEE9A0 (r8 = ctx), 0x1B7AA80 (rcx = ctx), plus the decal object's
   byte +0x108 for the first call. This shows which path draws the misplaced decals (surfaces vs decal objects).
3. A/B: toggle Fix A with a key while looking at a blood pool, a scorch mark and a placed ground decal. Prediction:
   with Fix A, pools and scorch marks sit on the ground in both eyes; placed (box-projected) decals look the same in
   both modes.
4. Parallax sanity check (predicted by 1.1): with Fix A on and off, a near object (hand-height crate) shows no
   left/right shift against the background other than the FOV difference. If it does shift, 1.1 is wrong; log
   `pos9` inside 0x3C60BD0 (ctx = r9 param_5, `[ctx+0x30]`) per view.

## Raw files

s1 decal jobs, s2 draw path, s4/s6 engine param registration and table load, s9 cull bucketing, s11 decal object
methods, s13/s15/s17 stage 0x0B (LinearizeDepth 0x3E2C960, ExtractNormal 0x3E2CC60), s18 decal CB 0x3CEEE20 and
world 0x3C5E8B0, s24/s25 mesh draw and object CB, s26/s27 controller vtables and slot 9, s19/s28/s29 slot-9 readers,
s30/s31 stage 0x3B surfaces, s32 stage 0x46 prepare. Shader listings: `shaders_asm\`, `shaders_mat_asm\`.

## Open leads

* The exact cb0 row <-> engine-param mapping (rows 12/13 = tiling/params) comes from each shader's runtime offset
  table. It is inferred from content, not read.
* Names of the decal params DAT_145FCD890/898/8A0 (registration fns 0x50C200/0x50C060/0x50C2D0/0x50C3A0 not
  decompiled).
* Stage 0x1B's reprojection consumes P x V with translation. Whether the shader applies it to world or to
  camera-relative positions was not checked. Under Fix A the hook should use the head translation (it does
  automatically, since it reads +0x00).
* The gameplay controller's object size, for Fix B's proxy.
