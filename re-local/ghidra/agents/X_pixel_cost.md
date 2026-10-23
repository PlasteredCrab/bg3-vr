# Pixel cost of same-frame stereo - agent X, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_H, read-only, headless, request server
`agents\X\XServer.java`, plus `bg3vr_disasm` and two small byte-scan scripts, `X\rva_str.py` and `X\nvwrap.py`).
RVAs are relative to 0x140000000. Raw output is in `re-local\ghidra\agents\X\` (x1..x23). Builds on A (rendering),
O (render resolution, upscaler sizes, DLSS feature), N (stereo lighting), H (graphics options) and S (stage 0x0B).
"Settings" means the graphics options object `[0x604AA10]`.

Starting point (measured by the user, not by me): about 26 fps in stereo against 49 fps flat at 1920x1200 on an
RTX A4000. Stereo draws 3572x1834 = 6.55 MP and flat draws 2.30 MP, so 2.84x the pixels. **The deployed
graphicSettings.lsx has `AntiAliasing = 3` and no upscaler, which is DLAA: DLSS at native resolution, run once per eye
(section 1.2). Every pass before the upscaler runs at the full 6.55 MP.**

## Savings for the VR mod

Ranked by expected gain divided by risk. The gain estimates are pixel arithmetic, not measurements. How much of the
38 ms stereo frame scales with pixels is unknown until the A/B run in section 7. T (shared passes) and W (CPU) may
cap the gain.

| # | what | expected gain and why | RVA / setting / hook | how | risk | confidence |
|---|------|------------------------|----------------------|-----|------|------------|
| 1 | **DLSS Performance (or Balanced) instead of DLAA** | Everything before the upscaler (G-buffer, decals, clustered lighting compute, SSAO, fog, sky, forward, bloom, DOF, god rays, lens flare, exposure) drops from 6.55 MP to 1.64 MP (Performance, 25 %) or about 2.2 MP (Balanced, about 34 %). DLSS itself already runs today, as DLAA, so its own cost stays about the same. This is the largest pixel lever. | Settings +0x77 = 3 (DLSS), +0x7A = 4 (Performance; 3 = Balanced), +0x80 = 0. Game setter **0x406FC70**(settings, mode). Render size **0x406FD20** -> NGX optimal settings 0x40CD860. Per-view DLSS step vtable 0x58D4E30: prepare 0x3EF3490 -> 0x3EF3790, create 0x3EF3920 -> 0x40BD060, evaluate job **0x3EF34B0** | Zero code: pick DLSS / Performance in the game's graphics menu while the VR size is in force. The menu applies through 0x1B551D0 -> 0x4071DD0 -> resize event -> the mod's 0x585510 hook, which keeps the VR size. In the mod: on the event-pump thread call 0x406FC70(settings, 4), then set `resolution::g_pending` so the existing pump path re-sends the size through 0x585510. Optionally raise `[render] vr_resolution` to 100 at the same time (DLSS then outputs 2232x2292 per eye from 1116x1146) | Low-medium. DLSS needs correct per-eye motion vectors and jitter. The mod's stage 0x40 hook (0x3E12730) and its jitter-preserving projection rewrite are already in. Possible ghosting where the eyes disagree. Each eye has its own NGX feature and history (1.3) | Code path Confirmed; fps gain Unconfirmed |
| 2 | **Depth of field off** | DOF runs in dialogues and cutscenes (camera DOF) and where the level's lighting enables it: a COC, half- or quarter-resolution blur and composite chain per eye. In VR it is also a comfort problem. No cost outside those scenes | Settings **+0x50 "DOF"** (int, default 2), +0x54 "DOFQuality" (default 1). DOF step vtable 0x58D36F0 (ctor 0x3ED2AD0), prepare **0x3ED3290**, job 0x3ED3300 | Write `[0x604AA10]+0x50 = 0`. The prepare reads it every frame. Or set it in the menu or file (key "DOF") | Very low | Confirmed (prepare code, name from the option-name table at 0x3BB77BB) |
| 3 | **FogQuality 1 or 2** | The volumetric fog froxel tile is 9 / 12 / 18 / 24 px for FogQuality 0 / 1 / 2 / 3. Froxels per eye scale with 1/tile^2, so FogQuality 1 needs 56 % of the Ultra froxels and FogQuality 2 needs 25 %. The default is 0 (Ultra, 9 px) | Settings **+0x4A FogQuality**. Read by **0x3CC85D0** (view +0x229C tile size, +0x29B0 grid), which runs from the view ctor 0x3CC5AA0, from 0x3CCCC20 and from view slot 6 0x3CC86D0. Fog sub-job 0x3E3CCE0 dispatches `ceil(viewport / tile)`. 0x3E42130 has a FogQuality == 3 path | Write `[0x604AA10]+0x4A` before the eye view is created, or use the menu (the key "FogQuality" is in the file loader) | Low (softer haze) | Tile mapping Confirmed; when a runtime change takes effect Unconfirmed |
| 4 | **Lens flare, god rays, bloom toggles** | Each is a chain of full-screen passes per eye at render resolution. Lens flare and god rays add little to the image in VR. Bloom is part of the look, so it is a trade-off | Settings **+0x59 LensFlareEnabled** (prepare 0x3EE1CF0), **+0x58 GodRaysEnabled** (prepare 0x3ED0B40; also needs the lighting flag +0x25D and the sun in front), **+0x4C BloomEnabled** (prepare 0x3EEB4B0; also needs the lighting flag +0x25C) | Write the byte to 0. All three prepares read it every frame. The same names work in graphicSettings.lsx | Very low (look only) | Confirmed |
| 5 | **UI rendered at the panel size, not at 3572x1834** | Noesis lays out and draws the UI at the frame's output size (`frame+0x4328/+0x432C`) on every resize (F 1.4). The UI is shown only on a VR panel, so a 1600x1200 UI renders 3.4x fewer pixels, with less CPU tessellation too | GameUI vf[0x60] **0x120E0E0** SetSize(w,h). Called by 0x585510 after every frame resize | In the 0x585510 detour, after the original returns, call SetSize(panel w, h) (O's plan; resolution.cpp doesn't do this yet) | Medium: gpu_trace's UI capture and the composite 0x3E4D2A0 assume a frame-sized UI texture (F open lead: whether only the top-left w x h is drawn) | Mechanism Confirmed; size effect Unconfirmed |
| 6 | SSAO off | One SSAO chain per eye at render resolution | Settings **+0x5A SSAOEnabled** (stage 0x37 prepare 0x3E4F430; the lighting consumers 0x3D987C0 and 0x3E35820 unbind 0x74EA5362) | Write 0 | Low-medium (flatter contact shading) | Confirmed |
| 7 | **NVIDIA VRS (fixed foveation) on the G-buffer and forward passes** | Pixel-shader work in the lens periphery drops by up to 4x (2x2) there. It only helps pixel-shader passes. The main deferred lighting (0x3E35C80) is compute (DispatchIndirect), so VRS can't reduce it | Bracket with **0x3CD4330** (stage wait+close; the stage's commands execute inside it, 4.2), filtered on the stage vtable: 0x0B **0x58C5CA8** (its slot 4 0x3E2BA30 executes both sub-buffers +0x2160 and +0x4280), 0x46 0x589D768, 0x00 0x58BE598, 0x36 0x58C6C10, 0x18 0x58C6F58, 0x3C 0x55F4AB0. Finer: the set-render-target handler **0x40CA2E0** (opcode 0x0B, called from the executor at 0x40C8BC0) | The mod calls NVAPI itself (the game doesn't use VRS, 4.1). One R8_UINT shading-rate image of ceil(W/16) x ceil(H/16) over the frame render target, with one foveal centre in each half, covers both eyes at once. Enable it only while the four G-buffer MRTs (0x4C0D1B61, 0x3B0A2BF7, 0xA2037A4D, 0xD5044ADB) or the forward target are bound. Not while LinearizeDepth / ExtractNormal run (they are in stage 0x0B too) | Medium-high: new third-party SDK (NVAPI headers; user approval needed); coarse G-buffer edges; interaction with DLSS jitter | Hook points Confirmed; NVAPI behaviour Unconfirmed |
| 8 | Hidden-area mask (lens visibility mask) primed into depth before stage 0x0B | Quest 2 lens masks hide part of each eye image; every opaque, decal and forward pixel there is wasted. Lighting tiles over masked pixels see no geometry | Same bracket as #7: after the scene depth 0x22F3E110 is cleared by the first set-RT of stage 0x0B (0x40CA2E0) | Draw the XR_KHR_visibility_mask mesh per eye half at the near depth into 0x22F3E110 | Medium-high (depth convention, DLSS sees the hard edge) | Hook Confirmed; runtime availability of the mask on Quest 2 via SteamVR Unconfirmed |
| 9 | Finer render scale than the DLSS modes | Any ratio, e.g. 0.6 per axis | Hook **0x406FD20** (render size from output size; only called on resize by 0x40729D0 and at frame creation) and return output x k. Alternative: render-area scale `frame+0x4358` (0x4073520 scales the render rect by sqrt) | Hook returns k x output while DLSS is on. Don't write +0x4358 directly: dynamic resolution 0x4073C50 resets it to 1.0 every frame on DX11 (vf+0x50 = 0x579DA0 returns false) | Medium: NGX must accept the ratio (the feature is created with the view's own render/output viewports, 0x3EF3920) | Code Confirmed; NGX acceptance Unconfirmed |
| 10 | Lower `[render] vr_resolution` (already in the mod) | Pixel count scales with percent^2: 60 % = 3.69 MP (56 % of today) | resolution.cpp | Config only. headset.cpp's blitter scales each half to the XR image | None, but blurrier | Confirmed (mod code) |

Not recommended:
* **FSR2** (+0x77 = 2): BG3's FSR2 is the engine's own implementation (shaders `FSR2_*.bshd`), and its history, lock
  and luma resources are fixed stage-manager keys (e.g. 0xF0D0353F, 0x8E2A9722 in 0x3EF8C00 / 0x3EFCB70). Keys are
  shared by every view of the frame (A 7), so both eyes would use one set of FSR2 history textures. Whether each eye
  only touches its own sub-rectangle was not traced (Unconfirmed). DLSS has no such issue.
* **FSR1** (+0x77 = 1, mode +0x78 = 3 -> /2.0): spatial only, no history, per-view step 0x58D5970 (prepare
  0x3EFD550). Safe per eye. It's the fallback for a non-NVIDIA GPU, at lower quality than DLSS.
* **CAS** only runs without an upscaler (prepare 0x3F02B60: `CASEnabled && +0x77 < 1`). It's off in the user's file.

## 1. Upscaling per view

### 1.1 Where the internal size is decided (Confirmed, O section 3 + x2)

The render size is decided **once per frame**, not per view: 0x40729D0 sets `frame+0x20/+0x24 =
0x406FD20(output size)`, and each view's viewports are the frame rects times its Scale/Offset (0x407B8C0). With the
mod's layout (Scale 0.5 x 1) every eye gets exactly half of the render, upscaled and display rects. 0x406FD20 divides
by fixed factors for FSR1/FSR2 and asks NGX `DLSSOptimalSettingsCallback` for DLSS (0x40CD860; mode byte -> NGX
PerfQualityValue: 1 -> UltraQuality, 2 -> MaxQuality, 3 -> Balanced, 4 -> MaxPerformance, 5 -> UltraPerformance,
6 -> native). The call covers the whole 3572-wide frame, so each eye gets half of NGX's answer.

### 1.2 Today's settings run DLAA per eye (Confirmed code; file read)

* The deployed `graphicSettings.lsx` has AntiAliasing 3, no ResolutionUpscaleType (type 0) and CASEnabled 0.
* 0x406FD20 with type 0: render = output.
* DLSS step prepare 0x3EF3790: the feature is (re)created when `+0x77 == 3 || +0x80 == 3`, with `step+0x82 = (AA == 3)`.
  0x3EF3920 then creates it with quality 2 (NGX MaxQuality) instead of the DLSS mode. That is DLAA: DLSS at a 1:1
  ratio, evaluated once per eye view.
* TAA (step 0x58D46C0, prepare 0x3EE9720) runs only for AA 2 or in an offscreen frame with a temporal upscaler. It
  doesn't run in the main frame with DLAA.
* Jitter (0x3F11710) is on for AA 2/3/4 and upscaler types 2/3/5.

### 1.3 Per-eye histories (Confirmed)

* Post-process stage init 0x3B21E10 (x2) builds the step list per view, including the eye view created through
  0x588E50 -> 0x3AC4EA0 (which lists stage 0x21). The DLSS step (vtable 0x58D4E30), the FSR2 step (ctor 0x3EF3E40)
  and the FSR1 step (0x58D5970) are added when the view's frame type `[[view+0x10]+0x40] == 0`, which is true for
  both eye views (same main frame). XeSS (0x58D5A40) is added only when `[[0x604AAE8]+0x1F8]+0x1B0` is set.
* DLSS: the NGX handle lives at `step+0x78`, one per view. It is created from that view's ReadState render
  viewport (+0x2180) and upscaled viewport (+0x21A0), and evaluated with the subrect bases +0x2188 and +0x21A8
  (O r25/r26). **History is internal to each NGX feature, so the eyes' histories can't mix.** The inputs (colour,
  depth, velocity 0xC602F0F9) are frame-sized shared keys; each eye reads only its sub-rectangle.
* Motion vectors: stage 0x40 per view. The mod already feeds it the eye camera (0x3E12730 hook, N). The mod's
  projection rewrite keeps the game's jitter terms (`eye_projection(..., projection[8], projection[9])` in
  stereo.cpp). An offset added to P[8]/P[9] is a pixel shift independent of P[0], so the jitter stays pixel-correct
  with the eye's wider FOV.
* TAA (AA 2) also keeps per-step, per-view state: the previous viewport's UV bounds at step+0x98..+0xA4 and history
  validity at +0xA8, cleared when the controller pointer or its cut flag +0x12 changes (0x3EE9720, 0x3EE9880, x18).
  Its history texture is a shared key; each view reads it clamped to its own UV rectangle. Usable, but DLSS is better.
* Split screen: the game's own split-screen view runs this same per-view code (O 3, B), so two views with
  independent DLSS features are an engine-supported setup.

### 1.4 What the mod must do for DLSS per eye

1. Set DLSS and the mode (table #1). The game's setter 0x406FC70 writes `+0x77 = 3, +0x78 = 4, +0x79 = 5,
   +0x7A = mode, +0x7B = 6, +0x80 = 0, +0x81 = 0`, and the sharpness +0x7C = 0.35 if the mode changed.
2. Re-apply the size through 0x585510 (resolution.cpp's pending path). That refreshes the DLSS mode table
   0x40CD530, the mip bias 0x40765C0 (now negative, from the mode), the render size 0x40729D0, all frame-sized
   targets and the view viewports. On the next prepare each eye's step sees the new viewports and recreates its
   feature (0x3EF3790).
3. Keep the eye views' Scale/Offset at (0.5,1)/(0 or 0.5, 0). Each eye gets render (W/2)*r x H*r and output W/2 x H.
4. NGX availability is `[[0x604AAE8]+0x1F8]+0x1A4`. If DLSS were unavailable, 0x3AC3180 would fall back at
   start-up; log it.
5. Order in the chain: steps before the DLSS step (god rays, DOF, exposure, lens flare, bloom, chromatic aberration,
   LUT/grading) run at render resolution. Tone map, vignette and HDR calibration run after it, at output resolution.
   The game's outline output (0x17FCD00) is inserted after step id 0x23.

## 2. Render scale and dynamic resolution (Confirmed)

* Per view: WriteState Scale/Offset (+0x21E8/+0x21F0) set the size of all three of that view's viewports
  together (render, upscaled, display; 0x407B8C0). Lowering Scale shrinks the eye's output in the back buffer too,
  which is the same as lowering vr_resolution. It isn't a separate render-only scale.
* Per frame: the render-area scale `frame+0x4358` scales the render rect by sqrt(scale) (0x4073520), but dynamic
  resolution 0x4073C50 rewrites it every frame from `frame vf+0x50` GPU timings, clamped to [0.7, 1]. On DX11 that
  slot is 0x579DA0 (returns false), so it is forced back to 1.0 each frame. Only usable by hooking 0x4073C50 or
  0x406FD20 (#9).
* No "RenderScale" graphics option exists. The two "RenderScale" strings (0x5586750, 0x5589F08) are UI view-model
  property names (referenced from 0x13FC2E0 / 0x155E6E0 / 0x13FE8C0, next to ScreenResolution and AspectRatio).
  "SSREnabled" (0x5899A80) is registered as a FixedString by 0x4E1650, but no name global is stored for it, so the
  options loader can't read it: dead option (as H found for TexelDensity*).

## 3. Expensive effects: switches (Confirmed unless marked)

Post-process steps of stage 0x21, in list order (0x3B21E10). Names come from the shader name each step's slot +0x78
loads (x15), which corrects some of A's labels. "Prepare" is slot +0x70, read every frame.

| step vtable | what | prepare | gate | per-eye cost / note |
|---|---|---|---|---|
| 0x58D3100 | split-screen border clear (ClearRTGraphics) | 0x3ECFD60 | runs when the view's upscaled viewport is < 1.0 in x or y (true for both eyes) | job 0x3ECFD90 draws only the bands above and below the view; for full-height eye views both are empty, so no draws |
| 0x58D3270 | god rays (PPGodRaysBlur) | 0x3ED0B40 | settings +0x58 GodRaysEnabled, lighting `[view+0x28D8]+0x25D`, sun facing the camera | render res |
| 0x58D36F0 | depth of field (PPDOF*, half/quarter variants) | 0x3ED3290 | settings +0x50 "DOF" != 0 and the camera/lighting DOF block enabled (0x3D953B0) | render res; mostly dialogues |
| 0x58D3820 | FadeBlending | 0x176D530 (always) | - | not traced |
| 0x58D3928 / 0x58D3C78 | exposure (new / old path) | 0x3EDE110 / 0x3EDFC00 | settings +0x75 picks the path | second eye already skipped (eyefix.cpp) |
| 0x58D3D90 | re-expose | 0x3EE0C80 | follows the exposure steps | |
| 0x58D4068 | lens flare | 0x3EE1CF0 | settings +0x59 LensFlareEnabled and lighting +0x94 | render res |
| 0x58D41A8 | darken environment | 0x176D530 (always) | - | not traced |
| 0x58D42B0 | radial blur | 0x176D530 (always) | job 0x3EE6070 works from gameplay effect volumes of type 2 (CullStage +0x21C8) | effect-driven |
| 0x58D4418 | SMAA | 0x3EE7160 | AA == 1 | off |
| 0x58D46C0 | TAA | 0x3EE9720 | AA == 2 (main frame) | off with DLAA/DLSS |
| 0x58D47B8 | TAA companion | 0x3EEAF40 | same as TAA | off |
| 0x58D4940 | bloom (PPBloom*) | 0x3EEB4B0 | settings +0x4C BloomEnabled and lighting +0x25C | render res |
| 0x58D4A38 | chromatic aberration | 0x176D530 (always) | job 0x3EEFD20 draws only if a type-3 effect volume has intensity > 1e-7; otherwise it only aliases input to output | ~free normally |
| 0x58D4C08 / 0x58D4D38 | LUT build / grading | | | |
| 0x58D4E30 | DLSS / DLAA | 0x3EF3490 | 1.2 | per eye |
| 0x58D5018 | FSR2 | 0x3EF4140 | +0x77 == 2 | shared history keys |
| 0x58D5970 | FSR1 | 0x3EFD550 | +0x77 == 1 | |
| 0x58D5A40 | XeSS | 0x3EFF340 | | |
| 0x58D5D10 | (+0x6C gated) | 0x3EFFD80 | settings +0x6C | |
| 0x58D5FC0 | tone map | 0x176D530 (always) | | output res |
| 0x58D60C8 | CAS | 0x3F02B60 | +0x81 CASEnabled and no upscaler | off |
| 0x58D61E8 | vignette (PPVignette) | 0x3F03510 | lighting `[view+0x28D8]+0x25E` (no user option) | output res, one pass |
| 0x58D6380 | HDR calibration | 0x3F040E0 | settings +0x6D | |

* No motion blur or film-grain step exists (no such step in 0x3B21E10, no such shader names in the exe's strings).
* Stage-level effects: SSAO stage 0x37 (prepare 0x3E4F430: +0x5A SSAOEnabled and atmosphere +0x18C).
  Volumetric fog: tile size by +0x4A FogQuality (0x3CC85D0). Clouds stage 0x44: CloudsQuality +0x49 picks
  `VolumetricClouds_Q<n>.bshd` (0x3E47840, reloaded when it changes). SSSS +0x4B goes into the frame CB per frame
  (0x4073700 at frame creation; A: 0x4072CB0 per frame). Shadows belong to agent T.
* Settings offsets were read from the options save function 0x3ABFB90 (H dec_gfx_io.c): +0x4A FogQuality,
  +0x49 CloudsQuality, +0x4B SSSS, +0x4C BloomEnabled, +0x50 DOF, +0x54 DOFQuality, +0x58 GodRaysEnabled,
  +0x59 LensFlareEnabled, +0x5A SSAOEnabled, +0x81 CASEnabled. "DOF" and "DOFQuality" are inline FixedStrings at
  0x5EF2658 / 0x5EF265C. Their names come from the name table built at 0x3BB7789..0x3BB780B, whose strings at
  0x58A6270 / 0x58A6278 read "DOF" and "DOFQuality".
* Cleanest override for every toggle: write the settings byte. All the prepares above read `[0x604AA10]` every
  frame, so a plain store works with no patch. The menu writes the same fields. FogQuality is read when the view's
  tile grid is rebuilt, so set it before the eye view is created.

## 4. Variable rate shading and pass hook points

### 4.1 The game doesn't use VRS (Confirmed as far as direct calls go)

* D3D11 has no core VRS. The exe links the NVAPI static library: the query loader 0xAB50 resolves
  `nvapi_QueryInterface` and stores it at 0x5F728E8. There are 262 wrapper functions calling through it (x16).
  **Only 5 wrappers have direct callers in game code:**
  * ID 0xD22BDD7E, from the device teardown at 0x4071A45;
  * IDs 0x07F9B368 and 0xE5AC921F, from 0x4361Dxx;
  * IDs 0x65B93CA8 and 0x2216A357, from the command executor at 0x40C8FD4 / 0x40C900B, the same place as the UAV
    overlap opcodes 0x25/0x26 (A 5.2).
* None of the five is a VRS call. Their names weren't checked against the NVAPI header, so they are Unconfirmed.
  AMD AGS exposes only UAV overlap, device creation and clip rects here.
* So VRS has to come from the mod: load nvapi64.dll (installed with the driver), get `nvapi_QueryInterface`, and
  use the D3D11 VRS entry points from NVIDIA's NVAPI SDK header (create a shading-rate resource view, set it, set the
  per-viewport rates). The header is a new third-party component: per docs/third-party.md it needs the user's OK
  and a checked download.

### 4.2 Where to switch it on and off (Confirmed)

* Recording and execution are separate. Stage jobs record opcodes on worker threads. A view's end-recording
  0x3CC7010 calls each stage's slot 8. For scene stages that slot is 0x3CD4330: wait for the stage job, then slot 4,
  which runs the command stream through 0x407F240 -> executor 0x40C8A10 on the buffer's context (`[cmd+0x48]`, the
  device's context, A 1.5). **So a detour on 0x3CD4330 that checks `*(void**)stage` sees exactly the window in which
  that stage's draws reach D3D11**, on the render thread, view by view (main eye, then the second eye).
* Stage 0x0B's slot 4 is 0x3E2BA30, which executes two buffers, +0x2160 (depth-first) and +0x4280 (opaque). Stage
  0x0B also records LinearizeDepth (0x3E2C960) and DeferredDecal_ExtractNormal (0x3E2CC60) full-screen passes (S).
  Coarse shading there would corrupt linear depth and normals at the periphery. So use the finer hook: the
  set-render-target opcode handler **0x40CA2E0** (single caller, the executor at 0x40C8BC0). Enable VRS when the four
  G-buffer MRTs are bound and disable it on any other set.
* Pixel-shader passes worth VRS: G-buffer 0x0B and 0x46 (decals, 0x3B surfaces), forward/transparent 0x00, sky 0x36,
  emissive 0x18 / 0x17 / 0x3C. Not useful: clustered lighting 0x1B. Its main sub-job 0x3E35C80 shades with
  DispatchIndirect (opcode 0x2B, one dispatch per classified tile list), which is compute, not pixel shading. Post
  steps are also full-screen draws, but coarse post shading looks bad.
* Both eyes share one render target, side by side, so a single shading-rate image with a foveal circle in each half
  serves both eye views without switching.

## 5. Passes at the whole back-buffer size

* Per-view post steps draw into shared frame-sized targets, but only inside their own viewport. The split-screen
  border clear does nothing for full-height views (3, first row).
* The clustered lighting tile grid (+0x29A8) and the fog froxel grid (+0x29B0) are sized from the frame render
  size (0x3CC9190 stores the frame size at view+0x2290). But the fog dispatch in 0x3E3CCE0 covers only the view's
  own viewport divided by the tile size, plus the volume depth. So the frame-wide grid costs memory, not work per
  eye (Confirmed for this dispatch; the other 0x1B dispatches were not checked one by one).
* Full back buffer, once per frame (special view, A 10): clear 0x19, DownsampleResolver 0x45 (prepare 0x3E4A670;
  whether it skips when display == output is Unconfirmed), and the UI composite 0x07 (0x3E4D2A0, display viewport =
  the whole back buffer, one full-screen triangle). The composite is needed, since it produces the image the mod
  copies.
* **The UI itself is laid out and drawn at the full output size** (#5). That is the one clear case of work sized by
  the frame that VR doesn't need.
* Exposure is computed once (the second eye's exposure job is skipped by eyefix.cpp).

## 6. Function and data index

| rva | what |
|---|---|
| 0x3B21E10 | post-process stage init, builds the per-view step list |
| 0x3B237E0 / 0x3B23890 | post stage prepare (calls every step's +0x70) / execute |
| 0x3ECFD60, 0x3ED0B40, 0x3ED3290, 0x3EDE110, 0x3EDFC00, 0x3EE0C80, 0x3EE1CF0, 0x3EE7160, 0x3EE9720, 0x3EEAF40, 0x3EEB4B0, 0x3EF0930, 0x3EF3490, 0x3EF4140, 0x3EFD550, 0x3EFF340, 0x3EFFD80, 0x3F02B60, 0x3F03510, 0x3F040E0 | step prepares (section 3) |
| 0x3EF3790 / 0x3EF3920 / 0x3EF34B0 | DLSS step prepare / create / evaluate |
| 0x3EF3E40 / 0x3EF40C0 / 0x3EF7460 | FSR2 step ctor / init / resource set-up (keys 0xF0D0353F, 0x8E2A9722, ...) |
| 0x406FD20, 0x40CD860, 0x406FB00/0x406FBC0/0x406FC40/0x406FC70 | render size, NGX optimal size, upscaler setters (O) |
| 0x4073C50, 0x4073520 | dynamic resolution, render rect with area scale |
| 0x3CC85D0 | fog froxel tile size from FogQuality |
| 0x3E4F430 | SSAO prepare |
| 0x3E47840 | clouds shader quality |
| 0x3CD4330, 0x3E2BA30, 0x407F240, 0x40C8A10, 0x40CA2E0 | stage close (execution window), 0x0B close, command execution, executor, set-RT handler |
| 0xAB50, 0x5F728E8 | NVAPI query loader and pointer |
| 0x3E35C80 | deferred lighting sub-job (DispatchIndirect) |

## 7. One-run measurement plan

Log everything in one run, as the user prefers. Start-up and every 300 frames:

1. **Settings snapshot**: `[0x604AA10]` +0x77..+0x7C, +0x80, +0x81, +0x49, +0x4A, +0x4B, +0x4C, +0x50, +0x54,
   +0x58, +0x59, +0x5A. Also NGX available `[[0x604AAE8]+0x1F8]+0x1A4`. Frame +0x20/+0x24 (render), +0x28/+0x2C
   (output), +0x4358.
2. **Per eye view**: ReadState +0x2180/+0x2184 and +0x21A0/+0x21A4. Find the DLSS step in stage 0x21's list
   (+0x2150, count +0x215C) by vtable exe+0x58D4E30 and log +0x78 (NGX handle, must differ between the eyes), +0x82
   (DLAA flag), +0x84..+0x90 (cached sizes) and +0x95 (ok).
3. **GPU time per stage**: in a detour on 0x3CD4330 (and 0x3E33E80 for stage 0x1B), wrap the original in D3D11
   timestamp queries on the immediate context, keyed by (view, stage vtable). Do the same on the post stage per step
   if possible. Read back two frames later. This gives the real share of each pass in the 38 ms, and how much of it
   is pixel-bound, which the estimates above lack. Log the top 15 per eye every 300 frames.
4. **A/B keys** (one run, a hotkey each, timings logged around each switch). Each toggle is a settings write
   followed by a resize through the pending path where needed:
   * a. vr_resolution 80 -> 57 % (half the pixels): measures the pixel-bound fraction.
   * b. DLAA -> DLSS Performance -> DLSS Balanced.
   * c. DOF 2 -> 0 (in a dialogue).
   * d. FogQuality 0 -> 2 (needs view re-creation or a resize; log +0x229C and +0x29B0 per eye before and after).
   * e. LensFlare, GodRays, Bloom, SSAO off.
   * f. UI SetSize to 1600x1200.
   Expected: (a) gives about 1/(1 - 0.5 x pixel share) speed-up. (b) cuts every pre-upscale stage's GPU time to
   about 25-35 % and leaves the DLSS step's time about the same.
5. **Image checks** with DLSS on: look for ghosting at depth edges and whether the eyes disagree (a stereo DLSS
   artefact). Log the stage 0x40 hook counter (g_velocity_rewrites) and the jitter offsets from PerCamera[1] +0x60
   (P[8], P[9]) per eye. They must be equal for both eyes in a frame.

## Raw files

x1/x3/x5 step vtables, x2 post stage init/prepare/execute, x4 step ctors, x6 step prepares, x7 string refs, x8
option-name initialisers, x9 "DOF" globals refs, x11 border clear / stage 0x40 prepare / frame CB / fog tile / clouds /
SSAO prepares, x12/x14/x16 NVAPI loader, call sites and wrapper IDs with callers, x13/x19 FSR2, x15 shader-name refs,
x17 chromatic aberration / radial blur / vignette / FSR2 set-up jobs, x18 TAA and bloom jobs, x20/x22 stage close
path, x21 scene stage vtables, x23 callers.

## Open leads

* Names of the five called NVAPI IDs (check against the SDK's nvapi_interface.h).
* Whether NGX accepts arbitrary render/output ratios for #9 (feature creation is per view at whatever the viewports
  are).
* Whether DownsampleResolver 0x45 runs when display == output (prepare 0x3E4A670 depends on `+0x4288` set at init
  0x3E4A4C0).
* Whether FogQuality changes take effect without re-creating the view (who calls view slot 6 0x3CC86D0 and when).
* Where the other stage 0x1B dispatches size their grids (only the fog dispatch was checked).
* Whether the UI composite and the mod's UI capture still work after SetSize to a smaller size (F open lead).
