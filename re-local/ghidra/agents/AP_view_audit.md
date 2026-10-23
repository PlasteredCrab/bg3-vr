# GameRenderView audit: main (left eye) vs eye view (agent AP, 2026-10-23)

Static analysis of bg3_dx11.exe 4.1.1.7631656. Ghidra project BG3_F (read-only, -noanalysis, request server
`agents\AP\APServer.java`, stopped). bg3vr_disasm. Mod source (views.cpp, stereo.cpp, eyefix.cpp, bootstrap.cpp) and the
15:08-15:11 log (`bin\bg3vr\bg3vr.log`; field diff 15:10:53 = `AL\al0_fielddiff_1510.txt`). RVAs relative to 0x140000000.
"main" = pipeline main view 0x1D542C015B0, "eye" = CreatePlayerView view 0x1D62208C6D0 (log 15:10:22).
Confirmed = read in code or log at the cited place. Unconfirmed = section 4 settles it. Raw output: `agents\AP\` (ap1..ap9).

Note: the 15:10:53 diff was taken with the 15:03 DLL. The environment copy (AL fix 1), SSAO (AL fix 3), fog-history
exposure (AI fix 1) and the exposure redirect are in the current source (DLL 15:41), so their diff rows are marked "fixed
in code". They are not repeated as findings.

## 1. Real per-view differences, ranked, with fixes

| # | difference | what reads it, visual effect | fix | thread | risk | status |
|---|-----------|------------------------------|-----|--------|------|--------|
| 1 | **Cinematic flag `+0x2961` changes without a fog refresh for the eye.** In every conversation the game calls **0x3CCCC20(view, 1)** on join and (view, 0) on leave, only for player-manager views: 0x1F90B00 (callers 0x1D4C6B0, 0x1D5B890, 0x1D5BE40, 0x1D62890, timeline code; agent G 2.5), 0x1D11580 (variant 1), 0x1D118A0 / 0x1D11AE0 (0). 0x3CCCC20 sets the flag, makes a fog-layer entry for (atmosphere id, variant) (0x3CCCCC0), and sets `+0x28B0`/`+0x28E8` so the next tick re-blends. The eye gets only stereo.cpp's raw byte copy of `+0x2961` (render thread, stereo.cpp:611). views.cpp calls 0x3CCCC20 for the eye only after `copy_environment`, and that now runs only when main's **targets** differ from the eye's. When the dialogue keeps the lighting, or the game's request loop gives both views the same new lighting, the eye never refreshes. | Atmosphere blend **0x3CC9DB0** (runs only while `+0x28B0`): every fog-layer entry's weight goes to 1 if (id == target atmosphere id and variant == `(+0x2961 != 0)`), else to 0. It also sets active atmosphere `+0x230`/`+0x234` from the variant block `atm + 0x230 + variant*0x98` when `atm+0x360` is set. Tick **0x3CC8760** refreshes only entries that match the current variant. So in a conversation, main switches to the variant-1 fog layer and the variant-1 `+0x230/+0x234`. The eye keeps the variant-0 layer (weight 1) and the old `+0x230/+0x234`. Readers: fog density step **0x3E3A650** (`+0x2950`/`+0x295C`) and the froxel depth distribution **0x3CC70B0** (`[[view+0x28A0]+0x234]` = fog far, called in begin-recording). Effect: different fog density, height fog, haze colour and froxel slicing in the right eye during dialogues. That fits the 2026-10-22 report ("right eye clearly darker during a conversation"). | views.cpp `sync_environment` (game thread, every frame): keep `static std::uint8_t g_eye_variant = 0xFF` (reset when the eye view changes). Read `v = byte [main+0x2961]`. If `v != g_eye_variant`, call `refresh_eye_fog(main, eye, true)` (it calls 0x3CCCC20(eye, v)) and set `g_eye_variant = v`. Also set `g_eye_variant` inside `refresh_eye_fog`. Compare with the remembered value, not with `[eye+0x2961]`: the render-thread byte copy may already have changed the eye's byte. The stereo.cpp byte copy can stay. | game thread (same as the game's 0x1F90B00 calls) | low: the game's own per-player-view call; it also rebuilds the froxel grid (0x3CC85D0) and broadcasts 0x407AC70, as it does for main | Mechanism **Confirmed** (0x3CCCC20, 0x3CC9DB0, 0x3CC8760, 0x1F90B00 / 0x1D11580 call sites, stereo.cpp:611, views.cpp `sync_environment` early return). Size: depends on `atm+0x360` (has a cinematic variant) in the user's scenes: **Unconfirmed** (plan 1) |
| 2 | **Request ids `+0x2888` (atmosphere) and `+0x28C0` (lighting) are -1 in the eye** (diff: eye `+0x2888` = FFFFFFFF, `+0x28C0` = FFFFFFFF; main 0x1D401CD1 / 0x1D801CD1). `call_set_environment` passes a local `id = -1`. | **0x3CC6DC0** (begin-recording, every view) sets the reset flag `+0x2AED` when `+0x2AF8 != +0x2888` after a blend, and when the active ids change. Readers of `+0x2AED`: stage 0x1B fog history reset (0x3E32B50), old exposure job 0x3EDFC40 (Immediate instead of Progressive), 0x3EDE160, 0x3EF4170. A request that changes only main's request id resets main's fog history for one frame and not the eye's: a short haze mismatch after some environment requests. The setter's "same request, refresh target only" test (0x3CCAAC0: `+0x28C0 == *id`) also behaves differently for later game requests on the eye (it restarts an equal-to-equal blend; no visible change). | In `call_set_environment` pass a pointer to main's own field: `set_atmosphere(eye, [main+0x2880], (int*)(main+0x2888), blend, true)` and `set_lighting(eye, [main+0x28B8], (int*)(main+0x28C0), blend, true)`. The setter only reads `*id` and copies it with 0x40F7BC0 (0x3CCAAC0 lines 33-35, 99-100 of al1), so **do not release** it afterwards (the mod does not own that reference). | game thread | low | Field values **Confirmed** (diff); reset logic **Confirmed** (0x3CC6DC0); how often main's request id changes alone: **Unconfirmed** (plan 2) |
| 3 | **Camera lights twice, in both eyes.** The end of every lighting set, **0x3CCB020**, spawns up to 4 camera-light entities per view into `+0x2968..+0x2987` (diff: both views hold a live handle at `+0x2968`). It hides each view's light from the other split index with 0x3C6B810(visual, mask, 0) = clear `mask` in `visual+0xF4`: index 0 -> clear 0x400, index 1 -> clear 0x200 (0x3CCB5B5..0x3CCB5D4). The eye has index 0 when the mod sets its lighting, so its light keeps 0x200 like main's. Each view's tick places its lights at `[view+0x2260]` (WriteState camera = main's real controller for both) plus the forward offset. | Two coincident camera lights. Both eye views render with index 0 (cull mask 0x201, A 1.3), so both eyes would see both. Same in both eyes, so not an L/R difference, but the camera light is about twice as bright as flat play. | Mid-hook at **0x3CCB5D1** (`45 33 C0` xor r8d,r8d, then the call 0x3C6B810; it is the join of the 0x200 and 0x400 paths, nothing jumps into 0x3CCB5D2..0x3CCB5D8). There: `view = [rsp+0x48]`; if view is the eye, set `rdx = 0x600` (clear both player bits: invisible to everyone). Main's light then lights both eyes. | game thread (inside the lighting setter) | low | Spawn and mask **Confirmed** (code, diff). That light culling uses `visual+0xF4` player bits like geometry: **Unconfirmed** (plan 3) |
| 4 | **Per-entity overlay entries missing in the eye** (not lighting). The sets at `+0x2CA8` / `+0x2CB8` (diff: first dword 0x29 in main, 0 in the eye) and the entries of the view's post object `+0x2BE0` (ctor 0x180F4B0, job 0x180FB20) are filled only through GetPlayerView: 0x17E1CD0, 0x17E2AA0, 0x17E80F0, which call 0x1810800, 0x18109C0, 0x1810B90, 0x1810D60 on `[view+0x2BE0]`. | Job **0x180FB20** draws one pass per entry with a colour from the global settings table `[0x14604B1B8] + (type + 0xE8)*0x10` (gamma 2.2) and the entry's position data. Whatever main draws there is missing in the right eye. What the overlay shows in game: Unconfirmed (R: room-related). | Inline hooks on 0x1810800 / 0x18109C0 / 0x1810B90 / 0x1810D60: call the original; if `rcx == [main+0x2BE0]`, call it again with `[eye+0x2BE0]` and the same other arguments. Do not mirror the sets `+0x2CA8/+0x2CB8` (they hold references; 0x17F3360 releases them). Only after plan 4 shows main's object has entries. | game thread | medium (4 hooks, game data structure) | Fill path **Confirmed** (code); visual **Unconfirmed** |
| 5 | **G-buffer render-pass descriptors captured with split index 1 at setup.** Stage 0x0B init **0x3E2CF50** writes `1 + (index != 0)` (0x3E2D3E4..0x3E2D3F1) into descriptor field +0xC of keys 0x3B0A2BF7, 0xFF193183 and others (0x3E2D58A, 0x3E2DA39); the eye was built with index 1, so it got 2. The descriptors go to 0x17DCB10 / 0x4095F50. | Meaning of the field (load action, owner mask, ...): **Unconfirmed**. The eye's G-buffer shows no fault so far. | None until plan 5. | - | - | Capture **Confirmed**; effect **Unconfirmed** |

Setup-time captures already handled elsewhere (index 1 during `create`): stage 0x02 distant cache index, 0x03 / fog-lighting
declared keys (AE / AK; distant cache off in VR), stage 0x44 volumetric clouds (built only for the main view or index 1,
0x3AC51C0; the eye has its own 0x44, log 15:10:22.373, which is what we want).

## 2. The 15:10:53 field diff, classified

545 differing qwords in `+0x0000..+0x2CFF` (diff_eye_views does not read `+0x2D00..+0x2D8F`).

| offsets | main / eye | what it is | class |
|---------|-----------|-----------|-------|
| +0x08, +0x20 | own addresses | list link / self pointer | expected |
| +0x28, +0x30 | stage array; 0x1D vs 0x1C stages | eye lacks stage 0x0A (debug-draw overlay, main view only, A 4.2; log 15:10:22.350 vs .373) | expected |
| +0x40, +0x48, +0x58..+0x78 | own objects | job context (root ctor 0x407B080), MOC (rf ctor), command recorder pointers | expected |
| +0x2B0..+0x2058 (~480 qwords) | 0 / garbage | inside the view's command recorder (0x407F110 at +0x50 -> 0x40C8730). The eye's allocation (0x17F82F0, 0x4175960) is not zeroed. +0x2058 (= recorder+0x2008) is never written; no reader of view+0x2058 in render code (ap2) | expected (own object storage) |
| +0x2148, +0x2150 | 0 / garbage, 0 / 1 | +0x2148 = recorder+0x20F8 (unwritten, no view reader); +0x2150 = "recording" byte (root ctor 0, set at begin-recording, cleared by 0x3CC7010): the eye was recording when the diff ran; +0x2151.. padding | expected |
| +0x2168, +0x2178, +0x2188, +0x2198, +0x21A8, +0x21B8, +0x21C8 and +0x21F0..+0x2250 | 0 / 0.5, 1036, 1786 | ReadState / WriteState offset and pixel offsets of the right half (0x407B800 / 0x407BB80) | expected |
| +0x21D0 / +0x2258 | bytes +6, +7 = FFFF in eye | state padding after OwnerPlayer (root ctor writes +0x21D0 byte and +0x21D8 only); +0x21D4 readers (0x3EE46C0, 0x3EE6070, 0x3EEFD20, 0x3EF2860) read a CullStage array count, not the view | expected (garbage, no reader) |
| +0x21D8, +0x2B00 | real controller / proxy | ReadState camera and last-seen camera (stereo.cpp proxy ring + keep_view_history) | expected |
| +0x2268 (+0x226C) | 0 / 1 | side. Readers 0x3E16830 / 0x3E17490 / 0x3D30E40 / 0x3DD21D0 index per-object previous-frame records by side (`[obj + side*4 + 0x110/0x120]`, frame compare at 0x3E1723A..0x3E17270). Side 1 gives the eye its own motion history | expected, keep it |
| +0x2270 | 0 / 1 | view index (render loop 0x4072B30, every frame). 0x3E3A650 / 0x3E3CC00 record the shared fog volumes 0xE66299BB/0x9165A92D, 0xBCE29AFA/0xCBE5AA6C and flag a reset only when index is 0; jobs 0x3E3ABB0 / 0x3E3CCE0 clear when `DAT_145EF2CB0/CB4` == own index. Main does it for both | expected |
| +0x2278, +0x2288 (+0x228C) | 0 / garbage | never written by the ctors; readers of +0x2278/+0x2288 are stage 0x1F (0x17DA740), stage 0x1E (0x3E01710, 0x3E018A0) and class 0x3A94xxx objects, not views | expected (garbage, no reader) |
| +0x2298 (+0x229A/B) | 0 / FFFF | byte +0x2298 (grid +1 column flag, 0x3CCCC20 / 0x3CC86D0) equal; upper bytes unwritten, only byte reads in render | expected |
| +0x22B0 (+0x22B4), +0x22B8 | 0 / garbage | +0x22B0 = fog far (0x3CC70B0, equal). +0x22B4..+0x22BF never written; their readers 0x3DFC660 (stage 0x35), 0x3E24970 (CullStage) read other objects | expected (garbage, no reader) |
| +0x2340..+0x281F (42 qwords) | camera values | PerView[0]/[1] and PerCamera[0]/[1]: eye position, rays, inverse view, P[8] ±0.1228; +0x2410 / +0x2460 / +0x2A40 1-ulp sun differences | expected |
| +0x2820, +0x2838, +0x2850, +0x2868 | own | GPU constant buffer objects (rf ctor) | expected |
| +0x2880..+0x28E8 | env objects, ids, blend state | environment: main blending 2.0/2.0, eye 0/0 (AL 1.2) | fixed in code (AL fix 1); ids: finding 2 |
| +0x2898 (+0x289C), +0x28B0 (+0x28B4), +0x28C0 (+0x28C4), +0x28D0 (+0x28D4), +0x28E8 (+0x28EC) | 0 / garbage in upper halves | padding next to written fields; no reader of those offsets in render code | expected |
| +0x2910..+0x2958 | capacity 4 / 2 | fog-layer map and lists: allocation size only; counts equal (1) at that time | expected (contents: finding 1) |
| +0x2960 (+0x2963..+0x2967) | 0 / garbage | +0x2960..+0x2962 equal; +0x2963.. unwritten, no displacement hit | expected |
| +0x2968 | different handles | camera light entity per view | finding 3 |
| +0x29B8 (+0x29BC), +0x29D8 (+0x29DC), +0x2A50 (+0x2A51..), +0x2AF8 (+0x2AFC), +0x2B08 (+0x2B0C) | garbage upper halves | written fields are dword/byte; upper parts unwritten, no reader | expected |
| +0x29C0, +0x29C8 | own | exposure record fallbacks (rf ctor), redirected for the eye by stereo.cpp | expected |
| +0x2A40, +0x2A48 | 1 ulp | sun direction from each view's own active atmosphere (tick 0x3CC8760 lines 254-263) | expected |
| +0x2AF8 | id / -1 | atmosphere request id seen (0x3CC6DC0) | finding 2 |
| +0x2B38, +0x2B40, +0x2B58 | own | grading LUT objects (AM) | expected |
| +0x2BD0 (+0x2BD2..+0x2BD7) | BF80 / garbage | config dword cfg+0x60: CreatePlayerView fills only the word 0x0101 (0x17F82F0 `local_28`), so +0x2BD2/+0x2BD3 are stack garbage; +0x2BD4.. unwritten. Only bytes +0x2BD0 (MOC) and +0x2BD1 (0x3D95620, 0x3E383C0, 0x3E393D0, 0x3E39C40, 0x3E3C330, 0x3E3EDD0, 0x3E41A80) are read | expected (garbage, no reader) |
| +0x2BD8 | 0 / garbage | unwritten; readers are 0x12xxxxx classes and 0x36B0C00 (not a view) | expected |
| +0x2BE0, +0x2C80, +0x2CB0, +0x2CC0 | own | overlay post object, focus CB, set buckets | expected (objects); entries: finding 4 |
| +0x2CA8, +0x2CB8 | 0x29 / 0 | entity sets filled only for player views | finding 4 |

All uninitialised fields were checked against a scan of every displacement +0x2150..+0x2D8F (ap5) and the listed odd offsets
(ap2): none has a render-side reader on a view. They are harmless today, but values can change between runs.

## 3. Field map (who sets what)

Size 0x2D90 (0x17F82F0 allocates it). Ctors: root 0x407B080 -> rf 0x3CC5AA0 -> GameRenderView 0x17F1E60 (ap1, R r14/r6).

| offsets | contents | set by | per-frame writer | lighting/shadow/fog/post readers |
|---------|----------|--------|------------------|------------------------------------|
| +0x00..+0x48 | vtable, frame +0x10, res mgr +0x18, stages +0x28/+0x30, job ctx +0x40, MOC +0x48 | root / rf ctor | - | stage gate 0x17DA690 |
| +0x50..+0x2150 | command recorder, recording byte +0x2150 | root ctor (0x407F110) | slot 3 / slot 4 0x3CC7010 | - |
| +0x2158..+0x21DF | ReadState (enabled, scale/offset, render/upscaled/display viewports, owner, controller) | root ctor | frame sync 0x4072CB0 (copy of WriteState); mod proxy at 0x17F21E0 | everything (viewport CB 0x40816E0, 0x3CCCFD0, 0x3F3CF00, SSAO, DLSS) |
| +0x21E0..+0x2267 | WriteState; +0x2260 camera | root ctor; 0x3CC90B0 (SetCamera) | RenderViewportSystem 0x192ED80 (player views only); mod `place_view` | - |
| +0x2268 / +0x226C / +0x2270 / +0x2274 | split index / side / view index / offscreen | root ctor; AddPlayerView 0x3BAA1A0 (index 1) | 0x192ED80 (side, player views); 0x4072B30 (index, all views) | see section 2 |
| +0x2280 | secondary vtable | ctors | - | - |
| +0x2290..+0x22BF | view size, grid flag +0x2298, tile +0x229C, slices +0x22A0, fog far +0x22B0 | rf ctor; 0x3CCCC20 (player views), 0x3CC86D0 / 0x3CC9190 (resize) | 0x3CC70B0 (+0x22AC, +0x22B0) | 0x3F3CF00, fog sub-stages |
| +0x22C0..+0x22EF | froxel depth distribution | rf ctor (0) | 0x3CC70B0 in begin-recording, all views | 0x3F3CF00 (+0x170..), fog |
| +0x2338 | camera-volume clip object | rf ctor | slot 7 -> 0x17F3060 (all views); slot 15 0x17F3280 | MOC only |
| +0x2340..+0x281F | PerView[0..1], PerCamera[0..1] | ctors 0x3CC5740 / 0x3B91570 | 0x3CC7490 x2 (all views); mod rewrite at 0x3CC6CB9 | every shader b11/b12 |
| +0x2880..+0x290F | environment target/start/active, blend, pending | rf ctor; set-scene 0x3CC9D30 / 0x3CCAFA0 | tick 0x3CC8760 (all views); requests 0x1E1E1D0 (views whose camera matches); 0x19D1F70 (player views); 0x3C49530 (primary only); mod copy | 0x3CC7490, 0x3E1FF80, 0x3E35C80, 0x3E4F430 / 0x3E4F490, post chain, clouds 0x3E47BB0 |
| +0x2910..+0x295F | fog-layer map, weights +0x2940, entries +0x2950 / count +0x295C | 0x3CCCCC0 (only via 0x3CCCC20) | 0x3CC9DB0 weights, 0x3CC8760 params | 0x3E3A650 |
| +0x2960..+0x2962 | registered byte, cinematic flag, camera byte | rf ctor; 0x3CCCC20 (player views); 0x17F7CD0 (+0x2962) | mod byte copy (+0x2961) | 0x3CC9DB0, 0x3CC8760, 0x3E1F520, 0x17FCF10, 0x17F2B80, 0x3EE6070 (+0x2962) |
| +0x2968..+0x2987 | camera lights | 0x3CCB020 (end of 0x3CCAAC0) | tick places them | lights |
| +0x2988..+0x29BC | +0x2988/+0x2990 (0x3CCB670), +0x2998/+0x29A4 (0x3CCA490), tiles +0x29A8..+0x29B8 | rf ctor, atmosphere setter, 0x3CCCC20 | - | 0x3CC7490, 0x3F3CF00 |
| +0x29C0..+0x2AAF, +0x2AC8..+0x2AE7 | exposure fallbacks, atmosphere textures / LUT handles | rf ctor; 0x3CCB970 / 0x3CCC260 (from 0x3CC9700) | - | 0x3CCD4D0, 0x3E35C80 / 0x3E3CCE0 (+0x2A78, +0x2A98), sky 0x3E45750 |
| +0x2A40..+0x2A6C | sun dir, day/night fade +0x2A4C, moon +0x2A50, cloud scroll +0x2A68/+0x2A6C | rf ctor | tick 0x3CC8760 (all views) | 0x3CC7490, 0x3E1C940 (stage 0x02), 0x3E47BB0 |
| +0x2AB0..+0x2AC7 | atmosphere scalars | 0x3CC9700 | - | 0x3CC7490 |
| +0x2AE8..+0x2AED | locks, player byte +0x2AEA, reset +0x2AED | rf ctor; 0x17E7FD0 (+0x2AEA, player view); 0x1F7FC10 / 0x1F80030 (locks) | 0x3CC6DC0 (+0x2AED); mod copies locks and +0x2AEA | 0x3E32B50, 0x3EDFC40, 0x3EDE160, 0x3EF4170, 0x3E45750 |
| +0x2AF0..+0x2B08 | ids seen, last controller, scene-changed | rf ctor; 0x3CCC550 | 0x3CC6DC0, tick | - |
| +0x2B10..+0x2B68 | scene, scene SRV, LUTs | 0x3CCC550, 0x3CC6630 | - | stage gate, 0x3EF0990 / 0x3EF2860 |
| +0x2B70..+0x2BD3 | creation config | rf ctor (cfg) | - | 0x3DFB6A0 (atlas), 0x3F3CF00, 0x3E015F0, +0x2BD1 readers |
| +0x2BE0 | overlay post object | 0x588E50 | entries via GetPlayerView callers | 0x180FB20 |
| +0x2BE8..+0x2BFD | player handle, room | GV ctor; 0x17E7FD0 (player view) | mod copy | stage 0x33 0x1816520, room filter 0x17E3320 |
| +0x2C00..+0x2C7F | focus CB (b7) | GV ctor | 0x17F2B80 (begin-recording, all views, from +0x2CC8..) | see-through shaders |
| +0x2C98 / +0x2CA0 | camera override / game camera | 0x17F7CD0, 0x17F2B30, 0x1D11AE0 | - | - |
| +0x2CA8 / +0x2CB8 | entity sets | GV ctor (0x17F3760) | 0x17E1CD0 / 0x17E2AA0 / 0x17E80F0 (player view) | overlay |
| +0x2CC8..+0x2D87 | focus own/other + 5 positions | GV ctor | slot 2 0x17F2230 (own player only), slot 7 animates; mod copies main's | 0x17F2B80, MOC |

Per frame for every view (Confirmed): 0x4072CB0, 0x4072B30, slot 7 0x17F2910 (focus animation, tick 0x3CC8760, 0x17F3060),
begin-recording 0x17F21E0 -> 0x17F2B80 -> 0x3CC6C00 (0x3CC6DC0, 0x3CC7490 x2, 0x3CC70B0, uploads), slot 2 0x17F2230.
Per frame / per event only for player views: 0x192ED80, 0x17E7FD0, 0x17F2230 "own" data, 0x17E1CD0 / 0x17E2AA0 / 0x17E80F0,
0x19D1F70, 0x1D11580 / 0x1D118A0 / 0x1D11AE0 / 0x1F90B00 (-> 0x3CCCC20). Index-0 only: shared fog volume reset (0x2270).
Primary view only: 0x3C49530, stage 0x0A.

## 4. One-run logging plan (extend `diff_eye_views`, views.cpp)

Run: load outdoors, stand 10 s, start a conversation, stay 20 s, leave it, walk into a new area.

1. `kBytes` 0x2D00 -> **0x2D90**. Do not print qwords in **+0x0080..+0x214F** (recorder); log only how many differ.
2. After the qword diff, for both views log:
   * bytes +0x2961, +0x2962, +0x2AE8..+0x2AED, +0x28B0, +0x28E8; dwords +0x2888, +0x2898, +0x28C0, +0x28D0, +0x2AF0, +0x2AF4,
     +0x2AF8; floats +0x28A8, +0x28AC, +0x28E0, +0x28E4.
   * fog layers: `[+0x2950]`, count `+0x295C`; per entry (0xA4 bytes): dword +0x00 (id), +0x04 (variant), float +0xA0 (weight),
     floats +0x08..+0x27; weights `[+0x2940]` (count dword +0x294C).
   * atmospheres: byte `[[+0x2880]+0x360]`, floats `[[+0x2880]+0x230]`, `+0x234`, `+0x2C8`, `+0x2CC` (variant 1);
     floats `[[+0x28A0]+0x230]`, `+0x234`.
   * froxels: floats +0x2290..+0x22EF.
   * camera lights: qwords +0x2968, +0x2970, +0x2978, +0x2980; `[[+0x28B8]+0x288]`, bytes +0x289..+0x28C.
   * overlay: dwords +0x2CA8, +0x2CAC, +0x2CB8, +0x2CBC; `[[+0x2BE0]+0x78]`, `[[+0x2BE0]+0xF0]`.
3. When to call it: every 600 frames; from `sync_environment` whenever `[main+0x2961]` changes, and again 120 frames later.
   Prediction for finding 1 without the fix: after joining, main has an entry (id, 1) with weight 1 and its (id, 0) weight
   0; the eye has only (id, 0) with weight 1, and `+0x22B0` differs if `+0x234` differs between the variant blocks. With
   the fix both lists match within one frame.
4. Log-only mid-hook **0x3CCB5D1**: view role, byte `[view+0x2268]`, edx (mask), rcx (visual). Prediction: eye mask 0x400.
   For plan 3: A/B the fix (hotkey) in a dark interior next to a wall, gpu_trace one frame, count the light records of
   stage 0x35 / 0x1E in each view (two at the camera position without the fix, one with it).
5. G-buffer descriptors: log-only mid-hook **0x3E2D3F1** (edi = descriptor value, view = `[r15+8]`), and in one gpu_trace
   frame compare, for each view's stage 0x0B, the resources bound for keys 0x3B0A2BF7 / 0xFF193183 / 0x4C0D1B61 /
   0xA2037A4D and whether a ClearRenderTargetView hits them. Same resources and no extra clear: the value has no effect.
6. Finding 2: every 60 frames and on change, `+0x2888`, `+0x28C0`, `+0x2AF8`, `+0x2AED` of both views. Prediction: main's
   `+0x2AED` = 1 on frames where the eye's stays 0.

## Raw files (agents\AP\)

ap1 ctors 0x3CC5AA0, 0x407F110, CreatePlayerView 0x17F82F0, 0x4073230, slots 0x17F21E0, 0x3CC6FD0, 0x3CC7070, 0x17F2670,
0x407BEB0, 0x17DF050, 0x17F3280, 0x17F26F0, 0x17F2180, 0x3CC9190; ap2 displacement scan of uninitialised/odd offsets;
ap3 split-index readers 0x3E00310, 0x3E1F150, 0x3E3CA20, 0x3E51DE0, 0x3F79E80..0x3F7DFD0; ap4 disassembly 0x3E2CF50;
ap5 every displacement +0x2150..+0x2D8F; ap6 the same grouped by offset for view-anchored functions (`group.py`);
ap7 overlay object 0x180F4B0 / 0x1810800 / 0x1810B90 / 0x1810D60 / 0x180FB20; ap8 0x17F3060, 0x3CCB970, 0x3CCC260,
0x3CCB670, 0x3CC6630, 0x3E45750, 0x3CCD270, 0x3CC85D0, 0x3CC92B0 and callers; ap9 callers of the cinematic-flag setters;
`parse_diff.py`, `diff_hi.txt` (diff above +0x2058).
