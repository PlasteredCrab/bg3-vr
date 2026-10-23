# GPU work the second eye can share with the first - agent T, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_A, read-only, headless, request server
`agents\T\TServer.java`; disassembly with bg3vr_disasm). RVAs are relative to 0x140000000. Raw output is in
`re-local\ghidra\agents\T\` (t1..t14). Agent A's decompiles (`agents\A\dec\`) are cited as A/dec/<file>.
"Confirmed" means read from decompiled code or disassembly at the cited address. "Unconfirmed" items say what
would confirm them. Every percentage below is an **estimate**. Nothing has been measured; section 6 is the
plan that measures it.

Scope (changed mid-task by the coordinator): GPU-side passes that can be computed once and shared, the cascade
fit, and quality knobs. CPU cost, job parallelism and the eyefix serialisation belong to agent W. Resolution,
upscaling and expensive post effects belong to agent X. What I saw in those areas is in section 7 (Overlaps).

Terms: **main** = the game's main player view (left eye). **eye** = the view made by CreatePlayerView 0x17F82F0
(right eye). It was built with split index 1 and renders with index 0 (views.cpp).

## Savings for the VR mod (ranked by expected gain / risk)

| # | what | est. share of the eye view's GPU cost (reasoning) | RVA / hook | how | risk | confidence |
|---|------|------|-----------|-----|------|------------|
| 1 | **Sun cascaded shadow maps: render once (main), eye reuses them** | 10-25 % outdoors (estimate). The eye re-culls and re-draws every shadow caster into 4 cascades of 1024² or 2048² (ShadowQuality 0 = 2048). That is a full extra scene pass per cascade. It also re-runs the distant-shadow cache. The eye's cache renders the index-1 keys, which nothing reads (pure waste today). The eye's skinning (0x41) also skins every cascade caster. | prepare **0x3E1A410** (stage 0x02 slot 5); copy at the existing hook **0x3CC6CB9** (eye's begin-recording, before its stages execute) | (a) After 0x3E1A410 returns for the eye's stage 0x02 (`[stage+8] == eye`), set `[stage+0x18] = 0`. The game takes this same path itself when the sun is off: no culls, no 0x32 cascade jobs, no downsample job, no cache update, and 0x41 drops the cascade casters. (b) In 0x3CC6CB9 for the eye, copy from main's stage 0x02 to the eye's: `+0x21D8` (byte), `+0x21E0..+0x240F`, `+0x241B`, `+0x2470..+0x24AF`, `+0x2688`, and `+0x24B0..+0x25AF` (4 matrices) **re-based** to the eye origin (section 2.4). The textures (key 0x3E344F11, 4 slices) and the cache textures (index-0 keys) are still in place when the eye's commands run. | low-medium: one prepare flag plus a plain-data copy. Main's matrices are final before the eye's begin-recording. | Mechanism Confirmed (code); visual equality Unconfirmed (validation log 6.2) |
| 2 | **Local-light shadow atlas: skip the eye's re-render when its allocation equals main's** | 0 % in daylight with no shadowed lights; possibly 10-30 % in torch-lit interiors (estimate). Stage 0x1E renders every allocated light every frame, 4 atlas tiles per point light, with no "unchanged" check (Confirmed, 0x3E01E00). | eye's stage 0x1E job entry **0x3E01E00** (rcx = stage+0x20, view = `[rcx-0x18]`) | Let the eye's 0x35 allocation run (CPU, cheap; it writes eye-relative records). At the eye's 0x1E job entry, wait for main's 0x35 counter (main ctx+0x60, `0x410E880(c,1)`). Compare per light (matched by light object `*entry`): entry `+0x10` (tile) and `+0x18` (scale) in both views' 0x35 lists (`stage35+0x2178`, count `+0x2184`). All equal: return without recording (main's atlas tiles 0xC91746C5 / 0xFA76F838 are the eye's). Any difference: run normally. | medium: the per-frame check and the tile-field meaning are Unconfirmed. The fallback keeps it visually safe. | Shadow content is viewer-independent: Confirmed. Equality rate: Unconfirmed (log 6.3) |
| 3 | **Drop the eye's wasted distant-shadow-cache rendering** (comes with #1; listed separately because it is pure waste today) | small to moderate (progressive cache updates, estimate) | ctor 0x3E194E0 stores `stage02+0x241A = [view+0x2268]` (= 1 at the eye's build) | Covered by #1. Without #1: set the eye's `stage02+0x241B = 0` and point the eye's stage 0x03 `+0x2150` at main's 0x02 (R §3). Agent R's caveat applies: 0x03 then also takes main's cascade matrices, which are wrong by the eye offset under `eye_origin = proxy`. So prefer #1. | low | Confirmed (R §3 + 0x3E194E0 + 0x3E1F520) |
| 4 | Quality knobs for both eyes in VR (global, not per view) | depends on the setting | `[0x604AA10]+0x43` ShadowQuality, `+0x49` CloudsQuality, `+0x4A` FogQuality, `+0x48` distant-shadow cache (section 5) | Lower ShadowQuality 0 -> 1 (cascade maps 2048 -> 1024, Confirmed tables). CloudsQuality selects `VolumetricClouds_Q<n>.bshd`. Settings are global, so both eyes change together and stay identical. | low (the game's own options) | Confirmed (readers); gain Unconfirmed |
| 5 | Per-view knobs for the eye only | not recommended | view config `+0x2B70..+0x2BCF` (local-light atlas records), atmosphere `+0x54/+0x58/+0x70` (shadow distance / cascade count) | Possible, but any per-eye difference in shadows shows as binocular rivalry. Use only if #1/#2 fail. | high (visible) | Confirmed fields |

Not shareable / nothing to share (section 4): SSAO (0x37), clustered light assignment (0x05), deferred lighting and
volumetric fog (0x1B), sky (0x36), volumetric clouds (0x44), camera velocity (0x40), surface meshes (0x3D), room
masks (0x33), G-buffer and emissive passes. No runtime GI or probe capture, GPU particles, wind or atmosphere-LUT
compute shaders exist in the shader list. Light probes are loaded assets. GPU skinning (0x41) is per view (4.3).

---------------------------------------------------------------------------------------------------------

## 1. How a stage runs for a view, and how the eye can skip one

* Run flag: `stage+0x18`, set each frame by the stage's prepare (slot 5), called from 0x4094E60 (frame sync).
  Most prepares AND the gate 0x17DA690 with stage-specific checks. Execute (slot 6) submits the job only if
  `+0x18`. The base wait+close (slot 8, 0x3CD4330) and the overrides return or discard when `+0x18 == 0`.
  0x02's own slot 8 0x3E1B9D0 returns at once (Confirmed). So forcing `+0x18 = 0` after the prepare is the
  clean "skip" and uses the game's own path. Example: 0x3E1A410 clears it when there is no sun.
* Keyed resources (A §7) are frame-level: one resource per key, shared by all views. Views execute in order
  on one context (main's stream, then the eye's; A §1.5). So when the eye's commands run, a key holds what
  main last wrote, unless the eye rewrites it. Persistence across views and frames is Confirmed live. The
  exposure luminance written by one view is read by the other the next frame (docs/re-notes/stereo.md).
* Consumers reach producer stages through pointers captured at construction (factory 0x3AC51C0, t1).
  0x1B gets (cull 0x08, 0x02, 0x03, 0x37, 0x05, 0x1E). 0x1E gets (cull, 0x35, 0x47). 0x02 gets (0x47, counters).
  0x03 gets the cull stage, and its `+0x2150` holds 0x02. 0x41 reads 0x1E at job `+0x2150` and 0x02 at job
  `+0x2158`, i.e. stage `+0x2170 / +0x2178`. So the cheapest sharing **leaves the pointers alone**: it
  disables the eye's producer and copies main's CPU-side outputs into the eye's (disabled) producer object.
  The consumers then read correct data through their usual pointers.
* Stage -> job counter (context counters at `[view+0x40]+8*i`, from 0x3AC51C0 and the graph in 0x40878C0;
  t1): 0x08 +0x40; 0x35, 0x02, 0x03 +0x60; per-light cull sub-jobs of 0x1E +0x50; cascade culls +0x48;
  0x47, 0x41 +0x58; draw stages (0x05, 0x0B, 0x1B, 0x1E, 0x46, 0x36, 0x44, 0x18, 0x17, 0x40, 0x00, 0x21, 0x3F,
  0x48 ...) +0x68; 0x37, 0x3E +0x70; cascade render jobs +0x08..+0x20. Edges: +0x40 -> +0x60 -> +0x50 ->
  +0x58 (+0x48 also) -> +0x68 and +0x08..+0x20; +0x08/+0x10 -> +0x28; +0x18/+0x20 -> +0x30; -> +0x38;
  (+0x68, +0x70, +0x38) -> +0x78; (+0x78, +0x00) -> +0x80. Given here for agent W; Confirmed (decode of 0x40878C0).

## 2. Sun cascaded shadow maps (stage 0x02 + its 0x32 cascade renderers + 0x03 resolve)

### 2.1 What runs per view (Confirmed)

* Stage 0x02 (vtable 0x58C4E18, ctor 0x3E194E0, init 0x3E19910, prepare 0x3E1A410, execute 0x3E1B0C0, slot 8
  0x3E1B9D0, per-frame 0x3E1A490, job 0x3E1BB10). A labelled 0x1E as "sun cascades". That is wrong: **0x02 is
  the sun cascades and 0x1E is the local-light shadow renderer** (2.5, 3).
* Cascade count = atmosphere `[view+0x28A0]+0x70` (1..4), copied to `stage+0x2688` by 0x3E1A490.
  Resolution `stage+0x2684` = 0x800 or 0x400 from ShadowQuality `[0x604AA10+0x43]` through tables
  0x58C4F80 {1,0,0,0} (resolution selector) and 0x58C4F70 {1,1,0,0} (`+0x267C`, a filter flag the 0x03
  resolve tests). So ShadowQuality 0 -> 2048 and flag 1; 1 -> 1024 and flag 1; 2 -> 1024 and flag 0 (0x3E1AF90;
  table bytes read from the file).
* Render targets: 0x3E1AA10 creates 4 RT sets `stage+0x25C0..+0x25D8` on **key 0x3E344F11, slices 0..3** (PSO
  `+0x25B0`/`+0x25B8`). One frame-level key, shared by all views.
* Execute 0x3E1B0C0 (stage-level, render thread, inside the view's begin-recording):
  1. split distances `+0x2470..`: near = max(atm+0x58, 0.01), far = atm+0x54, log/linear blend atm+0x6C;
  2. for each cascade object `[stage+0x2180+i*8]`: fit (2.2), ortho projection, look-at, texel snap;
  3. matrix `stage+0x24B0+i*0x40` = (cascade view with translation replaced by `R*(campos - cascadepos)`) x
     the texture bias matrix at 0x5EF2C70, where **campos = the view's controller slot 9**
     (`[[view+0x21D8]]+0x48`). `+0x2490+i*4` = cascade +0x1C, `+0x24A0+i*4` = 2 x radius;
  4. submits the cascade cull jobs (cascade object +0xD0) to `+0x2650` (ctx+0x48) and the distant-cache cull;
     then the 4 cascade render jobs (0x32 objects at `+0x2140..`, job 0x3E18CE0) to ctx+0x08..+0x20;
  5. 0x3E1C940 distant-cache update (when `[frame+0x40] == 0`), then 0x3E1BB10 (ShadowMapDownsample, and every
     32 frames a staging readback into a CPU height grid stored on the cascade objects +0x470..+0x4DF).
* Consumers: the clustered CB builder 0x3F3CF00 (from 0x03's 0x3E1FF80, 0x1B sub-jobs, 0x00, 0x05) copies
  `+0x2490..+0x24AF`, the 4 matrices `+0x24B0..+0x25AF`, `+0x2688` and `+0x2684`. It gates on `0x03+0x2238`,
  `0x02+0x268C` and `param_5+0x44`, **not on 0x02's run flag** (N/r2_cb.c 64-131). The 0x03 resolve job 0x3E1F520
  reads `0x02+0x241B`, `+0x21D8` and `+0x2470+count*4` (last split). It gets the distant-cache matrix from
  0x3E1D830(`0x02+0x21A0`, which, campos) with campos = its own view's controller slot 9 (t8 lines 226-245, 376-383).
* 0x41 (skinning) adds every caster of 0x02's cascades and of 0x1E's lights to its per-view skinning set, but
  only when that stage's `+0x18` is set (0x3E15670, t5).

### 2.2 The cascade fit (question 2) - Confirmed

* 0x3B96B00(out, `ctrl+0x318`) reads the **camera entity** handle (`+0x318` entity, `+0x320` world) from the
  view's controller, and looks up a component through 0x3B97240. 0x3B94540 takes the camera object
  `[component+8]` (or 0x17F3A50(world) as fallback). It uses that object's FOV `+0x14`, aspect `+0x20`, inverse
  view rows `+0xD0/+0xE0/+0xF0`, and position `+0x100` (CameraController layout).
* Fit: bounding-sphere radius of the frustum slice [near_i, far_i] from tan(fov/2) and aspect. Centre =
  position + forward * the sphere-centre depth. The radius is capped at the far split. Then the centre is
  snapped in light space with `fmodf` to a grid of 2r/resolution (stable cascades).
* 0x3B94B00 builds the caster cull volume: the cascade's side and near/far planes, plus each of the camera's 6
  frustum planes (camera object `+0x210..+0x268`) whose normal faces away from the light.
* Sun direction: view `+0x2A40` (per view, from the view's lighting object; A 3.3).

So **the fit and the caster culling do not use the eye's projection or the eye's pose**. They use the camera
entity's controller: the head, with the game's symmetric FOV. Both proxies copy `+0x318/+0x320` (S Fix B), so
main and eye fit identical world-space cascades. The only eye-dependent term is the translation in step 3
(campos = the view's slot 9 = that eye's position under `eye_origin = proxy`).

### 2.3 What must match for reuse

| input | main vs eye | status |
|-------|-------------|--------|
| camera entity / its controller (FOV, aspect, pose, frustum planes) | same object | Confirmed by code that the fit reads it. That `[component+8]` is the mod's `g_controller`: Unconfirmed (log 6.2) |
| sun direction view+0x2A40 | per view, from each view's lighting object (the eye gets main's via copy_environment) | Unconfirmed bit-equal. Irrelevant under #1 (the eye takes main's matrices) |
| atmosphere +0x54/+0x58/+0x68/+0x6C/+0x70 | per view's `[view+0x28A0]` | same after copy_environment (Unconfirmed bit-equal; log) |
| cascade resolution +0x2684, flag +0x267C | from the global ShadowQuality | same (Confirmed) |
| camera-relative translation | eye slot 9 vs main slot 9 | **differs by the eye offset; re-base (2.4)** |
| distant cache data `+0x21D8`, `+0x21E0..+0x240F`, `+0x241B` | world-space, matrices made per consumer from campos (0x3E1D830) | copy as-is (Confirmed layout: ctor 0x3E194E0 zeroes these as plain arrays; 0x3E1D830 reads parity at cache+0x38, matrices at +0x40.., origins at +0x240..+0x26F) |

Coverage caveat (pre-existing, not caused by sharing): the spheres and the caster planes cover the **game's**
FOV, not the wider asymmetric eye frusta. Eye pixels outside the fitted slice fall into the next cascade or
outside all of them, and casters outside the game frustum can be culled. N's item 4 (write a symmetric FOV and
aspect that enclose both eyes into the controller `+0x14/+0x20`) fixes it for both eyes at once. That only works
if the camera-entity object above is that controller (log 6.2).

### 2.4 Re-basing main's matrices to the eye (Confirmed algebra from 0x3E1B0C0)

Per cascade i, M = 16 floats at `+0x24B0 + i*0x40` (rows r0..r3, row-major as written). The code builds
r0..r2 from the cascade view's rotation rows and r3 from `sum_j Delta_j * rotrow_j` (w = the view's `+0xCC`),
all times the bias matrix, with Delta = campos - cascadepos. So with d = slot9(eye) - slot9(main):

```
r3_eye[k] = r3_main[k] + d.x * r0[k] + d.y * r1[k] + d.z * r2[k]      (k = 0..3)
```

This is exact if the cascade view's rotation rows have w = 0 (`+0x9C/+0xAC/+0xBC` = 0, affine view: Unconfirmed
numerically; log 6.2 checks it). Under `eye_origin = head`, d = 0 and it is a plain copy. Take main's slot 9 as
it was at main's 0x3E1B0C0. Reading it again at the eye's 0x3CC6CB9 gives the same value, because both run in
one render-thread pass and the controller is not updated in between (Unconfirmed; log both).

### 2.5 Why this is safe for the textures (Confirmed)

Main's whole stream (its 0x02/0x32 renders, then its 0x03/0x1B/0x00 reads) executes before the eye's. With the
eye's 0x02 off, nothing rewrites key 0x3E344F11 or the index-0 cache keys before the eye's 0x03/0x1B/0x00 read
them. The eye's resolve selects the cache keys by live index (0x3E1F683: index 0 -> 0xC0F3544B/0x447C4203),
and those are main's (R §3). The 0x3E1BB10 downsample output (key 0x32935570) is also main's.

## 3. Local-light shadows (stages 0x35 and 0x1E)

### 3.1 What runs per view (Confirmed)

* **0x35** (allocation, job 0x3DFB6A0, ctx+0x60). It gathers the view's CullStage light buckets
  (cull+0x100/+0x130/+0x160; entries of 0x330 bytes, `*entry` = light object; 0x3DFBF80). It sorts them by a
  camera-dependent priority (0x3DFC0D0 into entry +0xC). The fade is smoothed per stage (`+0x21A8` map) and the
  budget fades from the view config. The tile size comes from the screen size (config `+0x2B7C` / controller
  FOV `+0x14`, distance, radius `+0x324`), rounded up to a power of two. Tiles come from a quadtree
  (0x3F77820). Point lights (`light+0xD3 == 0`) take 4 tiles. It writes per entry: `+0x10` tile, `+0x18` scale,
  `+0x324` fade, `+0x320` record index. 0x3DFC660 writes matrices relative to the view's slot 9; 0x3DFD980
  writes 0x60-byte records. It uploads the records into key **0x738395A7**. The entries live in the view's own
  CullStage arrays, so the views do not race on them.
* **0x1E** (render, vtable 0x58C49C0, init 0x3E01560, prepare 0x3E01870, execute 0x3E018A0, per-frame
  0x3E01710, job 0x3E01E00). Per-light caster cull jobs (0x3DFFFF0 objects, counter ctx+0x50) run first. The job
  then renders **every allocated light every frame** into the depth atlas **0xC91746C5** (0x3E03C40/0x3E04400;
  size from config `+0x2B74` by ShadowQuality) and linearises it into **0xFA76F838** (0x3E03370, LinearizeDepth
  and the ortho variant). The light's shadow camera is a local CameraController made from the light's
  transform. It is installed as the render context's controller (job +0x2198/+0x2218), so instance data is
  relative to the light, not the viewer (0x3E01E00; 0x3CD87B0 reads ctx+0x30). The atlas content therefore does
  not depend on the viewer: only the tile placement does.

### 3.2 Sharing (Unconfirmed design)

Copying main's per-light results into the eye's entries would also need re-based 0x3DFC660 matrices and record
contents. I did not decompile those consumers (0x05/0x1B/0x00 reading entry +0x320 and 0x738395A7) far enough
to specify it. The safe variant is #2 in the table: the eye keeps its own allocation and records (eye-relative,
consistent with its own lighting) and skips only the atlas re-render on frames where its tile assignment equals
main's. Tile size is a power of two from distance and FOV, and the eyes are 3 cm apart. So most frames should
match, except lights near a size threshold or at the frustum edge (Unconfirmed; log 6.3). Main's 0x35 runs early
(ctx+0x60), so waiting for it from the eye's 0x1E job costs little. The eye's per-light caster culls and their
0x41 skinning still run (CPU, agent W).

Per-view atlas config (`+0x2B70 + q*0x20`, q = ShadowQuality): the eye's comes from CreatePlayerView's player-view
block. If it differs from main's, the shared key 0xC91746C5 is requested with two sizes (Unconfirmed; log the
0x60 bytes of both views once).

## 4. Other per-view passes (GPU)

### 4.1 Must stay per eye (screen or frustum space)

| stage | why per eye | note |
|-------|-------------|------|
| 0x0B, 0x46, 0x18, 0x17, 0x3C | G-buffer / emissive geometry | required |
| 0x05 clustered assignment | froxel bins from cb11/cb12 (N 1.4) | required |
| 0x1B lighting and volumetric fog | per-pixel and froxel volume (key 0xDA734765, fixed-size 3D texture created in 0x1B's sub-stage, sub1B.c 1235) | FogQuality `+0x4A == 3` selects the other apply shader (0x3E42130, s1B.c 3803). Other uses of +0x4A not traced |
| 0x03 sun-shadow resolve (PCSS) | screen-space mask 0x94009344 | required; cost scales with eye resolution |
| 0x37 SSAO | screen space | required (agent X for quality) |
| 0x36 sky | `PPEmissiveSkydome(_Procedural)` per pixel | required, cheap; no sky/atmosphere LUT shaders exist in the list |
| 0x44 volumetric clouds | ray-march `VolumetricClouds_Q<n>` + LP/Downsample/UpsampleAndBlend into 0x3B9705AE / 0xC3C184CD / 0x2EDB4AC1 (A/dec/st_44.c) | per eye; Q from CloudsQuality `+0x49` (0x3E47840). Clouds are far away, so a reprojected share is possible in principle, but no engine support was found |
| 0x40 velocity, 0x33 room masks, 0x3D surface meshes (dispatch sized from linear depth 0xFF193183, 0x18126A0) | screen space | required |

### 4.2 Nothing to share (no per-view work found)

* GI / probes: the shader list (A/strings_shaders.txt) has no capture or prefilter shader. LightProbe exists only
  as streamed ECS components (`ls::LightProbeLoadComponent`, `LightProbeComponent`), plus the static
  `Engine_IBL_LUT.dds`. Unconfirmed that no other path renders probes at runtime.
* Particles, wind, foliage: no particle or wind compute shader. Wind strings are cloth parameters
  (`ClothWindDirectionOffsetFrequency`). Simulation is not part of the render views (Unconfirmed where it runs).
* Exposure: already shared by eyefix (0x3EDE160 skipped for the eye).

### 4.3 GPU skinning 0x41 (per view; sharing not proposed)

0x41's job 0x3E15670 skins the union of: the view's visible objects, 0x1E's per-light caster lists (if 0x1E runs)
and 0x02's cascade lists (if 0x02 runs). Its output buffers are per stage (`+0x2178/+0x2198` by frame parity).
The per-object data writers 0x3F59B10 / 0x3F5D2E0 read the view's controller slot 9 and slot 11 (+0x58). At
least slot 9 feeds a distance used through curves at object +0x360/+0x380, so the two eyes' data is not
guaranteed to be bit-identical (Confirmed that the camera is read; what the slot-11 value does is Unconfirmed).
Sharing would need draws to bind main's 0x41 output: not attempted. #1 and #2 shrink the eye's 0x41 work anyway.

## 5. Quality knobs (question 4)

Global graphics options `[0x604AA10]` (names from H 1.3). All are global, so both eyes change together:

| field | name | effect found | readers |
|-------|------|--------------|---------|
| +0x43 | ShadowQuality 0..2 (0 best) | cascades 2048 (0) / 1024 (1, 2); flag `+0x267C` 1/1/0 (0x03 binds an extra sampler state when 1); local-atlas config record `+0x2B70+q*0x20` | 0x3E1AF90, 0x3E194E0, 0x3DFB410/0x3DFB5A0, 0x3E01560/0x3E015F0, 0x3E1F520 (Confirmed) |
| +0x48 | distant-shadow cache on/off | `stage02+0x241B` (0x3E1A490, 0x3E194E0) | Confirmed |
| +0x49 | CloudsQuality 0..3 | `VolumetricClouds_Q<n>.bshd` | 0x3E47840 (Confirmed) |
| +0x4A | FogQuality 0..3 | `== 3` switches the fog apply shader | 0x3E42130 (Confirmed); other readers not found |
| +0x3C/+0x3D/+0x3F | ModelDetail / DetailDistance / InstanceDistance | LOD and cull distances, both eyes (H) | H report |

Per-view (eye-only possible, not recommended for shadows, see table #5): view config `+0x2B70..+0x2BCF`
(local-shadow atlas size, tile sizes, screen-size factor `+0x2B7C`, budget `+0x2B88`, distances `+0x2B84/+0x2B8C`,
read each frame by 0x3DFB6A0). Atmosphere `+0x54/+0x58/+0x6C/+0x70` (shadow distance, near, split blend, cascade
count) through `[view+0x28A0]`. That object may be shared with main after copy_environment (Unconfirmed).

## 6. Logging plan (one run measures it all)

### 6.1 Per-stage GPU / CPU time per view

The engine has a GPU timer profiler, but it is dead in this build. The executor 0x40C8A10 handles opcodes 0x42 / 0x43 with
0x40CF280 / 0x40CF510: ID3D11Device::CreateQuery(TIMESTAMP_DISJOINT), context Begin/End/GetData, named scopes,
results in 0x90-byte records. It is active only if renderer `+8` is set. **No call site emits 0x42 or 0x43**
(byte scan of every call to the command allocator 0x40F9B30 for `mov byte [reg], 0x42/0x43`; t/opfind.py).
Opcodes 3/4 are state-cache resets, not markers. So the mod measures itself:

1. **Per view:** hook view slot 4 (end-recording) **0x3CC7010** (rcx = view). It closes the view's buffer and
   calls every stage's slot 8, which executes that stage's commands on the immediate context. Around the
   original: QPC, and `context->End(timestampQuery)` before and after. Do the same for the special view.
   One `TIMESTAMP_DISJOINT` Begin/End per frame (first 0x3CC7010 entry .. after the last). Read results 3 frames
   later from a ring (`GetData(..., D3D11_ASYNC_GETDATA_DONOTFLUSH)`).
2. **Per stage:** for the two eye views, give each stage object a private vtable copy whose slot 8 (+0x40) is a
   thunk. It records (view, id from slot 1) and timestamps/QPC around the original. Slot 8 = job wait + command
   execution. Also hook **0x407F240** (command-buffer close = execute; rcx = cmd, stage = cmd-0x30 for stage
   buffers) to split the wait from the execution time. Sub-buffers, e.g. 0x02's 0x32 objects `+0x30` and
   `stage02+0x2690`, are attributed to the current slot-8 stage.
3. Every 120 frames, log per (view, stage id): mean / max GPU ms, execute ms, and slot-8 wait ms. Also total
   per view and the GPU frame time. If the gpu_trace draw counter is armed, add draws per stage.

### 6.2 Validation for #1 before switching it on (log only)

Each 60th frame, in 0x3CC6CB9 for the eye, with the eye's 0x02 still running:
* main and eye 0x02: `+0x18`, `+0x2688`, `+0x2684`, `+0x267C`, `+0x268C`, `+0x2470..+0x248F`, `+0x2490..+0x24AF`.
  Then max |M_eye - rebase(M_main)| over the 4 matrices (expect ~1e-5), and the 3 w-components `+0x9C/+0xAC/+0xBC`
  of each cascade controller (expect 0).
* `[view+0x21D8]` of both views and its `+0x318/+0x320`. The fit's camera object: in 0x3B94540, log
  `[component+8]` once per view and compare with the mod's `g_controller`. Log both slot-9 positions at the
  start of each view's 0x3E1B0C0 and at 0x3CC6CB9.
* The cache fields: main vs eye `+0x21D8`, `+0x241A`, `+0x241B`.
Then switch #1 on (`[views] shared_sun_shadows`) and compare 6.1 timings with it on and off in the same spot.

### 6.3 Validation for #2

At the eye's 0x1E job entry 0x3E01E00 (after waiting for main ctx+0x60): the light count of both 0x35 lists, the
number of lights in the eye's list missing from main's, and the number of matched lights with a different
`+0x10` or `+0x18`. Log a histogram every 120 frames (what fraction of frames would skip). Also log both views'
config `+0x2B70..+0x2BCF` once.

## 7. Overlaps (seen in passing; not pursued, see the owning agent)

* **Agent W - the shared-slot flicker has a cheaper fix than serialising (Confirmed code, effect Unconfirmed).**
  Stage 0x47 execute 0x3E53DE0 copies `[view+0x2268]` into `stage47+0x219C` (instruction at **0x3E53E01**,
  `mov [rcx+0x219C], edx`, 6 bytes). Its job 0x3E53FA0 writes each object's ring offset to
  `obj+0xE0 + [stage47+0x219C]*4`. Every reader takes the index from **the 0x47 stage in its render context**
  (`[ctx+0x20]`), not from the view: 0x3E13520, 0x3F59B10, 0x3F5D2E0, 0x3F7B480, 0x3F7C240 (t3, t4). So writing
  `[eye_stage47+0x219C] = 1` right after 0x3E53E01 gives the eye its own slot (+0xE4), which the game's own
  2-player split screen already uses. The other ~60 `+0x2268` readers keep index 0. With that, the eye's
  stage-0x47 -> main-recording edge (eyefix.cpp `link_eyes`) should no longer be needed. The objects in 0x47's
  list are those with `obj+0x2D0` / `+0x2D8` data (size `+0x104 * +0xF8`); their writers 0x3F59B10 / 0x3F5D2E0
  read the view's controller (4.3).
* Agent W: job-graph decode and stage -> counter map in section 1. The eye has its own MOC (`[view+0x48]`, made
  by CreatePlayerView). #1 removes the eye's cascade cull jobs (ctx+0x48) and cascade render jobs
  (ctx+0x08..+0x20). #2 does not remove the eye's per-light caster culls (ctx+0x50).
* Agent X: SSAO (0x37), clouds (CloudsQuality `+0x49`), fog (`+0x4A`), the post chain. The grading/display-LUT
  compute steps (`PPComputeLUT*`, `PPComputeDisplayLUT_*`) look view-independent and so are shareable like
  exposure. I did not locate the step (0x3EEFD20, which A's table lists as "LUT compute", is something else: it
  weighs CullStage entries of type 3 by player mask; t12). The 0x03 PCSS resolve and the 0x1B lighting scale
  with eye resolution.
* The eye was built with split index 1, so it has stage 0x44 (clouds), and its 0x02 cache captured index 1 at
  construction (renders keys nobody reads; fixed by #1).

## Raw files (agents\T\)

t1 job context 0x40878C0 / arm 0x407B540 / factory 0x3AC51C0; t2 stage 0x47/0x48; t3 `+0x219C` users; t4 readers
of the 0x47 slot; t5 0x41 skinning and the 0x47 upload; t6 cascade fit 0x3B96B00 / 0x3B94540 / 0x3B94B00 and 0x02 ctor
0x3E194E0; t7 resource allocation 0x4098470 (not analysed); t8 0x02 setup / 0x03 prepare and resolve / cache update;
t9 local-light shadow functions; t10 0x35 light gathering; t11 GPU profiler handlers; t12 0x3EEFD20 and the surface mesh
job; t13 `+0x2688/+0x268C/+0x2684/+0x267C` users; t14 0x3E1D830 / 0x3E1F310. Helpers: rd.py (read exe data),
strs.py (string search), opfind.py (opcode emitter scan).

## Open leads

* Whether the resource manager (0x4098470) ever aliases two keys' memory within a frame. Persistence across views
  is observed live, so it seems not to, but this was not read.
* The exact semantics of the 0x35 entry `+0x10` (tile) for the #2 comparison, and the readers of entry `+0x320` /
  key 0x738395A7 in 0x05 / 0x1B / 0x00 (needed for a full copy-based local-shadow share).
* Where the colour-grading LUT step lives in stage 0x21's list (for agent X).
