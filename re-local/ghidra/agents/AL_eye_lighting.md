# Right eye: shadow and lighting inputs that differ from the left eye (agent AL, 2026-10-23)

Static analysis of bg3_dx11.exe 4.1.1.7631656. Ghidra project BG3_G (read-only, -noanalysis, request server
`agents\AL\ALServer.java`, stopped). bg3vr_disasm. The game's own DX11 shader PPSAO (agent N's extraction). The mod's
source (views.cpp, stereo.cpp, shadows.cpp) and the 15:08-15:11 log (`bin\bg3vr\bg3vr.log`, the run with the 15:03
DLL; field diff at 15:10:53 saved as `AL\al0_fielddiff_1510.txt`). RVAs are relative to 0x140000000. "main" = left eye
view, "eye" = the CreatePlayerView view (right eye). Confirmed = read in code or in the log at the cited place.
Unconfirmed = section 3 says how one run settles it. Raw output: `re-local\ghidra\agents\AL\` (al1..al20).

Scope: shadows and lighting of opaque/forward geometry. Fog volumes (AK) and the post chain (AM) are left to them;
where my findings touch theirs, I say so.

## Fix table (ranked by likely visual impact)

| # | difference | why | fix (exact) | thread | status |
|---|-----------|-----|-------------|--------|--------|
| 1 | **The eye's lighting and atmosphere are frozen at the start of every blend main does.** Main finishes the blend to the new lighting; the eye keeps (mostly) the old one until the next change. Inputs that come from these objects: sun colour and intensity (PerView rows 0-1 from atmosphere +0x24..+0x38), sun-shadow resolve constants (atmosphere +0x5C/+0x60/+0x68, 0x3E1FF80), cascade distances (+0x54/+0x58/+0x6C/+0x70), **SSAO on/radius/strength** (+0x18C/+0x190/+0x194/+0x198, read by the SSAO prepare 0x3E4F430, the SSAO job 0x3E4F490 and clustered lighting 0x3E35C80), fog/sky rows, fog layer parameters, the whole lighting struct (+0x10..+0x25E, lerped by 0x3D5B880), cloud angle (+0x260/+0x264), exposure settings (lighting+0x10, AE), camera lights (+0x288..+0x2A4). | The game's lighting request loop **0x1E1E1D0** (atmosphere **0x1E1DF90**) applies every request to **every view of the frame whose WriteState camera matches** (`[view+0x2260]+0x80`), so it can give the eye the same 2-second blend as main (which path made the 15:10:37 change is Unconfirmed, plan 1; the freeze happens either way). Then the mod's `sync_environment` sees main's **active** lighting id change (same pointer, new id: 0x3CCAAC0 sets the active's id at blend **start**) and calls 0x3CCAAC0(eye, **main's active object**, id -1, **blend 0**). That cancels the eye's blend and makes main's active **as it was at t≈0** (old values, new id) the eye's target. Main's +0x28E0/+0x28E4 (elapsed/duration) = 2.0/2.0 in the 15:10:53 diff, the eye's 0/0; the copy was logged at 15:10:37.794 (lighting 244318769 -> 252707377). | views.cpp `sync_environment` / `copy_environment`: (a) compare **target** ids (atmosphere `[+0x2880]+8`, lighting `[+0x28B8]+8`), not the active pointer/id. If they are equal, do nothing: the game is blending the eye too. (b) If they differ (a request the eye missed: player-view-only 0x19D1F70, timeline list 0x1F7FC10/0x1F80030, primary-view 0x3C49530, or one made before the eye existed), call **0x3CC9700(eye, [main+0x2880], main+0x2888, remaining, 1)** and **0x3CCAAC0(eye, [main+0x28B8], main+0x28C0, remaining, 1)**, with `remaining = blending ? duration - elapsed : 0` (atmosphere +0x28B0/+0x28AC/+0x28A8, lighting +0x28E8/+0x28E4/+0x28E0). Source = main's **target** object, not its active one; last arg 1 = force (as 0x3C49530 does). Then `refresh_eye_fog` as now. (c) Each frame copy main's lock/pause bytes +0x2AE8, +0x2AE9, +0x2AEB, +0x2AEC into the eye first, so the game's own requests are gated the same way. | game thread (where the copy runs today) | Mechanism **Confirmed** (code 0x3CCAAC0, 0x3CC9700, 0x3CC8760, 0x1E1E1D0; log). Size of the visual difference: Unconfirmed (plan 1) |
| 2 | (already handled by AK / commit 91dcc5f) Far terrain shadowed and far haze gone in the **15:08 run** | That run had the 15:03 DLL: the eye's cache index forced to 1 ("right eye reads its own distant-shadow cache (index 1 ...)" in the log). AK 2: the index-1 cache is the eye's own and likely empty/unfilled in practice. | No new action: the distant cache is off in VR by default (91dcc5f), `left` mode is the default (2be901f). If it is ever re-enabled with `left`: the eye's stage 0x03 init **0x3E1EAC0** (0x3E1ED69) and its fog-lighting init 0x3E3C740 declared the **index-1** keys (0xB7F464DD/0x337B7295) at build time, so the index-0 keys the eye then reads are not in its stages' declared reads (AK 2.3). | - | Confirmed (log, code) |
| 3 | **SSAO rebuilds view positions with a centred frustum**, so each eye's AO is computed on a sheared copy of its scene (about b/a ≈ 0.15-0.2 for Quest 2, mirrored between the eyes) | Stage 0x37 job **0x3E4F490** builds PPSAO cb0[1] = `(2/(w*P0), -2/(h*P5), (-1-P2)/P0, (1+P6)/P5)` from the **proxy +0x110** (0x3E4FA1A..0x3E4FD36). P[2] and P[6] are 0 in this matrix layout; the eye's off-centre terms are P[8]/P[9]. Same bug class as AH's surface tiles. PPSAO uses cb0[1] for every sample position (asm lines 85, 129, 158, ...). | Mid-hook at **0x3E4FD36** (`movss [rbp+0xA0], xmm0`, 8 bytes; the join of both branches, the jmp at 0x3E4FD03 targets it). There: view = `[rbp-0x60]`, controller = rsi, w/h = int `[rbp+0x30]`/`[rbp+0x34]`. If `stereo::eye_of(view) >= 0` and `byte [rsi+0x12] == 0`: P = the view's PerCamera block [0] projection (unjittered), write float `[rbp+0xA8] = (-1 - P[8]) / P[0]` and `[rbp+0xAC] = (1 - P[9]) / P[5]`. Runs 4 times per view (one per SSAO level); the offsets don't depend on the level. | render-side job | Code + shader **Confirmed**; visual size Unconfirmed (plan 3) |
| 4 | The eyes' own sun cascades differ by more than the eye offset (shadows.cpp: "largest difference 5.20e-02", sharing rejected). Each eye's cascades are self-consistent, so this shows as different cascade coverage / transitions, not as a global darkening. | Not found statically. T 2.2 says the fit uses the camera entity's controller (same object for both proxies). The error is the same 5.20e-02 in all 60 checks while the matrix rows change, which points at a split / radius value rather than a translation. | Logging plan 4 names the element. Do not share the cascades until it is understood. | render thread | Unconfirmed |
| 5 | Camera-attached lights (lighting +0x288 on, +0x289+i per light, templates +0x294, offsets +0x2A4) | **0x3CCB020** (end of every 0x3CCAAC0) spawns up to 4 light entities **per view** into view+0x2968..+0x2987 and hides each from the other split index: index 0 -> 0x3C6B810(visual, **0x400**, 0), index 1 -> 0x200 (0x3CCB5B5..0x3CCB5D4). Each view's tick 0x3CC8760 places its own lights at `[view+0x2260]` translate + forward x offset. Both views have one (+0x2968 differs in the diff). If the eye's was made at index 0, **both eyes** see two camera lights at the same spot (brighter than flat play); if at index 1, the eye's is invisible to both. Either way it is the same in both eyes. | None needed for L/R. If plan 5 shows two visible lights: after each copy, hide the eye's lights from index 0 with 0x3C6B810(visual, 0x200, 0) (the eye renders with index 0, main's light already covers both eyes). | game thread | Mechanism Confirmed; which case: Unconfirmed (plan 5) |
| 6 | Not lighting, noted for agent AJ: see-through / fade entity sets +0x2CA8/+0x2CB8 (main 41 entries, eye 0) | Filled only through GetPlayerView (0x17E1CD0, 0x17E2AA0, 0x17E80F0) and fed to the view+0x2BE0 post object (0x1810800/0x18109C0/0x1810B90) | Out of scope here | - | Confirmed (code, diff) |

Order: fix 1 first (it is the mod's own interference and touches sun, AO and shadow constants). Fix 3 next. 2 is done.
4 and 5 need the run.

## 1. The environment copy (fix 1)

### 1.1 How a view holds atmosphere and lighting (Confirmed, al1/al3, Z z6/z9)

| | atmosphere | lighting |
|-|-----------|----------|
| target ("to"), copy of the requested resource | +0x2880 (id at obj+8), request id +0x2888 | +0x28B8, request id +0x28C0 |
| blend start ("from"), copy of the active at request time | +0x2890 | +0x28C8 |
| active (what the renderer reads) | +0x28A0 | +0x28D8 |
| elapsed / duration / blending | +0x28A8 / +0x28AC / +0x28B0 | +0x28E0 / +0x28E4 / +0x28E8 |
| lock (requests only recorded) / pause | +0x2AEC / +0x2AE8 | +0x2AEB / +0x2AE9 |
| pending request while blending | +0x28F0 (+0x28F8 id, +0x28FC blend) | +0x2900 (+0x2908, +0x290C) |

views.cpp `fog_layers` prints +0x2880 as "from" and +0x2890 as "to": it is the other way round.

* Setter **0x3CCAAC0**(view, src, int* id, float blend, char force) (0x3CC9700 is the same for atmosphere):
  * if the view is blending and neither target nor start is src and !force: park src as pending, return;
  * if active id == src id and stored id == *id and !force: only refresh the target copy;
  * else: start = copy of the current active, elapsed = 0, duration = blend, blending = 1, **active id = src id**
    (0x3A087B0), active +0x50..+0xDF = src's, active +0xE0..+0x16F = the start's +0x50.., target = copy of src,
    then 0x3CCB020 (camera lights).
* Tick **0x3CC8760** (view slot 7 0x17F2910, every view; the log shows the eye's runs): elapsed += dt;
  t = elapsed/duration (1 when duration is 0); lighting: 0x3CCA490 = lerp(start, target, t) into the active
  (0x3D5B880 over +0x10..+0x25E, +0x268..+0x284, +0x2B8..+0x2C4); atmosphere: 0x3CC9DB0 (0x3D32440 over +0xC..,
  0x3D56B50 +0x98.., 0x3D584C0 +0x108.., SSAO +0x18C..+0x19C, fog layer weights).
* So with blend 0 the next tick sets active = target.

### 1.2 What happens on a lighting change (Confirmed)

1. Game thread: **0x1E1E1D0** (called from 0x1E0D450) walks the frames' view lists (`frame+0x42E0`) and calls
   0x3CCAAC0 on every view with `[view+0x2260]` set, `[[view+0x2260]+0x80]` equal to the request camera's and a
   scene `+0x2B10`. The eye is in the main frame's list ("frame lists it: true") and its WriteState camera is
   main's controller (log: "SetCamera(view 0x1D62208C6D0, camera 0x1D4DA240280)"). So a request on this path
   starts the same blend (2 s here) in both views. (If the change came through a main-only path, 1.4, the eye
   gets nothing from the game and step 3 alone decides its state; the result is the same.)
2. Main's active keeps its pointer, gets the new id, and still holds ≈ old values (t ≈ 0).
3. The mod's `sync_environment` compares main's **active pointer + id**: the id changed, so it copies main's active
   into the eye with blend 0, force 0. On the eye the "same" test fails (stored id is the request's, the mod passes
   -1), so it takes the full path: duration 0, target = main's active at t ≈ 0.
4. Next tick: eye active = that target. Main goes on to the new lighting over 2 s. The eye never follows: its
   active pointer and id don't change again, so the mod doesn't copy again, and the game's own blend on the eye
   was cancelled.

Log evidence: 15:10:37.794 "the main view's environment changed (copy 1) (... lighting 0x1D4E2F5F400 id 244318769
-> 0x1D4E2F5F400 id 252707377)"; 15:10:53 field diff: `+0x28E0 main 4000000040000000 split 0000000000000000`
(main elapsed = duration = 2.0, eye 0/0).

### 1.3 What reads these objects (Confirmed)

* PerView builder 0x3CC7490 (N r1_begin.c 117-409): sun colour/intensity from active atmosphere +0x24..+0x38
  (x +0x2A4C), lighting +0x260/+0x264, sky/fog rows from **both** +0x2880 and +0x2890 with t, +0x148, +0x144/+0x158,
  +0x8C/+0x90/+0x94, +0x14C, +0x1B0.
* Sun-shadow resolve CB 0x3E1FF80 (al11 2416..): atmosphere +0xC.., +0x5C, +0x60, +0x68.
* Clustered lighting 0x3E35C80 (al11 459-475): atmosphere +0x50 (sun on), +0x198/+0x19C (AO terms), +0x18C (AO on,
  and stage 0x37 running).
* SSAO prepare **0x3E4F430**: run only if settings +0x5A and atmosphere **+0x18C**. SSAO job **0x3E4F490**
  (0x3E4FC6F..0x3E4FC88): cb0[3] = atmosphere +0x198, +0x190, +0x194.
* Cascades: split distances from atmosphere +0x54/+0x58/+0x6C/+0x70 (T 2.1).
* Fog layers: atmosphere +0x230 + variant*0x98 (Z).
* Camera lights: lighting target +0x288..+0x2A4 (0x3CCB020, 0x3CC8760).

### 1.4 Other paths that give main a new environment and not the eye (Confirmed, al9/al10)

* 0x19D1F70: re-applies a player view's own target (GetPlayerView 0x3BAAE20).
* 0x1F7FC10 / 0x1F80030: a list of views at `[param_1+0x18]+0x5F0`; applies a lighting/atmosphere override and sets
  the lock +0x2AEB / +0x2AEC (timeline/dialogue type override; the list membership of the eye is Unconfirmed).
* 0x3C49530: applies to the pipeline's primary view `[DAT_146046F98+0xA0]` only (force 1).
These are why a copy is still needed; fix 1(b) gives the eye main's target with main's remaining blend time.

### 1.5 Per-player-view initialisation (the class of bug 0x3CCCC20 was) (Confirmed, al7/al10)

* 0x1D11580, 0x1D118A0, 0x1D11AE0, 0x1F90B00 and slot 14 (0x17F2180) / 0x3CC6BB0 do only: 0x3CCCC20 (variant,
  +0x28E8 = +0x28B0 = 1 (re-run both blends), fog layer entry, froxel grid), the viewport (0x407B800), input slot
  (slot 12, 0xFF), +0x2BFD, and camera attach (0x3CC90B0, +0x2CA0). The mod already calls 0x3CCCC20 after each copy.
* All 60 callers of GetPlayerView 0x3BAAE20 were decompiled (al10) and scanned for view fields +0x2100..+0x2D90.
  Lighting-relevant ones: only 0x19D1F70 (1.4). The rest are room/see-through data (+0x2BE0, +0x2BF8..+0x2BFD,
  +0x2CA8/+0x2CB8, +0x2CC8..), camera/viewport (+0x2170/+0x2174, +0x21A0.., +0x2962, +0x2C98/+0x2CA0) and outline
  data, which the mod already copies or which are not lighting.

## 2. Other inputs checked

### 2.1 SSAO (fix 3) (Confirmed, al12/al13)

* Disassembly 0x3E4FA1A..0x3E4FD36: `xmm8 = P[0]` ([rsi+0x110]), `[rbp+0x20..] = P[0..3]`, `[rsp+0x70..] = P[4..7]`;
  `[rbp+0xA8] = (-1 - P[2]) / P[0]`, `[rbp+0xAC] = (P[6] + 1) / P[5]`, `[rbp+0xA4] = -2/(h*P[5])`,
  `xmm0 = 2/(w*P[0])` stored at 0x3E4FD36 into `[rbp+0xA0]`; copied to cb0[1] at 0x3E4FD79.
* rsi = `[view+0x21D8]` (ReadState controller = the eye's proxy). With `[views] eye_surface_mesh` the proxy +0x110
  is the eye's projection (stereo.cpp 844-854), so the scale is right but the off-centre P[8]/P[9] are ignored.
  Without it, the scale was the head's union FOV too.
* PPSAO.bshd.asm: `mad r2.xy, r2.xyxx, cb0[1].xyxx, cb0[1].zwzz` then `mul ..., cb11[12].y` for every tap. Normals
  are not rotated by cb11 rows (only cb11[5] and cb11[12] are read), so the error is a shear of the whole AO input.
* SSAO keys 0x361F94A4, 0xD3052CFC, 0x74EA5362, 0x8517349C are frame-wide; each view writes its own viewport
  (cb9) and is consumed before the eye's stream runs (GPU order main, eye). No cross-eye history.

### 2.2 Sun shadows (stages 0x02 / 0x03)

* 0x03 resolve job **0x3E1F520**: per-view inputs are the view's own stage 0x02 (cascade matrices, cache records,
  parity), `[view+0x2961]`, the active atmosphere (0x3E1FF80), depth/normal keys, and a full-screen pass chosen by
  the controller's orthographic byte. No camera projection read on the CPU side (position comes from cb11/cb12,
  rewritten per eye). Confirmed.
* Cascade sharing is off (rejected), so each eye renders its own cascades into key 0x3E344F11 and consumes them before
  the next view runs. Self-consistent. Difference 4 in the table is unexplained.
* Distant cache: AK's report; see table row 2.
* Shadow casters: all shadow culls need the view bit 0x200 for index 0 (AA); both eyes render with index 0. Same
  casters.

### 2.3 Clustered lighting 0x3E35C80 (Confirmed, al11)

* CB 0x3F3CF00 per view (tile window from the view's viewport and split index 0 for both; froxel distribution
  +0x22C0..+0x22EF equal in the diff).
* Keys 0x64959AC0, 0x738395A7 (local-light records of 0x35), 0x1115DE1, 0x9331233, 0x5CA3D9F0, 0xAC2F0E9,
  0x94009344 (sun mask), 0xFA1D09B3, 0xF7D82BFB: all written by the same view earlier in its own stream.
* Textures from the view: +0x2A78 (t52) / +0x2A98 (t53), LUTs via 0x3CCD4D0; equal pointers in the diff.
  Probe cubemaps come through the view's CullStage (`[stage1B+0x2130]+0x1A0/+0x280..`, 0x3E32910): the same probes
  for two cameras 6 cm apart.
* Exposure t26 = 0x3CCCB20 (already main's for the eye).
* AO terms from the active atmosphere: fix 1.

### 2.4 Local lights (0x35 / 0x1E)

* Per view and self-consistent (T 3). The atlas config +0x2B70..+0x2BCF is equal in the 15:10:53 diff.
* Stage 0x05 helpers 0x3E09CB0 / 0x3E0AB10 read the proxy's P[0]/P[5] (+0x110/+0x124) and near/far for light
  screen size; scale only, no offset term needed.

### 2.5 Fields that differ only as uninitialised memory in the eye (Confirmed harmless for the offsets checked)

The eye view's allocation was not zeroed: +0x1258..+0x2058 hold old strings, and +0x22B4/+0x22B8, +0x2963..+0x2967,
+0x29BC, +0x29DC, +0x2BD2..+0x2BD7 differ. FindDisplacement (al14): no render-side reader of +0x22B4/+0x22B8/+0x22BC
on a view (0x3DFC660 reads them from the stage 0x35 object: its param_1 + 8 is the view, called from 0x3DFB6A0 with
`param_1 - 0x20`; 0x3A94xxxx / 0x3A97E20 are another class);
+0x2962 is read by 0x3EE6070 (post, equal 0 in both); +0x2BD0 is written by the ctor. +0x2AF8 (atmosphere id seen)
= -1 in the eye only triggers one history reset in 0x3CC6DC0.

### 2.6 Index-1 captures at build time (Confirmed, al6)

Readers of `[view+0x2268]` that run at init: 0x3E194E0 (0x02 cache index, known), 0x3E1EAC0 (0x03 declared cache
keys), 0x3E3C740 (fog lighting declared keys), 0x3E2CF50 (stage 0x0B G-buffer init: RT descriptor value 1 for index 0,
2 for index 1 on the G-buffer keys 0x4C0D1B61 / 0x3B0A2BF7 / 0xA2037A4D; the eye got 2, the split-screen player-2
value; what the value controls is Unconfirmed, and the eye's G-buffer showed no fault so far), 0x3CCB020 (camera-light
mask, row 5).
0x3E1BB10 / 0x3E3D11A / 0x3E1F683 read it per frame.

## 3. One-run logging plan

One run: load outdoors in daylight, stand still 10 s, then walk into an area or start a dialogue that changes the
lighting (the 15:10:37 kind of change), stand 10 s, then look at a wall corner and the ground near the feet (AO).

1. **Environment (fix 1).**
   * Game thread, every 60 frames and on every change, for main and eye: ids of the objects at +0x2880, +0x2890,
     +0x28A0, +0x28B8, +0x28C8, +0x28D8 (obj+8); +0x28A8/+0x28AC/+0x28B0; +0x28E0/+0x28E4/+0x28E8; +0x2AE8,
     +0x2AE9, +0x2AEB, +0x2AEC; +0x2961.
   * Content diff, same rate: compare the 32-bit words of the two **active** lighting objects (+0x10..+0x2EF) and
     active atmosphere objects (+0x0C..+0x377) as floats (relative 1e-4); log the count of differing words and the
     first 16 (offset, main, eye). Always print atmosphere +0x54, +0x58, +0x5C, +0x60, +0x68, +0x6C, +0x70, +0x18C,
     +0x190, +0x194, +0x198 for both.
   * Log-only detours on **0x3CCAAC0** and **0x3CC9700**: view role, src id (`[rdx+8]`), `*r8`, blend (xmm3),
     force (byte at [rsp+0x28] on entry), return address. Call sites (return address = site + 5): exe+0x1E1E34B /
     0x1E1E10F (request loop), 0x3CC8B04 / 0x3CC8871 (pending), 0x1F7FF82 / 0x1F80396 (override list), 0x3C49568
     (primary), 0x19D1FC8 / 0x19D1FA5 (player view), 0x3CCAFEE / 0x3CC9D7E (scene attach), the mod's own.
   * Prediction today: at the change, two calls from 0x1E1E34B (main and eye, same blend), then the mod's call (blend
     0) on the eye; afterwards eye +0x28E4 = 0 and the content diff is non-zero for good. With fix 1: no mod call when
     targets match, both views show the same elapsed/duration, and the content diff is 0 once the blend ends.
2. **Distant cache:** nothing new (AK's plan; off in VR now).
3. **SSAO (fix 3):** mid-hook 0x3E4FD36 in log-only mode first: per view, first level only, every 120th frame: w, h,
   `[rbp+0xA0..+0xAC]` before, P[0], P[5], P[8], P[9] of block [0], the corrected pair. Prediction: `[rbp+0xA8]` =
   -1/P[0] in both views, the corrected value differs by -b/a ≈ ±0.15..0.2 (opposite signs per eye). A/B the fix
   (hotkey, 20 s each) looking at a wall corner; gpu_trace one frame and read back the AO target 0x74EA5362 at the
   same world point in both eyes (with and without the fix).
4. **Cascade difference (row 4):** in shadows.cpp `validate`, log the index and both values of the element that
   gives the largest error (matrix i, row, column, or split slot), plus both views' atmosphere +0x54/+0x58/+0x6C/+0x70,
   view +0x2A40..+0x2A48 (sun direction) and `[view+0x21D8]+0x318/+0x320`. Prediction: one split / radius slot,
   not a translation.
5. **Camera lights (row 5):** mid-hook **0x3CCB5D1** (rcx = visual, edx = mask, view = `[rsp+0x48]`): log view role,
   `[view+0x2268]` at that moment, mask, visual. Every 300 frames log +0x2968..+0x2987 of both views and the lighting
   target's +0x288, +0x289..+0x28C. Prediction: the eye's light was hidden with mask 0x400 (index 0 at copy time),
   so both eyes see two camera lights; then apply the row-5 fix and compare brightness near the camera.

## Raw files (agents\AL\)

al0 the 15:10:53 field diff; al1 0x3CCAAC0 / 0x3CCA490 / 0x1B76AA0 / 0x17F2910; al2 / al14 / al16 displacement scans;
al3 0x3CCB020 / 0x17E1CD0 / 0x17E2AA0 / 0x17F3360 / 0x17F3610 / 0x17F2060 / 0x3CC6DC0; al4 0x1E1DF90 / 0x1E1E1D0;
al5 / al8 / al19 callers; al6 split-index readers; al7 per-player-view updaters and 0x3CCCC20; al9 setter callers;
al10 all GetPlayerView callers; al11 0x3E35C80 / 0x3E1F520 / 0x3E1FF80; al12 / al13 SSAO 0x3E4F430 / 0x3E4F490 /
0x3E4DBA0 and disassembly; al15 0x3DFC660 / 0x3DFD980 / 0x3DFB6A0; al17 0x3E09CB0 / 0x3E0AB10 / 0x3F4C7C0 /
0x3D04210; al18 0x3E32910; al20 lerps 0x3D5B880 / 0x3D32440.

## Open leads

* Why the eyes' own cascades differ (row 4).
* Whether the eye is in the 0x1F7FC10 override list (`+0x5F0`); fix 1(b) covers it either way.
* For AM: the lighting object also feeds post settings (exposure settings via lighting+0x10, AE); fix 1 makes those
  equal too.
