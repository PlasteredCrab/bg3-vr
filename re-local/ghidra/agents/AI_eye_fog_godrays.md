# Right eye: weaker sun glow, haze and light shafts (agent AI, 2026-10-23)

Static analysis of bg3_dx11.exe 4.1.1.7631656. Ghidra project BG3_A (read-only, -noanalysis, request server
`agents\AI\AIServer.java`, stopped). bg3vr_disasm. Engine shaders extracted read-only with LSLib Divine from
`Data\EngineShaders.pak` into `AI\shaders\` and `AI\shaders2\`, disassembled with `S\dis_bshd.py` into `AI\shaders_asm\`
(PPGodRaysBlur, PPGodRaysBlurAndCombine, PPReExpose, OldPPExposureCompute_*, PPExposure, DeferredFog_*, PPTAA, ...).
Agent Z's fog shaders (`Z\shaders_asm\`). The mod's 14:02 log (`bin\bg3vr\bg3vr.log`) and source (stereo.cpp,
eyefix.cpp, views.cpp). RVAs are relative to 0x140000000. "main" = left eye view, "eye" = right eye view.
cbN[i] = constant buffer bN, row i. Raw output: `re-local\ghidra\agents\AI\` (ai1..ai12).

## Fix table

| # | cause | RVA / hook | what to write, when | risk | status |
|---|-------|-----------|---------------------|------|--------|
| 1 | **The eye's fog history is rescaled by main's exposure over the eye's own exposure.** VolumetricFogLighting keeps 93 % of last frame's in-scatter and multiplies it by `t26[0] / t1[0]` (this frame's / last frame's exposure). t26 comes from 0x3CCCB20(view), which the mod now redirects to main for the eye. t1 is picked **inline** in the same job from the view's **own** stage 0x21 step, so the redirect never reaches it. For the eye the factor is `E_main(n-1) / E_eye(n-2)` instead of about 1. The history then settles at `0.07 X / (1 - 0.93 k)`: k = 0.9 gives 43 % of the right haze, k = 0.5 gives 13 %, k > 1.075 runs away. Main's factor stays about 1. Only the in-scatter (rgb) is scaled, so the sun lobe and the haze over far terrain go, and extinction stays. This started today: the history only survives since `[view+0x2B00]` keeps the proxy (keep_view_history), and the t26 redirect is also new. | **mid-hook 0x3E3E337** in the fog lighting job 0x3E3CCE0 (render-side worker, while recording). At the hook: rsi = view, r8 = the t1 record. Bytes: `F2 0F 10 05 69 8E 7B 01` (movsd xmm0,[rip+..], RIP-relative, so the trampoline must relocate it). 0x3E3E337 is the join of 4 branches; nothing jumps into 0x3E3E338..0x3E3E33E. | If rsi is the eye view and the redirect is on: `r8 = original 0x3CCCB20(main)` (call the trampoline, the same record t26 got). The ratio becomes exactly 1 for the eye. The eye's history is then not rescaled while exposure adapts (about 14 frames of lag). Keep it behind the same switch as the redirect ([views] eye_exposure_from_left). Alternative with no new hook: turn the redirect off (both eyes self-consistent again, but each eye has its own exposure level, as in the 13:20 run). | low (one register, one view, the game's own record type) | Mechanism **Confirmed** (code 0x3E3E210..0x3E3E337, bind slots 0x58C6A78 = t26 / 0x55F71A8 = t1, shader line 193-199, log shows both steps run). Size and sign of k in the user's scenes: Unconfirmed (plan 1). |
| 1b | (consistency, optional) PPReExpose of the eye uses the eye's own steps | post step vtable 0x58D3D80, job **0x3EE0D10** (shader PPReExpose: `out = colour * t0[1] * t1[0]` = colour × E(n)/E(n-1), from the view's own step +0x58 / +0x60) | Only if plan 1 shows a whole-image brightness gap after fix 1: make the eye bind main's two records here too (mid-hook after the record pick at 0x3EE0EAA..0x3EE0F2x, or skip the eye's old exposure job 0x3EDFC40 and point its step records at main's). Both ratios are about 1 in steady state, so this matters only while adapting. | low-medium | Code Confirmed; visual need Unconfirmed |
| 2 | God rays fainter (right eye, 338-362 s) or too strong (80 s) | none of their own | The god-ray step takes **sky pixels only** (linear depth == 1) whose luminance passes a threshold (lighting +0x23C), from the scene colour after the fog. Its geometry is per eye and correct (section 2). With the eye's in-scatter scaled by k, fewer (k < 1) or more (k > 1) sky pixels pass, so the shafts follow fix 1. No separate fix. | - | Shader logic Confirmed; link to fix 1 Unconfirmed (plan 1 A/B) |
| 3 | One froxel column (and one light-cluster column) is shared by both eyes | none; tile size 9 px (froxels), 72 px (clusters) | The eye's viewport starts at render x = 1036 (log). 1036 = 9·115 + 1, so froxel column 115 is in both views' dispatch windows (`udiv cb9[1].z / 9`, limit `cb9[2].x / 9`). The eye runs later on the GPU and overwrites main's column 115 in the ping-pong volume, the depth-max tiles and the gather volume. Next frame main reads the eye's column as history. Effect: a 9-render-pixel seam at the inner (right) edge of the left eye. Fix if visible: make the per-eye render width a multiple of 72 (resolution.cpp picks the output size), or render the eye with split index 1 for stage 0x1B only (the game's player-2 offset, cost: the +1 grid column needs `[view+0x2298]`). | low | Confirmed (arithmetic from code and the logged 1036 px viewport); visibility Unconfirmed |
| - | Hypothesis A (both eyes in the same froxel region): **ruled out** | - | Froxel and depth-tile grids are frame-wide (view+0x2290 = frame render size, 0x3CC9190). Each view dispatches and reads only its own tile window (cb6 +0x150..+0x15C from its viewport, cb0[10] clamp in the shader). Only column 115 overlaps (row 3). History parity `[[view+0x10]+0x4280] & 1` is the same frame counter for both, which is correct for disjoint regions. | - | Confirmed |
| - | Hypothesis B (sun position from the wrong camera): **ruled out** | - | Sun UV is computed in the PPGodRaysBlur VS from cb12[0] (sun direction) through cb11 rows 0-2 (view rotation) and 4-7 (projection), then mapped into the view's part of the frame by cb9[0]. cb11 is the view's PerCamera block, which the mod rewrites per eye (eye pose, eye projection). The head's union FOV, main's camera and stale data are not used. The prepare gate 0x3ED0B40 uses the proxy's forward axis (same rotation as the head). | - | Confirmed (shader, 0x3ED0B40); which PerCamera block is bound for post steps: Unconfirmed (both are rewritten, so it doesn't matter) |

Order: fix 1, then run plan 1. Fix 3 only if a seam shows.

## 1. Why the eye's in-scatter toward the sun is weaker

### 1.1 The fog history update (Confirmed, VolumetricFogLighting asm lines 104-207)

* Each froxel reprojects into last frame's volume with cb0 rows 0-3 (the mod's per-eye reprojection at 0x3E32C44)
  and clamps to its own tile window (cb0[10]).
* History value: `H = t0(reprojected) * (t26[0] / t1[0]) * 0.93`. The new sample gets weight 0.07. If `t1[0] == 0`
  the factor is 1.
* Only rgb (in-scatter) is scaled. The sun term is `sun colour x HG phase(dot(cb12[0], view ray)) x shadow`. The view
  ray comes from the PerView corner rays cb12[7..9] (rewritten per eye) and the froxel's frame UV. So the phase lobe
  is per eye and correct. The lobe is the brightest in-scatter, so any gain on the history shows most there.

### 1.2 Where t26 and t1 come from (Confirmed, 0x3E3E210..0x3E3E35E)

| slot | how it's picked | for main | for the eye today |
|------|-----------------|----------|-------------------|
| t26 (`[58C6A78]` byte 6 = 0x1A) | `call 0x3CCCB20(rsi = view)` at 0x3E3E232: stage 0x21 step, record of parity `(frame-1)&1`, fallback `[step+8]+0x29C8` | E_main(n-1) | **main's** E(n-1) (mod hook on 0x3CCCB20) |
| t1 (`[55F71A8]` byte 6 = 0x01) | inline: `0x407AE00(view+0x18, 0x21)`; `[0x604AA10]+0x75` picks step +0x2148 (old) or +0x2140 (new); record of parity **`frame & 1`** if `step+0x18` and the buffer and its valid byte are set, else `[step+8]+0x29C8`, or `view+0x29C8` with no stage | E_main(n-2) (main's 0x1B runs before its own 0x21 job writes this parity) | the **eye's own** E_eye(n-2) |

* A scan of .text for the parity pattern (`ai5_parity_sites.txt`) finds the current-parity read only at 0x3E3E2A8 /
  0x3E3E2F9. The fog lighting job is the only reader of t1.
* The log confirms both steps run: the eye's 0x3CCCB20 result was its fallback `eye+0x29C8` (0x1C365543F98) on the
  first call and a step record (0x1C231ED2CD8) by call 20000. Main's (0x1C2197382D8) is not `main+0x29C8`. The new
  exposure job ran 0 times, so these are the old steps (job 0x3EDFC40).
* The stored exposure (OldPPExposureCompute_Progressive): `[0] = 1/(1.2·L)`, `[1] = 1.2·L`, `[2] = log L`. Each eye's
  job meters its own image region. So `E_eye != E_main` whenever the two images differ in average brightness.

### 1.3 Why it went wrong today

* Before keep_view_history, `+0x2AED` was 1 every frame (AE fix 3), so cb0[9].w reset the history every frame and the
  ratio was never used.
* Before the 0x3CCCB20 redirect, t26 and t1 were both the eye's own, so the ratio was about 1.
* With both changes the eye's history gets a constant gain k per frame. The fog is the main brightness in these
  scenes, so the eye's image changes, its own meter moves E_eye, and k moves further. This can run either way: a
  darker right eye (k < 1, the 338 s and 200/260 s shots) or a bright band (k > 1, the 80 s shot). Which way in each
  shot is Unconfirmed until plan 1.

### 1.4 Other inputs checked (Confirmed, no per-eye error found)

* Fog layers, atmosphere and lighting ids, distant-shadow cache index: log 14:02-14:03 shows the same single layer
  (weight 1) in both views, cache index main 0 / eye 1, parity main 0 / eye 1.
* Froxel grid and tile windows: section 3.
* Jitter: froxel jitter from t6 and cb13[0].z (frame index), the same for both views; fine for disjoint regions.
* Stage 0x40 velocity and stage 0x1B reprojection: per-eye hooks active (log 14:02:56).
* DLSS: one NGX feature per view's post step (agent O), so no shared upscaler history.

## 2. God rays (Confirmed)

* Post step vtable 0x58D3270. Prepare 0x3ED0B40: on if `[view+0x28D8]+0x25D`, `[0x604AA10]+0x58` and
  `dot(forward of [view+0x21D8], [view+0x28A0]+0x3C) > 0`.
* Job **0x3ED0BE0** (`job-0x18` = view; lighting = `[view+0x28D8]`), three full-screen passes, each with the view's
  viewport and cb9 (0x40814F0):
  1. PPGodRaysBlur, cb0 = {0, lighting+0x23C}: reads scene colour t0 and linear depth 0xFF193183 (t1), writes
     0x695D4542.
  2. PPGodRaysBlur, cb0 = {1, +0x23C}: plain radial blur, writes 0xF05414F8.
  3. PPGodRaysBlurAndCombine, cb0 = {2, +0x238, +0x234, +0x23C}: blur again, weight `(1 - |uv - sun_uv|)^cb0.y`,
     times cb0.z, added to the scene colour, output key 0xB7DCDE54.
* Pass 1 takes 8 samples from the pixel toward the sun. A sample counts only if its depth == 1.0 (sky) and its
  luminance >= the threshold. So shafts are bright sky behind gaps, smeared toward the sun.
* Sun UV (VS of all passes): `p = cb12[0]` rotated by cb11[0..2], projected by cb11[4..7], NDC to UV, scaled by
  cb9[0]. Per eye and correct with the mod's PerCamera rewrite. Samples are clamped to the view's own window
  (cb9[4..6]), so the eyes don't read each other.
* Intermediates 0x695D4542 / 0xF05414F8 are frame-sized keys used only by this step (ai8). Each view writes and reads
  its own viewport in GPU order main, then eye. No cross-talk.
* So the eye's shafts differ only through the scene colour they read: the sky around the sun after the fog was added.
  That is fix 1.

## 3. Split-index-addressed resources in stage 0x1B (Confirmed)

* Frame-wide grids: froxels `ceil(view+0x2290 / 9) + (view+0x2298 != 0)` columns (0x3CC85D0 / 0x3CC9190), clusters
  72 px. view+0x2290 = the frame render size (2072 x 1064 in the log).
* Per view: dispatch start `cb9[1].zw / 9`, end `cb9[2].xy / 9`, t7/t8 (depth max) read at `column + cb9[2].z` (split
  index), history x offset `+ cb9[2].z`, tile window cb6 +0x150 `(x/9 + 0.5 + split index) / grid`.
* The game separates player 2 by split index 1 plus one spare grid column. With both eyes at index 0 and x = 1036,
  only column 115 is shared (fix 3).
* Ping-pong pairs 0xE66299BB / 0x9165A92D (density) and 0xBCE29AFA / 0xCBE5AA6C (lighting), gather 0xDA734765, depth
  max: all addressed this way. No resource in the fog or god-ray path is selected by split index other than the
  distant-shadow cache (already fixed).

## 4. One-run logging plan

One run: the nautiloid spot looking at the sun through the hull (338 s), then the far terrain (200 s), then the 80 s
spot. Hotkeys or VR menu toggles, 15 s each, with a log line at each toggle.

1. **Exposure ratio** (fix 1):
   * Mid-hook 0x3E3E337 first in log-only mode. Every 60 frames per view: view role, the t26 record (0x3CCCB20
     result after the redirect), r8 (t1 record), whether r8 is `step + 0x88/0xB8 + p*0x10` or a `+0x29C8` fallback,
     the parity `[[[step+0x38]+0x2000]+8] & 1`, step +0x18, valid bytes +0xC8/+0xC9 (old step).
   * The values: extend gpu_trace for CS dispatches whose UAV slot 1 is a Texture3D and whose SRV slots 1 and 26 are
     structured buffers (the VolumetricFogLighting signature). CopyResource both buffers (first 24 bytes) to staging,
     Map two frames later (DO_NOT_WAIT), log `[0]`, `[1]`, `[2]` of each with the current view marker. Do it once a
     second.
   * Prediction: main `t26[0]/t1[0]` within a few % of 1. Eye ratio clearly below 1 at 338 s (weak glow) and above 1
     at the 80 s band.
   * A/B (zero code): `[views] keep_view_history = false` (history reset every frame). Prediction: both eyes' glow and
     shafts match (noisier fog). `[views] eye_exposure_from_left = false`. Prediction: the glow matches again, the
     overall exposure level may differ per eye.
   * A/B: fix 1 on. Prediction: glow, haze and shafts match, with history kept.
2. **God rays** (row 2): gpu_trace for one frame: list the three PPGodRays draws per view (RT key, viewport, SRVs),
   read back the pixel at the sun position in 0x695D4542 and in the combine output for both views, and log
   lighting +0x234 / +0x238 / +0x23C and prepare result `step+0x18` per view. Prediction: same thresholds and gate in
   both; pass-1 brightness tracks the fog difference and equalises with fix 1.
3. **Shared column** (row 3): screenshot the inner edge of the left eye with fog in view, fix 1 on. Log the eye's
   ReadState render viewport x / w and `[view+0x2298]`. Prediction: x = 1036, a faint 9-render-pixel seam.
4. **Re-expose** (row 1b): only if a whole-image brightness gap remains after fix 1: log per view the two records
   0x3EE0D10 binds and their `[0]`, `[1]` values the same way as item 1.

## Raw files (agents\AI\)

ai1 god-ray step slots (0x3ED0AF0..0x3ED2120); ai2 god-ray job 0x3ED0BE0 and resource creators; ai3 callers of the
froxel resize / CB builder; ai4 +0x29C8 displacements; ai5 exposure parity readers (scan_parity.py); ai6 / ai7 / ai10
exposure readers, PPReExpose step (vtable 0x58D3D80, job 0x3EE0D10); ai8 key references (findimm.py); ai9 string
xrefs; ai11 hook site 0x3E3E337; ai12 t26 / t1 selection 0x3E3E1F0; `shaders_asm\` god rays, PPReExpose, exposure,
TAA, bloom, lens flare.
