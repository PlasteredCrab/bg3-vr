# CPU side, job graph and frame pipelining - agent W, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656. I used Ghidra project BG3_G (read-only, headless, request server
`agents\W\WServer.java`) and the project's disassembler. I also read the mod's own source and the 2026-10-22 headset
log (`bin\bg3vr\bg3vr.log`, job dumps and report lines). RVAs are relative to 0x140000000. Raw output is in
`re-local\ghidra\agents\W\` (w1..w47). Each finding is marked **Confirmed** (read from code, disassembly or the log) or
**Unconfirmed** (with the check that would settle it). Agent T covers the GPU passes that can be shared between the
eyes (shadows, LUTs, volumetrics) and agent X covers resolution, upscaling and post effects. This report covers the
CPU threads, the job graph and latency.

## Savings for the VR mod

Ranked by expected gain divided by risk. "fps" rows make frames faster. "latency" rows don't raise fps, but they
fix the camera lag and jumps.

| # | what | expected gain and why | RVA / hook | how | risk | confidence |
|---|------|----------------------|-----------|-----|------|------------|
| 1 | **Replace eyefix's whole-view serialisation with a per-view slot for stage 0x47** | fps. Today the eye's stage 0x47 counter waits for the main view's end of recording (main ctx+0x78), and every eye recording stage except 0x35/0x02/0x03/0x37 and the cull hangs off that counter. So the eye records only after the main view has finished recording (2.3). The game thread can't start its next update until **all** recording jobs of the frame are done (3.2). This wait therefore adds the eye's recording critical path to the frame. Removing it lets both views record in parallel again on the 6 workers. | mid-hook at **0x3E53E01** (`mov [rcx+0x219c], edx` in stage 0x47 execute 0x3E53DE0), or a hook after 0x3E53DE0 returns | When `[rcx+8]` (the stage's view) is the eye view, store **1** instead of the view's split index 0. The writer (job 0x3E53FA0) and every reader (0x3E13520, 0x3F59B10, 0x3F5D2E0, 0x3F7B480, 0x3F7C240) index the per-object slot by `stage47+0x219C`, so the eye then uses `object_data+0xE4` and the main view `+0xE0`. After that, turn off the extra job edge (`[views] serialize_eyes = false`). Everything else keeps index 0. | low-medium: index 1 is the game's own second-player slot. One open risk: shared per-object state in stage 0x41 (skinning), which shares the eye's counter, is not fully ruled out (2.4). | race mechanism and readers: Confirmed. Gain: Unconfirmed (measure, section 5) |
| 2 | **Raise the job-system worker cap of 6** | fps, if the workers are saturated. The scheduler computes `min(affinity bits, [0x5FBDF2C] - 3)` and then clamps it to **1..6**. This machine (i7-10700, 16 logical CPUs) runs 6 workers; the log shows exactly 6 worker thread ids. With two views the recording, VFX data, skinning and animation jobs all share those 6. | immediate `06` at **0x4106E96** (`mov eax, 6` at 0x4106E95, in scheduler ctor 0x4106CF0, called from 0x40F5BD0 early in WinMain) | Patch the imm32 to e.g. 10..12 from the proxy DLL's attach, before WinMain reaches 0x40F5BD0. Make it an ini option, default off. The worker arrays at sched+0x60 and sched+0xE0 are 16 pointers each, so the value must stay at or below 16. | medium: the game was only tested up to 6. Per-thread structures sized at start-up were not checked (2.6). | cap and array size: Confirmed. Gain: Unconfirmed |
| 3 | **Skip the eye's stage 0x3E (texture/mesh streaming queries)** | fps (worker CPU). Stage 0x3E runs sphere queries of 40 m around the camera entity's position and around other entity positions (0x3CD1E80 + 0x3CD27F0 each), then computes texture levels. The positions come from the camera entity, not the view, so the eye repeats the main view's queries. | stage 0x3E execute **0x3E51D10** (submits job 0x3E51DE0 to `[stage+0x78]`) | When `[rcx+8]` is the eye view, return without submitting. The stage's wait (0x3F7A6F0 on `[stage+0x78]`) then only waits for whatever else was submitted to that counter. | low. Only the residency factor k (eye FOV / viewport) differs, and the main view's request is enough. | mechanism: Confirmed. Gain size: Unconfirmed |
| 4 | **Cap the DXGI frame queue at 1** | latency, up to 2 frames less (about 77 ms at 26 fps) when the GPU is the bottleneck. The game never calls SetMaximumFrameLatency: the IDXGIDevice1 and IDXGISwapChain2 interface IDs are not in the exe, and the swap chain has no waitable flag (log: flags 0x802). The render thread only waits for its CPU jobs, never for the GPU (3.3), so the GPU queue depth is whatever DXGI or the driver allows. Microsoft documents a default of 3. | none in the exe. Done by the mod on the game's device (`render.cpp` already has it from the swap chain) | `IDXGIDevice1::SetMaximumFrameLatency(1)` once the device is known. Log `GetMaximumFrameLatency` first. Alternative with no code: NVIDIA Low Latency Mode = On for bg3_dx11.exe. | low. It may cost a little fps if the CPU/GPU overlap was hiding stalls. | game side: Confirmed (absence). Default 3 and the effect: Unconfirmed until logged |
| 5 | **Fix the pose prediction horizon in the mod** | latency/jumps. `headset.cpp` predicts `1 + measured lag` display periods ahead. The measured "lag 5" counts calls of 0x4085150 for the controller, not frames. The log shows about 5 of those calls per presented frame (4.1), so the real lag is about 1 game tick. The pacer's `XrFrameState` can also be up to a full game frame old when `begin_frame` uses it (4.3). | `headset.cpp` `submit_tracked` (lines ~600-660), `xr.cpp` `Session::Pacer` | Predict from the current time: `xrConvertWin32PerformanceCounterToTimeKHR(now)` plus a measured pipeline time (game update start to Present, plus GPU time). Don't use `predictedDisplayTime + period * lag`. Log the calls per frame. | low | code: Confirmed. Call ratio: Confirmed from the log arithmetic |
| 6 | **Quality levers that cut per-view CPU** (agent H's settings, with twice the effect in stereo) | fps. Each draw is recorded and submitted twice, so fewer objects save twice. AnimationLODDetail updates entities only every 2nd or 4th frame (H: 0x19E48A0 / 0x19E5110). | `[0x604AA10]` +0x3D DetailDistance, +0x3E AnimationLODDetail, +0x3F InstanceDistance, +0x42 DynamicCrowds (H section 1) | Expose as VR presets. Keep the player's own skeleton at full rate: time slicing is by entity id, so check the player's id or keep AnimationLODDetail at 0. | low (visual) | mechanisms: Confirmed by H. Gains: Unconfirmed |
| 7 | **Cull once for both eyes** (share CullStage 0x08) | fps (worker CPU). Saves one occluder rasterisation, one scene query, the per-object bucketing and the bucket sorts per frame. It also makes both eyes draw the same object set. Both views already cull with the **same frustum planes** (camera entity component +0x210, 2.5), so the results differ only by each view's MOC and the eye position used for distances. | CullStage job 0x3E24970 (eye), buckets +0xBB8..+0x2150, 0x47 list via 0x3E53EC0 | Make the eye's CullStage wait for the main one (add an edge: eye ctx+0x40 waits for main ctx+0x40). Then replace the eye's job with a copy of the main stage's output: every bucket array, the light list +0x20F8, the lists +0x2070/+0x2110/+0x298, and the visible list it pushes into the eye's stage 0x47. Repointing the eye's consumer stages at the main CullStage is not enough, because the eye's stage 0x47 still needs its own list. | high: many fields, and per-view outputs (fades, room masks) must be copied correctly | frustum sharing: Confirmed. The copy recipe: Unconfirmed (needs a field-by-field pass of 0x3E24970 / 0x3E220C0 / 0x3E22790) |
| 8 | **Budgeted main-thread queue 0x3DF9340** | fps, only while the queue is busy. Before begin-recording, the render thread drains a message queue (`[App+0x1C8]`) for up to **6 ms** per frame, or 60 ms when `[[0x6047B40]+8]` is set. That time is directly on the frame's critical path. | 0x3DF9340, budget constants 6 / 0x3C in the function body | Measure first (section 5). If it is often near 6 ms during play, lower the budget (it is a QPC loop around 0x41867F0). | unknown: the queue's purpose was not identified | budget: Confirmed. Purpose: Unconfirmed |

Not shareable on the CPU (Confirmed): per-draw instance data (0x3CD87B0) and the per-object VFX-style data built by
stage 0x48 (0x3E541B0 -> 0x3E54280 -> 0x3F59B10 / 0x3F5D2E0) are camera-relative or camera-facing. They use the
view controller's slot 9 position and slot 11 vector, and with per-eye proxies (agent S) the eyes differ.

Related leads for the other agents (not ranked here):
* Agent T: stage 0x41 (GPU skinning) runs per view and re-skins every visible character for the eye. Its output is
  per stage, double-buffered by frame parity (0x3CCDAC0).
* Agent X: 0x4073C50 is a dynamic-resolution controller (scale at frame+0x435C, clamped to [0.7, 1.0]). In DX11 its
  timing source, RenderFrame slot 10, is the empty stub 0x579D90, so it receives no timings.
* Exposure: eyefix's exposure hook (0x3EDE160) ran 0 times in the 10-22 run ("exposure jobs 0" while stereo was on).
  That exposure step didn't run at all, so the skip saves nothing there. Whether the old exposure path (job
  0x3EDFC40, A section 8) is the one running: Unconfirmed. Log both.

## 1. Threads and the per-frame sequence (Confirmed)

| thread | entry | per frame |
|--------|-------|-----------|
| main thread = render thread | WinMain 0x5777D0 loop: `while (!App->slot29()) App->slot24()`. Slot 24 is 0x3AC3510 (vtable 0x5499F58+0xC0), which jumps to **0x3DF8DE0** | 1. wait for event ev0, then ev2 (0x4108440: wait while running queued jobs); 2. budgeted queue 0x3DF9340 (up to 6 ms); 3. wait for ev3; 4. 0x3DF9570: frame slot 5, then render loop **0x4072B30** (begin-recording of every view, then all view counters armed and released); 5. arm the frame counters; 6. **0x3DF97C0** frame end: submit the "release game thread" job (0x3DF8BB0) to the frame counter `[0x6048680]`, then **0x4073040**: for each view in order (main, eye, special), view slot 4 closes and **executes** its command buffers. Each stage's slot 8 first waits for that stage's counter (0x410E880, which runs jobs while waiting) and then runs 0x407F240, which translates the stream into D3D11 calls on one context. Then frame slot 4 = frame sync 0x4072CB0 for the next frame (WriteState -> ReadState, frame CB), and frame slot 6; 7. wait for the frame counter; 8. deferred-release lists (0x40BF540, 2 deep). Present is FlipBuffers 0x40908A0 (frame vtable 0x58EE490 slot 8), where the mod's on_present runs. |
| "GameThread" | created by 0x3DF9F20 (thread name string "GameThread"). Run function **0x3F770A0** (vtable 0x58DC2E0 slot 1) | loop: time step (QPC delta, clamped to 0.0667 s), SetEvent(ev0), **game update = App slot 0x288 (0x5889E0)**, SetEvent(ev2), SetEvent(ev3), then **wait for ev1** (0x4108440) |
| 6 workers | WorkerThreadScheduler `[0x614FBC0]`, count at sched+0x20 (2.6) | all stage jobs, the cull, VFX data, skinning, animation |

ev1 is set only by job 0x3DF8BB0 (vtable 0x58C47F0 slot 2, the job object at +0x10 of the render system object).
Frame end 0x3DF97C0 submits it to the frame counter `[0x6048680]`, whose predecessors are every view's ctx+0x80
(runtime dump below). It runs once **every recording job of every view** has finished. It then waits for counter
`[0x6048688]` (fed by the CullStage job and by 0x3BB1DC0) and sets ev1.

## 2. Job graph per view and what eyefix does

### 2.1 Counters (Confirmed: runtime dump 10-22 16:24:25 "after create", stage fields `+0x2138`/`+0x78`)

Each view's context `[view+0x40]` has 17 counters (0x40878C0). The predecessor edges observed at runtime:

```
camera occluder job (MOC raster, 0x3F38A40) -> +0x40 -> +0x60 -> +0x50 --+
                                                          +0x48 ---------+-> +0x58 -> +0x68 ------------+
                                                                              +0x58 -> +0x08/+0x10/+0x18/+0x20 -> +0x28/+0x30 -> +0x38 --+-> +0x78 -> +0x80 -> frame counter [0x6048680]
                                                                                        +0x70 (no preds) -------------------------------+
special view: +0x40 waits for main +0x78 and eye +0x78 (0x3AC4EA0)
```

Stage jobs by counter (main and eye are identical, 28 stages):

| counter | stages (job submitted to it) |
|---------|------------------------------|
| +0x40 | 0x08 CullStage: Confirmed. 0x3AC51C0 passes `[ctx+0x40]` to its ctor 0x3E229C0, which reaches 0x3F7A530 (`mov [rcx+0x78], r9`) through 0x3F7E920; execute 0x3E245E0 submits to `[stage+0x78]`. Stage 0x3E uses the same execute and wait pattern, but its counter value was not checked. |
| +0x60 | 0x35 (shadow atlas), 0x02, 0x03 |
| +0x58 | **0x47** (per-object data offsets), **0x41** (GPU skinning) |
| +0x68 | 0x48, 0x1E, 0x05, 0x0B, 0x3D, 0x46, 0x36, 0x44, 0x18, 0x3C, 0x17, 0x40, 0x1B, 0x1A, 0x00, 0x1F, 0x33, 0x21, 0x3F |
| +0x70 | 0x37 (SSAO) |
| +0x78 | 0x0A (main only) |

### 2.2 Without eyefix: the views record in parallel (Confirmed)

Render loop 0x4072B30 calls begin-recording (slot 1) for every view first. Begin-recording submits each stage's
job to its counter, and the counters are not released yet. A second loop then releases every view's counters
(0x407B540). So both views' jobs become runnable at the same moment, and nothing links the two graphs except the
special view and the frame counter.

### 2.3 With eyefix: the eye's recording runs after the main view's recording (Confirmed)

`eyefix.cpp link_eyes` adds the edge **eye ctx+0x58 waits for main ctx+0x78** (log 16:24:25.003: the predecessors
of eye +0x58 become eye +0x50, eye +0x48, main +0x78). Main +0x78 is reached only when all of main's +0x68 stages,
the +0x08..+0x38 sub-job chains and +0x70 are done. Everything on eye +0x58 and +0x68 therefore starts only after
the main view's recording has completely finished: 0x47, 0x41, then 0x48, 0x0B, 0x46, 0x1B, 0x21 and the rest. Only
the eye's occluder raster, cull, 0x3E, 0x35/0x02/0x03 and 0x37 still overlap the main view.

What that costs (mechanism Confirmed, size Unconfirmed):
* **Game thread**: it waits for ev1, which needs every recording job of the frame (section 1). So the eye's serial
  recording time is added directly to the time the game thread sits idle.
* **Render thread**: it executes the main view's buffers as they become ready. It then has to wait in 0x410E880
  for the eye's stages, which only started when main's last stage finished.
* Section 5 measures it directly: the start time of the eye's 0x47 job against the time main +0x78 completes.

### 2.4 The race eyefix prevents, and the cheaper fix (Confirmed except where noted)

* Stage 0x47 execute 0x3E53DE0 copies `(int)(char)[view+0x2268]` into `stage+0x219C` (store at 0x3E53E01). Its job
  0x3E53FA0 walks the view's visible list (`stage+0x21C8`, count `+0x21D4`). For each object it writes that object's
  offset in this view's ring buffer to `[obj+0x2D0]+0xE0+idx*4` (type 0) or `[obj+0x2D8]+0xE0+idx*4` (type 1). The
  ring buffer is per stage (3 buffers, `frame % 3`).
* The readers take the index from the **render context's stage 0x47** (`ctx+0x20`), never from the view: 0x3E13520
  (velocity, stage 0x40), 0x3F59B10 / 0x3F5D2E0 (data fill from stage 0x48), 0x3F7B480 / 0x3F7C240 (draw).
* The mod renders the eye with split index 0 (`views.cpp`: "1 for the setup, 0 for rendering"), so both views write
  the same `+0xE0` slot. The main view's draws can then read the eye's offset into the main ring buffer.
* Fix: give only the eye's stage 0x47 index 1 (table row 1). Index 1 is the slot the game uses for its own second
  split-screen player: 0x3BAA1A0 sets `[view+0x2268] = 1`, and 0x3E53DE0 copies it unchanged. That `+0xE4` is not
  used by anything else on these object types is Unconfirmed: the layout leaves room for it, because the next fields
  used are +0xF8 / +0x104 / +0x10C. Log `+0xE0/+0xE4` of a few objects to check.
* Skinning (0x41) shares counter +0x58. Its job 0x3E15670 and the helpers 0x3E15C50 / 0x3E16830 write only to
  stage-local lists and their own output buffers. Its output is per stage, double-buffered by `frame & 1`
  (0x3CCDAC0). I found no shared per-object writes there. The deeper callees (0x3E16330 and below) were not checked,
  so this stays a test item: watch skinned characters for flicker once the edge is gone.

### 2.5 Culling inputs per view (Confirmed)

* CullStage job 0x3E24970 calls 0x3E246B0. That function finds the view's MOC in the camera entity's per-view list
  (0x3B88E40, matched by view). It takes the **frustum planes from the camera entity's component**:
  `0x17F3A50(world, entity)+8` is passed to 0x3CEE3D0, which copies 6 or 10 planes from that object's +0x210. It
  takes the **camera position from the view's ReadState controller, slot 9** (with agent S's proxies, that is the
  eye position), and the player mask from `[view+0x2268]` (0x201 for index 0).
* Then come the scene query 0x3CD1E80, chunks of 100 (counter +0x2388, then 0x3E22790 per item) and of 200 (counter
  +0x2380, then 0x3E220C0 bucketing). The job waits for those chunks itself. After that: light lists, the push into
  the view's stage 0x47 list (0x3E53EC0 with `[cull+0x2360]`), and the bucket sorts (0x3E26790, 3 x 10 sub-buckets
  plus 4 more).
* The entity handle the proxies copy (+0x318 / +0x320) points at the same camera entity, so **both eyes cull with
  identical planes**. Each view rasterises its own occluders: the eye has its own camera cull entry, and the eye's
  ctx+0x40 gained an occluder predecessor in the later dumps (16:27:32 "freeze": `preds[0x1DE6F443F80]`).

### 2.6 Worker count (Confirmed)

Scheduler ctor 0x4106CF0 counts the bits of the affinity mask at `[[0x604B1C0]+0x20]` and takes
`min(bits, [0x5FBDF2C] - 3)` (1 if `[0x5FBDF2C] < 4`). It then clamps the result to 1..6 and stores it at
sched+0x20. 0x4107560 creates that many threads in two groups, arrays at sched+0x60 and sched+0xE0, 16 slots each.
The start-up call (0x3AC2391) passes -1, which means "use the default". The 10-22 log names exactly six threads for
skeleton work (24144, 42540, 42120, 34920, 19864, 23332). Two things are Unconfirmed: what `[0x5FBDF2C]` holds
(probably the logical CPU count; log it), and whether any per-thread table is sized for at most 6 (search before
raising the cap).

## 3. Frame pipelining

### 3.1 Game thread and render thread: one tick of overlap (Confirmed)

The handshake in section 1 gives this per-tick timeline:
* GameThread: update N+1 starts when ev1 is set, i.e. when every recording job of frame N is done.
* Render thread: frame N+1 starts after ev0 and ev2, i.e. after update N+1 has finished.
* Update N+1 overlaps the render thread's D3D11 execution of frame N, its frame sync and its Present.
* Begin-recording, the job recording phase and the D3D11 execution of frame N+1 can't overlap update N+1.

So the engine keeps exactly one game tick in flight beyond the frame being rendered. There is no deeper ring of
render frames on the CPU: the render thread waits for the frame counter at the end of every 0x3DF8DE0. The rings
that exist are sized for GPU latency, not for CPU queueing: per-object data ring of 3 (0x3E53FA0 `frame % 3`),
skinning output of 2, deferred release lists of 2 (0x40BF540).

Implication: the period is about `game update + (begin-recording + recording critical path) + the D3D11 execution
that doesn't overlap the update`. Recording is the part that blocks both threads, which is why section 2.3 matters.

### 3.2 Where the D3D11 work happens (Confirmed)

All command buffers are translated into D3D11 calls when they are closed (0x407F240, under the renderer lock
`[cmd+0x38]+0x78`). That happens on the render thread inside 0x3DF97C0 -> 0x4073040 -> view slot 4 -> stage slot 8,
in view order. Recording on workers only builds the engine's own command streams. Every draw of both eyes is
therefore submitted by one thread. This part grows linearly with the draw count, and only fewer draws (rows 6 and 7,
agent T's shared passes) reduce it.

### 3.3 Render thread and GPU (Confirmed except the DXGI default)

The render thread never waits for GPU completion: the frame counter covers CPU jobs only. Present (0x40908A0) uses
sync interval 0 plus ALLOW_TEARING when VSync is off (the log shows interval 0, flags 0x200). The only limit on how
far the CPU can run ahead of the GPU is DXGI's maximum frame latency or the driver's. The game doesn't set it (row 4).
At 26 fps and likely GPU-bound, up to 3 queued frames would be about 115 ms between the pose that was used and the
photons. Checking this needs one `GetMaximumFrameLatency` log line plus the GPU timestamps of section 5.

### 3.4 The mod's VR present (from the mod's source)

`render.cpp` mid-hooks the Present call inside FlipBuffers, on the render thread. `headset.cpp on_present` then:
* calls `xr::Session::begin_frame`. With `[xr] async_wait` (default on), a pacer thread calls `xrWaitFrame` right
  after each `xrBeginFrame`. `begin_frame` only waits (up to 500 ms) when no waited frame is ready yet. At 26 fps
  against a 72-120 Hz headset, one normally is, so the render thread rarely blocks here;
* copies the back buffer into the swap-chain images (GPU copy);
* calls `xrEndFrame`. Whether SteamVR's `xrEndFrame` blocks for D3D11 can't be determined statically.

So the VR present is unlikely to block the render thread for long, except inside the runtime's `xrEndFrame`. The
pacing line (`xr: pacing: ...`) and the timers in section 5 settle it. The 10-22 log predates the pacing line.

## 4. Latency facts and the "5 updates" measurement

### 4.1 What "lag 5" counts (Confirmed)

The mod's `camera_view_update` hook sits at 0x4085150. That is the controller's view rebuild (0x4084CB0 / 0x4084DC0
/ 0x4084ED0, slot 9, 0x408BD60, then the +0x10 dirty flag is cleared). It is reached from 0x4086010 / 0x40851F0
(about 30 callers, including picking 0x4085960 and shadow stage code 0x3E1B0C0 / 0x3E1BB10 / 0x3E1C940) and from
0x407BE10 (aspect, via frame sync 0x4073C50, resize 0x40729D0 and viewport placement 0x407B800). Each call pushes
one entry into the mod's applied-view history. The log gives the rate:
* 16:24:28.225 -> 42.186: 1510 head-tracked writes against 302 presents, i.e. **5.0 per frame**;
* 42.186 -> 45.033: 375 writes against 76 presents, i.e. 4.9 per frame.

"Measured lag 5" (16:24:29.406, matched at render view+0x2660) therefore means about one presented frame, which
fits the one-tick pipeline of 3.1. It does not mean 5 frames.

Hazard (mechanism Confirmed, effect Unconfirmed): some of these calls come from render-side code. Stage 0x02's
execute 0x3E1B0C0 runs on the render thread, and its job 0x3E1BB10 runs on a worker. The mod writes the head pose
into the shared controller on every call. The controller can therefore change while the views are being recorded.
Log thread id and return address per call (section 5) before relying on it.

### 4.2 Prediction horizon in the mod (Confirmed, mod code)

`submit_tracked` locates the next head pose at `predictedDisplayTime + predictedDisplayPeriod * (1 + measured)`,
with `measured` = 5 (calls, not frames). At 90 Hz that is about 67 ms ahead. The real horizon is about one game tick
plus the render and GPU time.

### 4.3 Staleness of the pacer's frame state (Confirmed, mod code; timing per the OpenXR spec)

`Pacer::run` calls `xrWaitFrame` as soon as the previous `xrBeginFrame` is done and keeps the result. `begin_frame`
uses it at the next Present, about 38 ms later at 26 fps. The runtime returns from `xrWaitFrame` within its own
frame interval, so the `predictedDisplayTime` used for the next pose can be more than a display period in the past.
This is row 5 of the table.

## 5. One-run measurement plan

Everything below logs aggregates every 60 frames (mean / p95 / max in ms) per thread and per view role (main, eye,
special). Use QPC, keep the async log, and use plain atomics per bucket.

1. **Render-thread phases** (inline hooks or mid-hooks; render thread only, thread id from 0x3DF8DE0's entry):
   * the whole of 0x3DF8DE0;
   * inside it: the three event waits (calls to 0x4108440 at its start), 0x3DF9340 (budgeted queue), 0x3DF9570
     (begin-recording; split per view with a hook on view slot 1 **0x17F21E0**), 0x3DF97C0 (frame end), and the
     final `0x410E880([0x6048680])`;
   * FlipBuffers 0x40908A0, split into the mod's `begin_frame` wait, the blit, `xrEndFrame` and
     `IDXGISwapChain::Present`.
2. **Render-thread D3D11 vs waiting**: hook **0x407F240** (close/execute; time per call, attributed to the view
   whose slot 4 **0x3CC7010** is on the stack, which a TLS set in a 0x3CC7010 hook gives) and **0x410E880** on the
   render thread only (time keyed by counter, named with `views::counter_name`). Expected output per view: "D3D11
   submit X ms, waiting for stage jobs Y ms (largest: counter ...)".
3. **Game thread**: hook App slot 0x288 **0x5889E0** (update time G) and the 0x4108440 call at the end of 0x3F770A0
   (time waiting for ev1).
4. **Worker jobs per stage and view**: hook the job-run functions and key them by `[job-0x18]` (view; for CullStage
   `[job-0xC8]`):
   * cull 0x3E24970 and 0x3E51DE0; 0x47 0x3E53FA0; 0x48 0x3E541B0; 0x41 0x3E15670;
   * 0x0B 0x3E2BD90 and 0x3E2C3B0; 0x46 0x3AECC70, 0x3EBBB50, 0x3EBC0C0, 0x17F1B40;
   * 0x1E 0x3E01E00; 0x02 0x3E1BB10; 0x03 0x3E1F520; 0x05 0x3E06930; 0x1B 0x3E32B50's sub-jobs (A section 4.2);
   * 0x18 0x3E4B680; 0x17 0x3E4C0E0; 0x3C 0x1800B00; 0x36 0x3E45750; 0x44 0x3E47BB0; 0x40 0x3E12770;
     0x37 0x3E4F490; 0x00 0x3D961D0; 0x1F 0x17DB570; 0x33 0x1816520; 0x21 0x3B239B0; 0x3F 0x3E54C00;
     0x35 0x3DFB6A0; 0x3D 0x18126A0.

   For each one log the total, and also the start and end times relative to the counter release (a hook on
   0x407B540). Those timestamps give the critical path directly. The key number: **eye 0x3E53FA0 start minus main
   ctx+0x78 completion**, which is the cost of eyefix (section 2.3).
5. **GPU**: D3D11 timestamp queries (one disjoint query per frame) on the immediate context. Put them at entry and
   exit of 0x3CC7010 for each view, around the special view and around the mod's blit. Read them back 3 frames late.
   This gives GPU ms per view for agents T and X and decides CPU-bound against GPU-bound.
6. **One-off config lines**: `[[0x614FBC0]+0x20]` (workers), `[0x5FBDF2C]`, the affinity mask, DXGI
   `GetMaximumFrameLatency`, `[[0x604AA10]+0x3D..+0x43]`, and the draw count per view (count calls of 0x3CD8640 by
   `[ctx+0x10]`).
7. **Camera update calls**: for each 0x4085150 call on the followed controller, log a per-frame histogram of thread
   id and return address (section 4.1).
8. **A/B in the same run** (hotkey or VR menu, 20 s each, the timers above running): `serialize_eyes` on/off (only
   with row 1 in place), eye stage 0x3E skip on/off, and max frame latency 3/1. Use the index-1 check from 2.4: dump
   `[obj+0x2D0]+0xE0/+0xE4` of 4 visible objects once per second.

How to read it:
* If the render thread is busy (phases 1+2) for close to the frame time and the GPU time is lower, the frame is
  CPU-bound on the render thread: fewer draws (rows 6 and 7, agent T) or less waiting (rows 1 and 2).
* If G is close to the frame time, the game thread is the limit: settings levers.
* If the GPU time per frame is close to the frame time, it is GPU-bound: agents T and X, and row 4 only helps
  latency.

## Raw files

w2/w7 camera update and its callers; w4/w8 render loop, frame sync, end views, frame end, dynamic-resolution
controller, FlipBuffers; w9/w10/w11 main loop and WinMain; w12/w17/w19 vtables (RenderFrame dx11/base, App); w13
deferred release; w20/w21 App slot 83, budgeted queue, scheduler wait/run; w22/w23/w33 CullStage (execute, job,
params, frustum, ctor), stage 0x3E; w24/w26/w27/w28/w46/w47 scheduler start-up and worker cap; w32 skinning helpers;
w34 stage chooser/builder; w37..w44 GameThread, events and the ev1 job; w45 stage 0x3D. T's t2/t4/t5 were read for
stage 0x47 and its readers. Scripts: `W\tools\strgrep.py`, `W\tools\findbytes.py`.

## Open leads

* The identity of the budgeted queue in 0x3DF9340 (`[App+0x1C8]`, 0x41867F0 dispatches item vtable slot 1).
* Readers of TripleBuffering (`[0x604AA10]+0x29`): the displacement scan was too noisy to isolate them.
* Whether the object types with `+0x2F8` = 0/1 handled by 0x3F59B10 / 0x3F5D2E0 are VFX particles (they build
  camera-facing data from controller slots 9 and 11).
* The full output list of CullStage, needed for row 7.
