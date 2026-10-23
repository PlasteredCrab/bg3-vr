# Right eye after lighting: post chain and per-view state (agent AM, 2026-10-23)

Static analysis of bg3_dx11.exe 4.1.1.7631656. Ghidra project BG3_F (read-only, -noanalysis, request server
`agents\AM\AMServer.java`, stopped). bg3vr_disasm. Post shaders extracted read-only with LSLib Divine from
`Data\EngineShaders.pak` into `AM\shaders\`, disassembled with `S\dis_bshd.py` into `AM\shaders_asm\`. Mod source
(stereo.cpp, eyefix.cpp, views.cpp, resolution.cpp) and the 15:10 log (`bin\bg3vr\bg3vr.log`). RVAs are relative to
0x140000000. "main" = left eye view, "eye" = right eye view. "lighting object" = `[view+0x28D8]` (the view's active
lighting resource copy). Raw output: `re-local\ghidra\agents\AM\` (am1..am24).

## Fix table (ranked by likely impact on "right eye darker, more contrast, less haze and glow")

| # | difference | why | fix (exact) | thread | status |
|---|-----------|-----|-------------|--------|--------|
| 1 | **The eye's lighting object is a stale snapshot of main's.** Every post parameter lives in it: grading sets and weight (+0x50..+0x170), tonemap fade-to-black **+0x244**, display-LUT param +0x240, bloom on/threshold/intensity +0x25C/+0x22C/+0x230, god rays +0x25D/+0x234/+0x238/+0x23C, vignette +0x25E/+0x248..+0x258, exposure records +0x1BC+i*0x2C. Sun, fog and sky values are in the same object (AL/AK scope). | views.cpp `sync_environment` copies only when main's active **id or pointer** changes, and copies main's **active** object at that moment. The game's setter 0x3CCAAC0 changes the active id at the **start** of a blend (0x3A087B0) but only the grading sets. The other fields move later, in main's per-frame tick 0x3CC8760 -> 0x3CCA490 -> 0x3D5B880 (lerp start->target by elapsed/T). So the copy takes the old values. The eye then blends with T = 0 to that snapshot (target `+0x28B8` = main's active, 0x3C49F00) and stays there. Main's id does not change again, so no new copy. Log 15:10:37.794: main lighting id 244318769 -> 252707377 in the **same object** 0x1D4E2F5F400, copied the same frame. If the game also applies the request to the eye itself (0x1E1E1D0 loops every view whose camera id matches), the mod's copy overwrites the eye's blend target with main's old active values anyway. | **Mirror the game's setter.** Inline hook **0x3CCAAC0** (`48 89 5C 24 20 55 57 41 56`): detour `bool(view, src, int* id, float T, bool force)`; call the original; if view == main, call the original again with (eye, src, id, T, force). Same for the atmosphere setter 0x3CC9700 (AK). Then in `sync_environment` **skip** `copy_environment` when the eye's active lighting and atmosphere ids already equal main's. The eye's own tick then blends over the same time and broadcasts stage slot 10 (0x407AC70) each blend frame, which recomputes its grading LUT, as main does. Fallback without a new hook: in `sync_environment` re-copy every frame while `[main+0x28E8]` (lighting blend) or `[main+0x28B0]` (atmosphere blend) is set, and once more on the frame it clears. | game thread (requests come from 0x1E1E1D0; the tick runs from view slot 7 0x17F2910) | Mechanism **Confirmed** (0x3CCAAC0, 0x3CC8760, 0x3CCA490, 0x3D5B880, 0x3C49F00, views.cpp). That the two objects differ in the user's scenes: Unconfirmed (plan A, B) |
| 2 | Grading LUT is per view and only rebuilt on the view's own environment change | The LUT step (id 0x24, vtable 0x58D4C08, job 0x3EF0990) writes the view's own 32^3 LUT `[view+0x2B58]` (created by 0x3CCC880 from set-scene 0x3CCC550). Its prepare 0x3EF0930 skips it unless step+0x155 is set (by slot 10 0x3EF1B40, reached only through 0x407AC70 from the view's tick) or the shared display-LUT key 0xAA4A7766 was reallocated. Grading apply (id 0x23, 0x3EF2860) reads `[view+0x2B40]`. So the eye's grading is frozen at whatever its lighting object held at its last own blend. | Fixed by #1 (the eye's own blends broadcast). Check `[eye+0x2B38]` is non-zero (plan D). | game thread / render | Confirmed (code); state Unconfirmed |
| 3 | Exposure: eye still meters and re-exposes with its own records | Old path is active (log: "exposure jobs 0" = new job 0x3EDE160 never runs). The eye's old job 0x3EDFC40 un-exposes its image with **its own** last record, but the image was pre-exposed with **main's** (0x3CCCB20 redirect). Shader math (OldPPCalculateEV64x64 + Progressive): `dL_eye = a*(avglog S_eye + 3 - comp - L_main)`, `dL_main = a*(avglog S_main + 3 - comp - L_main)`. So L_eye = L_main + an offset that drifts with the L/R scene difference until it hits the clamp. PPReExpose (id 0x22 default, vtable 0x58D3D90, job **0x3EE0D10**) multiplies by `t0[1]*t1[0] = 2^(L(n-1) - L(n))` of the eye's own records. While the offset is free the eye's ratio equals main's; at a clamp it stays 1 while main's moves. Effect: a few % during adaptation only. No steady gap. | (AI fix 1b, exact sites) Mid-hooks with r8 = the record, rsi = job (view = `[rsi-0x18]`), all `48 8D 54 24 50`: **0x3EE0EFB** (t0, new path), **0x3EE0F40** (t0, old path), **0x3EE0F85** (t1, both). For the eye: if r8 is inside the eye's step (+0x88..+0xA7 old / +0xB8..+0xD7 new) rebase it to main's step (main's `[stage21+0x2148]` / `+0x2140`, cache from main's own ReExpose job); if r8 = `eye+0x29C8` use 0x3EE04D0(main's old step). Only then may the eye's old job 0x3EDFC40 (`48 8B C4 48 89 58 08`, view = `[rcx-0x18]`) be skipped. | render-side job (worker) | Math Confirmed (shaders, 0x3EDFC40, 0x3EE0D10, 0x3EE04D0, 0x3EE0C80); size Unconfirmed (plan F) |
| 3b | eyefix's exposure skip is a latent black-eye bug | eyefix skips 0x3EDE160 for the eye. Today it never runs, so it does nothing ("shared exposure: Ready" is misleading). If the new path is ever active (`[0x604AA10]+0x75 == 0`), the eye's new step stays "valid" (stage slot 8 0x3B23910 sets +0xF8 because step +0x18 is set) but its buffers are never written. ReExpose (0x3EE0E9F..0x3EE0F13) then binds them: black or NaN. | Remove the 0x3EDE160 skip, or keep it only together with the ReExpose redirect of #3 (both paths). | - | Confirmed (code) |
| 4 | TAA history is reset every frame in both eyes | TAA prepare **0x3EE9720** clears history (step+0xA8) when `[view+0x21D8] != step+0xB0`. The proxy ring puts a new pointer there every frame. keep_view_history only fixes +0x2B00. Not active in VR now (resolution.cpp sets AntiAliasing 0 + DLSS, so the TAA step and step 0x2B are off). Only matters if the user picks the game's TAA. | If TAA is ever used in VR: in stereo.cpp `on_view_begin`, also write the TAA step's (id 0x30) `+0xB0` = this frame's proxy when the real controller is unchanged (same rule as +0x2B00). | render thread | Confirmed (code); not a brightness cause |
| 5 | Vignette is centred on each eye's viewport | PPVignette uses the viewport UV (0.5, 0.5), not the lens centre. Gate: lighting +0x25E (prepare 0x3F03510). Symmetric per eye, so not L/R, but it darkens the nasal and temporal edges unevenly versus the lens. Its params come from the lighting object (#1). | Optional: hook prepare 0x3F03510 and set `[rcx+0x18] = 0` in VR for both eyes. | render | Confirmed (shader, code) |
| - | DLSS: no per-eye exposure difference | One NGX feature per view (step id 0x25, `step+0x78`). Evaluate 0x3EF34B0 -> 0x40C21B0: ExposureTexture = null, Pre.Exposure = 1.0, Exposure.Scale = 1.0, create flags 0x6B (AutoExposure, sharpening, HDR). Subrects from each view's own +0x2188 / +0x21A8. Jitter 0x3F11710 from the frame counter (same sequence, separate features, fine; stereo.cpp keeps the jitter terms in P[8]/P[9]). Reset = step+0x94, set only by feature create 0x3EF3790/0x3EF3920 (viewport or mode change). | none | - | Confirmed |
| - | Tone map: identical | Tonemap (id 0x31, job 0x3F00FA0 -> 0x3F01D00 in SDR) reads no exposure. LUT = static `Engine/Textures/ZCAM_SDR_4.dds` in SDR (display-LUT key 0xAA4A7766 only for HDR). Its only per-view input is cb0.x = **lighting +0x244** (lerp to black) and the LUT step's +0x138. | via #1 | - | Confirmed |
| - | Bloom, god rays: no own per-eye fault | Bloom (vtable 0x58D4940, prepare 0x3EEB4B0, job 0x3EEB4E0) and god rays (AI) clamp every tap to the view's rect (cb9[5..7]); main's chain runs to the end before the eye's. Inputs that differ: the lighting object (#1) and the scene colour (fog, AK). | via #1 / AK | - | Confirmed |
| - | Post-process volumes: same in both | Darken environment (0x3EE46C0, CullStage entries type 1), chromatic aberration (0x3EEFD20, type 3) and grading apply (0x3EF2860, type 4) filter by player bit `[view+0x2268]` (0 in both) and weight 0x3F88090, which projects with the proxy's +0x190 (head VP) into each view's own upscaled viewport. | none | - | Confirmed (code); counts Unconfirmed (plan E) |
| - | Pre-exposure readers all redirected | All 6 callers of 0x3CCCB20 (0x3CD446B, 0x3E37263, 0x3E3E232, 0x3E45971, 0x3E45EEC, 0x3E4882C) go through the hook. Lit surfaces **are** pre-exposed by t26 (ClusteredLighting_0 lines 383, 558, 814, 1217, 1375, 1885: lights, IBL x100, sun, aerial perspective), contrary to AE. The only other `+0x29C8` reader, 0x36B0C00, is not a game render view. | none | - | Confirmed |

Order: #1 first (it is the only persistent per-eye difference found after lighting). Then run plan A-G. #3 only if plan F
shows a gap during adaptation. Fix #3b in any case.

## 1. Post chain of a view (stage 0x21, init 0x3B21E10) - Confirmed

Order of execution = order in `stage+0x2150`. id = step slot 1. prepare = slot 14 (+0x70), job = slot 2 of the job
vtable at step+0x20. 0x176D530 = always on. "VR" = state with the mod's profile (DLSS Balanced, AA 0, CAS 0, DOF off,
lens flare off).

| # | id | step vtable | prepare | job | shaders | per-view inputs | VR |
|---|----|------------|---------|-----|---------|-----------------|----|
| 1 | 0x22 | 0x58D3100 | 0x3ECFD60 | 0x3ECFD90 | ClearRTGraphics | - | ? |
| 2 | 0x22 | 0x58D3270 | 0x3ED0B40 | 0x3ED0BE0 | PPGodRaysBlur(+AndCombine) | lighting +0x25D/+0x234..+0x23C, proxy forward | on |
| 3 | 0x22 | 0x58D36F0 | 0x3ED3290 | 0x3ED3300 | PPDOF* | - | off |
| 4 | 0x26 | 0x58D3820 | always | 0x3EDD3D0 | FadeBlending | - | on |
| 5 | 0x22 | 0x58D3928 (stage+0x2140) | 0x3EDE110 | 0x3EDE160 | PPCalculateEV/Histogram/Exposure | own records | off (+0x75) |
| 6 | 0x22 | 0x58D3C78 (stage+0x2148) | 0x3EDFC00 | 0x3EDFC40 | OldPPCalculateEV64x64, OldPPExposureCompute_* | own records, lighting exposure record | on |
| 7 | 0x22 | 0x58D3D90 | 0x3EE0C80 | 0x3EE0D10 | PPReExpose | own records (t0 = n-1, t1 = n) | on |
| 8 | 0x22 | 0x58D4068 | 0x3EE1CF0 | 0x3EE1D40 | PPLensFlare_* | - | off |
| 9 | 0x22 | 0x58D41A8 | always | 0x3EE46C0 | PPDarkenEnvironment(Mask) | CullStage type 1 | on |
| 10 | 0x22 | 0x58D42B0 | always | 0x3EE6070 | PPRadialBlur | - | on |
| 11 | 0x22 | 0x58D4418 | 0x3EE7160 | 0x3EE71B0 | SMAA* | - | AA |
| 12 | 0x30 | 0x58D46C0 | 0x3EE9720 | 0x3EE9880 | PPTAA | history key 0xA9F7A535, controller compare | off |
| 13 | 0x2B | 0x58D47B8 | 0x3EEAF40 | 0x3EEAFA0 | (TAA history copy) | - | off |
| 14 | 0x22 | 0x58D4940 | 0x3EEB4B0 | 0x3EEB4E0 | PPBloom* | lighting +0x25C/+0x22C/+0x230 | on |
| 15 | 0x22 | 0x58D4A38 | always | 0x3EEFD20 | PPChromaticAberration | CullStage type 3 | on |
| 16 | 0x24 | 0x58D4C08 | 0x3EF0930 | 0x3EF0990 | PPComputeLUT(_Fade/_GradingLUT), PPComputeDisplayLUT_ACESv1/ZCAM | lighting +0x10..+0x17C, +0x240; own LUT +0x2B58 | when flagged |
| 17 | 0x23 | 0x58D4D38 | always | 0x3EF2860 | PPColorGradingApplyLUT | own LUT +0x2B40, CullStage type 4 | on |
| - | - | 0x180F4B0, 0x17FCD00 | | | outlines (inserted after 0x23 by 0x588E50) | `[view+0x2961]` | on |
| 18 | 0x25 | 0x58D4E30 | 0x3EF3490 | 0x3EF34B0 | DLSS | own NGX feature | on |
| 19 | 0x28 | 0x58D5018 | 0x3EF4140 | 0x3EF4170 | FSR2_* | - | off |
| 20 | 0x27 | 0x58D5970 | 0x3EFD550 | 0x3EFD570 | FSR(_RCAS), FSR_Tonemap | - | off |
| 21 | 0x29 | 0x58D5A40 | 0x3EFF340 | 0x3EFF380 | (inject) | - | ? |
| 22 | 0x22 | 0x58D5D10 | 0x3EFFD80 | 0x3EFFDB0 | PPDefault | - | ? |
| 23 | 0x31 | 0x58D5FC0 | always | 0x3F00FA0 | PPToneMap_ApplyLUT(_v1/v20/v21) | lighting +0x244, LUT step +0x138 | on |
| 24 | 0x22 | 0x58D60C8 | 0x3F02B60 | 0x3F02B80 | CAS | - | off |
| 25 | 0x22 | 0x58D61E8 | 0x3F03510 | 0x3F03530 | PPVignette(_HDR) | lighting +0x25E/+0x248..+0x258 | per lighting |
| 26 | 0x22 | 0x58D6380 | 0x3F040E0 | 0x3F040F0 | Calibration_MaxLuminance | - | HDR |

No film grain shader exists. Sharpening = DLSS sharpness 0.35 (both eyes). No post resource is chosen by split index.
Keys are frame-wide; each view writes its own viewport, main's chain runs before the eye's on one context.

## 2. Evidence for #1 (lighting object)

* Setter 0x3CCAAC0(view, src, id*, T, force) (am14):
  * a blend already running with other ids -> the request is parked in `+0x2900` (copy made now), applied by the tick later;
  * same active id and same request id -> no blend, but the **target** `+0x28B8` is still assigned from src (0x3C49F00);
  * otherwise: start `+0x28C8` = copy of the current active, elapsed `+0x28E0` = 0, T `+0x28E4`, blending `+0x28E8` = 1,
    active id = src id (0x3A087B0), active grading +0x50.. = src, +0xE0.. = start's, +0x170 = elapsed/T; then target = src.
* Tick 0x3CC8760 (view slot 7 0x17F2910, per view; the eye's runs: its fog layer reached weight 1.000 in the log):
  while `+0x28E8`, elapsed += dt, `0x3CCA490(view, elapsed/T)` -> `0x3D5B880(active+0x10, start+0x10, target+0x10, w)`:
  lerps bloom (+0x22C/+0x230 with +0x25C), god rays (+0x234..+0x23C with +0x25D), tonemap (+0x240, +0x244), vignette
  (+0x248..+0x258 with +0x25E), sets grading weight +0x170 = w, copies grading sets, exposure records via 0x3D59D30.
  Any change -> `0x407AC70(view+0x18)` -> every stage's slot 9 and 10 -> post stage 0x3AECE90 -> every step's slot 10 ->
  LUT step 0x3EF1B40 sets +0x155 -> LUT rebuilt.
* `copy_environment` calls the setter for the eye with (main's **active**, id -1, T 0, force 0). With T 0 the eye's next
  tick sets eye active = that snapshot (w = 1). So the eye gets main's grading target (written at blend start) plus
  main's **not yet blended** bloom, god-ray, tonemap-fade, vignette, exposure, sun and fog values.
* Log 15:10:22.427: the first copy was parked (eye ids stayed 212860945 / 216006673). 15:10:37.794: main's lighting id
  changed inside the same object and was copied the same frame.

## 3. Exposure detail (#3) - Confirmed

* OldPPCalculateEV64x64: `log2(lum * (1 / t1[0])) + 3`, t1 = the view's previous record (job+0x68+p*0x10, fallback
  view+0x29C8). OldPPExposureCompute_Progressive: target = avg - cb0.z, `L += (target - L)(1 - 2^(-k*cb0.w))`
  (k = 2 when brighter), clamp [cb0.x, cb0.y], store [0] = 1/(1.2*2^L), [1] = 1.2*2^L, [2] = L. cb0 from
  0x3D598E0(view, lighting+0x10) record: min/max/compensation + 0.36, speed `[0x614FB84]`.
* Immediate instead of Progressive when the previous slot is invalid or `[view+0x2AED]`.
* PPReExpose PS: `out = colour * t0[1] * t1[0]`. Bindings 0x3EE0D10: old path t0 = 0x3EE04D0(old step) (record
  (frame-1)&1 or view+0x29C8), t1 = old step +0x88 + (frame&1)*0x10. Prepare 0x3EE0C80: previous valid and
  `[view+0x2AED] == 0`.

## 4. One-run logging plan

One run, same spots as AI/AE (nautiloid interior -> exterior, 338 s, 200 s, 80 s), plus one dialogue. Log lines get the
frame counter and thread id. Hotkey A/B lines are logged.

* **A. Lighting objects** (game thread, `views::on_frame`, every 30 frames and on any change): per view `[view+0x28D8]`,
  id (+8), CRC32 of bytes +0x10..+0x2EF, and when the CRCs differ, the first 40 differing dwords as `+off main eye`.
  Always print +0x170, +0x17C, +0x22C, +0x230, +0x25C, +0x234, +0x238, +0x23C, +0x25D, +0x240, +0x244, +0x248..+0x258,
  +0x25E, and the 0x2C-byte record at 0x3D598E0(view, lighting+0x10). Blend state per view: +0x28E0, +0x28E4, +0x28E8,
  +0x28C0, +0x2900, +0x28A8, +0x28AC, +0x28B0, +0x28F0, and the start/target ids (`[+0x28C8]+8`, `[+0x28B8]+8`).
  Prediction: after each main id change the eye's CRC differs until the next copy; +0x244 / bloom / grading differ.
* **B. Setter calls**: log-only inline hook 0x3CCAAC0: view role, src, src id, *id, T, force, return address, thread.
  Answers whether the game already feeds the eye (0x1E1E1D0) and the blend times used.
* **C. Ticks**: entry of 0x3CC8760 (role, +0x28E8 before/after, sampled every 30 frames) and every 0x407AC70 call
  (view = rcx-0x18). Gives the order of main's tick versus `sync_environment` in a frame.
* **D. LUT**: count of LUT job 0x3EF0990 per view (view = `[rcx-0x18]`) per second; at prepare 0x3EF0930 log
  +0x148/+0x154/+0x155/+0x18 per view once a second; `[view+0x2B38]`, `+0x2B40`, `+0x2B58` once.
* **E. Post gates per view** (once a second): +0x18 of every step in `stage21+0x2150` (with its slot-1 id and vtable),
  and the number of CullStage entries of types 1/3/4 with weight > 0 (as 0x3EE46C0 / 0x3EEFD20 / 0x3EF2860 count them).
* **F. Exposure values**: mid-hooks 0x3EE0F40 / 0x3EE0F85 in log mode: per view the two records bound. Once a second
  gpu_trace copies both views' old-step buffers (first 24 bytes) to staging, Map 3 frames later, log [0], [1], [2].
  Prediction: L_eye - L_main is a slowly drifting offset, both move together.
* **G. Where the gap enters** (gpu_trace, one frame per spot): mark each view's stage 0x21 job (hook 0x3B239B0 with
  `marker`), and after every draw/dispatch inside it sample a 32x32 box at the centre of each eye's viewport of the
  bound RT/UAV (existing `sample_target`), plus the scene colour before step 1. Log mean luminance main vs eye per step.
  If the eye is already darker before step 1, the cause is before post (fog/lighting: AK/AL). If it appears at step
  14/16/17/23/25, it is #1.
* **A/B keys** (20 s each): mirror setter (#1) on/off; fallback re-copy-while-blending on/off; ReExpose redirect (#3)
  on/off; vignette off.

## Raw files (agents\AM\)

am1 post stage init/prepare/execute, 0x3EE04D0, 0x3D598E0; am2/am3 step and job vtables; am4 step shader loaders;
am5 LUT step 0x3EF0930/0x3EF0990/0x3EF1B40, grading apply 0x3EF2860, tonemap 0x3F00C00/0x3F00FA0; am6 bloom, vignette,
CAS, ReExpose prepare, old exposure prepare, PPDefault, chromatic aberration; am7/am11/am12/am13/am20 callers;
am8 tonemap 0x3F01D00/0x3F01830; am9 users of +0x2B40/+0x2B58; am10 LUT creator 0x3CCC880; am14 tick 0x3CC8760,
setter 0x3CCAAC0, slot 7 0x17F2910, 0x407AC70; am15 blend 0x3CCA490, 0x3CCB020, assign 0x3C49F00; am16 darken,
FadeBlending, TAA, DLSS prepare/evaluate; am17 NGX evaluate 0x40C21B0; am18 request applier 0x1E1E1D0; am19 ReExpose
binds (disasm); am21 volume weight 0x3F88090; am22 0x407BF00; am23 lighting lerp 0x3D5B880; am24 stage 0x3F.
Scripts: vtd.py (vtable dump), findbytes.py, call50.py, riprefs.py. Shaders: `shaders\`, `shaders_asm\`.

## Open leads

* 0x3D59D30 (exposure records and other +0x194.. fields in the lerp): not decompiled; plan A shows the values.
* Thread and order of view slot 7 versus `views::on_frame` (plan C).
* Stage 0x3F (0x3E54C00) was not identified; it is not a colour post step.
