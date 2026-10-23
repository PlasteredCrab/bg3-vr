# Right-eye fog, missing surface chunks, head-roll twist (agent Z, 2026-10-23)

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_A, read-only, headless, request server
`agents\Z\ZServer.java`), the mod's own 2026-10-23 log (`bin\bg3vr\bg3vr.log`, field diffs main vs eye view), and
the game's DX11 shader bytecode. RVAs are relative to 0x140000000. Raw output is in `re-local\ghidra\agents\Z\`
(z1..z25, z_d<rva>.txt disassembly). Engine shaders were extracted read-only with LSLib Divine from
`Data\EngineShaders.pak` (VolumetricFog*, DeferredFog_*, DepthMaxZx9, DilateMax2, EoCFog) into `Z\shaders\` and
disassembled with `S\dis_bshd.py` into `Z\shaders_asm\`. Material shaders were read from agent S's
`S\shaders_mat_asm\`. cbN[i] = constant buffer bN, 16-byte row i. "main" = the game's player view (left eye),
"eye" = the view made by CreatePlayerView 0x17F82F0 (right eye).

## Fixes for the VR mod

| problem | cause | RVA / hook | how | risk | confidence |
|---------|-------|-----------|-----|------|------------|
| 1. Right eye has no distance / height fog or haze | The fog density comes from a per-view **fog-layer list** at view+0x2950 (count +0x295C, 0xA4-byte entries keyed by atmosphere id and the +0x2961 variant, weight at entry+0xA0). Entries are created only by **0x3CCCCC0**, which only **0x3CCCC20** calls; 0x3CCCC20 runs for views registered with the player manager (0x1D11580, 0x1D118A0, 0x1D11AE0, 0x1F90B00) and from view slot 14. The eye view never gets it. copy_environment (0x3CC9700 / 0x3CCAAC0) changes the eye's atmosphere, but the blend 0x3CC9DB0 only re-weights **existing** entries: the entry for the new atmosphere is never made, the old one fades to weight 0, and the density sub-stage 0x3E3A650 stops at the first entry with weight < 0.01. No fog layers means no fog density, so there is no volumetric fog, height fog or haze in that eye. | game thread, in `views.cpp` right after every `copy_environment` (and once after create) | Call **0x3CCCC20(eye, byte [main+0x2961])**: it creates the entry (0x3CCCCC0), sets +0x28B0 / +0x28E8 so the eye's next environment tick (0x3CC8760 from view slot 7 0x17F2910) runs 0x3CC9DB0 and brings the new entry's weight to 1, and refreshes the froxel grid (0x3CC85D0). Minimal alternative: call **0x3CCCCC0(eye)**, then set byte [eye+0x28B0] = 1. Log the lists before and after (logging plan 1). | low (it's the game's own per-player-view refresh; 0x3CCCC20 also writes +0x2961, which the mod already copies from main, and +0x2298 from [0x604B1B8]+0x1BC, which is 0 in both views in the log) | Mechanism Confirmed (code). That the eye's single entry has weight < 0.01 or the wrong id: Unconfirmed (the log has counts, not contents). Logging plan 1 settles it. |
| 1b. (secondary) fog history scaled by exposure | VolumetricFogLighting multiplies its 93 % history by t26/t1 = this frame's / last frame's exposure from the view's own post-stage exposure step (0x3CCCB20, falls back to view+0x29C8). If the eye's two exposure buffers don't advance together, its haze settles at a different level. | none (diagnostic) | Only if fog is still weaker after fix 1: log the eye's exposure-step state (plan 1, item 4). | - | Shader dependency Confirmed; that it differs per eye: Unconfirmed (W saw the 0x3EDE160 step run 0 times, so eyefix's skip doesn't apply) |
| 2. Blocky chunks of blood / ground decals missing, different per eye | **CPU culling** uses one frustum for both eyes, built by **0x4086010** from the controller's **game FOV +0x14 (55 deg in the log) and aspect +0x20 (1.6)** at the head pose. CullStage takes its planes from the camera entity (W 2.5), not from the proxy. Ground decals are separate culled objects (S 1.3: CullStage buckets), so tiles below about -27.5 deg pitch, or outside about +/-40 deg yaw, are dropped. Each eye's asymmetric field of view reaches past that frustum on a different side, so the gaps differ per eye. | camera.cpp `on_view_update` (view update 0x4085150 entry, game thread), before 0x40851F0 / 0x4086010 run | Agent N item 4: write controller **+0x14** = `2*atan(max(u,-d))` and **+0x20** = `max(r,-l)/max(u,-d)` over **both** eyes' tangents (a symmetric frustum that encloses both eyes), and set ProjectionDirty +0x11 each frame (0x407BE10 resets the aspect when a camera is set). Every other reader of +0x14 / +0x20 is already overridden per eye (PerView corners, stage 0x1B / 0x40 hooks, PerCamera rewrite). Proxies copy the fields, so they stay consistent. | low-medium: a wider cull costs some CPU and draws; LOD (H) keyed on FOV may change | Frustum builder inputs Confirmed (0x4086010). That CullStage's +0x210 planes come from this frustum: Unconfirmed (plan 2). That the missing tiles are culled objects rather than shader discards: Unconfirmed (plan 2 A/B) |
| 2b. (if 2 doesn't remove them) pool cells | Surface pools are drawn by 0x1B7AA80 as one screen-covering grid per surface. The PS (Surface_Default) discards per **cell**: t37 mask, grid bounds, a 2x2 gather of the cell-type texture t0 against cb1[1].z, and a +/-0.2 height test with t1. Per-eye inputs are consistent with the proxy (world = rel + cb11[13] = eye). The one non-eye input is the box: inverse projection **+0x150** of the real controller (the head's union FOV), with +0xD0 from the proxy. That is a superset, so it isn't a cause. | `stereo.cpp on_view_begin` (0x17F21E0) | Optional: also write the eye's projection / inverse projection into proxy +0x110 / +0x150 so the box is exact. If blocks persist, run the race A/B: `[views] serialize_eyes = true` (the eyes have recorded in parallel since 10-23; a shared upload recorded by the eye would execute after main's draws). | low | Shader cell logic and readers Confirmed; race: Unconfirmed |
| 2c. per-object slot readers | Surfaces and decals take the per-object slot from the render context's stage 0x47 (ctx+0x20 = `0x407AE00(view+0x18, 0x47)` in 0x17F1B40 and 0x1800B00), i.e. stage47+0x219C, which the 0x3E53E01 hook sets to 1 for the eye. | none | No change needed. | - | Confirmed (code) |
| 3. Things "twist" with head roll | Two camera-facing paths build their quad basis from the **rolled** camera. (a) Material VS sprites (e.g. `VFX_Surface_AlphaBlend_Unlit_Emissive_VAT_Sprite_01`) expand each corner along the camera right/up taken from **cb11[0..2]** (view rows), so the sprite stays screen-aligned and rolls with the head. (b) CPU-built particles (stage 0x48 fills **0x3F59B10 / 0x3F5D2E0**) take their orientation from controller **slot 11 = 0x3CF0760** (the camera entity's ECS transform). camera.cpp writes the full head pose including roll into that transform, and the proxy does not override slot 11. Both eyes get the same roll (no binocular conflict), but sprites rotate in the world as the head rolls. | (b): proxy vtable slot 11 (+0x58) in `stereo.cpp` | (b) Give the proxy a slot-11 function that returns a Transform with the eye position and the head rotation **with roll removed** (yaw/pitch only). Slot 11 is reached only through `[view+0x21D8]` on the render side; the game thread calls slot 13 on the real controller. (a) has no CPU-side fix: cb11[0..2] must stay the true view rows (decal / surface rays use them, S 1.2). It needs a shader-bytecode patch of the affected VFX materials (basis from world up), or acceptance. | (b) low; (a) high effort | Mechanisms Confirmed (shader, code). That the brain jars use one of these paths: Unconfirmed (plan 3) |

## 1. Problem 1: fog per view

### 1.1 Pipeline (Confirmed)

Stage 0x1B (execute 0x3E32B50) owns the volumetric fog sub-stages. Vtables are in 0x58C6380..0x58C6720 (z12). Job
pointers and the shader set at stage+0x2188 (+0x2190 perspective, +0x22D0 orthographic) give:

| step | job (sub-stage init) | shader | key resources |
|------|------------------------|--------|---------------|
| density + history | **0x3E3ABB0** (sub-stage 0x14, init 0x3E39C40, execute 0x3E3A650) | VolumetricFogInitialize (set +0x128) | writes 0xE66299BB / 0x9165A92D (frame-parity pair), t4 = fog layers uploaded from the sub-stage (+0x70, count +0x4C), t5 / t17 / t18 = fog volumes and cluster lists |
| in-scattering | **0x3E3CCE0** (sub-stage 0x15, init 0x3E3C330) | VolumetricFogLighting (set +0x130) | writes 0xBCE29AFA / 0xCBE5AA6C (parity by `[ [view+0x10]+0x4280 ] & 1`, stage+0x2184), sun cascades 0x3E344F11 / 0x861C73EF, distant-shadow cache keys by [view+0x2268] (0: 0xC0F3544B / 0x447C4203, 1: 0xB7F464DD / 0x337B7295), local atlas 0xFA76F838, exposure t26 = 0x3CCCB20(view), t1 = previous exposure |
| integrate | **0x3E3F200** | VolumetricFogGather (set +0x138) | writes 0xDA734765 |
| apply | **0x3E42130** (sub-stage init 0x3E41A80) | DeferredFog_Trilinear / _Cubic (FogQuality 3) | t24 = 0xDA734765 only if sub-stage+0x58 = [view+0x2BD1], else a null texture |
| forward | stage 0x00 | material FOR shaders | 0xDA734765 |

Per-view constants:
* cb9 (0x40816E0): row 0 = viewport scale/offset in the frame, row 1 = size and pixel offset, row 2 = last pixel and
  **z = [view+0x2268]**.
* cb6 (0x3F3CF00): froxel z distribution = view+0x22C0..+0x22EF, written each frame by 0x3CC70B0 in begin-recording
  when [view+0x21D8] and [view+0x28A0] are set. Tile window +0x150..+0x15C from the render viewport, plus the split
  index for x. Grid from +0x29B0 (0x3CC85D0 / 0x3CC9190).
* cb0 of Initialize / Lighting: stage+0x2420..+0x24DF. Rows 0-3 = reprojection (the mod's 0x3E32C44 hook makes it
  per eye), rows 5-7 = the previous frame's distribution, row 8 = tile / frame size, row 9.w = history reset
  (view+0x2AED or stage+0x2410), row 10 = tile window, row 11 = the view's ReadState scale/offset
  (view+0x2170 / +0x2178).

History, region bounds and the split-index offset are consistent per view. The shaders reproject into the view's
own region of the frame-wide volume, clamped to the tile window. The frame-parity ping-pong is the same for both
views. DepthMaxZ / DilateMax (t7 / t8) use the same offsets. **No geometric per-eye error was found in the froxel
pipeline** (Confirmed from shaders + CB builders).

### 1.2 What the log shows (2026-10-23 10:59:16 field diff, main vs eye; Confirmed from the log)

* Identical in both views: froxel distribution +0x22C0..+0x22EF (it was **zero** in the eye at 10:49:33 right
  after create, before the camera arrived), grid +0x229C = 9 and +0x29B0, +0x2298 = 0, config +0x2BD1 = 1, and
  the PerView fog/atmosphere block +0xA0..+0x16F (only the sun-direction epsilon differs). The same 28 stages in
  the same order.
* Environment ids match after the copies (`views: ... copied; split view's active atmosphere id now ...`). The
  **first** copy at 10:49:30 was queued, not applied (0x3CC9700 parks it at +0x28F0 while a blend runs). The eye
  then kept the level default (atmosphere 212860945 vs main's 135266849) until the 10:53 copy.
* The fog-layer bookkeeping differs. Map +0x2920: main 2 keys / capacity 4, eye 1 / 2. Keys +0x2930: main 2, eye 1.
  Weights +0x2940: capacity 4 vs 2. List +0x2950: capacity 4 vs 2, count 1 in both. The entries' ids and weights
  are not in the log.

### 1.3 Fog layers (Confirmed, z18 / z20 / z6)

* The density sub-stage execute **0x3E3A650** reads `[view+0x2950]`, at most 4 entries. Each entry's weight
  +0xA0 must be >= 0.01, **otherwise the loop stops**. From each kept entry it builds the 160-byte t4 layer records
  (+0x10 and +0x58 transforms via 0x3D55B60 with the controller's slot-9 position and forward vector).
  0x3E3ABB0 uploads them. With no valid entry, Initialize has no height-fog layers: zero density, hence no in-scatter
  or extinction. That is exactly "no fog / haze at all", which shows most on distant backdrops.
* Entry creation: only **0x3CCCCC0** appends an entry for (atmosphere id at [+0x2880]+8, [view+0x2961] != 0), with
  weight 0x34000000 (about 1e-7) and a copy of atmosphere +0x230 + variant*0x98. Callers: only **0x3CCCC20**
  (z7). 0x3CCCC20's callers are 0x1D11580, 0x1D118A0, 0x1D11AE0, 0x1F90B00 (player-view systems, B / N 4), view
  slot 14 0x17F2180, and 0x3CC6BB0. The ctor 0x3CC5AA0 only initialises the list, and the dtor 0x3CC6630 frees it.
* Re-weighting: **0x3CC9DB0** (called by the environment tick 0x3CC8760 while +0x28B0 is set) sets each entry's
  weight toward 1 if it matches the current target atmosphere, otherwise toward 0. It never adds entries.
  0x3CC8760 also copies the target atmosphere's +0x230 block into a matching entry each frame.
* copy_environment calls 0x3CC9700 / 0x3CCAAC0. They change +0x2880 / +0x2890 / +0x28A0 and set +0x28B0, but they
  don't touch the list. So after the eye's atmosphere changes, its list holds no entry for the active atmosphere,
  and its fog density is zero. The main view gets 0x3CCCC20 through the player-manager systems when its
  environment changes.

### 1.4 Things checked and ruled out (Confirmed)

* rewrite_view_block writes only PerView +0x30..+0x6F and +0x70..+0x9F. The fog terms +0xA0..+0x16F come from
  0x3CC7490 untouched. The proxy copies +0x18 / +0x1C (near/far), so the froxel distribution 0x3CC70B0 matches
  main's.
* 0x3E3CCE0's cascade matrices come from 0x3E1D830(stage02+0x21A0, ..., slot-9 position). With the proxy that is
  the eye position, which is consistent with the eye's camera-relative froxels.
* The view index +0x2270 (eye = 1) is used by 0x3E3ABB0 / 0x3E3CCE0 / 0x3E3CC00 only for the debug-capture
  globals DAT_145EF2CB0 / CB4.
* The created view has a scene, the same stages, [view+0x2BD1] = 1, and the same grid and FogQuality tile.

## 2. Problem 2: surfaces and ground decals

### 2.1 Surface draw (Confirmed, s30 / s31 / z24 / z25)

* Sub-stage 0x3B (vtable 0x55F40E0): prepare 0x17F1AC0 runs only when the view's scene [view+0x2B10] is the level
  scene. Job **0x17F1B40** builds a render context: +0x10 view, +0x20 = **stage 0x47 of the view**, +0x30 = [view+
  0x21D8] (proxy). It calls **0x1B7AA80** for each entry of its surface list (sub-stage+0x60: count +0x7C, objects
  +0x70, type +0x60). 0x1800B00 does the same for the emissive variant (RT 0x2EDB4AC1).
* 0x1B7AA80 builds the surface's box world matrix from the context controller's **+0x150..+0x18F (inverse
  projection)**, **+0xD0..+0xFB (inverse view)**, near `min(+0x18, 0.95)`, and the view's ReadState scale
  +0x2170 / +0x2174. It writes it through 0x3CEEE20, which uses a per-draw CB allocation (object vtable +0xB8), not
  shared memory. Then DrawIndexed(gx*gy*6, start = surface index * gx*gy*6) of the shared grid.
* Surface_Default VS: positions from the vertex buffer, or from t126 at the per-object offset cb0[9].x. clip =
  cb11[8..11] * p.
* Surface_Default PS: discards per pixel on the t37 mask. world = rel + cb11[13]. It discards outside the grid
  `(world.xz - cb1[0].xz) / (cb1[1].w * cb1[1].xy)`. **Per cell** it discards unless one of the 4 gathered texels
  of t0 equals the surface's type cb1[1].z. Then the height fade uses t1 * 1310.7 + cb1[0].y against world.y
  (+/-0.2). The cell test makes the edges blocky.
* With the proxy, cb11[13], cb12[3..5], the depth and the vertex origin are all the eye's. This is consistent (S
  1.1-1.2; the proxy fixes it).

### 2.2 Culling (Confirmed for the frustum builder)

* 0x4086010 builds the controller's 8 frustum corners (+0x2B0..+0x30F) and planes (0x408C6E0 into the rf::Camera
  at +0x90) from **+0x14 FOV, +0x20 aspect, +0x18 / +0x1C** and the axes. The perspective path never reads the
  projection matrix. The mod's projection override (camera.cpp, head union FOV into +0x110 / +0x150) therefore
  doesn't widen the cull.
* Log: `camera: fov 0.9599 rad (55.0 deg) near 0.100 aspect 1.600`. That is a cull frustum of about +/-27.5 deg
  vertical and +/-39.8 deg horizontal, at the head. A Quest 2 eye covers roughly +/-48 deg vertically and is
  asymmetric horizontally.
* CullStage (W 2.5) takes its planes from the camera entity's component (+0x210, via 0x17F3A50 / 0x3CEE3D0) and
  its player mask from [view+0x2268]. Both eyes share it. Decals are culled objects (S 1.3 bucketing). The link
  "+0x210 planes = the controller frustum from 0x4086010" is Unconfirmed, as is whether the surface list
  (sub-stage+0x60) is per-view culled.

### 2.3 Per-object slot (Confirmed)

Both surface draw paths set ctx+0x20 to the view's own stage 0x47. The draw / fill readers (W: 0x3E13520,
0x3F59B10, 0x3F5D2E0, 0x3F7B480, 0x3F7C240) and 0x3F5D2E0 (seen here) index `[obj+0x2D0/0x2D8]+0xE0 +
[stage47+0x219C]*4`. No reader using [view+0x2268] was found on the surface or decal paths.

## 3. Problem 3: camera inputs the render code reads

### 3.1 Controller fields (proxy = copy of the real controller with +0x90 / +0xD0 / slot 9 replaced)

| field | readers (render side) | in the eye view today |
|-------|-----------------------|------------------------|
| +0x10 / +0x11 dirty flags | 0x4086010, 0x4085150, 0x40851F0 | copied (game thread only) |
| +0x12 orthographic | 0x3E32B50, 0x3CC6CB9 hook, 0x1B7AA80 path | copied |
| +0x14 FOV, +0x20 aspect | 0x4086010 (cull frustum), 0x3CC7490 corners (rewritten by the mod), 0x3CCCFD0 jittered projection (0x1B / 0x40 hooks and the block [1] rewrite replace it), slot 3 0x3CF0870 | **game FOV 55 deg / 1.6, symmetric, not per eye** |
| +0x18 near, +0x1C far | 0x3CC70B0 (froxels), 0x3E32B50 (+0x25C0), 0x1B7AA80 (box depth), LinearizeDepth, 0x3F3CF00 | same for both (correct) |
| +0x24..+0x3F in-object transform | none found in render code | game value |
| +0x90 view | 0x3CC7490 -> PerCamera +0x00, 0x3E33030 in 0x1B (hooked), 0x3E12240 (hooked) | eye (proxy) |
| +0xD0 inverse view | 0x3CC7490 -> PerView +0x30 (rewritten), **0x1B7AA80 surface box** | eye (proxy) |
| +0x110 projection | 0x3CC7490 block [0] (rewritten), 0x3E12240 non-TAA path (hooked) | head union FOV |
| +0x150 inverse projection | **0x1B7AA80 surface box** | head union FOV (superset of each eye) |
| +0x2B0..+0x30F corners / planes | CPU cull (2.2) | game FOV at the head |
| slot 9 (+0x48) translate | every camera-relative draw, 0x3CC7490, 0x3E3CCE0, 0x3E3A650, culling position | eye (proxy) |
| **slot 11 (+0x58) = 0x3CF0760** world transform | **0x3F59B10 / 0x3F5D2E0** particle basis and distances | **head rotation incl. roll, head position** |
| +0x318 / +0x320 entity | Enter/Exit, cull entry, slot 11 | same camera entity |

### 3.2 Roll-dependent shading (Confirmed mechanisms)

* VAT_Sprite VS: corner = centre + v0.x * (row 0 of the view rotation) + v0.y * (row 1), read as dp3 with
  (1,0,0) / (0,1,0) over cb11[0..2]. The quad lies in the view plane and keeps its screen orientation, so it rolls
  with the head.
* 0x3F5D2E0 (alignment modes 0 / 2): it turns the slot-11 quaternion into a 3x3 basis for the particle quads,
  which is the camera's rotation including roll.
* The FakeParticles VS transforms the object axes and its vertex-animation offsets with the full view rotation
  (view x object). That is world-consistent, with no roll artefact.
* Glass / refraction (VFX_Surface_AlphaBlend_Distortion) uses cb11[0..2] / cb11[8..11] only: per eye, no roll
  assumption found.
* PerView corners are rebuilt from the eye's rotated view (N fix), so they're consistent.

## 4. One-run logging plan (everything Unconfirmed above)

All in one run: load a save outside the nautiloid window (distant landscape), then a blood pool, then the brain
jars.

1. **Fog layers** (game thread, in `sync_environment`, every 60 frames and right after each copy). For main and
   eye log:
   * [+0x2880]+8 (target atmosphere id) and +0x28A0 id;
   * +0x28A8 / +0x28AC / +0x28B0;
   * +0x2961;
   * count +0x295C, and for each entry: id (+0x00), variant (+0x04), weight (+0xA0);
   * the map counts +0x292C / +0x293C.

   Prediction: main has an entry for its target id with weight 1. The eye has none, or one with weight < 0.01. Then
   enable the fix (`[views] eye_fog_layers = true`, calling 0x3CCCC20(eye, main+0x2961) after each copy) and log the
   same lines plus the 0x3E3A650 result: sub-stage+0x6C (layer count, sub-stage 0x14 of the eye's stage 0x1B). It
   should be >= 1 in both views.
2. **Culling**:
   * At 0x3E246B0 log the 6 planes the cull uses for both views (`0x17F3A50(world, entity)+8`, +0x210) next to the
     controller's corners +0x2B0..+0x30F and +0x14 / +0x20. Prediction: the planes are the controller's.
   * A/B a key that writes the enclosing FOV / aspect (fix 2) while looking down at a blood pool. Prediction: the
     missing tiles come back.
   * Count 0x3CEE9A0 (decal draw) and 0x1B7AA80 (surface draw) calls per view, with and without the fix.
   * If blocks remain with fix 2, toggle `[views] serialize_eyes` (race A/B, 2b).
3. **Roll**:
   * Hook 0x3F5D2E0 entry; count calls per view and log the emitter's alignment byte +0x2F4.
   * With the GPU trace (gpu_trace), capture the draws covering the jar (pixel pick) and log their shader names.
   * Roll the head 30 deg. Prediction: the jar content is a VAT_Sprite / particle material. Then apply the proxy
     slot-11 roll-free transform (3b) and repeat.
4. **Exposure (1b, only if fog still differs)**: for each view, the stage 0x21 exposure step (+0x2140 or +0x2148
   per [0x604AA10]+0x75): +0x18, +0x78 / +0xA8 buffer pointers, valid bytes +0xC8 / +0xF8, and the parity counter
   `[[step+0x38]+0x2000]+8`.

## Raw files

z1 / z3 / z5 / z19 displacement scans; z2 exposure (0x3CCCB20, 0x3EDE160); z4 froxel grid (0x3CC85D0, 0x3CC86D0,
0x3CC9190, 0x3E32810, 0x3CCCC20); z6 environment tick 0x3CC8760, 0x3CCB670, 0x3CCB970, 0x3CC6DC0; z7 / z10 / z11 /
z15 / z17 / z22 callers; z8 the 10:59 field diff; z9 0x3CC9700 / 0x17F2910; z12 / z23 vtables; z13 fog sub-stages;
z14 viewport CB 0x40816E0 / 0x407B8C0; z16 fog shader loader 0x3E30790; z18 density sub-stage 0x3E3A650; z20 fog
layer list 0x3CCCCC0 / 0x3CC9DB0 / dtor 0x3CC6630; z21 particle fills 0x3F59B10 / 0x3F5D2E0 and controller slots;
z24 / z25 surface paths; z_d*.txt disassembly of the stage 0x1B sub-jobs; `shaders_asm\` fog shaders.

## Open leads

* Who calls 0x1F90B00 and when. It probably made the eye's single entry at some point. If it runs on settings
  changes, the eye recovers until the next atmosphere change, which would fit the haze being sometimes weaker and
  sometimes missing.
* What CullStage's +0x210 planes are (component type, writer).
* Whether the 0x3B surface list (sub-stage+0x60) is a culled bucket or the global surface set.
* The exact material on the brain jars.
