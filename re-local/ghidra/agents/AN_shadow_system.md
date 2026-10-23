# Shadow system map and two-view audit - agent AN, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656. Ghidra project BG3_A (read-only, -noanalysis, request server
`agents\AN\ANServer.java`, stopped). bg3vr_disasm. Shaders extracted read-only with LSLib Divine from
`Data\EngineShaders.pak` into `AN\shaders\`, disassembled with `S\dis_bshd.py` into `AN\shaders_asm\`. Mod source as of
the 15:41 DLL (shadows.cpp, stereo.cpp, views.cpp, resolution.cpp, cull.cpp, eyefix.cpp) and the 15:08-15:11 log.
RVAs are relative to 0x140000000. "main" = left eye view, "eye" = right eye view (CreatePlayerView 0x17F82F0).
Confirmed = read in code, shader or log at the cited place. Unconfirmed = section 4 says how one run settles it.
Raw output: `re-local\ghidra\agents\AN\` (an1..an17, list at the end).

## 1. Per-view differences still present (ranked)

The shadow inputs are now equal or correctly eye-specific in steady state. What is left is transient, timing, or
by-design per-eye work. Nothing static in the shadow path explains a permanently darker right eye by itself.

| # | difference | where | status | fix |
|---|-----------|-------|--------|-----|
| 1 | **During every environment blend the eye blends a different path.** `copy_environment` gives the eye main's target with the *remaining* time. The eye's blend starts from its own active object at copy time, main's from its start at request time. So for the whole blend the eye has different sun colour, sun direction, PCSS size, cascade splits, shadow pull-back, SSAO strength and cloud-shadow rows. Both end equal. | views.cpp `copy_environment`; setters 0x3CC9700 / 0x3CCAAC0; tick 0x3CC8760 (sun dir view+0x2A40 from active atm +0x3C/+0xA4); PCSS 0x3E1FF80 (atm +0x5C/+0x60/+0x64/+0x68); splits 0x3E1B0C0 (atm +0x54/+0x58/+0x6C) | Mechanism Confirmed. Size Unconfirmed (plan 4.1) | Pass main's full duration (`+0x28AC` / `+0x28E4`), not the remaining time, then write the eye's elapsed `+0x28A8` / `+0x28E0` = main's. The eye's start is its previous active, which equals main's start when the eye was in sync before the request, so t and the lerp match frame for frame. |
| 2 | **Sun cascades are fitted twice and may differ.** Sharing was rejected at 15:10:25-27 with a constant 5.20e-02. That run had the eye's environment parked (AL/AM: copy at 15:10:22.427 did not apply), so its atmosphere differed from main's while the 60 checks ran. Rejection is permanent for that eye view. Each eye's own cascades are self-consistent, so this shows as different cascade edges and shimmer, not darkness, unless the environment differs (row 1). | shadows.cpp `validate`, `on_execute` | Cause of the 5.2e-2: Unconfirmed (plan 4.2 names the element). Rejection logic: Confirmed (code). | Start validating only when both views' atm +0x54/+0x58/+0x68/+0x6C/+0x70 and view +0x2A40..+0x2A48 are equal and neither view is blending; log the worst element; retry after a rejection (e.g. every 10 s). While sharing, stop for a frame when those fields differ. |
| 3 | **The cascade fit reads the live camera controller, not the per-frame proxy.** 0x3B94540 uses `[component+8]` of the camera entity (FOV +0x14, aspect +0x20, inverse view +0xD0..+0x10F) and may even call 0x4085150 / 0x40851F0 on it. Main's and the eye's executes (0x3E1B0C0) run at different times. If the game thread writes the controller in between, the two eyes fit cascades to different head poses. | 0x3E1B0C0 -> 0x3B96B00(proxy+0x318) -> 0x3B94540 | Code Confirmed. Concurrent writes: Unconfirmed (plan 4.3) | Sharing (row 2) removes it. Without sharing: snapshot the camera object fields at main's execute and give the eye the same (not worth it; share instead). |
| 4 | **Local-light shadow allocation is per eye.** Stage 0x35 job 0x3DFB6A0 sorts lights by a priority from the eye's slot 9, applies the tile budget, fades with its own per-stage history (`+0x21A8`) and picks the tile size from distance, radius and FOV. At the budget or size thresholds one eye can shadow a light the other does not, or at another resolution. | 0x3DFB6A0, 0x3DFBF80, 0x3DFC660, 0x3DFD980 | Mechanism Confirmed. Frequency Unconfirmed (plan 4.4) | First log. If it happens: give the eye main's decisions per light (tile `+0x10`, scale `+0x18`, fade `+0x324`, record `+0x320`) and let the eye's own 0x3DFC660 / 0x3DFD980 write eye-relative records. |
| 5 | Camera-attached lights (lighting +0x288) are spawned per view by 0x3CCB020 at the end of every 0x3CCAAC0, including each forced copy. Both eyes see both lights. Symmetric, not left/right. Whether they cast shadows: Unconfirmed. | 0x3CCB020, tick 0x3CC8760 | AL row 5 | AL plan 5. |
| 6 | Distant cache, if re-enabled in "left" mode: the eye's 0x03 init (0x3E1EAC0 at 0x3E1ED69) and fog-lighting init (0x3E3C740) declared the index-1 keys, but the eye then reads the index-0 keys. The eye also still refreshes and renders its own index-1 cache, which nobody reads. | shadows.cpp Left mode | Declared-key effect Unconfirmed (AK G1). Off by default, so no effect today. | Keep `vr_distant_shadows = false`. If turned on, use AK's G1 check; optional: zero the eye's `+0x241B` after 0x3E1A490 and before its execute to skip its own cache work (the post-execute copy restores main's value for the readers). |

Equal by design (no fix): the fog per-frame view ordinal `[view+0x2270]` (0 main, 1 eye; written by 0x4072B30) gates
the ping-pong history-reset bookkeeping to ordinal 0 (0x3E3AB19, 0x3E3CC50, consumed at 0x3E3AC6D / 0x3E3CD9D). This
is the split-screen player-2 path. Confirmed code; effect on the eye's region after a reallocation Unconfirmed.

## 2. Mistakes in the mod's current code

| # | file | mistake | evidence | fix |
|---|------|---------|----------|-----|
| M1 | shadows.cpp | Cascade validation runs in the first 60 frames of a new eye view and rejects for good. It does not check that the eyes' environments are equal, so a late or parked environment copy rejects sharing for the session. | log 15:10:25-27 (constant 5.20e-02 while the eye's environment copy was parked); code `validate`, `on_execute` | Row 2 fix. |
| M2 | views.cpp | `copy_environment` passes the remaining blend time and leaves the eye's elapsed at 0, so the eye follows a different blend path (row 1). | code lines 289-313; setter semantics 0x3CCAAC0 (AL 1.1) | Row 1 fix. |
| M3 | stereo.cpp | `on_ssao_rays` fixes only the offsets `[rbp+0xA8]/[rbp+0xAC]`. The scales `[rbp+0xA4]` (written at 0x3E4FCEB) and xmm0 (stored to `[rbp+0xA0]` at the hook site 0x3E4FD36) come from the proxy's +0x110. They are the eye's only while `[views] eye_surface_mesh` is on (default). With it off, scale and offset come from different projections. | disassembly 0x3E4FCB9..0x3E4FD36 (an11) | Also set `ctx.xmm0 = 2/(w*P0)` and `[rbp+0xA4] = -2/(h*P5)` from PerCamera[0], w/h = int `[rbp+0x30]`/`[rbp+0x34]` (constants at 0x591CEFC / 0x591D360). |
| M4 | shadows.cpp | `on_cache_index` logs `ctx.rip - g_module_base`, which is the trampoline, not the hook site ("exe+0xFFFFFFFFFFFF0144"). Cosmetic. | log 15:10:25.608 | Log the known RVA of the hook that fired. |
| M5 | shadows.cpp (latent) | When sharing, if the eye's execute ever runs before main's in a frame, `share()` uses last frame's matrices (counted as `g_stale`) with this frame's cascade texture. One-frame shadow offset during motion. | code `share`; order Unconfirmed | Keep the counter; if it is ever non-zero, fall back to the eye's own pass for that frame (decide in `on_prepare` from last frame's order). |

Verified correct against the game code (no change):
* Left mode copy range. Cache = stage02+0x21A0. Parity cache+0x38 = +0x21D8. Matrices cache+(2l+1+p)*0x40 and
  projections cache+(2l+5+p)*0x40 = +0x21E0..+0x23DF. Origins cache+0x240..+0x26F = +0x23E0..+0x240F. On flag
  cache+0x27B = +0x241B (0x3E1D830, 0x3E1C940, 0x3E1A490). Not copying +0x241A (index), +0x2419, +0x2454 is right.
* Own-mode mid-hooks 0x3E3D121 / 0x3E1F68A: bytes and meaning (rcx = view, al = live split index) match; left alone
  while sharing is right (records are main's).
* Re-basing in `rebased()`: the matrix is V' x P' with V' row 3 = sum_j Delta_j V_row_j (0x3E1B0C0 lines 136-213), so
  `r3 += d.x r0 + d.y r1 + d.z r2` is exact when the cascade view's w terms (+0x9C/+0xAC/+0xBC) are 0.
* resolution.cpp `+0x48 = 0`: 0x3E1A490 copies `[0x604AA10]+0x48` into +0x241B every frame for main-frame views; the
  resolve (cb0[0].y) and fog (cb1 +0x04) then skip the cache in both eyes.
* stereo.cpp proxies: slot 9 feeds the cascade translation (0x3E1B0C0), 0x3E1D830, the local-light records
  (0x3DFC660) and the cull query position (0x3F7D370 / 0x3F7DFD0 / 0x3E246B0); this matches cb11[13] and the
  rewritten corner rays the shaders use. Slot 11 is not read by any shadow code found. The proxy's +0x318/+0x320 copy
  keeps the camera entity (and so the cascade fit) shared.
* cull.cpp: bit 0x0001 removes the head from CullStage (0x201) only. Cascades need 0x200|bit, cache 0x8200, local
  lights 0x220 (AA). Both eyes render with index 0, so both see the same casters.
* eyefix.cpp: the eye's stage 0x47 slot 1 is read through the render context's own 0x47 stage, so the eye's cascade and
  local-light draws use the eye's slot. Streaming skip: residency is global, no shadow effect.

Correction to AL: 0x94009344 is not the sun shadow mask. The mask is **0x861C73EF** (0x03 renders it, 0x3E1F88E; read by
stage 0x00 0x3D9603D, clustered lighting 0x3E35934, fog 0x3E3C80B). 0x94009344 is a structured buffer of 64-byte
per-object records taken from CullStage +0x3B8..+0xBB8 by 0x3E1F310 (resolve shader t38, flag at +0x28).

## 3. The shadow system

Threads: env tick (view slot 7 0x17F2910 -> 0x3CC8760) on the game thread. Stage prepares (slot 5) in frame sync
0x4094E60. Stage executes (slot 6) in each view's begin-recording 0x3CC6C00, render thread. Jobs on workers. Slot 8
(wait + execute commands) at end-recording 0x3CC7010, render thread, immediate context, main's stream before the eye's.
Keys are frame-wide; each view writes then reads its own (A 1.5, T 1).

### 3.1 Sun cascades - stage 0x02 (per view)
* ctor 0x3E194E0 (+0x241A = split index at build: 1 for the eye; +0x267C/+0x2684 from ShadowQuality), init 0x3E19910
  (RTs 0x3E1AA10 on key 0x3E344F11 slices 0..3; cache cull object +0x2460; cache 0x3E1C230), release 0x3E1A1B0.
* per-frame 0x3E1A490: count +0x2688 = atm+0x70 (1..4); +0x2678 tracks `[0x604AA10]+0x43` (re-runs 0x3E1AF90 /
  0x3E1B010); cascades +0x220 = resolution; +0x241B = `[0x604AA10]+0x48` (main-frame views only).
* prepare 0x3E1A410: run if controller, +0x268C, scene `[view+0x2B10]`, a global flag, atm+0x50 (sun on).
* execute 0x3E1B0C0 (render thread): splits +0x2470 from atm +0x58/+0x54/+0x6C; per cascade: 0x3B96B00(proxy+0x318)
  -> 0x3B94540 fit (camera object FOV/aspect/inverse view, sun dir view+0x2A40, pull-back atm+0x68, texel snap) ->
  ortho 0x40848B0 on the cascade controller +0x1F8 -> 0x3B94B00 caster planes (cascade planes + camera planes
  +0x210..+0x268 facing away from the sun) -> matrix +0x24B0 = V' x (P x bias 0x5EF2C70), translation from view slot 9;
  +0x2490 = cascade +0x1C, +0x24A0 = 2r. Submits cascade culls (job 0x3F7D300 -> 0x3F7D370, ctx+0x48), the cache cull
  (0x3F7E2F0 -> 0x3F7DFD0), the 4 cascade render jobs (0x32 objects +0x2140.., 0x3E18CE0, ctx+0x08..+0x20), then
  0x3E1C940 (cache refresh) and 0x3E1BB10.
* 0x3E1BB10: every 32 frames downsamples the last cascade into 0x32935570 and reads it back 3 frames later into a CPU
  grid stored on every cascade object +0x470..+0x4DF. Only 0x3F7D370 reads +0x470 (cascade cull). No render consumer
  (0x32935570 appears only in stage 0x02 code; an5).
* slot 8 0x3E1B9D0: returns at once when +0x18 = 0 (the sharing skip is safe).
* cascade cull 0x3F7D370: planes from view+0x2338 (clip volume picked each tick by 0x17F3060 from the WriteState
  camera: equal), cascade planes, mask 0x200|{2,4,8,0x10}[i], query position = view slot 9, CPU grid +0x470.

### 3.2 Distant shadow cache (inside stage 0x02, cache = +0x21A0)
* 0x3E1C230 / 0x3E1DB10: keys by +0x241A (0 -> 0xC0F3544B 2048 / 0x447C4203 1024; 1 -> 0xB7F464DD / 0x337B7295).
* 0x3E1C940 (gate +0x241B): 16x16 tiles of 256 units around the camera entity offset by view+0x2A40; flips parity
  +0x21D8; writes the records of the new parity (world space).
* tile cull 0x3F7DFD0 (mask 0x8200), tile render 0x3E18CE0 -> 0x3E187D0 (slice by +0x21D8 at render time, clear when
  +0x2419).
* lookup 0x3E1D830(cache, level, campos): parity (p-1)&1, matrix relative to campos. Readers: resolve 0x3E1F520
  (keys by live index at 0x3E1F683), fog lighting 0x3E3CCE0 (0x3E3D11A). Off in VR (+0x48 = 0).

### 3.3 Sun shadow resolve - stage 0x03 (per view)
* init 0x3E1EAC0: blue noise `Engine_BlueNoise_Grey_64.dds` (global), shader sets `/Shadows/Generated/DirLight_Cascaded*`
  (+0x2158) and orthographic (+0x21C0), declared reads 0xFF193183, 0xA2037A4D, 0xD5044ADB, 0x3E344F11 and the cache
  pair by build index; output key 0x861C73EF; +0x2238 = atm+0x50.
* prepare 0x3E1F2F0 (& +0x2238). job 0x3E1F520 (worker): 0x3E1F310 uploads 0x94009344 records; 0x3E1FF80 sets
  +0x2140 = atm+0x64 x 0.5 deg->rad, +0x2144 = atm+0x68, +0x2148 = atm+0x5C, +0x214C = atm+0x60, picks the shader by
  count and 0x02's +0x2678, builds cb6 with 0x3F3CF00; cb0: +0x00 = `[view+0x2961]`, +0x04 cache on, +0x08 parity,
  +0x0C last split, +0x10/+0x50 cache matrices (0x3E1D830, slot 9); extra sampler state when +0x267C == 1.
* shader DirLight_Cascaded_PCSS_4x2048: position = linear depth x cb12[7..9] ray (camera-relative), normal from
  derivatives, backface vs cb12[0]; cascade select cb6[4..19] with blend border cb6[1].z; PCSS blocker search (Vogel
  disk, rotation from blue noise and cb13[0].z/w), penumbra from cb6[1].x/w, cb6[2], cb6[3]; t1/t5/t38 flip the normal
  offset for backlit material types 3/4; cache past cb0[0].w. No history.

### 3.4 Local-light shadows - stages 0x35 and 0x1E (per view)
* 0x35 job 0x3DFB6A0: lights from CullStage buckets +0x100/+0x130/+0x160; priority (entry +0xC) from slot 9; budget
  +0x2134 (point lights take 4 tiles); fade history +0x21A8 (step `[0x614FB84]`); screen size from config
  `+0x2B70 + q*0x20` (+0x2B7C, +0x2B84, +0x2B88, +0x2B8C) and controller FOV +0x14; `[view+0x2274]` (0 in both)
  disables the distance fade; quadtree tiles 0x3F77820; per entry +0x10 tile, +0x18 scale, +0x324 fade; records
  0x3DFC660 (relative to slot 9) and 0x3DFD980 into 0x738395A7.
* 0x1E: prepare 0x3E01870, execute 0x3E018A0, per-frame 0x3E01710 (per-light cull objects 0x3DFFFF0 -> 0x3E00310,
  mask 0x220, ctx+0x50), job 0x3E01E00: a CameraController per light from its transform as the context controller,
  tile rects from config +0x2B70/+0x2B74, depth into 0xC91746C5 (0x3E02440), linearised into 0xFA76F838 (0x3E02DF0).
  Atlas content is viewer-independent.

### 3.5 Ambient occlusion - stage 0x37 (per view)
* prepare 0x3E4F430: `[0x604AA10]+0x5A` and atm+0x18C. job 0x3E4F490: cb0[1] rays from controller +0x110 (+ mod hook
  0x3E4FD36), cb0[3] = atm +0x198/+0x190/+0x194; PPSAO_Deinterleave / PPSAO / PPSAO_Blur / Reinterleave; keys
  0x361F94A4, 0xD3052CFC, 0x74EA5362 (AO result), 0x8517349C. Blur is depth-aware in screen space, no history.

### 3.6 Consumers
* clustered lighting 0x3E35C80 (ClusteredLighting_N): cb6 cascades, mask 0x861C73EF, atlas 0xFA76F838 + records
  0x738395A7, AO 0x74EA5362 with atm +0x198/+0x19C, cloud shadows t42/t43 with PerView rows 13..22 (atm +0x218/+0x21C
  minus view +0x2A68/+0x2A6C, view +0x2AB0/+0x2ABC, blend cb12[13].z). No camera projection (only cb11[12]/[13]), so no
  screen-space contact shadows (Confirmed for _0 and _4).
* forward / transparent stage 0x00 (0x3D961D0, declarations 0x3D987C0): cascades 0x3E344F11, mask 0x861C73EF, atlas,
  AO. Hair and transparent surfaces use this; there is no separate hair, character or translucent shadow pass in the
  engine shader list.
* fog lighting 0x3E3CCE0: cascades, mask, atlas, distant cache; history exposure (mod hook 0x3E3E337).
* skinning 0x41 (0x3E15670): adds cascade and per-light casters only when 0x02 / 0x1E run.
* shadow proxies: VisualFlags 0x40 -> cull bit 0x0001 cleared (0x3C64730); the head hide uses the same bit.

## 4. One-run logging plan (unconfirmed items)

One run: start VR outdoors in sun, stand 10 s, walk into an area or start a dialogue that changes lighting, stand 10 s,
then a torch-lit interior. Log on the render thread in the existing hooks unless noted.

4.1 **Blend paths (row 1).** Game thread, every 10 frames while either view blends, else every 120: per view
`+0x28A8/+0x28AC/+0x28B0`, `+0x28E0/+0x28E4/+0x28E8`, target/start/active ids, and active atm +0x54, +0x58, +0x5C,
+0x60, +0x64, +0x68, +0x6C, +0x70, +0x18C..+0x198, view +0x2A40..+0x2A50. Prediction today: values differ during
the blend and match after it. With the fix: equal every sample.

4.2 **Cascade check (row 2).** In `validate`, log the worst element (matrix i, row, column or split slot) with both
values, plus the 4.1 fields of both views and stage03 +0x2140..+0x214C of both. Do not reject while the environments
differ; log each retry. Prediction: the 5.2e-2 element is a split or +0x24A0 radius tied to an atmosphere difference.

4.3 **Fit timing (row 3, M5).** At entry of 0x3E1B0C0 per view: thread id, QPC, and a CRC of the camera object
controller +0xD0..+0x10F (pointer = `[[out+0x70]+8]`, out = 0x3B96B00's output; fallback 0x17F3A50(world)), and
of the proxy +0x90..+0xCF. Count frames where main's and the eye's camera CRC differ, and frames where the eye's
execute precedes main's (also report `g_stale`). Prediction: equal when the head is still; differs in motion if the
game thread writes concurrently.

4.4 **Local lights (row 4).** At the eye's 0x1E job 0x3E01E00 (view `[rcx-0x18]`), after main's 0x35 finished: per
light (matched by `*entry`) selected in one eye only, different `+0x10` tile size or `+0x18` scale, and `|fade|`
difference > 0.05. Histogram every 120 frames. Prediction: zero most frames; non-zero near the budget.

4.5 **Cloud shadows.** Every 120 frames, CRC of PerView[0] rows 13..24 (`view+0x2340+0xD0..+0x18F`) for both views,
and view +0x2A68/+0x2A6C. Prediction: equal (AE field diff 12:44:47).

4.6 **GPU confirmation.** gpu_trace one frame per spot: at each view's 0x03 resolve draw, read back the 0x861C73EF
pixel at the viewport centre, and the AO 0x74EA5362 pixel at the same place. Compare main and eye. If the masks match
and the image still differs, the cause is after shadows (AL/AM/AK).

4.7 **SSAO (M3).** Log-only at 0x3E4FD36 once per 600 frames per view: xmm0, `[rbp+0xA4..+0xAC]`, proxy +0x110
P0/P5, PerCamera[0] P0/P5/P8/P9, and the `eye_surface_mesh` flag.

## Raw files (agents\AN\)

an1 / an2 / an7 / an12 / an14 displacement scans (+0x2A40.., cloud fields, +0x2338, +0x2274/+0x23B4, +0x226C/+0x2270/
+0x2278); an3 +0x218 search; an4 0x3E1BB10, 0x3E19910, 0x3E1EAC0, 0x3E1DB10, 0x3E1A1B0, 0x3E1A370, 0x3E1B9D0,
0x3E1F310, 0x3E1F2F0, 0x3E1D830; an5 key references (findimm.py); an6 consumers of the shadow keys; an8 / an9 / an10
clip volume 0x17F3060 / 0x17F3280 / slot 7 0x17F2910; an11 SSAO disassembly; an13 0x1B75D50; an15 / an17 view
ordinal readers; an16 0x3E01E00, 0x3E01710, 0x3E01870, 0x3E018A0, 0x3E4F430; `shaders_asm\` DirLight_Cascaded_*,
ShadowMapDownsample, PPSAO_*, PPSSSS_H, SSSRApply, ClusteredLighting_4, VolumetricClouds_*.
