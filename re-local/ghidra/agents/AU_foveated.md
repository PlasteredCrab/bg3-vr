# Fixed foveated rendering for the two eye views - agent AU, 2026-10-23

Static analysis only. Game: bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_F, read-only, -noanalysis, request server
`agents\AU\AUServer.java`, stopped), plus `bg3vr_disasm`. Driver file: `C:\Windows\System32\nvapi64.dll` 32.0.15.8216,
read in place (exports, id tables, five functions disassembled; never copied, loaded or run). RVAs are relative to the
image base. Raw output is in `re-local\ghidra\agents\AU\`. Builds on X (pixel cost, VRS hook points), O (render size,
DLSS per eye), A (stages, opcodes), T, W, AI/AK (fog), and the mod's source.

## Recommendation

| # | what | expected gain | where | how | risk | confidence |
|---|------|---------------|-------|-----|------|------------|
| 1 | **NVIDIA VRS through NVAPI, phase 1: the G-buffer fill** (stage 0x0B opaque draws and stage 0x46 decals / game objects) | Pixel-shader invocations in those passes drop to about 43 % (zones 25° / 35°, see 3.3). The pass time drops less, because vertex work, G-buffer writes and depth stay per pixel. The share of the frame is unknown (section 1) | Stage window: detour **0x3CD4330** (stage wait + execute), filtered by stage vtable 0x58C5CA8 (0x0B) and 0x589D768 (0x46). Per bind: the existing gpu_trace hooks on `OMSetRenderTargets` / `OMSetRenderTargetsAndUAVs` / `RSSetViewports` | One R8_UINT shading-rate texture over the whole render target, one foveal centre per eye half. VRS on only for binds with 4 RTVs plus a DSV inside those windows. Off everywhere else | Medium: coarse material detail in the periphery; DLSS sees it | NVAPI calls and struct layouts Confirmed in nvapi64.dll; hook points Confirmed; gain Unconfirmed |
| 2 | Phase 2: forward 0x00, emissive 0x18 / 0x17 / 0x3C, sky 0x36 | Same per-pass ratio on those passes | Same detour, vtables 0x58BE598, 0x58C6F58, 0x58C7030, 0x55F4AB0, 0x58C6C10 | VRS on for any bind in those windows whose RT0 is exactly the render size | Low-medium (transparent effects coarser at the edges) | as #1 |
| 3 | Phase 3 (after an image check): sun-shadow resolve 0x03 (PCSS, full-screen PS) and the DeferredFog apply draw (0x3E42130, inside 0x1B) | Unknown; PCSS resolve is per-pixel filtering, so likely worth it | vtable 0x58C5048 (slot 8 0x3CD4330); 0x1B: detour **0x3E33E80** (0x1B's own slot 8), vtable 0x58C6038 | Same size rule. Dispatches in 0x1B are compute and are not affected | Medium (coarse shadow edges, fog banding at the edges) | Hooks Confirmed; image effect Unconfirmed |
| 4 | Hidden-area tiles at 4x4 (later: cull) | Small; only tiles wholly outside the lens view | `XR_KHR_visibility_mask` from the runtime | Rasterise the mask into the tile grid; fully hidden tiles get the coarsest rate | Low at 4x4; medium with cull | Runtime support Unconfirmed |

Never VRS: velocity 0x40 (DLSS input), LinearizeDepth and DeferredDecal_ExtractNormal (inside 0x0B, data passes), SSAO
0x37, clouds 0x44, outline / room masks 0x1F / 0x33, shadow stages 0x02 / 0x1E (other render-target size), post 0x21
including DLSS, the special view (UI 0x19 / 0x45 / 0x07), and the mod's own blit. Compute passes are not affected by
VRS at all, so the clustered lighting (0x3E35C80, DispatchIndirect) and the fog froxel passes gain nothing.

Rejected: radial density masking (all 8 stencil bits are in use, and in a deferred renderer it saves only the
G-buffer pixel work, which VRS already saves; 2.2). Multi-resolution or lens-matched shading (needs the game's vertex
shaders changed, and DLSS, the froxel fog and the light clusters need a linear projection; 2.3). The engine has no
VRS or foveation support (2.4).

## 1. Where the GPU time goes: what is known

* No per-pass GPU timing exists for this game. X, T and W all list it as unmeasured. The current
  `bin\bg3vr\bg3vr.log` (2026-10-23 17:01) is a mono session; its only timing is the XR frame interval
  (5-second averages from 29 to 74 ms). The only stereo number is X's: 26 fps stereo against 49 fps flat, with DLAA.
* The mod now runs DLSS Balanced by default (`resolution.cpp`, `g_dlss_mode` 3). Every pass before the upscaler runs at
  about 58 % per axis: about 1036 px wide per eye for a 1786-px eye (AI logged the eye viewport at render x = 1036).
  Height about 1064 (Unconfirmed; logged by section 5).
* Which passes VRS can reduce (pass types from A, AK, T, X; Confirmed unless marked):

| pass | type | VRS |
|------|------|-----|
| 0x0B opaque G-buffer (4 MRT + depth) | pixel shader | yes (phase 1) |
| 0x0B LinearizeDepth, ExtractNormal | full-screen PS, data | no |
| 0x46 decals, game objects (same 4 MRT) | PS | yes (phase 1) |
| 0x00 forward / transparent | PS | yes (phase 2) |
| 0x36 sky, 0x18 / 0x17 / 0x3C emissive | PS | yes (phase 2) |
| 0x03 sun-shadow resolve | full-screen PS (AK: "the 0x03 resolve draw") | phase 3 |
| 0x1B DeferredFog apply 0x3E42130 | PS draw (AK G3) | phase 3 |
| 0x1B lighting 0x3E35C80, froxel fog | compute | not affected |
| 0x37 SSAO, 0x40 velocity, 0x21 post, DLSS | PS / compute | no |

So VRS reaches the G-buffer, forward, emissive and sky passes, and optionally the shadow resolve and fog apply. It
cannot reach the lighting compute. How big that group is in the frame is the first thing section 5 measures.

Estimate, not a measurement: if the phase 1+2 passes take 20 to 35 % of the GPU frame at DLSS Balanced, and 60 to 80 %
of their time scales with pixel-shader work, then 57 % fewer invocations saves about 7 to 16 % of GPU frame time. It
saves nothing if the frame is CPU-bound on the render thread (W 3.2).

## 2. Options compared

### 2.1 (a) NVIDIA VRS through NVAPI - recommended

**The game doesn't use it** (Confirmed): none of the VRS function ids occur anywhere in the exe (byte search, `au13`),
the 262 NVAPI wrappers X found don't include them (X x16), and there are no VRS / foveation strings (`au5`).

**nvapi64.dll can be used without the SDK** (Confirmed except the names, see below). It exports only
`nvapi_QueryInterface` (RVA 0x102E00) and `nvapi_Direct_GetMethod` (`nv_exports.txt`). Its `.data` holds the dispatch
table as {function pointer, id} pairs. The VRS entries sit together at file offset 0x4E6560..0x4E65D8 (a second copy
of the table is at 0x4F04F0..0x4F0568; `nv_tables.txt`):

| id | function (name from NVIDIA's public nvapi_interface.h) | implementation RVA | args (Confirmed from the code) | struct check (Confirmed) |
|----|------|------|------|------|
| 0x0150E828 | NvAPI_Initialize | - | () | - |
| 0x52B1499A | NvAPI_D3D1x_GetGraphicsCapabilities | 0x2DC100 | (device, u32 version, caps*) | version 0x10020 (32 bytes) or 0x20040 (64 bytes), 0x2DC20D |
| 0x34F7938F | NvAPI_D3D11_RSSetViewportsPixelShadingRates | 0x30EE30 | (context, desc*) | desc version 0x10010 (0x30EF45); numViewports <= 16 (0x30EFC0); per-viewport stride 0x44: byte enable at +0, 16 x u32 rate table at +4 (copy loop 0x30F080) |
| 0x99CA2DFF | NvAPI_D3D11_CreateShadingRateResourceView | 0x30DA70 | (device, resource, desc*, out**) | desc version 0x10018 (0x30DB8E); Format at +4 must be 60 or 62 (R8_TYPELESS / R8_UINT, 0x30DC40); ViewDimension at +8: 4 = Texture2D (MipSlice +0xC) or 5 = Texture2DArray (+0xC, +0x10, +0x14) (0x30DDA2) |
| 0x1B0C2F83 | NvAPI_D3D11_RSSetShadingRateResourceView | 0x30EB80 | (context, view) | a null view skips the view check (0x30ECF8), so null should unbind; check the return code |
| 0x092442A1 / 0xA942373A | RSGet / RSSetPixelShadingRateSampleOrder | 0x30DF90 / 0x30E300 | not needed | - |

How certain the names are: the ids are live entry points in the installed driver (Confirmed). The id-to-name binding
comes from NVIDIA's public header. It is backed by three checks: the same source gives Initialize 0x0150E828,
Unload 0xD22BDD7E and Begin/EndUAVOverlap 0x65B93CA8 / 0x2216A357, and the exe calls exactly those ids at the matching
places (loader 0xAB50, device teardown 0x4071A45, the UAV-overlap opcodes 0x40C8FD4 / 0x40C900B); the six VRS ids are
adjacent in the DLL table in the header's declaration order; and each function's argument use and struct version match
the documented signatures. **The SDK is not required.** Two things can't be read from nvapi64.dll, because the DLL
passes them through to the driver (escape at 0x30F122): the `NV_PIXEL_SHADING_RATE` values and the caps bit layout.
The start-up self-test in 3.2 measures both. If the user wants a second check, the source would be NVIDIA's `nvapi.h`
and `nvapi_interface.h` (github.com/NVIDIA/nvapi, MIT); that is a download and needs approval. It is optional.

What VRS changes: only how often the pixel shader runs. Depth, stencil and edges stay per pixel (that is how VRS is
specified in D3D12, and NVAPI drives the same Turing+ hardware; Unconfirmed for this game, check in 5). So DLSS gets
unchanged depth and motion vectors (0x40 stays off) and a colour input with coarser detail inside surfaces at the edges.
Whether DLSS's jittered accumulation hides that or shows shimmer is Unconfirmed (image check in 5).

Hook points (Confirmed): stage commands reach D3D11 only inside each stage's slot 8, on the render thread, view by view
(X 4.2, W 3.2). From the 17:03 job dump in the log: slot 8 is 0x3CD4330 for 0x0B (vtable 0x58C5CA8), 0x46 (0x589D768),
0x00 (0x58BE598), 0x03 (0x58C5048), 0x36 (0x58C6C10), 0x18 (0x58C6F58), 0x17 (0x58C7030), 0x3C (0x55F4AB0), 0x40
(0x58C4C28), 0x44; and 0x3E33E80 for 0x1B (0x58C6038). 0x3CD4330 waits for the stage job and then tail-jumps to the
stage's slot 4 (X x20), so a wrapping detour sees the whole window. Stage 0x0B's slot 4 0x3E2BA30 runs both sub-buffers:
depth-first (RT set `stage+0x63E0`, sub-job 0x3E2BD90) and opaque (RT set `+0x63E8`, sub-job 0x3E2C3B0, which clears
the G-buffer at its set-RT) (A dec/st_0B.c).

### 2.2 (b) Radial density masking - rejected

* Scene depth-stencil: key **0x22F3E110**, bound by stage 0x0B's init 0x3E2B3E0 (`0x40956C0(mgr, stage, 0x22F3E110,
  0x5230002, 0x5230002)`), resolved per frame in 0x3E2BA60 (0x40979D0) into the two RT sets. The set-RT opcode
  handler 0x40CA2E0 clears depth and stencil separately per load action: `ClearDepthStencilView` (context +0x1A8)
  with flags 1, 2 or 3 (`au1`). Confirmed.
* **No stencil bit is free.** The game creates depth-stencil states with stencil on (0x40BB750 is the only
  `CreateDepthStencilState` caller, device +0xA8; arguments read at every call site, `au12`, `dssargs.py`):

| creator | stencil read / write mask | use |
|---------|---------------------------|-----|
| 0x3E2B3E0 (stage 0x0B) | 0x1F / 0x1F (replace, always, depth off) and 0x40 / 0x40 | next to FadePattern / LinearizeDepth |
| 0x4076BB0 (global state table) | 0x0F read / 0x30 write on 10 depth-tested states; single bits 0x01, 0x02, 0x04, 0x08, 0x10; 0x40; 0xFF | geometry and mask passes |
| 0x3E3FCE0 (0x1B module, `PPSSSS_H/V`) | 0x80 / 0x80 | subsurface scattering |
| 0x1814C70, 0x2128E90 | 0xFF | game side / UI |

  Every bit appears. Which states touch 0x22F3E110 at runtime was not traced, so "free" can't be shown for any bit.
* The depth format: the engine's format enum maps to DXGI in **0x40903A0** (e.g. 0x54 -> 45 D24_UNORM_S8_UINT,
  0x55 -> 40 D32_FLOAT, 0x56 -> 20 D32_FLOAT_S8X24_UINT; `au7`). The format of 0x22F3E110 itself was not traced.
  Section 5 logs the DSV format.
* Reconstruction: lighting (0x1B), SSAO, fog, the PCSS resolve and DLSS all read the G-buffer and depth. A
  checkerboard hole would have to be filled in all four MRTs and in depth right after 0x46, before 0x1B. Lighting
  would then run at full rate anyway. So RDM saves only G-buffer pixel work, the same as VRS, with more passes and more
  artefacts.

### 2.3 (c) Lower resolution, multi-resolution, lens-matched shading - rejected

* Uniform lower resolution already exists: DLSS Balanced / Performance and `[render] vr_resolution` (X #1, #10). VRS
  stacks on top of it.
* Multi-res or lens-matched shading would need the game's vertex (or geometry) shaders changed for every material.
  DLSS needs a linear, jittered input. The froxel fog and light-cluster grids are computed from a linear screen mapping
  (AI: `cb9` tile math). Splitting each eye into extra views (inner and outer) doubles the draw submission on the render
  thread, which W shows is already a bottleneck.

### 2.4 (d) Engine support - none

No VRS, shading-rate or foveation strings in the exe (`au5`). The game calls only 5 NVAPI functions (X 4.1); none is
VRS. AMD AGS use is UAV overlap only.

## 3. Implementation plan

### 3.1 NVAPI access (new `native/core/src/foveation.cpp`)

1. `GetModuleHandleW(L"nvapi64.dll")` (the game's loader 0xAB50 already loaded it); else
   `LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)`. `GetProcAddress("nvapi_QueryInterface")`.
2. Call Initialize (id 0x0150E828) once. The game calls Unload (0xD22BDD7E) at device teardown 0x4071A45: release the
   mod's view and texture before that (on the mod's shutdown path). Whether Initialize is reference-counted is
   Unconfirmed; log both return codes.
3. Resolve 0x52B1499A, 0x34F7938F, 0x99CA2DFF, 0x1B0C2F83. Any null: health "Foveation: Unavailable".
4. Only on vendor 0x10DE (render.cpp already logs the adapter). Caps: call with version 0x10020 and a 32-byte buffer;
   log the raw dwords. The VRS bit is bit 1 of dword 0 per the header (Unconfirmed); the self-test decides.
5. The mod's own declarations (layouts Confirmed in 2.1; rate values from the header, checked by 3.2):

```cpp
struct NvViewportRates { std::uint8_t enable; std::uint8_t pad[3]; std::uint32_t table[16]; }; // 0x44
struct NvViewportsRates { std::uint32_t version = 0x10010; std::uint32_t count; NvViewportRates* viewports; };
struct NvSrrvDesc { std::uint32_t version = 0x10018; std::uint32_t format = 62; std::uint32_t dimension = 4;
                    std::uint32_t mip = 0, first = 0, size = 0; };                                    // 0x18
// NV_PIXEL_SHADING_RATE values: from the header, Unconfirmed until the self-test; expected list in 3.2
```

### 3.2 Start-up self-test (settles what the DLL can't show)

On the game's device, once, before enabling anything: a 192x32 R32G32_FLOAT target, a shading-rate texture of 12x2
tiles, table[i] = i (i = 0..11), tile column i uses index i. Draw a full-screen triangle whose pixel shader writes
`SV_Position.xy`, read it back (staging), and per tile column measure the block size of equal values. Expected per the
header: 0 cull, 1..5 supersampled (x16, x8, x4, x2, x1: 1x1 blocks), 6 = 2x1, 7 = 1x2, 8 = 2x2, 9 = 4x2, 10 = 2x4,
11 = 4x4. Then draw again with the viewport moved 8 px right: block edges staying on multiples of 16 means the
texture is indexed by render-target pixel, not by viewport. Log the map, the tile size and the indexing. If 2x2 or the
tile size isn't found, disable the feature. The mod already compiles shaders with D3DCompile (blit.cpp), so no new
component is needed.

### 3.3 The shading-rate texture

* Size: ceil(Wr / 16) x ceil(Hr / 16) of the render size `frame+0x20 / +0x24` (O; about 2072x1064, so 130x67 tiles).
  R8_UINT, Usage DEFAULT, BindFlags SHADER_RESOURCE, one mip. View: `NvSrrvDesc` with Texture2D. Rebuild it when the
  render size or either eye's FOV changes (resize path, DLSS mode change); fill with `UpdateSubresource` on the render
  thread.
* Eye rectangles: each eye view's ReadState render viewport, size `+0x2180` and offset `+0x2188` (O, Confirmed fields).
  Main view = left eye (offset 0), eye view = right eye (offset 0.5) (T, views.cpp).
* Foveal centre per eye: the FOV that `stereo.cpp` writes into that view's projection (`proxy->fov` or the eyes' FOV
  in `rewrite_block`; expose the last one per eye). With `l = tan(left)`, `r = tan(right)`, `u = tan(up)`,
  `d = tan(down)` (left and down negative, as in `eye_projection` and `headset.cpp eye_uv`):
  lens axis at `cx = x0 + w * (-l) / (r - l)`, `cy = y0 + h * u / (u - d)`. Each eye's centre is off the middle of its
  half because the FOV is asymmetric. That the view's forward axis is the lens's sharp centre is an assumption
  (Unconfirmed; check with the debug tint in the headset, section 5).
* Per tile: for each of its 4 corners inside an eye rectangle, the tangent
  `tx = l + (px - x0) / w * (r - l)`, `ty = u - (py - y0) / h * (u - d)`, angle `a = atan(hypot(tx, ty))`. Use the
  smallest angle (the finest rate). The tile column that spans both eyes (x 1024..1039 for a 1036 split) takes the
  finer of the two.
* Zones (ini, defaults): index 0 = 1x1 below 25°, index 1 = 2x1 from 25° to 35°, index 2 = 2x2 above 35°, index 3 =
  4x4 for tiles wholly inside the hidden-area mask (phase 4). On a symmetric 90° x 90° eye (an illustration; the real
  FOV is logged), the shares are 17 / 21 / 62 % of the pixels and the invocations 43 %. Other settings: 20° / 30° gives
  37 %, 30° / 40° gives 52 %.
* Hidden area (phase 4): enable `XR_KHR_visibility_mask` if `xr.cpp` finds it (log the extension list first), get the
  hidden triangle mesh per view, map its vertices with the same tangent formula, mark tiles fully covered.

### 3.4 When VRS is on (render thread only)

* Detour 0x3CD4330 and 0x3E33E80. On entry read the stage vtable `*(void**)stage` and the view `[stage+8]`. If the
  view is one of the two eye views, stereo is on, and the vtable is in the enabled phase's list, set a thread-local
  "window = stage id". Call the original. On return clear the window and turn VRS off if it is on. Check the vtables and
  the first bytes of both functions at install, as resolution.cpp does.
* In gpu_trace's `OMSetRenderTargets` and `OMSetRenderTargetsAndUAVs` hooks (already installed in VR by `watch_ui`),
  call a foveation callback before the original:
  * window 0x0B or 0x46: on if NumViews == 4 and a DSV is bound; that leaves out LinearizeDepth, ExtractNormal and the
    depth-only pass;
  * phase 2/3 windows: on if RT0's texture is exactly Wr x Hr (so no half-size or shadow target ever gets the image);
  * otherwise, or `D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL` outside a window: off.
  * on = `RSSetShadingRateResourceView(ctx, view)` + `RSSetViewportsPixelShadingRates(ctx, {count 1, enable, table})`;
    off = the same with enable 0 and a null view. Use the `self` context of the hook.
* In the `RSSetViewports` hook: while on, re-send the viewport rates after the original. Whether the driver keeps them
  across RSSetViewports is Unconfirmed; section 5 measures both ways.
* At Present (render.cpp `on_present`): force off, so the mod's blit and the next frame start clean.
* gpu_trace's `count_draw` takes `g_mutex` on every draw even when no trace is armed. Check `g_armed` first; foveation
  needs these hooks all the time.
* Config: `[render] foveation = off | gbuffer | scene | all`, `foveation_inner_deg = 25`, `foveation_outer_deg = 35`,
  `foveation_hidden_area = 0`, `foveation_debug = 0` (debug: a fourth table that shades zones 1..3 at 4x4 so the zones
  are obvious in the headset). Health line "Foveation".

### 3.5 Risks

* VRS state leaking into a pass with a different target size (shadow maps, half-size post): blocky corruption. Guarded
  by the window, the per-bind rule, the off at window exit and at Present.
* Driver drops the per-viewport state on RSSetViewports: no gain. Guarded by the re-send.
* DLSS: shimmer or crawling texture detail at the edges. Mitigation: milder zones, or DLSS Performance without VRS.
* Alpha-tested foliage and the FadePattern dither: `discard` drops a whole coarse pixel, so edges get blockier in the
  periphery.
* Coarse normals in the G-buffer make specular coarser at the edges.
* The eyes must use matching zones, or the periphery looks different in each eye. The construction is mirror-similar
  per eye's own FOV.
* Not on AMD / Intel; game updates move the vtables (checked at install).

## 4. Where everything is

| item | RVA / value | status |
|------|-------------|--------|
| stage slot 8 (wait + execute) | 0x3CD4330; 0x1B: 0x3E33E80 | Confirmed (X x20, log) |
| stage vtables | 0x0B 0x58C5CA8, 0x46 0x589D768, 0x00 0x58BE598, 0x36 0x58C6C10, 0x18 0x58C6F58, 0x17 0x58C7030, 0x3C 0x55F4AB0, 0x03 0x58C5048, 0x1B 0x58C6038, 0x40 0x58C4C28 | Confirmed (log job dump) |
| 0x0B slot 4, RT sets | 0x3E2BA30; `+0x63E0` depth-first, `+0x63E8` opaque (0x3E2BA60) | Confirmed |
| set-RT opcode handler | 0x40CA2E0 (clears, ClearDepthStencilView flags 1/2/3) | Confirmed |
| depth-stencil state creation | 0x40BB750 (device +0xA8); op map 0x40C2FC0, func map 0x40C38A0 | Confirmed |
| engine format -> DXGI | 0x40903A0 | Confirmed |
| view render viewport | ReadState `+0x2180` size, `+0x2188` offset | Confirmed (O) |
| NVAPI table in nvapi64.dll 32.0.15.8216 | .data file offset 0x4E6560..0x4E65D8 | Confirmed |

## 5. One-run measurement plan

Start-up lines (once):
1. NVAPI: module path and version, each resolved function pointer, Initialize return, caps raw dwords.
2. Self-test: rate value -> block size map, tile size, indexing (render target or viewport).
3. XR instance extensions (whether `XR_KHR_visibility_mask` is there).
4. Per eye: the FOV angles, the render rectangle, the foveal centre in pixels, tiles per zone, predicted invocation
   ratio.
5. One frame inside the 0x0B and 0x46 windows: every bind's RTV count, RTV formats and sizes, DSV format and whether
   it has stencil, and whether VRS was turned on. This settles the gating rule and the depth format.

Every 300 frames:
6. GPU time per (eye view, stage) from D3D11 timestamp queries placed in the two detours (around the original), plus
   0x3B23910 (stage 0x21 slot 8, post and DLSS), plus one disjoint query per frame. Read back 3 frames late. Log the
   top 15 per eye and the frame total.
7. VRS counters: turns on / off per view, re-sends after RSSetViewports, binds refused by the size rule, any NVAPI
   return code other than 0.
8. The existing XR frame-interval line and the render thread's busy time (W's plan item 1) to tell GPU-bound from
   CPU-bound.

A/B in the same run (VR menu or hotkey, 20 s each, standing still, each pair done twice as A B A B):
9. foveation off / gbuffer / scene / all. The change in each stage's GPU ms is the real gain per pass. A stage whose
   time doesn't move has VRS inactive (gating or driver state).
10. Viewport re-send on / off (phase 1). If times only drop with it on, keep it.
11. Zones 20/30, 25/35, 30/40.
12. DLSS Balanced and Performance, each with `scene`.
13. Debug tint on for one segment: the sharp zone must sit in the lens's sweet spot in each eye.
14. Eye-image screenshots with foveation off and on in the same pose: check the periphery for DLSS shimmer, foliage
    edges, fading objects and the seam column between the eyes.

How to read it: if the phase 1+2 stages are a small share of the GPU frame (from 6), the gain is small whatever the
zones; then DLSS Performance or the other X levers matter more. If the frame is CPU-bound (8), VRS won't raise fps.

## Raw files (agents\AU\)

au1 set-RT handler 0x40CA2E0; au2 CreateDepthStencilState search; au3 0x40BB750; au4 texture-creation search; au5 exe
strings (VRS, stencil, NVAPI); au6 CreateTexture2D wrapper 0x40B9830; au7 format map 0x40903A0 and 0x40BD720; au8 stage
0x0B init; au9 callers of 0x40BB750; au10 stencil op / compare maps; au11 the other state creators; au12 their
disassembly (read by `dssargs.py`); au13 VRS ids in the exe (none). nvapi64.dll: `nv_exports.txt`, `nv_ids_found.txt`,
`nv_tables.txt`, `nv_disasm_vrs.txt`, `nv_setvp.txt`, `nv_setvp2.txt`, `nv_createsrrv.txt`, `nv_setsrrv.txt`,
`nv_caps.txt`, `nv_2e28a0.txt`. Scripts: `nvids.py`, `nvtab.py`, `pexports.py`, `scan.py`, `strs.py`, `zipscan.py`.

## Open leads

* `NV_PIXEL_SHADING_RATE` values, tile size, indexing: the self-test (3.2).
* Whether RSSetViewports clears the per-viewport rates: item 10.
* The DXGI format of 0x22F3E110 and the RT bindings of the depth-first and FadePattern draws: item 5.
* `XR_KHR_visibility_mask` on SteamVR with Quest 2: item 3.
* The GPU share of each pass: item 6.
* Whether the forward axis of each eye is the lens's sharp centre: item 13.
