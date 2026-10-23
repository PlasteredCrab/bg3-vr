# Right eye: distant haze and sky darker than the left eye (agent AE, 2026-10-23)

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_A, read-only, -noanalysis, request server
`agents\AE\AEServer.java`, stopped), bg3vr_disasm, the engine shaders already extracted by agents N and Z
(`N\shaders_asm\ClusteredLighting_0`, `Z\shaders_asm\VolumetricFog*`, `DeferredFog_*`, `EoCFog`), and the mod's
2026-10-23 12:44 log (`bin\bg3vr\bg3vr.log`). RVAs are relative to 0x140000000. Raw output is in
`re-local\ghidra\agents\AE\` (ae0..ae9). "main" = the game's player view (left eye), "eye" = the view made by
CreatePlayerView 0x17F82F0 (right eye). cbN[i] = constant buffer bN, row i.

## Fixes for the VR mod

| # | problem | cause | RVA / hook | how | risk | confidence |
|---|---------|-------|-----------|-----|------|------------|
| 1 | Far froxels (beyond the sun cascades) get no sun in-scatter in the eye; far terrain is shadowed in the eye | **Distant-shadow cache mismatch.** The eye's stage 0x02 cache was built with split index 1 (ctor 0x3E194E0 stores `stage02+0x241A = [view+0x2268]`), so its cache renders into keys **0xB7F464DD / 0x337B7295** (0x3E1C230 picks keys by cache+0x27A = stage02+0x241A). The eye renders with `[view+0x2268] = 0`, so its fog lighting job and its sun-shadow resolve bind **main's** cache textures 0xC0F3544B / 0x447C4203 (0x3E3D11A in 0x3E3CCE0, 0x3E1F683 in 0x3E1F520), but with the **eye's own** cache matrices, origins and parity (0x3E1D830 on eye stage02+0x21A0, parity byte stage02+0x21D8). The two caches refresh progressively and independently, so the eye looks up main's cache with the wrong matrices and often the wrong slice. VolumetricFogLighting uses this cache for every froxel past the last cascade split (cb1[0].w), so far froxels read as shadowed: no yellow sun in-scatter there. DeferredFog adds the integrated froxel in-scatter to every pixel, sky included (it clamps depth to the last slice), so the sky band and distant spires lose the haze. | render thread: mid-hooks at **0x3E3D121** and **0x3E1F68A** (`test al, al` right after `movzx eax, byte [rcx+0x2268]`; rcx = the view at both) | When rcx is the eye view and the eye's own stage 0x02 is running (shadow sharing not active), set `al = byte [eye_stage02 + 0x241A]` (1). The eye then binds its own cache textures, which its matrices and parity describe. This is what the game's own split-screen player 2 does. If `[views] share_shadows` ever reaches Sharing (eye's 0x02 off, main's cache block copied), leave al = 0. Zero-code A/B: write `[0x604AA10]+0x48 = 0` (distant-shadow cache off for both views; 0x3E1A490 copies it to stage02+0x241B every frame). | low (two byte-register writes, the game's own player-2 path) | Mechanism Confirmed (code, shader). That the cache is on (+0x48) and the two caches are out of phase in the user's scene: Unconfirmed (plan 1). |
| 2 | Sky, aerial perspective, fog in-scatter, clouds and forward smoke scaled differently per eye (only if plan 2 shows the values differ) | **Per-view exposure value.** These terms are multiplied by the exposure buffer that **0x3CCCB20(view)** returns, and lit opaque surfaces are not (ClusteredLighting_0 uses t26 only for the aerial-perspective in-scatter). Readers: 0x3E35C80 (clustered lighting, aerial perspective), 0x3E3CCE0 (fog in-scatter), 0x3E45750 (sky, twice), 0x3E47BB0 (clouds), 0x3CD43D0 (forward / emissive / surfaces). 0x3CCCB20 returns the view's own stage 0x21 exposure step buffer (new step +0x2140, or old step +0x2148 when `[0x604AA10]+0x75 != 0`), else view+0x29C8. eyefix only skips the **new** job 0x3EDE160. The log shows it ran **0 times** (12:44:44, 12:44:48), so the active path is the old step (vtable 0x58D3C78, job **0x3EDFC40**), or none. Each eye computes its own exposure; "shared exposure" is a no-op today. | render thread: hook **0x3CCCB20** (rcx = view) | For the eye return `0x3CCCB20(main)`. It is main's previous-frame SRV record (`step+0x88 + ((frame-1)&1)*0x10`). Main's job this frame writes the other buffer (`step+0xA8 + (frame&1)*0x10`, 0x3EDFC40), so the slot is stable while the eye's commands run. Also extend eyefix to the old job 0x3EDFC40 (view = `[rcx-0x18]`) only if the eye's own post chain should stop computing its own. | low | Readers and parity Confirmed (code). That the eyes' values differ: Unconfirmed (plan 2). |
| 3 | (both eyes, side effect) fog history and exposure adaptation reset every frame | 0x3CC6DC0 (begin-recording step 1) sets `[view+0x2AED] |= ([view+0x2B00] != [view+0x21D8])`, then `[view+0x2B00] = [view+0x21D8]`. stereo.cpp `on_view_begin` (0x17F21E0 entry, before the jmp to 0x3CC6C00) puts a proxy from an **8-slot ring** into +0x21D8 for both eyes, so the address changes every frame and +0x2AED is 1 every frame. Readers: 0x3E32F97 (stage 0x1B: fog history reset), 0x3EDFFF7 (old exposure job: Immediate instead of Progressive), 0x3EDE1EC, 0x3EF427D. | stereo.cpp `on_view_begin` | Right after writing the proxy into ReadState +0x21D8, also write `[view+0x2B00] = &proxy`, unless the real controller changed since the last frame (keep the game's camera-cut reset). | low | Confirmed (code; proxy addresses rotate in the log). Visual effect: Unconfirmed. |
| - | not a cause: atmosphere LUTs, sky textures, PerView sky / fog rows, depth convention | see 2.2 | - | no change | - | Confirmed (code + 12:44:47 field diff) |

Order: apply fix 1 (or its zero-code A/B first), then run plan 2 to decide on fix 2. Fix 3 is independent and helps both eyes.

## 1. What puts haze on far geometry and the sky

### 1.1 Passes (Confirmed)

| term | where | per-view inputs |
|------|-------|-----------------|
| Volumetric froxel fog | stage 0x1B: Initialize 0x3E3ABB0, Lighting **0x3E3CCE0**, Gather 0x3E3F200, apply **DeferredFog** 0x3E42130 | layers (fixed by Z), cascades t36, **distant cache t2/t3 + cb1**, exposure t26, PerView cb12, LUT t39 for the sun colour when cb12[15].x > 0 and cb12[23].w == 0 |
| Aerial perspective (precomputed atmospheric scattering) | **ClusteredLighting_N** (0x1B sub-job **0x3E35C80**), and forward materials via 0x3CD43D0 | t39 transmittance 2D, t40 scattering 3D, cb8 planet constants, cb12[0] sun, cb12[15], cb11[13] camera, **t26 exposure** |
| Sky dome | stage 0x36 job **0x3E45750** (`PPEmissiveSkydome` simple or procedural) | same LUTs (0x3CCD4D0), sky textures view+0x2AC8..+0x2AE0, **exposure**; drawn only if `[view+0x2AEA] == 0` (stereo.cpp copies main's) |
| Volumetric clouds | stage 0x44 job 0x3E47BB0 | LUTs (0x3CCD4D0), exposure |
| God rays (for comparison) | post step vtable 0x58D3270, prepare **0x3ED0B40**: `[lighting+0x25D] && [0x604AA10]+0x58 && dot(camera forward, [atm]+0x3C) > 0`; PPGodRaysBlur / BlurAndCombine on the scene colour | screen-space, no fog or cache input; present in both eyes as reported |
| EoCFog | `depth < 1` and a 2D region mask (t2) at the world xz: fog of war, not haze | - |

* DeferredFog_Trilinear PS: `out = (t1 + t2 + t3) * T + inscatter/32`, `T = 1 - a/32768`, slice from t0 depth by
  cb6[23..25] (linear then log). There is no sky test: sky pixels (in 0x2EDB4AC1, one of t1..t3) take the last
  slice. So the far haze on the sky and on the spires is the integrated far froxels. Only `cb6[25].w < 0` turns it
  off (view+0x22EC, equal in both).
* VolumetricFogLighting: sun term = sun colour x phase x shadow. Shadow = cascades (cb6[4..19], t36) inside the
  cascades. Past the last split (`cb1[0].w`) it is the distant cache: near level t3 with cb1[5..8], else far level
  t2 with cb1[1..4], slice `cb1[0].z`, on when `cb1[0].y`. The whole in-scatter is multiplied by t26[0] (exposure)
  before the store. History: x0.93 scaled by t26/t1, reset by cb13 / +0x2AED.
* ClusteredLighting_0, end: `colour = colour * T + S * t26 * cb12[15].x * 10`. t26 is read once and used only here.
* The fog lighting job's cb1 (0x3E3E07E..0x3E3E155): +0x04 = `stage02+0x241B != 0`, +0x08 =
  `([stage02+0x21D8] - 1) & 1`, +0x0C = `stage02+0x2470[count]` (last split), +0x10 / +0x50 = **0x3E1D830**(stage02
  +0x21A0, level 0 / 1, campos), +0x90 = count. stage02 = the view's own stage 0x02 (`[rdi+0x2148]`).
* 0x3E1D830: parity `p = ([cache+0x38] - 1) & 1`, matrix at `cache + (level*2 + 1 + p)*0x40`, origin at
  `cache + 0x240 + level*0x18 + p*0xC`.

### 1.2 Distant cache per view (Confirmed)

* Textures: 0x3E1C230 creates them by cache+0x27A (stage02+0x241A): 0 -> 0xC0F3544B / 0x447C4203, 1 ->
  0xB7F464DD / 0x337B7295 (2 slices each). ctor 0x3E194E0 (0x3E19654) sets +0x241A from `[view+0x2268]` at build
  time (1 for the eye; agent R section 3).
* Update: 0x3E1C940 (in 0x3E1B0C0, when `[frame+0x40] == 0`), gated by cache+0x27B (= stage02+0x241B). It queues
  16x16 tiles of 256 units around the camera entity (`[ctrl+0x318]`, the same entity for both views) into
  cache+0x2B4 and writes the records for the current parity. Each view refreshes on its own schedule.
* On/off: 0x3E1A490 copies `[0x604AA10]+0x48` into stage02+0x241B every frame (views with `[[view+0x10]+0x40] == 0`).
* Readers that pick textures by the **live** `[view+0x2268]`: 0x3E3D11A (fog lighting) and 0x3E1F683 (0x03 resolve,
  far terrain shadows). Both then use the view's own stage02 matrices and parity.
* Sun-shadow sharing is not active: log 12:44:50 "60 frames checked, largest difference 5.20e-02 ... each eye keeps
  rendering its own". So the eye's 0x02 and its index-1 cache run, and the eye reads main's index-0 textures.

### 1.3 Exposure per view (Confirmed code; values Unconfirmed)

* 0x3CCCB20: stage 0x21 of the view; `[0x604AA10]+0x75 == 0` -> step +0x2140 (buffers +0xA8, valid +0xF8, return
  +0xB8 + p*0x10), else step +0x2148 (buffers +0x78, valid +0xC8, return +0x88 + p*0x10); p = (frame-1)&1; fallback
  `[step+8]+0x29C8` (each view has its own, different pointers in the field diff).
* New step: prepare 0x3EDE110 (`+0x75 == 0` and lighting exposure settings +0x18), job 0x3EDE160. Old step (vtable
  0x58D3C78): prepare 0x3EDFC00 (`+0x75 != 0` and the same setting), job 0x3EDFC40 (job vtable 0x58D3C58). The
  settings come from 0x3D598E0(view, lighting+0x10), indexed by the camera entity's component; the proxy copies
  +0x318/+0x320, so both eyes get the same settings.
* 0x3EDFC40 meters the view's own region (CB from ReadState +0x2170..+0x217C) and runs Immediate (`+0xD0`) when the
  slot is invalid or `[view+0x2AED]`, else Progressive (`+0xD8`). It writes buffer `(frame & 1)` and reads `(frame-1)&1`.

## 2. Ruled out (Confirmed)

### 2.1 Same in both views
* Atmosphere LUTs view+0x29E0/+0x29E8 (t39), +0x2A00/+0x2A08 (t41), +0x2A20/+0x2A28 (t40), cloud maps +0x2A58 /
  +0x2A60 (t42/t43), sky textures +0x2AC8..+0x2AE0. Written only by **0x3CCB970** (static textures from
  0x5FCDFF0..0x5FCE048 by atmosphere +0x140 when +0x158 is set; streamed textures by atmosphere ids). 0x3CC9700 calls
  it on every apply, so copy_environment gives the eye the same ones. None of these qwords differ in the 12:44:45 /
  12:44:47 field diffs. Bound by **0x3CCD4D0** (t39/t40/t41/t42/t43 and cb8 planet constants from atmosphere +0x140).
* PerView rows 13..24 (+0xD0..+0x18F): 0x3CC7490 builds them from the from / to / active atmospheres, view+0x2A4C
  (1.0 both), +0x2A50 (0 both), +0x2A68 / +0x2AB0 / +0x2ABC (equal in the diff). Z already covered +0xA0..+0x16F.
* Depth convention: stereo.cpp `eye_projection` keeps the game's z terms (only [0], [5], [8], [9] change) for both
  eyes. DeferredFog has no depth == far test. Froxel distribution equal (Z).
* Froxel volume region: both views use index 0, but the tile window comes from each view's own render viewport
  (Z 1.1), so the regions don't overlap.

### 2.2 Symmetric (both eyes, so not the left/right difference)
* +0x2AED every frame (fix 3).
* Fog layers (Z fix): log 12:44:47 and 12:44:49 show `[id 787481121 variant 0 weight 1.000]` in both.

## 3. One-run logging plan

One run: load a save inside the nautiloid looking out at the spires (the user's 60 s spot). Log every 30 frames on
the render thread in the existing 0x3CC6CB9 hook for main and eye, and print a line on each A/B key press.

1. **Distant cache** (fix 1):
   * `[0x604AA10]+0x48`, `+0x43`, `+0x75`.
   * For each view's stage 0x02 (stage list view+0x28, id 0x02): +0x18 (runs), +0x241A, +0x241B, +0x21D8 (parity),
     +0x2454 (pending tiles = cache+0x2B4), +0x2419 (cache+0x279), the four origins +0x23E0..+0x240F
     (cache+0x240 + level*0x18 + p*0xC), and +0x2470..+0x248F (splits) with +0x2688 (count).
   * Prediction: eye +0x241A = 1, main 0; +0x241B = 1; parities or origins differ on frames where the right eye
     has no haze.
   * A/B key 1: `[0x604AA10]+0x48 = 0`. Prediction: the right eye's far haze returns and matches the left.
   * A/B key 2: the two mid-hooks of fix 1 on/off (count hits per view). Prediction: same result as key 1, with
     distant shadows kept.
2. **Exposure** (fix 2):
   * Per view: the pointer 0x3CCCB20(view) returns, and whether it is the step record or view+0x29C8; stage 0x21
     (id 0x21) +0x2140 / +0x2148 step: +0x18, +0x78 / +0x80, +0xC8 / +0xC9; calls of 0x3EDFC40 per view
     (`[rcx-0x18]`) and of 0x3EDE160.
   * The value: once a second CopyResource the returned record's buffer (4 bytes) to a staging buffer and Map it
     three frames later (DO_NOT_WAIT). Log main and eye floats.
   * A/B key 3: the 0x3CCCB20 redirect (eye -> main). Prediction if exposure matters: the sky band and haze match.
3. **+0x2AED** (fix 3): per view `[view+0x2B00]`, `[view+0x21D8]`, `[view+0x2AED]` after 0x3CC6DC0 (log it in the
   0x3CC6CB9 hook). Prediction: 1 every frame in both views; 0 after fix 3 except on real camera changes.
4. **Control** (expected equal): per view PerView[0] +0xF0..+0xFF and +0x170..+0x18F, view +0x29D8, +0x29E0,
   +0x2A00, +0x2A20, +0x2A58, +0x2A60, +0x2AC8..+0x2AE0, +0x2988, +0x2990, `[view+0x28A0]` +0x140 / +0x144 / +0x158,
   `[view+0x2AEA]`.

## Raw files (agents\AE\)

ae0 field-diff lines from the 12:44 log; ae1 0x3CCD4D0 / 0x3E1D830 / 0x3E45750 (sky) / 0x3E47BB0 (clouds); ae2
0x3E1C940 / 0x3E1C230 / 0x3E1A490 / 0x3E1A410 (distant cache, stage 0x02 prepare); ae3 displacement scan of the view
environment fields; ae4 / ae7 / ae8 callers (exposure, LUT binder, 0x3E1D830); ae5 / ae6 exposure steps (0x3EDFC00,
0x3EDFC40, 0x3EDE110, init 0x3EDF650) and god-ray prepare 0x3ED0B40; ae9 0x3D598E0; vt.py / strs.py helpers.

## Open leads

* What the exposure value means (pre-exposure vs. EV) and whether the tone map reads the step buffers directly. Only
  matters if plan 2 shows different values.
* The clear value of the cache slice that is being refreshed (decides whether a wrong-slice lookup reads as fully
  shadowed). Plan 1 makes it moot.
