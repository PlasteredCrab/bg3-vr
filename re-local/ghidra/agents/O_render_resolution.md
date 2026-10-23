# BG3 render resolution (DX11, 4.1.1.7631656) - agent O report

Static analysis only (Ghidra project `BG3_F`, headless, read-only; persistent server `agents\O\OServer.java`).
RVAs are relative to 0x140000000. Raw decompiles/dumps are in `re-local\ghidra\agents\O\` (cited as [rNN]).
"Confirmed" = read from the decompile/disassembly cited. "Unconfirmed" items say what would confirm them.

Builds on: A_rendering.md (frame/view layout, RenderViewState, stage resource manager), B_splitscreen.md
(view Scale/Offset), F_ui.md (UI SetSize 0x120E0E0, cursor mapping), H_lod_streaming.md (settings object
`[0x604AA10]`, loader 0x3ABDE40 in `agents\H\dec_gfx_io.c`).

## Hook points for the VR mod

| need | RVA | how to use | confidence |
|------|-----|------------|------------|
| Resize the game's main frame (render target size + swapchain) | **0x4091CF0** = `rf::dx11::RenderFrame::Resize(frame, w, h, dispW, dispH)`, vtable **0x58EE488 slot 1** (+0x08) | `w,h` = the game's output ("upscaled") size, `dispW,dispH` = swapchain size. Writes frame+0x30/+0x34 = disp, calls 0x4091C70 (ResizeBuffers to disp), then 0x40729D0 (upscaled = w,h; render = upscaler(w,h); views relaid out). For VR call it with w = dispW = 2*eyeW, h = dispH = eyeH. `frame = *(view+0x10)` of any of our render views (A 3.1). | Confirmed [r6, r8] |
| The game's own resize entry (does everything the game does on a window resize) | **0x585510** = renderer `vf[0x118]` OnWindowResize(renderer, event*); renderer = `*(void**)(base+0x6046F98)` (vtable 0x5499F58) | event = `{+0 HWND, +8 dispW, +0xC dispH, +0x10 w, +0x14 h}` (ints). For the main frame (`renderer+0x98` == frame index) it refreshes the DLSS optimal-size table (0x40CD530), sampler LOD bias (0x40765C0), calls frame `vf[1]` (0x4091CF0), retitles the window (0x3AC7B80) and calls GameUI SetSize(frame+0x4328, +0x432C). **Recommended single call to apply a size.** | Confirmed [r9, r14, r18, r21, r27] |
| Make the mod's size sticky (game re-posts sizes on window events, options apply, alt-tab) | hook **0x585510** (entry) | When VR resolution is on and `event->hwnd` is the game window, overwrite the 4 sizes with the mod's (w = dispW = 2*eyeW, h = dispH = eyeH) and call the original. Every game-initiated resize of the main frame passes here (SDL size-changed 0x57AAA0, window SetSize 0x57F200, device SetDisplayMode 0x4071DD0). | Confirmed (all three posters found) [r20, r21, r22] |
| Belt-and-braces: swapchain size | **0x4091C70** (frame, bufferCount) | Calls `swapchain->ResizeBuffers(count, [frame+0x30], [frame+0x34], 0, [frame+0x43B0])` (swapchain = `[frame+0x43A8]`, vtable +0x68). Also reached directly from 0x40929F0 (fullscreen/mode switch) and 0x4071DD0 (triple-buffer change) without going through 0x585510: hook its entry and rewrite frame+0x30/+0x34 if the mod size is on. | Confirmed [r8, r14] |
| Frame output/render size setter (also used by photo-mode hi-res screenshots) | **0x40729D0** = base `rf::RenderFrame` vtable 0x58EDA30 slot 1, `(frame, w, h)` | upscaled (+0x28/+0x2C) = w,h; render (+0x20/+0x24) = 0x406FD20(upscaler) unless `[frame+0x40]==2`; marks the stage resource manager dirty (`mgr+0x358`) so every frame-sized target is recreated next frame; per view: 0x407BB80 (viewports), 0x407BE10 (aspect), view slot 8 0x3CC9190 (tiles, stage resize). Do not hook it for VR: the screenshot path 0x3BB1DC0 relies on it. | Confirmed [r6] |
| Render size from output size (upscaler math) | **0x406FD20** | `{w,h}/f`: FSR1 1.3/1.5/1.7/2.0, FSR2 1.5/1.7/2.0/3.0/1.2/(5: 1.0), DLSS via NGX `DLSSOptimalSettingsCallback` (0x40CD860), XeSS via 0x40CDA80; type 0 = no reduction. Section 3. | Confirmed [r4] |
| Graphics settings (resolution, window mode, upscaler) | `[0x604AA10]` +0x0C Fullscreen, +0x0D FakeFullscreen (borderless), +0x10 ScreenWidth, +0x14 ScreenHeight, +0x18/+0x1C refresh, +0x20 adapter, +0x24 monitor, +0x28 VSync, +0x29 TripleBuffering, +0x77 upscaler type, +0x78 FSR1, +0x79 FSR2, +0x7A DLSS, +0x7B XeSS mode, +0x7C sharpness, +0x80 AntiAliasing, +0x81 CAS, +0x84 CAS sharpness | Read-only for the mod (the resolution fields are validated only on load/apply, 1.3). Upscaler fields can be written with the game's setters 0x406FB00 (off) / 0x406FBC0 (FSR1) / 0x406FC40 (FSR2) / 0x406FC70 (DLSS) followed by a resize call. | Confirmed [r1, H dec_gfx_io.c, r4] |
| Frame size fields (read for logging) | frame +0x20/+0x24 render, +0x28/+0x2C upscaled (output), +0x30/+0x34 display (= swapchain), +0x40 frame type (main = 0), +0x4308/+0x4328/+0x4348 pixel sizes of the render/upscaled/display rects, +0x4358 render-area scale, +0x42D8 output override target, +0x43A8 swapchain, +0x4250 stage resource manager | Log these before and after a resize. | Confirmed [r6, r23] |
| UI size | GameUI `SetSize` 0x120E0E0 (F 1.4) | 0x585510 re-applies `frame+0x4328/+0x432C` (= the output size, e.g. 3664x1920) on every resize; call SetSize(panel size) after each resize (from the 0x585510 hook, after the original returns). | Confirmed (F + [r9]) |
| Per-eye DLSS | step prepare **0x3EF3790**, create **0x3EF3920** -> 0x40BD060 (NGX CreateFeature), evaluate job **0x3EF34B0** -> 0x40C21B0 | Feature handle is per post-process step instance (`step+0x78`), i.e. per view; created with the view's own render viewport (ReadState +0x2180) and upscaled viewport (+0x21A0) sizes, evaluated with subrect bases +0x2188 / +0x21A8 and `DLSS.Enable.Output.Subrects`. Works per eye as-is; recreated automatically when a view's viewport changes. | Confirmed [r25, r26] |

## 1. How BG3 decides its render resolution

### 1.1 Three sizes per RenderFrame (Confirmed)

A `rf::RenderFrame` carries three sizes, set from a creation descriptor (ctor 0x4072580 [r23]) and later by
Resize:

| frame field | name used here | meaning |
|---|---|---|
| +0x20/+0x24 | render | upscaler input; size of the frame-sized render targets (`mgr+0x594/+0x598`) |
| +0x28/+0x2C | upscaled / output | the game's "resolution" (ScreenWidth x ScreenHeight); upscaler output (`mgr+0x59C/+0x5A0`) |
| +0x30/+0x34 | display | swapchain back-buffer size; stage 0x45 (DownsampleResolver, A 10) scales output -> display |

Rects (0x4073480 -> 0x4073520 [r23]): three 0x20-byte records {float2 scale, float2 offset, int2 size, int2
offset} at +0x42F8 (render), +0x4318 (upscaled), +0x4338 (display). The render rect is scaled by
`sqrt([frame+0x4358])` (render-area scale; default 1.0) and optionally letterboxed to the aspect
`[frame+0x42F4]` when byte `[frame+0x42F1]` is set (written only by the ctor = 0: unused, FindDisplacement
[r31]).

The main frame ("MainFrame") is created in 0x3AC70B0 [r6] via device `vf[+0x20]` = 0x4071AC0: render =
0x406FD20(settings size), upscaled = settings size (display-settings struct +0x18/+0x1C, i.e. ScreenWidth/
ScreenHeight), display = the window's client size (window `vf[0x38]`), type `[+0x40]` = 0. This matches the
logged state (stereo.md): render/upscaled 1920x1080 (graphicSettings.lsx ScreenWidth/Height), display
1920x1200 (borderless window on a 1920x1200 monitor).

### 1.2 Who sets them at runtime (Confirmed)

```
SDL pump 0x57AAA0  SDL_WINDOWEVENT_SIZE_CHANGED, window not fullscreen-desktop/minimized:
    event {hwnd, w,h, w,h}   (render follows the client size: windowed mode)          [r22]
Window SetSize 0x57F200 (SDL window wrapper vf[+0x30]):                                 [r21]
    fullscreen-desktop (SDL flags & 0x1001 == 0x1001, i.e. borderless "FakeFullscreen"):
        event {hwnd, SDL_GetWindowSize = monitor, requested w,h}   -> render size != window
    windowed: SDL_SetWindowSize(w,h); event {hwnd, client, w,h} posted now if size unchanged,
        otherwise later by the SDL size-changed event
Device SetDisplayMode 0x4071DD0 (device vtable 0x58ED9E8 +0x28), last arg "upscaler changed":
    event {hwnd, max(1,w),max(1,h), w,h}                                              [r14]
    -> renderer vf[0x118]
renderer OnWindowResize 0x585510 (derived) / 0x3AC7990 (base):                      [r9, r14]
    if main frame: 0x40CD530(NGX, w, h)  (DLSS supported-mode table for this output size)
                   0x40765C0             (sampler LOD bias from the upscaler, 0x4076630)
    frame vf[1] 0x4091CF0(frame, w, h, dispW, dispH)
        frame+0x30/+0x34 = disp; 0x4091C70 -> 0x40919B0 (release BB refs),
            swapchain->ResizeBuffers(0, disp, 0, flags) (vtable +0x68), 0x4095E50, 0x4091680 (new BB views)
        0x40729D0(frame, w, h): upscaled = w,h; render = 0x406FD20(w,h) (or = w,h if type 2);
            mgr dirty (+0x358/+0x359/+0x35A); 0x4073480 rects; per view 0x407BB80, 0x407BE10, vf[8] 0x3CC9190
    0x3AC7B80 (window title "%s (%dx%d) ...")
    GameUI(+0x130 of 0x6047958) vf[0x60] SetSize(frame+0x4328, frame+0x432C)   (F 1.4)
next frame: 0x4072CB0 -> 0x4094E60: mgr+0x358 set -> 0x409C9C0 recreates every frame-sized key
            (size = ceil(render or upscaled size / divisor), 0x409B0D0, A 7)                     [r30]
```

The `+0x118` slot: renderer vtable 0x5499F58 (F 2.1) + 0x118 = 0x549A070 = 0x585510 (pointer found by byte
search; base class slot 0x5899D28 = 0x3AC7990). The window HWND is the lookup key (0x3BB1A80 compares
`window vf[0xF8]()` of each frame; 0x57F1B0 returns the SDL WM-info HWND) [r27].

### 1.3 Graphics options: storage, validation, apply (Confirmed)

* Option name globals and the loader/saver field map: [r1] maps every name global 0x5FCCAD0..0x5FCCD00
  to its string; load 0x3ABDE40 (H) stores: ScreenWidth -> +0x10, ScreenHeight -> +0x14, Fullscreen ->
  +0x0C and FakeFullscreenEnabled -> +0x0D (both forced 0 when `param_2+0x40` is set), DisplayAdapterIndex
  +0x20, MonitorIndex +0x24, RefreshRateNumerator/Denominator +0x18/+0x1C, VSync +0x28, TripleBuffering
  +0x29, ResolutionUpscaleType (0..5) and FSR1Mode/FSR2Mode/DLSS2Mode/XESSMode into the packed bytes
  +0x77..+0x7B, UpscaleSharpness +0x7C, AntiAliasing (0..4) +0x80, CASEnabled +0x81, CASSharpness +0x84.
  Defaults (0x3ABD930): Fullscreen 0, **FakeFullscreenEnabled 1** (borderless), ScreenWidth/Height 0
  (-> desktop mode in borderless/fullscreen, 1920x1080 windowed, 0x4090180), VSync 1, TripleBuffering 1, AntiAliasing 2, upscaler type 0, FSR1 4, FSR2 5,
  DLSS 6, XeSS 6.
* Packed upscaler state: `+0x77` type (0 none, 1 FSR1, 2 FSR2, 3 DLSS, 5 XeSS), `+0x78` FSR1 mode,
  `+0x79` FSR2 mode, `+0x7A` DLSS mode, `+0x7B` XeSS mode. Setters: 0x406FB00 (type 0, modes reset to
  4/5/6/6, AA forced to 2 when leaving FSR2/DLSS/XeSS), 0x406FBC0 (FSR1, mode), 0x406FC40 (FSR2), 0x406FC70
  (DLSS) [r4]. 0x3AC3180 [r6] falls back to type 0 if DLSS (NGX `+0x1A5`) or XeSS (`+0x1B0`) is unavailable,
  and, when the upscaler type is < 2, AntiAliasing 3 -> 2 without DLSS and 4 -> 2 without XeSS (so AA 3 is
  DLSS-based (DLAA), AA 4 XeSS-based).
* Validation **0x4090180** [r15] (called from API init 0x4071270): monitor = min(MonitorIndex, count-1);
  0x48-byte monitor records at `[device+0x30]` (+0x18 mode list {w,h,refN,refD}, +0x24 count, +0x28 desktop
  mode {w,h,refN,refD} = IDXGIOutput::FindClosestMatchingMode of the desktop size, +0x38 same; filled by
  0x4070510 [r34], mode list only BGRA8 modes >= 1024x768 or 1280x720). Windowed: w,h clamped to
  [16, desktop size]. Fullscreen/borderless: w,h must be in the mode list, else the desktop mode; refresh
  chosen from the list. Result written back to `[0x604AA10]+0x10/+0x14/+0x18/+0x1C/+0x24`.
  **So graphicSettings.lsx cannot request 3664x1920 on a 1920x1200 monitor** in any mode (clamped or replaced).
* Options menu apply **0x1B551D0** [r19]: compares the edited copy with `[0x604AA10]`, copies it
  (0x1B55790), and if any display field or upscaler byte changed calls `device vf[+0x28]` = **0x4071DD0**
  `(device, mainFrame, w, h, refN, refD, monitor, fullscreen, fake, triple, vsync, upscalerChanged)`.
  0x4071DD0 [r14]: VSync -> frame+0x43B4, triple buffering change -> 0x4091C70; mode/monitor change ->
  0x40929F0 (ResizeTarget in exclusive fullscreen, SetFullscreenState 0x4092250, frame+0x30/+0x34 = w,h,
  ResizeBuffers); window vf[+0x30] SetSize(w,h) (0x57F200) / vf[+0x40] move to monitor / vf[+0x78]
  borderless; and if the upscaler changed it posts the resize event itself.
* Display mode list and window creation: API init 0x4071270 [r18] creates the SDL window ("Window",
  centred on the monitor, size from the validated settings) and the D3D11 device; device caps written there
  include max texture size `0x4000` (+0x15C/+0x160/+0x168).

### 1.4 Swapchain (Confirmed, 0x4090A50 [r27])

`IDXGIFactory2::CreateSwapChainForHwnd` (vtable +0x78) with DXGI_SWAP_CHAIN_DESC1 {Width/Height = display
size, Format from frame+0x4390 (0x40903A0), SampleDesc 1, BufferUsage 0x30, BufferCount 2 (+1 with triple
buffering), **Scaling = 0 (DXGI_SCALING_STRETCH)**, SwapEffect 4 FLIP_DISCARD (3 on old OS), Alpha 0,
Flags 2 | 0x800 (ALLOW_MODE_SWITCH | ALLOW_TEARING when supported)}; then MakeWindowAssociation(hwnd, 3).
Because Scaling is STRETCH, a back buffer larger than the window is legal and is scaled to the window by
DXGI/DWM. ResizeBuffers call site: 0x4091C70 (+0x68), also 0x4090FD0 (exclusive fullscreen path).
ResizeTarget: 0x40929F0 and 0x4090FD0 (+0x70).

### 1.5 Views (Confirmed)

Each view's viewports = frame rect x the view's Scale/Offset (0x407B8C0 [r23]); 0x407BB80 writes WriteState
render (+0x2218.. from frame+0x20), upscaled (from +0x28) and display (from +0x30) viewports; the camera
aspect comes from the **display** viewport (0x407BE10, A 3.2). View slot 8 0x3CC9190 stores the frame
render size at view+0x2290, light tiles (72 px, +0x29A8; tile size +0x229C) and calls every stage's
slots +0x48/+0x58(w,h). The split-screen layout (B: RenderViewportSystem / our views.cpp writing Scale 0.5
and Offset 0 / 0.5) is resolution-independent, so a wider frame gives each eye exactly half.

### 1.6 Other size paths (Confirmed)

* Photo-mode/high-res screenshot **0x3BB1DC0** [r8]: per request it sets `frame+0x42D8` to its own
  offscreen texture (the back-buffer key resolves to it: 0x40979D0 returns `+0x42D8` if set, else the
  current back buffer [r32]), forces the upscaler off (0x406FB00), calls **0x40729D0(frame, reqW, reqH)**,
  renders the frame 1 or 8 times (frame vf[+0x28], 0x4072B30, views, 0x4073040, vf[+0x30]), then restores
  the upscaler and 0x40729D0(old size). Proof that the engine renders a frame at an arbitrary size without
  touching the swapchain.
* Dynamic resolution 0x4073C50 (from 0x4072CB0) adjusts `+0x435C/+0x4358` in [0.7, 1] from frame
  `vf[+0x50]` GPU timings; on DX11 that slot is 0x579DA0 (`xor al,al; ret`), so the scale stays 1.0 [r32].

## 2. Cleanest way to set an arbitrary render size

Ranked (all Confirmed as code paths; the end-to-end result needs the runtime checks listed):

**A (recommended): output = display = 2*eyeW x eyeH through the game's resize entry, sticky via a hook.**
1. Get `renderer = *(void**)(base+0x6046F98)`; check `*(void**)renderer == base+0x5499F58`.
2. Get the game HWND (our swapchain's `GetDesc().OutputWindow`, or `frame+0x18` window `vf[0xF8]()`).
3. Hook 0x585510. In the detour, if VR resolution is on and `ev->hwnd` is the game window: set
   `ev->dispW = ev->w = 2*eyeW; ev->dispH = ev->h = eyeH` (keep both even), call the original, then call
   GameUI SetSize (0x120E0E0) with the VR UI panel size (F).
4. To apply or change at runtime (VR menu), build `{hwnd, W, H, W, H}` and call the original 0x585510 (or
   the hooked one) from the thread that runs the SDL pump 0x57AAA0 (the game's own caller; e.g. from a
   hook at the end of 0x57AAA0). To turn VR resolution off, send `{hwnd, client size, ScreenWidth,
   ScreenHeight}` (the values 0x57F200 would send).
5. Optional: hook 0x4091C70 to force `frame+0x30/+0x34` for the main frame, for the paths that resize the
   swapchain without the event (0x40929F0, triple-buffer toggle).

What gets recreated (all by the game): swapchain buffers (ResizeBuffers) and back-buffer views (0x4091680),
every frame-sized stage resource (mgr dirty -> 0x409C9C0 next frame), view viewports and aspect, light tiles
and stage resizes (view slot 8), the per-view DLSS feature (0x3EF3790 sees the new viewport sizes), the DLSS
mode table (0x40CD530), sampler LOD bias (0x40765C0), UI size. Not recreated: the per-view masked occlusion
culling buffer (MOC, A 1.3) keeps the resolution given at view creation (the only caller of the MOC slot-1 resize 0x41A2270
found in 0x3CC0000-0x3CD0000 is the view ctor 0x3CC5AA0, FindInDecompiled [r29]; a whole-exe search was not
done, so Unconfirmed that nothing else resizes it); a stale MOC size only affects cull precision.

Why display = output: (a) the mod's existing copy from the back buffer keeps working and gets 1832x1920 per
eye; (b) the camera aspect is computed from the display viewport (0x407BE10), so display must have the eye
aspect unless the mod overrides every projection; (c) STRETCH scaling keeps the desktop mirror correct.
Works with the two-view layout: each view takes half of render, upscaled and display rects.

**B: output > display (back buffer stays the window size).** Same event with `dispW/H` = client size. The
game renders 3664x1920 and stage 0x45 downsamples to the back buffer, so the eyes would have to be taken
from the pre-downsample output target instead of the back buffer, and the display viewport (camera aspect)
would be 960x1200 per eye. More work, no benefit.

**C: settings file / options menu.** Rejected: 0x4090180 clamps windowed sizes to the desktop and replaces
non-listed fullscreen/borderless sizes by the desktop mode.

**D: windowed window larger than the desktop** (render follows the client size in windowed mode). Rejected:
depends on Windows/SDL allowing a window bigger than the screen; A gives the same without moving the window.

**E: separate internal target via `frame+0x42D8`** (the screenshot mechanism). Possible but needs our own rf
texture object and suppresses the normal back buffer; A already decouples render size from the window.

## 3. Upscalers and per-eye use

Render size from output size (0x406FD20 [r4]); per view it is the same fraction of the view's half:

| type `+0x77` | mode byte | values |
|---|---|---|
| 0 none | | render = output |
| 1 FSR1 | +0x78 | 0: /1.3, 1: /1.5, 2: /1.7, 3: /2.0, other: /1.0 |
| 2 FSR2 | +0x79 | 0: /1.5, 1: /1.7, 2: /2.0, 3: /3.0, 4: /1.2, 5: native (/1.0) |
| 3 DLSS | +0x7A | NGX `DLSSOptimalSettingsCallback` with PerfQualityValue 1->4 (UltraQuality), 2->2 (MaxQuality), 3->1 (Balanced), 4->0 (MaxPerf), 5->3 (UltraPerformance); 6 -> render = output (0x40CD860) |
| 5 XeSS | +0x7B | 0x40CDA80 (not decompiled); 6 = native. Mip-bias code 0x4076630 uses ratios 1/3, 1/2.3, 1/2, 1/1.7, 1/1.5, 1/1.3 for XeSS quality ids 100..105 |

* Exception: frames with `[frame+0x40] == 2` always render at output size (0x40729D0).
* 0x40CD530 [r23] marks DLSS modes unsupported per output size; mode index 5 (NGX UltraPerformance) is
  marked unsupported when output width > 7679.
* Texture mip bias: 0x4076630 adds `log2(render/output)` to sampler LOD bias (via 0x4078030), refreshed by
  0x40765C0 on resize.
* **DLSS per eye (Confirmed [r25, r26]):** the NGX feature handle lives in each view's upscaler step
  (`step+0x78`); 0x3EF3790 compares the view's ReadState render viewport (+0x2180/+0x2184) and upscaled
  viewport (+0x21A0/+0x21A4) with the cached sizes and recreates the feature (0x3EF3920 -> 0x40BD060:
  `Width/Height` = view render viewport, `OutWidth/OutHeight` = view upscaled viewport, quality from
  `+0x7A` or MaxQuality for DLAA, `DLSS.Feature.Create.Flags` 0x6B, `DLSS.Enable.Output.Subrects`).
  Evaluate (0x3EF34B0 -> 0x40C21B0) passes `DLSS.Render.Subrect.Dimensions` = render viewport size,
  `Input.*.Subrect.Base` = render viewport offset (+0x2188/+0x218C) and `Output.Subrect.Base` = upscaled
  viewport offset (+0x21A8/+0x21AC), jitter from 0x3F11710. So each eye view gets its own DLSS feature and
  history, reading and writing its own half. Unconfirmed: that the split (second) view has its own
  post-process stage instance (it does if its stage list contains stage 0x21; check by logging `step+0x78`
  of both views' upscaler steps after enabling DLSS).
* FSR2 (step job 0x3EF4170) reads the same per-view viewport fields (+0x2180/+0x21A0/+0x2188/+0x21A8)
  [r26]; its context ownership was not traced (Unconfirmed).

## 4. Limits

* Max texture size 16384 (device caps written in 0x4071270; D3D11 limit) -> W = 2*eyeW <= 16384.
* Windowed sizes from settings are clamped to the desktop; fullscreen/borderless sizes must be listed
  modes (0x4090180). Direct calls (section 2 A) bypass this.
* Camera aspect = display viewport aspect (0x407BE10): keep display = output.
* A 12: scissor opcode 0x21 computes bottom = y + width (0x40C7080); with eye viewports narrower than tall
  (1832x1920) a pass using it would lose the bottom 88 rows. Its only emitter is 0x2130166 (A).
* UI: 0x585510 sets the UI to the output size (frame+0x4328/+0x432C); layout scale = h / 2160 (F 1.4), so a
  3664x1920 UI lays out as 4122x2160 units. The mod should SetSize the UI to its panel size after each
  resize (already the plan in docs/design/vr-ui.md). Mouse -> frame mapping (renderer vf[0x108] 0x3AC77D0)
  scales window points by frameSize/windowSize, so it stays consistent at any frame size (F 2.1).
* MOC resolution fixed at view creation (section 2 A).
* DLSS UltraPerformance disabled at output width >= 7680 (0x40CD530).
* Even sizes recommended: view rects are rounded per edge (0x407B8C0), so odd widths give eyes differing by 1 px.

## Open leads

1. **Thread of 0x585510.** Static: it is reached from the SDL pump 0x57AAA0 and 0x57F200/0x4071DD0 (main
   loop). Confirm with a runtime log of `GetCurrentThreadId()` in the 0x585510 hook vs. the render thread
   (0x4072B30) and do the mod's resize from the same thread.
2. **First-run log for approach A:** before/after calling 0x585510 with {hwnd, 3664,1920, 3664,1920} log
   frame +0x20/+0x28/+0x30, +0x4308/+0x4328/+0x4348, swapchain GetDesc, both views' ReadState +0x2180/
   +0x21A0/+0x2240 (display viewport) after one frame, and the upscaler step `+0x78/+0x84/+0x8C` per view.
3. Second view's post-process stage instance (section 3) and FSR2 context ownership (0x3EF4170).
4. XeSS render-size function 0x40CDA80 not decompiled.
5. UI target size after SetSize smaller than the frame (F open lead) still applies.
6. Meaning of frame type `[+0x40]` values 1/2 beyond A's "1 = offscreen" (only `==2` matters here).
7. The DownsampleResolver stage 0x45 input key (needed only for approach B).
