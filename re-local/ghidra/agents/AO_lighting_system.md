# Lighting system map and two-view audit (agent AO, 2026-10-23)

Static analysis of bg3_dx11.exe 4.1.1.7631656. Ghidra project BG3_G (read-only, -noanalysis, request server
`agents\AO\AOServer.java`, stopped). bg3vr_disasm. The game's ClusteredLighting_0, VolumetricFogLighting shaders
(agent N's extraction, `N\shaders_asm\`). Mod source: views.cpp, stereo.cpp, resolution.cpp, eyefix.cpp.
RVAs are relative to 0x140000000. "main" = left eye view, "eye" = the CreatePlayerView 0x17F82F0 view (right eye).
Confirmed = read in code at the cited address. Unconfirmed = section 4 says how one run settles it.
Raw output: `re-local\ghidra\agents\AO\` (ao1..ao16). Shadows are left to the shadow agent.

The leading question was AMBIENT. Answer: **all ambient in this renderer comes from environment probes, and the
global probe a view uses is chosen by the view's atmosphere REQUEST id (`[view+0x2888]`, the atmosphere trigger's
GUID). The mod's environment copy writes -1 there for the eye.** So the eye loses the area's probe (no or wrong
ambient diffuse and specular IBL, and no or wrong fog ambient). That gives darker shadowed areas and deeper blacks.

## 1. Per-view differences still present (ranked)

| # | difference | cause (Confirmed address) | fix | status |
|---|-----------|---------------------------|-----|--------|
| 1 | **Ambient: the eye has no (or the level-default) environment probe.** Surfaces lose ambient diffuse (t45/t47) and specular IBL (t46/t48); the fog loses its ambient SH (cb6[26..29]). Shadowed areas get only what local probes give. | The mod calls the setters with request id -1 (views.cpp `call_set_environment`). 0x3CC9700 stores it: `+0x2898 = old +0x2888; +0x2888 = *id` (0x3CC99BE..0x3CC99D4). CullStage execute **0x3E245E0** copies the view's `+0x2898`/`+0x2888` into stage `+0x2370`/`+0x2374` every frame. Cull job **0x3E24970** (probe loop right after its 0x3E246B0 call; ao9 lines 181-199, 756-757): for each scene probe (`[scene+0x1190]`, count `+0x119C`) that passes 0x3D53100 (view cull shape), `probe+0x8C == stage+0x2374` -> stage+0x198, `== +0x2370` -> stage+0x190; then `+0x280 = +0x198`, `+0x1A0 = +0x190`. Both are cleared at the start of every cull (0x3E26C40). No match = no global probe. 0x3E258B0 then takes the probe's entry for the view's atmosphere id (0x3D53A90) into the records +0x1A0 / +0x280 (cubemaps +0x8 / +0x18, SH +0x30..+0x6F, weight +0x270 / +0x350). 0x3E32910 binds the cubemaps; 0x3F3CF00 writes cb6[26..29] (SH), cb6[31].zw (intensities, probe +0x90/+0x94), cb6[32].x (blend), cb6[32].yz (mip counts). The game itself passes the trigger GUID (0x1E1DF90 via `[param_2[3]]` vtable +0x18 -> 0x40FA640) or -1 only for the level default (0x17F8160). | In `copy_environment` pass **main+0x2888** (atmosphere) and **main+0x28C0** (lighting) as the id pointer, as the game's own re-apply 0x19D1F70 passes the view's own field. After the call, copy the previous ids with the game's FixedString assign **0x40F7BC0**(eye+0x2898, main+0x2898) and (eye+0x28D0, main+0x28D0). In `sync_environment` also copy when `[main+0x2888] != [eye+0x2888]` or `[main+0x28C0] != [eye+0x28C0]`. | Mechanism Confirmed (0x3CC9700, 0x3E245E0, 0x3E24970, 0x3E26C40, 0x3E258B0, 0x3E32910, 0x3F3CF00, shader lines 903-1219). That main has a trigger id while the eye has -1 in the user's scenes: Unconfirmed (plan A). |
| 2 | **Dialogue / timeline lighting and atmosphere overrides never reach the eye**, and the eye's atmosphere request id is reset to -1 during them (feeds #1). | Timeline start 0x1D59830 -> 0x1F8FE20 finds views only through GetPlayerView 0x3BAAE20 (hash by player id; CreatePlayerView 0x17F82F0 registers nothing) -> 0x3CBA3E0 registers main -> 0x3CBCB80 applies the override with force and sets the lock `+0x2AEB` (lighting) / `+0x2AEC` (atmosphere). Same list for 0x1F7FC10 / 0x1F80030. `sync_environment` copies main's lock bytes into the eye **before** it calls `copy_environment`. A locked setter only records the id and returns 0 (0x3CCAAC0 top, 0x3CC9700 top). So: the eye's copy is swallowed, targets keep differing, and the copy (plus 0x3CCCC20, which sets both blend flags and rebuilds fog layers and the LUT) runs every frame for the whole dialogue. If only the lighting is overridden, the atmosphere setter is not locked and is re-forced every frame with id -1. | Do what the game's own primary-view path 0x3C49530 does: for each setter call on the eye, save the eye's lock byte, write 0, call the setter (force 1), then write main's lock byte. Call each setter only when that resource's target id or request id differs. | Confirmed (0x1D59830, 0x1F8FE20, 0x3BAAE20, 0x3CBA3E0, 0x3CBCB80, 0x3CBCA10, 0x3C49530, setters). Brightness of the override in the user's dialogue: Unconfirmed (plan B). |
| 3 | Cloud / atmosphere-local origin: PerView rows 20 and 22 are 0 in the eye. | 0x3CC9700 looks the request id up as an entity GUID and writes its position to `+0x2ABC..+0x2AC4` (old one to `+0x2AB0..`), 0x3CC99E0..0x3CC9B78. With id -1 the lookup fails and the eye keeps 0. PerView 0x3CC7BAE / 0x3CC7DB2 writes `-(+0x2AB0)` to row 20 and `-(+0x2ABC)` to row 22. ClusteredLighting uses rows 20/22 to place the local cloud-shadow maps t52/t53 (lines 1331-1373, gate cb12[13].w). | Fixed by #1 (the setter recomputes it from main's id). | Confirmed |
| 4 | Probe blend during atmosphere blends | 0x3E258B0 picks the start/target probe records by `stage+0x2370 == +0x2374` (prev request == current request). The eye's prev id is its own history, not main's. | Fixed by #1 (copy +0x2898 / +0x28D0). | Confirmed |
| 5 | Exposure adaptation lag (few %, only while adapting) | Eye's own exposure job 0x3EDFC40 and ReExpose 0x3EE0D10 (agent AM #3). | Only if plan C shows a gap: AM #3 mid-hooks. | Unconfirmed size |
| 6 | Froxel column 115 shared by both eyes (9 px seam) | agent AI row 3 | AI row 3 | Unconfirmed visibility |
| 7 | Local probe visibility per eye frustum | 0x3D53100 per view: only probes at the frustum edge can differ. Same probe set otherwise. | none | Confirmed code; negligible |
| 8 | Vignette centred per eye viewport | agent AM #5 | optional | Confirmed |

Not differences any more (checked against code): sun colour/intensity, fog/sky rows, LUTs, SSAO constants and AO terms
(targets + remaining blend copy, AL fix 1, done), SSAO projection (stereo.cpp `on_ssao_rays`, done), exposure value t26
(`on_exposure`, done), fog history t1 (`on_fog_history_exposure`, done), camera lights (same spot for both, AL row 5),
clustered tile window, post volumes, tone map, DLSS (AM).

## 2. Mistakes in the mod's current code

| # | where | mistake | fix |
|---|-------|---------|-----|
| M1 | views.cpp `call_set_environment` | The id argument is not an output. 0x3CCAAC0 / 0x3CC9700 only read `*id` (FixedString assign 0x40F7BC0 into +0x28C0 / +0x2888, and comparisons). Passing -1 erases the eye's request id, which the renderer reads (CullStage probe choice, PerView rows 20/22, 0x3CC6DC0 history reset, stage 0x3E probe streaming 0x3E53990). The `release_id` branch is dead code. | Pass `main+0x2888` / `main+0x28C0`. Drop the release. |
| M2 | views.cpp `sync_environment` | Lock bytes are copied before the copy, so any copy while main is locked is swallowed and retried every frame (with 0x3CCCC20 every frame). | Lock bypass per call (0x3C49530 pattern), then set main's lock. |
| M3 | views.cpp `sync_environment` | Compares target ids only. A change of request id with the same atmosphere (two triggers sharing one atmosphere, different probes) is missed. | Also compare +0x2888 and +0x28C0. |
| M4 | views.cpp `copy_environment` | Calls both setters when only one target differs; each forced call restarts that blend from the eye's active object. Harmless once M1/M2 are fixed, but wasteful (and today it re-writes -1 every frame in dialogues). | Call only the setter whose target or request id differs. |
| M5 | views.cpp comment lines 150-155 | Still says "from +0x2880 / to +0x2890"; it is target +0x2880, start +0x2890 (0x3CC9700: start = copy of active). | Comment only. |

Checked and correct:
* `force = 1` with `remaining = duration - elapsed`: full path in the setter (start = copy of the eye's active, elapsed 0,
  duration = remaining, blending 1, active id = src id, target = copy of src). If the eye is mid-blend and its start id
  equals src, the setter reverses instead (target/start swapped, elapsed = (1 - e/d) * T): also correct. Duration 0 is
  finished on the next tick (0x3CC8760: `elapsed >= duration` -> elapsed = duration, flag 0, t = 1).
* No blend can hang: elapsed only stops while the pause byte `+0x2AE8` / `+0x2AE9` is set. Writers: the ctor 0x3CC5AA0 and
  0x1213480 (passes the address; a debug toggle, not decompiled). The mod mirrors main's, so both stop and resume together.
* stereo.cpp exposure redirect (0x3CCCB20) and fog history t1 mid-hook (0x3E3E337): consistent (ratio 1 for the eye).
  The eye's pre-exposed ambient (shader line 1217: `x t26[0] x 100`) uses main's exposure.
* resolution.cpp VR profile: `[0x604AA10]+0x50` DOF (dword, prepare 0x3ED3290), `+0x59` lens flare (prepare 0x3EE1CF0),
  `+0x48` distant shadow cache (AE), god rays `+0x58` untouched (prepare 0x3ED0B40). Global, so the same for both eyes.

### Exact fix for copy_environment (game thread)

```
struct Res { size_t target, request, previous, lock; void* setter; };
Res atm  {0x2880, 0x2888, 0x2898, 0x2AEC, 0x3CC9700};  // blend: +0x28A8 elapsed, +0x28AC duration, +0x28B0 on
Res light{0x28B8, 0x28C0, 0x28D0, 0x2AEB, 0x3CCAAC0};  // blend: +0x28E0, +0x28E4, +0x28E8
for each Res r where id([main+r.target]) != id([eye+r.target]) || dword[main+r.request] != dword[eye+r.request]:
    u8 saved = eye[r.lock]; eye[r.lock] = 0;
    r.setter(eye, [main+r.target], main + r.request, remaining(main, r), /*force*/ true);
    FixedStringAssign_0x40F7BC0(eye + r.previous, main + r.previous);
    eye[r.lock] = main[r.lock];
refresh_eye_fog(main, eye)   // as now, once
```
Copy the pause bytes +0x2AE8 / +0x2AE9 every frame as now. Copy the lock bytes every frame as now, but after the above.

## 3. System map

### 3.1 Environment state per view (game thread)

| field | meaning | writers |
|-------|---------|---------|
| +0x2880 / +0x2890 / +0x28A0 | atmosphere target / start / active (copies, id at obj+8; size 0x378) | 0x3CC9700 |
| +0x2888 / +0x2898 | atmosphere request id (trigger GUID FixedString) / previous | 0x3CC9700 (also when locked) |
| +0x28A8 / +0x28AC / +0x28B0 | atmosphere elapsed / duration / blending | 0x3CC9700, tick 0x3CC8760, 0x3CCCC20 |
| +0x28B8 / +0x28C8 / +0x28D8 | lighting target / start / active (size 0x2F0) | 0x3CCAAC0 |
| +0x28C0 / +0x28D0 | lighting request id / previous | 0x3CCAAC0 |
| +0x28E0 / +0x28E4 / +0x28E8 | lighting elapsed / duration / blending | 0x3CCAAC0, tick, 0x3CCCC20 |
| +0x28F0 (+0x28F8 id, +0x28FC T) / +0x2900 (+0x2908, +0x290C) | pending atmosphere / lighting while blending | setters (non-force), applied by the tick when unlocked |
| +0x2AE8 / +0x2AE9 | pause atmosphere / lighting elapsed | ctor, 0x1213480 |
| +0x2AEB / +0x2AEC | lock lighting / atmosphere (requests only record the id) | 0x3CBCB80, 0x3CBCA10, 0x1F7FC10, 0x1F80030, 0x3C49530 (temp clear) |
| +0x2AB0 / +0x2ABC (vec3 each) | previous / current request entity position | 0x3CC9700 |
| +0x2A40 / +0x2A4C / +0x2A50 / +0x2A68 | sun direction / sun fade / moon flag / cloud scroll | tick 0x3CC8760 |
| +0x2950 / +0x295C, +0x2910, +0x2940 | fog layer list / count, weight map | 0x3CCCCC0, 0x3CC9DB0 |
| +0x2968..+0x2987 | camera light entities (4) | 0x3CCB020 (end of every lighting apply) |
| +0x29E0..+0x2AE0 | atmosphere LUTs, cloud maps, sky textures | 0x3CCB970 (every atmosphere apply) |
| +0x2AED / +0x2AF0 / +0x2AF4 / +0x2AF8 / +0x2B00 | history reset flag; ids and request id seen; controller seen | 0x3CC6DC0 (begin-recording) |

Writers of the environment (all call 0x3CC9700 / 0x3CCAAC0):
* Request loops **0x1E1DF90** (atmosphere) / **0x1E1E1D0** (lighting): every view in `frame+0x42E0` with
  `[[view+0x2260]+0x80] == request camera +0x80` and a scene. Skips a view whose target already has the request's
  resource and the same trigger (`*param_2[3] == *(param_2[1]+0x20)`), unless `param_2[1]+0x3A`. Request id = the
  trigger GUID. Force = `!*param_2[5]`. Reaches the eye (its WriteState camera is main's).
* **0x19D1F70**: GetPlayerView view only; re-applies its own target with its own request id, T 0.
* Timelines: **0x1D59830** start/end -> 0x3CBCB80 (lock + override, T 0, force) / 0x3CBCA10 (unlock + restore the stored
  ids, force); **0x1F7FA10** -> 0x1F7FC10 / 0x1F80030 (override change: unlock, apply with force, relock). Only views in
  the timeline map `+0x5C0/+0x5F0`, filled from GetPlayerView. Not the eye.
* **0x3C49530**: pipeline primary view `[DAT_146046F98+0xA0]` only; clears the lock, applies with force, restores it.
* Scene attach **0x17F8160**: 0x3CC9D30 / 0x3CCAFA0 (level defaults by id) with request id -1.
* Tick **0x3CC8760** (view slot 7 0x17F2910, game thread, per view): advances blends, applies pending requests if not
  locked, lerps (0x3CC9DB0 atmosphere, 0x3CCA490 lighting), sets sun direction / fade, places camera lights,
  broadcasts 0x407AC70 when anything changed (LUT rebuild).

### 3.2 Ambient (the full chain; all Confirmed unless marked)

| term | shader | input | source | per view? | eye vs main today |
|------|--------|-------|--------|-----------|-------------------|
| Global diffuse IBL | ClusteredLighting_0 lines 907-917: `t45(N) x cb6[31].z`, blended to `t47(N) x cb6[31].w` by cb6[32].x; x albedo x DFG t49.z (line 923-925) | t45/t47 cubemaps | 0x3E32910 from CullStage record +0x1A8 / +0x288 (then +0x210/+0x2F0) | view (its CullStage, stage 0x08) | **differs: probe chosen by request id (#1)** |
| Global specular IBL | lines 910-921: `t46(R, mip from roughness, cb6[32].y) x cb6[31].z`, `t48` / cb6[32].z; x (F0 x t49.x + t49.y) | t46/t48 | record +0x1B8 / +0x298 | view | **differs (#1)** |
| Probe intensities / blend | cb6[31].z/.w, cb6[32].x/.y/.z | probe +0x90 (x1 if CullStage+0x88), +0x94, weights +0x270/+0x350, mip counts | 0x3F3CF00 lines 223-423 | view | differs (#1) |
| Local probes (diffuse t54/t55, specular t56..t59 with alpha t60..t63, records t44) | lines 934-1216: box or sphere parallax, fades, blend over the global by weight | CullStage `+0x38` visible probes, entry per atmosphere id (`+0xD8`, 0x80 stride) | 0x3E258B0 / 0x3E26790 (object -> smallest containing probe, else global) | view (cull) | same, except frustum-edge visibility (#7); t44 upload not decompiled |
| Fog ambient | VolumetricFogLighting lines 353-356: L1 SH cb6[26..29] (0.282095 Y00) | probe SH (record +0x30..+0x6F) | 0x3F3CF00 lines 383-418 | view | **differs (#1)** |
| Pre-exposure of ambient | line 1217: `x t26[0] x 100` | exposure record | 0x3CCCB20 (redirected for the eye) | view | same (main's) |
| AO on ambient diffuse | lines 1444-1460: `t5` SSAO; `ambient x ao` | cb0[0].x = AO on (`atm+0x18C && [view+0x2158]+0x18`), cb0[0].y = atm+0x19C (AO on direct light), cb0[0].z = +0x198 | 0x3E35C80 lines 470-475; SSAO job 0x3E4F490 | view | same (targets copy + SSAO hook) |
| Specular occlusion | lines 1445-1454 from AO, NdotV, roughness | t5 | same | view | same |
| AO on direct light | line 1458-1461: `1 + cb0[0].y (ao - 1)` | atm +0x19C | same | view | same |
| Hemisphere / constant ambient colour | **none**: no other ambient term in the clustered or fog shaders | - | - | - | - |
| Cloud shadows (darken sun) | lines 1281-1374: t42/t43 (global), t52/t53 (local, rows 19-22) | PerView rows 13-22; rows 20/22 from +0x2AB0/+0x2ABC | 0x3CC7490 | view | **rows 20/22 differ (#3)** |
| Camera lights | ordinary local lights | lighting target +0x288..+0x2A4 | 0x3CCB020, tick | view entities | same spot in both (AL row 5) |
| Darken environment | post PPDarkenEnvironment (CullStage type 1) | weight 0x3F88090 | AM | view | same |
| Dialogue / timeline lighting | lighting object override | 0x3CBCB80 | main only | **differs (#2)** |

The forward / transparent path builds the same cb6 rows through 0x3F3CF00 (caller 0x3D961D0 at 0x3D9667F); the probe
choice is the same CullStage's. Callers of 0x3F3CF00: 0x3D961D0, 0x3E06930, 0x3E1FF80, 0x3E33F10, 0x3E35C80,
0x3E38940, 0x3E3ABB0, 0x3E3CCE0, 0x3E3F200, 0x3E42130, 0x3E54C00.

### 3.3 Other lighting inputs (summary; details in the cited reports)

| block | builder / job | inputs | thread | per view |
|-------|---------------|--------|--------|----------|
| PerView cb12 | 0x3CC7490 (rows: 0 sun dir, 1 sun colour from atm +0x24..+0x38 x +0x2A4C, 7-9 corner rays, 13-24 clouds/atmosphere from target/start/active atmospheres, +0x2A68, +0x2AB0/+0x2ABC) | atmosphere, lighting +0x260/+0x264 | render (begin) | yes; rays rewritten per eye (N) |
| PerCamera cb11 | 0x3CC6CB9 hook | proxy controller | render | yes (mod) |
| Clustered lighting | 0x3E35C80 (stage 0x1B sub-job), CB 0x3F3CF00, cb0 AO/sun flags | lights 0x64959AC0 / 0x738395A7, sun mask 0x94009344, t52/t53 +0x2A78/+0x2A98, LUTs 0x3CCD4D0, probes 0x3E32910 | render worker | yes (AL 2.3) |
| Local lights / clusters | stages 0x35 / 0x1E, ClusteredAssignment | view frustum | render | yes (T 3, AL 2.4) |
| Atmosphere LUTs | 0x3CCB970 (write), 0x3CCD4D0 (bind t39/t40/t41/t42/t43, cb8) | atmosphere +0x140/+0x158, ids | game / render | same pointers (AE 2.1) |
| Aerial perspective | ClusteredLighting end, `S x t26 x cb12[15].x x 10` | t39/t40, cb12[15], cb11[13] | render | yes |
| Sky dome | 0x3E45750 (gate `[view+0x2AEA] == 0`, copied by stereo.cpp) | LUTs, sky textures, exposure | render | same |
| Clouds | 0x3E47BB0 | LUTs, exposure | render | same |
| Volumetric fog lighting | 0x3E3CCE0 | cascades, distant cache, t26, t1 (redirected), probe SH | render | yes (AE, AI, AK) |
| Exposure | 0x3CCCB20 (record), jobs 0x3EDFC40 (old, active) / 0x3EDE160 (new) | metering per view | render | redirected (AE/AI/AM) |
| PPReExpose | 0x3EE0D10 | own records | render | own (AM #3) |
| Grading LUT | 0x3EF0990 (rebuilt on 0x407AC70 broadcast) | lighting +0x50..+0x17C, +0x240 | render | own; follows the eye's own blends |
| Tone map | 0x3F00FA0 | lighting +0x244, static LUT | render | same once lighting matches |

## 4. One-run logging plan

One run: load a save inside an area with its own atmosphere (interior or village), stand 10 s, start a dialogue, wait
10 s, end it, walk outdoors.

* **A. Ambient probe (decides #1).**
  * Game thread, every 30 frames and on change, main and eye: dwords +0x2888, +0x2898, +0x28C0, +0x28D0; bytes
    +0x2AEB, +0x2AEC; target/start atmosphere ids; vec3 +0x2AB0, +0x2ABC.
  * Render thread, same rate, in the 0x3CC6CB9 hook, for each view's stage 0x08 (vtable 0x58C5AA0, from the stage list):
    dwords +0x2370, +0x2374; qwords +0x190, +0x198; record +0x1A0 (probe ptr), +0x1A8, +0x1B8, float +0x270; record
    +0x280 (probe ptr), +0x288, +0x298, float +0x350; byte +0x88. Once per load: the scene probe list
    `[[view+0x2B10]+0x1190]` (count +0x119C): each probe pointer, +0x8C, +0xB0, +0x90, +0x94.
  * gpu_trace, one frame: at each view's ClusteredLighting dispatch (CS with SRV slots 45-48 cube) log the SRV
    resource pointers and their mip count / size. Read back 1 texel of t45 at +Y.
  * Prediction: main +0x2888 = a trigger id, stage+0x280 probe non-null with +0x8C equal to it; eye +0x2888 = -1,
    stage+0x280 null or a probe with +0x8C = -1; eye's t45/t46 null or different.
  * A/B key: apply fix #1 (copy main's request ids). Prediction: eye's stage+0x280 matches main's; shadowed areas match.
* **B. Dialogue lock (decides #2).** Log-only detours on 0x3CCAAC0 / 0x3CC9700: view role, src id `[rdx+8]`, `*r8`,
  T (xmm3), force (`[rsp+0x28]` on entry), lock byte at entry, return value, return address. Prediction: in the dialogue,
  one forced call on main from 0x3CBCB80 (through 0x3CCAFA0), then a mod call on the eye every
  frame returning 0 with the eye's lock = 1. With fix #2: one mod call returning 1.
* **C. Exposure (#5).** AM plan F (records of both views, staging read-back once a second).
* **D. Cloud rows (#3).** PerView block rows 20 and 22 per view (in the 0x3CC6CB9 hook). Prediction: eye rows 0 while
  main's are a trigger position; equal after fix #1.

## Raw files (agents\AO\)

ao1 setters 0x3CC9700, tick 0x3CC8760, 0x3CC9DB0, 0x3CCCC20, 0x40F7BC0, 0x3CCCCC0, 0x3A087B0; ao2 request loops
0x1E1DF90 / 0x1E1E1D0, 0x19D1F70, 0x1F7FC10, 0x1F80030, 0x3C49530; ao3 field scan (+0x2888, +0x2898, +0x28C0, +0x28D0,
locks, +0x2AB0, +0x2ABC); ao4 0x3CBCA10, 0x3CBCB80, 0x3CC6DC0, 0x3E245E0, 0x3E51D10; ao5 scan +0x2370/+0x2374/+0x15C;
ao6 0x3E258B0, 0x3E53990; ao7 scan +0x1A0/+0x280; ao8 / ao11 / ao15 callers; ao9 cull job 0x3E24970, 0x3E32910,
0x3D53A90, 0x3E26790; ao10 0x3D53100, 0x3CC9D30, 0x17F8160, 0x3CCAFA0, 0x3E26C40; ao12 0x1D59830, 0x1F7FA10,
0x17F82F0; ao13 0x1F8FE20, 0x3CBA3E0; ao14 cb6 builder 0x3F3CF00; ao16 0x3E1F310.

## Open leads

* t44 / local probe array upload (not decompiled); only frustum-edge differences expected.
* SSR (ClusteredLighting_*_SSR, SSSRApply): uses the per-eye cb12 rays (N); not decompiled here.
* 0x1213480 (pause byte writer): assumed a debug toggle from its address-only use; not decompiled.
