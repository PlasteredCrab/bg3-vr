# Stereo 3D characters and items in UI screens - agent V, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_F, read-only, headless, request server
`agents\V\VServer.java`), plus the game's DX11 CombineUI shaders (extracted read-only from `Data\EngineShaders.pak`
with the project's LSLib Divine into `agents\V\shaders\`, disassembled with d3dcompiler_47 into `agents\V\shaders\asm\`).
RVAs are relative to 0x140000000. Raw decompiles: `agents\V\v1..v40_*`. Builds on P_ui_character_3d.md (cited "P"),
A_rendering.md ("A"), B_splitscreen.md ("B"), S_decals.md ("S"). "SE" = Script Extender source. "Confirmed" = read
from code, disassembly or shader bytecode at the cited address; "Unconfirmed" items are in the logging plan (section 4).

**Main change to agent P's 3.1 design.** P's 3.1 puts the second eye view into the doll's own frame and makes the frame
2W x H. Two code facts work against that:
* In an offscreen (type 2) frame every view runs its own final composite stage 0x07, and that pass appears to clear
  the whole output render target (1.6). The second view's composite would then erase the first view's half.
* All type-2 frames are rendered one after another and fully finished on the render thread (0x3DF9570, 1.1).

So the recommended recipe is **a second offscreen frame per doll with one GameRenderView** that shares the doll's
scene, atmosphere, lighting and camera. Left eye = the game's doll frame, right eye = the mod's frame. This needs no
2W resize, keeps the game's texture as it is, gives each eye its own TAA and exposure history, and avoids the
stage 0x47 split-index race. The same-frame 2W variant is still documented (2.3, 2.4) in case the clear turns out to
be a load.

---------------------------------------------------------------------------------------------------

## Hook points / calls for the VR mod

| need | RVA | arguments / how | thread | confidence |
|---|---|---|---|---|
| Doll renderer ready | **0x1B72D00**(renderer) | true when camera entity `+0x3B0/+0x3B8` is valid, camera attached to the view (`+0x414`) and scene setup is done (`+0x528`) | game | Confirmed |
| Per-frame point for doll work | after **0x13E1A40** (PM update; existing hook) | iterate `PM+0x270[0..PM+0x26C)`, renderer = `entry+0x70` | game | Confirmed |
| Renderer destroyed | **0x1B76070**(renderer) (dtor body) | reached from PM removal 0x13E1790 and entry release 0x178AAE0 (both inline the PaperdollRenderer dtor), and from 0x178ABD0. Pre-hook: unbind and destroy the mod's eye-2 view/frame | game | Confirmed |
| Frame and view freed | **0x1B76620**(frameIdx, view) | defers itself to queue 2. On the immediate path: `0x40733E0(frame, view)`, GameRenderView dtor body, free `0x41701F0(0x40EA160(), view)`, then `0x3BB1A20(frameMgr, idx)`. Frees only the view it is given. **Use it for the mod's own frame/view too** | queue 2 (self-deferring) | Confirmed |
| Thread gate used by all doll frame/view code | `q = [[[0x604B1C0]+0x148]+0x10]`; `defer = q->vtbl[1](q)` | false: act now. true: push `{fn, payload}` into the queue: EnterCriticalSection(q+0x40); `rec = 0x40F9B30([q+0x10], 8+payloadSize, 8)`; `rec[0] = fn`; payload follows; Leave. `fn(payload*)` returns the payload size (0x1B79390 returns 0x18, 0x1B793C0 0x10, 0x1B795A0 8) | any | Confirmed (OS thread Unconfirmed) |
| Create the eye-2 frame | **0x3BB1880**(frameMgr, &desc, &name) | `frameMgr = [[[0x6046F98]+0x1D8]+0x30 + [0x604C6FC]*0xF8]`; desc 0x30 B: `+0x00 u64 0, +0x08 w,h, +0x10 w,h, +0x18 w,h, +0x20 u64 0, +0x28 int 2, +0x2C byte 0, +0x2D word 0`; name `{const char* "BasicGameObjectRenderer", u32 0x17}`. Returns index, frame = `[[frameMgr+0x18]+idx*8]` (0x43A0 B, vtable 0x58EDD18, `+0x40` = 2). Set `frame+0x42F0 = 0` | queue 2 | Confirmed |
| Create the eye-2 view | **0x4175960**(0x2D90, 0x10), then **0x17F1E60**(view, frame, float sx, float sy, float ox, float oy, const cfg*) | as 0x1B75D50: sx=sy=1, ox=oy=0 (xmm2/xmm3, [rsp+20]/[rsp+28], cfg [rsp+30]); cfg = 0x64-byte copy of view1 `+0x2B70..+0x2BD3` | queue 2 | Confirmed |
| Register view with frame | **0x4073230**(frame, view, 0) | appends to `frame+0x42E0` (count `+0x42EC`) and creates the view's stage-resource record | queue 2 | Confirmed |
| Build its stages | **0x3AC4EA0**([0x6046F98], view) | the base stage list, the same call 0x1B75D50 makes (not 0x588E50). Type-2 frame: adds 0x45 and 0x07, no job-graph links | queue 2 | Confirmed |
| Bind scene/env/camera to the eye-2 view | 0x3CCC550(v2, [view1+0x2B10]); 0x3CC9700(v2, [r+0x3C0], r+0x3C8, 0.0f, 0); 0x3CCAAC0(v2, [r+0x3D0], r+0x3D8, 0.0f, 0); `v2->vtbl[6](v2)`; **0x17F2B30**(v2, ctrl, 0) | mirrors 0x1B740F0 / 0x1B75FC0 / 0x1B76330 for view 1; ctrl = `[view1+0x2260]` | game (as the game does for view 1) | Confirmed calls; combination Unconfirmed |
| Keep the eye-2 environment in sync | post-hooks **0x1B76990** (atmosphere) / **0x1B77150** (lighting), rcx = renderer | the game replaces `r+0x3C0` / `r+0x3D0` and rebinds only view 1; repeat the bind for v2 | game | Confirmed |
| Per-eye camera origin (proxy) | existing hook **0x17F21E0** (begin-recording entry) | doll camera controller is **the same class as the gameplay controller: vtable 0x58B6750, 0x330 bytes** (allocated in 0x3B96300). Slot 9 = 0x3CF0740. The mod's proxy code applies unchanged; only the eye pose source differs | render | Confirmed |
| Per-eye projection | existing hook **0x3CC6CB9** | doll views pass through 0x3CC6C00 like every GameRenderView (0x3DF9570 -> 0x4072B30 -> view slot 1 0x17F21E0 -> 0x3CC6C00). Write off-axis tangents (1.5) | render | Confirmed |
| Doll camera data | renderer `+0x478..+0x483` eye (vec3), `+0x484..+0x48F` target (vec3), `+0x428` FoV used for the request, `+0x42C` eye, `+0x438` rotation quat, `+0x524` FoV (rad); controller `+0x14` FOV, `+0x18/+0x1C` near/far, `+0x20` aspect, `+0x90` view, `+0xD0` inverse view, `+0x110` projection | fit 0x1B748F0 -> apply 0x1B74E90 -> 0x1B77520 | game writes, render reads | Confirmed |
| Output textures | left: **0x1B77C80**(renderer) = `frame+0x42D8` else `frame+0x42C8[frame+0x42C0]`; right: the same two fields of the mod's frame | runtime: R8G8B8A8_UNORM (28), bind SRV or RTV, display size. Pointer changes on every resize (0x408B820 frees, 0x408B4F0 recreates) | render | Confirmed (format from the mod's log) |
| Doll frames finished | after **0x3DF9570** returns (render thread) | 0x3DF9570 renders every enabled type-2 frame in turn: sync (vtbl+0x28 = 0x4072CB0), 0x4072B30, 0x4072C30, 0x4073040 (close and wait), vtbl+0x20/+0x30 | render | Confirmed |
| Background alpha | CombineUI shaders | every variant writes `o0.w = 1.0`. The doll texture is opaque black outside the character. The controller UI hides this with `BlendingMode="Additive"` | - | Confirmed |
| Doll depth | key **0x22F3E110** in `[frame+0x4250]` (frame-owned, created by 0x4073700) | `0x4098180(mgr, 0x22F3E110)` gives the resource object; the D3D texture path and format are not traced | render | creation Confirmed; readback Unconfirmed |
| Hide the 2D doll | existing detour **0x155D9D0** | unchanged | UI | Confirmed (P) |
| Doll rect on the UI | **0x45FC6B0** `Noesis::Visual::PointToScreen(this, Point* out, const Point* in)` (SE signature, single match) | element = LSPaperdoll (track with 0x155DA40/0x155DAC0); URI FixedString `elem+0x298` == `entry+0x08`; pixel size `elem+0x2D0/+0x2D4` = finalSize x min(RenderScale,1) | UI/game | Confirmed |
| Frame resize (same-frame variant only) | **0x178AC60**(entry, w, h) | `frame->vtbl[1]` = **0x408B8B0**(frame, renderW, renderH, displayW, displayH), or deferred 0x1B79500 with `r+0x464 = 1` | game | Confirmed |
| Item icons | **0x2105800**(owner, Brush* out, const FS* icon, const FS* fallback) -> 0x3CC4320([0x6049020] TextureAtlasMap, icon, &uv, &atlas) -> 0x2105CE0 | static atlas ImageBrush with a Viewbox. Nothing is rendered at run time | UI | Confirmed |
| Item in 3D via the doll renderer | 0x1B74250 -> renderer vtbl+0x20 **0x1B77E10** -> 0x188A320 (ecl::Item) | the renderer accepts items. 0x188A320 sets clone `+0x32A = 1`, and 0x1B748F0 has an item-specific framing branch | game | Confirmed |
| Item in 3D in the world | 0x1888C10 + 0x18890D0(clone, level scene entity) + vtbl+0x20 0x188A320 + 0x3C785C0 transform | the same pattern as ecl::StealthVFXComponent (0x19CBD00 / 0x19C97A0 / 0x19C9B10), which uses a character | game | pattern Confirmed; for items Unconfirmed |

---------------------------------------------------------------------------------------------------

## 1. Facts the recipe rests on

### 1.1 How a doll frame is rendered (Confirmed)

* Frame creation 0x3BB1880, type 2: allocates 0x43A0 bytes, ctor 0x4072580(frame, desc, 5), vtable 0x58EDD18.
  `frame+0x40` = desc type (2), render/upscaled/display sizes from the desc. 0x408B4F0 creates the output target into
  `+0x42C8`, and 0x4073700 creates the frame's stage resource manager `+0x4250`, the frame CB and (since
  `+0x4391 = 0x54` from the ctor) the depth key 0x22F3E110.
* Per render frame, 0x3DF9570 (render thread) loops over the frame manager list and, for each frame with
  `+0x40 == 2` and `+0x42F0 != 0`, calls in order: `vtbl+0x28` (0x4072CB0: WriteState -> ReadState for every view of
  the frame), 0x4072B30 (begin-recording `vtbl[1]` for every view with IsActive, ReadState.Enabled and `+0x42F0`),
  0x4072C30 (view `vtbl[2]`), 0x4073040 (close command buffers, then wait on all 17 counters of each view), and the
  frame's `vtbl+0x20/+0x30`. **Each doll frame is finished before the next one starts.** The frame end 0x3DF97C0
  explicitly skips type 2.
* 0x3AC4EA0 only links job-graph edges when `frame+0x40 == 0`. Doll views have no edges to the main views.
* No occlusion object: 0x3CC5AA0 creates `view+0x48` only if `frame+0x40 == 0`. So camera Enter (0x3CF0080) registers
  nothing for doll views.

### 1.2 Lifecycle of a paperdoll renderer (Confirmed)

| event | where | what happens to frame/view/scene/camera |
|---|---|---|
| created | 0x13E1520 (PM add, from LSPaperdoll ArrangeOverride 0x155E160) -> 0x178A8E0 -> 0x1B75A90 | frame + view 1 are made by 0x1B75D50, immediately if `q->vtbl[1]` is false, else queued (0x1B79390). Renderer vtable is still the base 0x5646EE0 during that call |
| scene/camera set up | 0x13E1A40 -> 0x1B72D50 (until 0x1B72D00 is true) | camera entity (0x1B76330 -> 0x3B96300), `0x17F2B30(view1, ctrl, 0)`, `+0x414 = 1`; ls::Scene entity `+0x348`; atmosphere/lighting (0x1B76990 / 0x1B77150, rebound later on change); IBL; 3 lights; `+0x528 = 1` |
| entity changed | 0x13E1A40 -> 0x1B74250 + 0x1B79290(r, 1) | only the clone is replaced; frame, view, scene and camera stay |
| resized | 0x13E1D30 -> 0x178AC60 -> frame `vtbl[1]` 0x408B8B0 | frame resized in place: output textures released (0x408B820) and recreated (0x408B4F0), resource manager resized (0x4095E50), then 0x40729D0 recomputes viewports/aspect and calls `vtbl+0x40` (resize, slot 8) **for every view in the frame list** |
| closed | LSPaperdoll OnDisconnect -> 0x13E16D0 queues removal -> next 0x13E1A40 -> 0x13E1790 | renderer dtor 0x1B76070 (game thread): view1 disabled, controller detached `0x3CC90B0(view1, 0, 0)`, camera entity flagged for deletion, `0x3CCC550(view1, 0)`, clone/lights/IBL released, then **0x1B76620(idx, view1)** |

**Who frees what.** 0x1B76620 removes and frees only the view it is passed. The frame destructor (0x3BB1A20 ->
`frame->vtbl[0]` 0x408B480 -> 0x40727E0) frees the view-list array, sets the count to 0 and destroys the resource
manager. It never touches the views in the list. A mod view left in a game frame would leak, with dangling
`view+0x10` (frame) and `view+0x18` (resource manager). With the recommended separate frame this does not arise: the
mod owns its frame and destroys it itself with the same function.

The renderer keeps exactly one view (`+0x3A8`) and one frame index (`+0x3A0`). Users of `+0x3A8`: 0x1B740F0 and
0x1B75FC0 (scene for the given view, atmosphere/lighting for `+0x3A8`), 0x1B76990 and 0x1B77150 (atmosphere/lighting
rebinds), 0x1B76330 and 0x1B77520 (SetCamera), and the dtor. The only loop over views is in 0x1B740F0: it calls
`vtbl+0x30` (slot 6, 0x3CC86D0) on every view of its frame.

### 1.3 The doll camera (Confirmed)

* Created by 0x1B76330: entity with a random GUID name. **0x3B96300** allocates the controller (0x330 bytes,
  `vtable 0x58B6750`, `+0x310` = owner system, `+0x318/+0x320` = camera entity handle/world) and adds the
  ls::CameraComponent. The view gets `[component+8]` via 0x17F2B30. **The same class and size as the gameplay camera
  controller, so the mod's proxy works as is (vtable check `== base+0x58B6750` passes; copy 0x330 bytes; slot 9
  0x3CF0740 = the camera entity's ECS Transform +0x10).**
* Fit 0x1B748F0 (from the clone visual's AABB `vis+0x40..+0x54`; clone byte `+0x32A` switches to item framing):
  output struct `[0..2]` eye, `[3..5]` look target, `[6..11]` light frame, `[0x12..0x14]` light params. Eye x/y follow
  the target; eye.z = `-(0.5 x size / tan(0.24521877))`; a -0.2 y offset for characters.
* 0x1B744A0 stores the current fit at **renderer +0x478** (eye `+0x478`, target `+0x484`), the blend target at
  `+0x4CC`, the 0.05 s timer at `+0x520`.
* 0x1B74E90 -> **0x1B77520(r, FoV=r+0x524, rotQuat, eye, rot2, lightPoint, 0.1f, 300.0f)**: stores `+0x428` FoV,
  `+0x42C` eye, `+0x438` rotation, `+0x448` light point, `+0x454` rot2; issues the camera request 0x1E1A900 when the
  pose changed; sets near/far on the controller (`+0x18/+0x1C`, or from lighting `+0x280/+0x284`); re-attaches the
  controller to **view 1 only** (0x3CC90B0 if `view+0x2C98 == 0`, `view+0x2CA0`).
* FoV writes: 0x1B74810 / 0x1B78BC0 set controller `+0x14` (degrees -> rad, +5 deg if camera component byte `+0x17`)
  and dirty `+0x11`.

### 1.4 Atmosphere/lighting on a view are copies (Confirmed)

0x3CC9700(view, atmo*, const FixedString* id, float blend, bool force) copy-constructs view-owned objects (0x1B76AA0,
0x378 B) at `view+0x2890/+0x28A0` and compares resource ids (`+8`). The view never keeps the renderer's `+0x3C0`
pointer, so rebinding v2 after the game replaces `+0x3C0` is safe (no dangling pointer). 0x3CCAAC0 is called with the
same argument shape (not decompiled here).

### 1.5 Per-eye off-axis camera (design; inputs Confirmed)

The world's eye matrices cannot be reused for the doll (different scene). Treat the doll's native camera as the
"window":

* Doll camera space: origin = controller eye (camera entity transform; `+0x478` mirrors it), forward = controller
  rotation. Window plane: through the target `T = r+0x484`, at distance `d = |T - eye|`, size
  `h_d = 2 d tan(fov_v/2)`, `w_d = h_d x aspect` (controller `+0x14`, `+0x20`; check whether `+0x14` is vertical the
  same way the mod's camera module does for the gameplay camera).
* VR side: the mod places a window rectangle W (centre C, orientation R, width `w_m` metres) over the hidden 2D doll
  (section 2.7). Scale `s = w_m / w_d`.
* Per eye: `e = R^-1 (E_eye - C) / s` (window coordinates: x right, y up, z toward the viewer, doll units). Proxy
  camera: position `eye_cam = right*e.x + up*e.y + forward*(d - e.z)`, **rotation = the doll camera's own rotation (not
  the head's)**. Projection tangents at unit distance: `left = (-w_d/2 - e.x)/e.z`, `right = (w_d/2 - e.x)/e.z`,
  `down = (-h_d/2 - e.y)/e.z`, `up = (h_d/2 - e.y)/e.z`. With `e = (0,0,d)` this is exactly the game's own frustum.
  The tangents feed the mod's existing `rewrite_block` (which already takes per-eye FOV tangents). The proxy gets
  `+0x90` view, `+0xD0` inverse view and slot 9 = `eye_cam`.
* Present each eye's image on W itself (two quad layers with eye visibility, or drawn into each eye image at W's
  projected rectangle). The off-axis frustum makes the doll appear behind/through W with correct parallax. Content in
  front of the plane works while it stays inside the frustum (near 0.1 doll units).

### 1.6 Why not the same frame (code facts; the clear itself is Unconfirmed)

* Every doll view has its own stage 0x07 (0x3AC4EA0 adds 0x07 when `frame+0x40 != 0`). Its job 0x3E4D2A0 composites
  into the frame's output RT with the view's display viewport (0x407B8C0 with `frame+0x30/+0x34`, mode 2): each view
  would write only its half.
* But the RT set used (`[stage+0x2158]`, built in init 0x3E4C6F0 by 0x17DCB10 / 0x4095F50 from a one-record colour
  descriptor whose `+0x0C = 1`) goes through opcode 0x0B. Its executor 0x40CA2E0 calls
  **ID3D11DeviceContext::ClearRenderTargetView** (vtable +400) when a colour record's `+0x0C == 1`. The clear colour
  comes from the 16 zero bytes at 0x58BE570. That would clear the whole RTV at the start of each view's composite, so
  view 2 would erase view 1's half. Unconfirmed: that 0x4095F50 keeps the record unchanged (log item 1).
* Same frame also means shared keyed resources (TAA history, exposure) and both views using split index 0 in parallel
  (A 12, stage 0x47 race). The mod already has to work around both for the main eyes.

---------------------------------------------------------------------------------------------------

## 2. Implementation recipe (Part 1 steps 1-7)

Notation: `r` = PaperdollRenderer (vtable 0x5646D78), `e` = PM entry (vtable 0x55EE6F0), `F1`/`v1` = the game's doll
frame/view (`[frameMgr+0x18][r+0x3A0]`, `r+0x3A8`), `F2`/`v2` = the mod's. `RunQ2(fn)` = run now if
`q->vtbl[1](q)` is false, else enqueue on queue 2 (hook table row 3).

### 2.1 Step 1 - lifecycle (when to add, when to remove)

1. **Discover**: in the post-hook of 0x13E1A40 (game thread), for each entry with `e->vtbl == 0x55EE6F0`,
   `r = e+0x70`, `r->vtbl == 0x5646D78`: if `0x1B72D00(r)` is true, `r+0x3A8 != 0`, `r+0x3A0 != -1`, and the mod has
   no record for `r` yet: create the record {r, URI = e+0x08, F1 idx, v1, state = Creating} and `RunQ2(create)`.
   Capture everything `create` needs (F1 idx, v1, F1 display w/h `F1+0x30/+0x34`, cfg copy from `v1+0x2B70`,
   `v1+0x2274`) on the game thread so the queued job never touches `r`.
2. **Ready**: when `create` has stored F2/v2, the next game-thread tick does the bind (2.2 step 5) and enables.
3. **Every tick** (game thread, after 0x13E1A40): if `[v1+0x2260] != [v2+0x2260]`, call `0x17F2B30(v2, [v1+0x2260], 0)`.
   Mirror `F1+0x42F0` with `0x1B79150(F2idx, value)` (it defers itself). If `F1+0x30/+0x34` changed: `RunQ2(resize F2)`
   with `F2->vtbl[1](F2, w, h, w, h)` (0x408B8B0).
4. **Teardown**: pre-hook **0x1B76070** (rcx = r, game thread). For a recorded r: `v2+0x21E0 = 0`;
   `if [v2+0x2C98] == 0: 0x3CC90B0(v2, 0, 0)`; `v2+0x2CA0 = 0`; `0x3CCC550(v2, 0)` (the same sequence the dtor does
   for v1). Then **`0x1B76620(F2idx, v2)`**, which defers itself and does remove, delete and frame destroy. Drop the
   record. The game destroys the camera entity right after (the dtor body), so detaching v2 first matters.
   If the record is still Creating (job queued but not run), mark it Cancelled; the queued `create` must check that
   flag and, if set, destroy what it made with `0x1B76620`. Both run in queue-2 FIFO order before the game's own
   queued 0x1B76620 for F1.
5. Screen close, entity change and resize need nothing beyond 3 and 4 (1.2 table).

### 2.2 Step 2 - adding the eye-2 view (in its own frame)

`create` (queue-2 thread):
1. `desc` as in the hook table (w,h = F1 display size, or larger for sharpness; then resize F1 the same way via
   0x178AC60). `idx = 0x3BB1880(frameMgr, &desc, &name)`. If -1, fail. `F2 = [[frameMgr+0x18]+idx*8]`;
   `F2+0x42F0 = 0`.
2. `v2 = 0x4175960(0x2D90, 0x10)`. When `[0x604B304] != 0` the game also registers the block with its allocation
   tracker (0x4194620 under the critical section at `[0x40EA160()+0xB30]`, map `+0xB38`); do the same. Retail value
   of that flag: Unconfirmed.
3. `0x17F1E60(v2, F2, 1.0f, 1.0f, 0.0f, 0.0f, &cfg)`. The cfg is 0x64 bytes: three 0x20-byte shadow-atlas quality
   records at `+0x00/+0x20/+0x40` (A 9; the doll's are {0x400,0x200,0x80,0x200,40,55,0x100,8}, {0x200,...},
   {0x100,...}), byte `+0x60` = 1 (`view+0x2BD0`, the MOC gate: no effect in a type-2 frame), byte `+0x61` = 0
   (`view+0x2BD1`, read by stage 0x00 and the lighting sub-stages). Copy it from `v1+0x2B70` rather than rebuilding it.
4. `0x4073230(F2, v2, 0)`; `v2+0x2274 = v1+0x2274` (0 for paperdolls: 0x178A8E0 passes p7 = 0);
   `0x3AC4EA0([0x6046F98], v2)`. The root ctor sets Enabled = 1 in both states (0x407B080), so set
   `v2+0x21E0 = 0` and `v2+0x2158 = 0` until bound.
5. Bind (game thread, next tick): `a = [v2+0x28A0]` (exists after the ctor; 0x1B740F0 writes it right after view
   creation); `*(u64*)(a+0x364) = 0; *(u32*)(a+0x36C) = 0; *(u8*)(a+0x370) = 1` (as 0x1B740F0; meaning
   Unconfirmed); `0x3CCC550(v2, [v1+0x2B10])` (the same scene object 0x1852350 returns for `r+0x348`); if `[r+0x3C0]`:
   `0x3CC9700(v2, [r+0x3C0], r+0x3C8, 0.0f, 0)`; if `[r+0x3D0]`: `0x3CCAAC0(v2, [r+0x3D0], r+0x3D8, 0.0f, 0)`;
   `v2->vtbl[6](v2)`; `0x17F2B30(v2, [v1+0x2260], 0)`; `v2+0x21E0 = 1`; `0x1B79150(F2idx, F1+0x42F0)`.
6. Post-hooks 0x1B76990 / 0x1B77150: after the original, repeat the matching 0x3CC9700 / 0x3CCAAC0 for v2.

Free of v2 and F2: only through 0x1B76620 (it uses `0x41701F0(0x40EA160(), view)`, which matches 0x4175960, and
0x3BB1A20 for the frame). Nothing in the renderer keeps or iterates a list of views besides `+0x3A8` (1.2).

### 2.3 Step 3 - frame size

Recommended design: F1 keeps its size; F2 follows F1's display size (2.1 step 3). For sharpness the mod may raise both:
detour **0x178AC60**(entry, w, h) and call the original with `(k*w, k*h)`. It compares against `frame+0x20/+0x24`,
uses the game-thread or queue-2 path itself, and refreshes `entry+0x10/+0x18`. The UI only matters through the hidden
brush (0x155D9D0 is detoured).

Same-frame variant (if log item 1 shows a load instead of a clear): F1 to 2W x H by the same detour, with
`(2w, h)` -> `0x408B8B0(F1, 2w, h, 2w, h)` (args: render w/h -> `+0x20/+0x24` and upscaled `+0x28/+0x2C`, display w/h
-> `+0x30/+0x34`). The PM calls 0x178AC60 again only when the LSPaperdoll's arranged pixel size changes
(ArrangeOverride stores `(w,h)` in PM map `+0x2D8` when `elem+0x2D0/+0x2D4` change; 0x13E1D30 applies it). The detour
keeps the doubling permanent. While a deferred resize is pending, `r+0x464 = 1` and 0x1B77C80 returns null. 0x1B79500
clears it after `frame->vtbl[1]`.

### 2.4 Step 4 - per-view state

Recommended design: v1 and v2 both keep Scale (1,1) / Offset (0,0). Each has its own frame, keys, TAA history,
exposure and job context. The eyefix exposure skip is not needed for dolls (it would even be wrong: separate frames
have separate exposure buffers).

Same-frame variant: `0x407B800(v1, {0.5,1}, {0,0})`, `0x407B800(v2, {0.5,1}, {0.5,0})` (WriteState `+0x21E8` scale,
`+0x21F0` offset; it recomputes the pixel rects and the camera aspect from the display viewport, 0x407BE10). The two
views would share TAA history and exposure keys and race on stage 0x47 object slots (split index 0 for both). Link
v2's stage 0x47 counter (`[v2+0x40]+0x58`) behind v1's end of recording (`[v1+0x40]+0x78`) with 0x41074A0, as eyefix
does for the main eyes.

### 2.5 Step 5 - per-eye cameras

1. Game thread, after 0x13E1A40: compute `e_left/e_right` (1.5) from the current head pose and W, plus `d`, `w_d`,
   `h_d` from `r+0x478/+0x484` and the controller. Store them keyed by view (v1 = left, v2 = right).
2. Render thread, hook 0x17F21E0 (view = rcx): for v1/v2, read `ctrl = [view+0x21D8]`, check
   `vtbl == base+0x58B6750`, copy 0x330 bytes into a ring proxy, set the proxy vtable (slot 9 -> eye position),
   `+0x90` = eye view (doll camera rotation, eye position), `+0xD0` = its inverse, store into `[view+0x21D8]`.
   This is the same code path as the main eyes; only the pose source differs.
3. Hook 0x3CC6CB9: for v1/v2, rewrite both PerCamera blocks with the off-axis tangents. Also apply the stage 0x1B /
   0x40 hooks (N) to these views, as for the main eyes.
4. The doll clone spins on the game's input (`r+0x40C`; `r+0x404` absolute yaw); the camera itself is static except
   for the 0.05 s refit blend, so `d`/`w_d` change rarely.

### 2.6 Step 6 - output, background, depth

* Left = `0x1B77C80(r)`. Right = `F2+0x42D8` if non-zero, else `F2+0x42C8[F2+0x42C0]` (same formula). Both are rf
  texture objects whose first qword is the ID3D11Texture2D (the mod's `find_texture` found it at `+0x0`); 28 =
  R8G8B8A8_UNORM, bind SRV or RTV. Re-fetch after every resize. Copy after 0x3DF9570 returns (render thread, same
  immediate context, so it is ordered after the doll commands, A 1.5).
* **Background: opaque.** CombineUI.bshd, CombineUI_NoUI, CombineUI_HDR and CombineUI_NoUI_HDR all end with
  `mov o0.w, l(1.0)`. The scene background is black (the mod read 00 00 00 FF). There is no per-renderer clear colour
  or screen-type switch that changes alpha. The game's controller XAML draws the doll with
  `noesis:Element.BlendingMode="Additive"`, where black adds nothing. The other corner value (alpha 00, doll 2, clone
  handle still invalid, yaw 0) is most likely a texture not yet rendered (Unconfirmed; log item 6).
* Getting transparency, in order of preference:
  1. Depth mask: depth key **0x22F3E110** of each doll frame's resource manager `[F+0x4250]` (created by the frame
     in 0x4073700, sized from the frame). After 0x3DF9570, copy it and use `depth != far` as alpha. Resource object:
     `0x4098180(mgr, 0x22F3E110)`; the D3D path and format are Unconfirmed (log item 5). Linear depth (key 0xFF193183)
     is a stage key and may be aliased, so do not rely on it.
  2. Additive compositing (what the game does), if the mod composites the doll into its own panel/eye images.
  3. Luminance key on near-black: last resort (dark armour loses detail).

### 2.7 Step 7 - where the doll sits on the UI

* Track LSPaperdoll elements with hooks on OnConnectToView **0x155DA40** / OnDisconnect **0x155DAC0** (this = element).
  Match `*(int*)(elem+0x298) == *(int*)(entry+0x08)` (both are the URI FixedString; 0x155E160 passes `elem+0x298` to
  0x13E1520, and ArrangeOverride uses it as the key of map `+0x2D8`).
* Size in DIPs: `elem+0x2D0/+0x2D4` (int pixels) divided by `min(RenderScale, 1)`. RenderScale =
  `*(float*)0x45EE7B0(elem, [0x6005DE8], 4)`, the DP getter ArrangeOverride uses. The geometry `elem+0x2A0` is set to
  (0, 0, finalSize).
* Position: **0x45FC6B0**(elem, &out, &in) = Noesis::Visual::PointToScreen (SE BinaryMappings signature, single
  match). Hidden return pointer in rdx, input in r8 (the SE disassembly comments, matching the prologue). Call it for
  (0,0) and (w,h) on the UI thread (right after 0x13E1A40 is the same thread that runs ArrangeOverride consumers).
  "Screen" is the Noesis view surface. That it equals the UI texture the mod puts on the VR panel is Unconfirmed
  (log item 8).
* Visibility: an element can be connected but in a collapsed or animating panel. Use the PM entry's active byte
  `entry+0x79` (0x13E1A40 only updates entries with it set; its writer was not found, 4 item 7) together with
  `F1+0x42F0`.

---------------------------------------------------------------------------------------------------

## 3. Part 2 - items

### 3.1 Item icons are static atlases (Confirmed)

* The XAML binds `Icon` brushes (`Background="{Binding Icon}"`, `ImageBrush ImageSource="{Binding Icon.ImageSource}"
  Viewbox="{Binding Icon.Viewbox}"`), for example Container.xaml:39 `ls:LSEntityObject EntityRef=... Background=
  {Binding Icon}` and DataTemplates.
* The view-model side builds them with **0x2105800**(owner, Brush* out, const FixedString* icon,
  const FixedString* fallback) (28 callers in UI VM code 0x12EC..0x17C1). It looks the icon up in **ls::gTextureAtlasMap
  `[0x6049020]`** (SE symbol; Load 0x3CC50E0 at 0x18BC421) with 0x3CC4320(map, name, &uv[4], &atlas). Not found ->
  empty ImageBrush. Found -> 0x2105CE0: a Noesis ImageBrush with an ImageSource for the atlas texture (identifier at
  `atlas+0x60`, registered through 0x20F3D00), Stretch 3, ViewboxUnits 1, Viewbox = (u1, v1, u2-u1, v2-v1).
  The atlas definitions come from TextureAtlasInfo / IconUVList / IconUV data (serializer 0x3F18620/0x3F18A40,
  bank 0x3BB3860).
* So no item icon is rendered at run time. Items appear as 2D atlas cells in inventory, trade, loot, hotbar and
  tooltips.

### 3.2 Paths that render an item's 3D model (Confirmed except where noted)

* Only two BasicGameObjectRenderer subclasses exist (callers of ctor 0x1B75A90): PaperdollRenderer (0x178A8E0) and
  the 300x300 character-icon renderer (0x1E99CB0, character only).
* **The paperdoll renderer accepts items:** 0x1B74250(r, EntityRef) tests item-ness (0x12989E0) and calls
  `vtbl+0x20` 0x1B77E10 -> **0x188A320**(clone, ecl::Item*). That sets source = `item+0x10`, `clone+0x32A = 1` (item),
  the visual resource from the item template (0x1C7CC90 / 0x1C7CBF0 `+0x74`, 0x1C79450), then builds the clone with
  0x188B510. The camera fit 0x1B748F0 has a dedicated item branch (`+0x32A`: centre framing, no -0.2 offset,
  different minimum size). Which shipped XAML, if any, binds an item to an LSPaperdoll was not traced (Examine and
  Trade bind `EntityHandle`; Unconfirmed whether that is ever an item).
* No "ItemPreview" string exists in the exe (exact-string search). An item-preview fade type could not be found.
* World-space clones of the visual-clone base (0x1888C10, vtable 0x55FC810: `+0x18` 0x188A4C0 from character,
  `+0x20` 0x188A320 from item, `+0x00` 0x1888FB0 deleting dtor) used by the game:
  **ecl::StealthVFXComponent** (component type idx 0x60531AC, 0x4D8 B, clone at `+0x130`; created by 0x19CBD00 /
  0x69EC20). Its setup **0x19C97A0** sets `clone+0x32C = 1`, `+0x32B = 0`, calls 0x188CCE0, then
  **0x18890D0(clone, *(u64*)[level+0x28])** (the level's scene entity; the level is found in the level manager's map
  `+0x98/+0xA0` by level name), then `clone->vtbl[+0x18](clone, character)`. Its transform is written per frame in
  **0x19C9B10** with **0x3C785C0**(TransformSystem, ref, &pos, &rot, &scale, 1, 1). 0x1BD4CB0 makes another
  level-scene character clone for a player character (purpose Unconfirmed). This answers P's open lead 4: the main
  world's scene entity is `*(u64*)[level+0x28]` (Confirmed from these two users).

### 3.3 Recommendation for items

**A. Hovered or selected item (one at a time): reuse the paperdoll renderer, with the same stereo recipe as Part 1.**
* Create a mod-owned PM entry: game thread, `0x13E1520(PM, &ref, &uri, w, h, 0)` with `ref` = 16 bytes
  {item entity handle, client ECS world `[[0x5FDA240]+0x1A8]`} (copied to `entry+0x58`) and `uri` = a mod FixedString
  (e.g. `Assets/Uri/Paperdoll/vr-item-<n>`, created with the game's FixedString constructor). It takes the PM lock
  `PM+0x2B0` itself. From then on 0x13E1A40 drives it like any doll: set-entity 0x1B74250 (item path), scene setup,
  item-framed camera, spin `+0x40C`, output texture.
* Change the item: write the new handle to `entry+0x58` and set `entry+0x78 = 1` (dirty). The update re-runs
  0x1B74250 when `+0x78` is set or `[r+0x10] != entry+0x58`.
* Remove: `0x13E16D0(PM, &uri)` (queues removal; the next update destroys it, which fires the 2.1 teardown hook).
* Gate: 0x13E1A40 only processes entries with `entry+0x79 != 0`. The writer of `+0x79` was not found (4 item 7).
  Read it after creation; if it stays 0, the mod sets it.
* Cost: one extra small deferred render per eye. Fine for one item, too much for a whole grid.

**B. All visible inventory items: clone each item's visual into the main world next to the panel** (native stereo, no
extra render, depth correct, but lit and occluded by the level). Exact calls, mirroring StealthVFX (game thread):
1. `obj = 0x4188700(0x40EA160() + 0xE8, 0x3A0, 0x10, &tmp)` (the allocator 0x1BD4CB0/0x1EDB480 use), then
   `0x1888C10(obj)`.
2. `0x18890D0(obj, *(u64*)[level+0x28])` with `level = [[0x6007A28]+0x90]` (current level, B) or the map lookup
   0x19C9B10 uses.
3. `obj->vtbl[+0x20](obj, item)` (0x188A320) with `item = 0x793890(world, handle)` (as 0x1B74250 calls it). The visual
   load is asynchronous (0x188B510 requests it).
4. Each frame: `0x3C785C0(sys, &ref, &pos, &quat, &scale, 1, 1)` with
   `sys = *(u64*)([[[0x6046F98]+0x1D8]+0x30] + [0x6058CBC]*0xF8)` and `ref` built for the clone entity `obj+0x08` as
   0x19CB600 builds it (`+0x00` handle, `+0x08` world, `+0x10` Transform* or null, byte `+0x30` = may look up).
   Transform layout: quat `+0x00`, position `+0x10`, scale `+0x1C` (clamped >= 0). Scale shrinks large items to
   panel size.
5. Remove: `obj->vtbl[0](obj, 1)` (0x1888FB0), as 0x1BD4CB0 does when replacing its clone.
Do not use the paperdoll renderer's own scene for this (P 3.3: its 3 strong lights would enter the world).

---------------------------------------------------------------------------------------------------

## 4. One-run logging plan (everything Unconfirmed above)

Log on first occurrence, then every 300th frame. Use `[ui] paperdoll = diagnose` plus a `stereo_dolls = log` switch.

1. **Composite clear (decides same-frame vs separate frame).** At 0x13E1A40 for a ready doll: `stage07 =
   0x407AE00(v1+0x18, 0x07)`; `pso = [stage07+0x2158]`; dump `[pso]` 0x30 bytes (the colour record: expect `+0x0C = 1`
   for clear, `+0x28 = 0`) and the `0x2188` byte (UI vs NoUI variant).
2. **Threads.** `GetCurrentThreadId()` in: the 0x13E1A40 post-hook, the first immediate 0x1B76620, a queued
   `create` job, the 0x17F21E0 hook for v1, and Present. Expect create/destroy == the 0x3DF9570 thread.
3. **Creation of F2/v2.** idx, `F2+0x40` (2), `F2+0x20..+0x34` sizes, `F2+0x42F0`, `[F2+0x42EC]` (1), v2 stage
   count `[v2+0x34]`, `v2+0x2B10 == v1+0x2B10`, `[v2+0x2260] == [v1+0x2260]`, both ReadState rects (`+0x2180/+0x2184`)
   after the first sync, `[v2+0x28A0]` / `[v2+0x28D8]` non-null after the bind.
4. **Per-eye camera.** In 0x17F21E0 for v1/v2: controller vtable (expect base+0x58B6750), slot 9 before and after the
   proxy, `d`, `w_d`, `h_d`, `e_left/e_right`, the tangents. In 0x3CC6CB9: hit count per doll view.
5. **Depth.** After 0x3DF9570: `res = 0x4098180([F+0x4250], 0x22F3E110)`. Dump 0x80 bytes, find the D3D object
   (the mod's `find_texture`), log its desc (format, bind, size == frame size?), and read the centre and a corner
   (expect centre < far, corner = clear value).
6. **Alpha.** Read 4 pixels (corners, centre) of both outputs after 0x3DF9570, plus the clone handle `r+0x08`.
   Expect alpha 0xFF everywhere once the clone exists.
7. **Entry `+0x79`.** For game entries and a mod-made item entry: `+0x54`, `+0x55`, `+0x78`, `+0x79` at creation and
   for the next 10 updates.
8. **UI rect.** PointToScreen (0,0) and (w,h) for each tracked element, `elem+0x2D0/+0x2D4`, RenderScale, and the
   mod's UI texture size.
9. **Environment rebinds.** Counts of 0x1B76990 / 0x1B77150 per renderer and of the mod's v2 rebinds.
10. **Clone position.** Transform of `r+0x08` (via `0x140E59570(world, handle)`): expect position (0,0,0), rotation
    from `r+0x400..+0x408`. This confirms the window plane through the target.
11. **World clone (B).** For one item: `obj+0x08` handle after `vtbl+0x20`, `obj+0x348`, visual loaded after N
    frames, and the Transform after 0x3C785C0 (scale field honoured?).

---------------------------------------------------------------------------------------------------

## 5. Raw files (agents\V\)

v1 teardown (0x3BB1A20, 0x4073230, 0x17F2060, 0x1B793C0, 0x1B79500, 0x3BB1880, 0x4072580), v2 vtables
(frame 0x58EDD18 / 0x58EDA30 / 0x58EE490, renderer 0x5646D78 / 0x5646EE0), v3 frame (0x40733E0, 0x408B480, 0x408B8B0,
0x408B4F0, 0x4073700), v4 stages (0x3AC4EA0, stage 0x07 0x3E4C6F0 / 0x3E4D0F0 / 0x3E4D2A0, stage 0x45), v5 (0x40729D0,
0x17F2B30, 0x17F1E60, 0x3CC90B0, 0x3DF97C0, 0x4073040), v6 callers, v7 frame loops (0x3BB1DC0, 0x3DF9570), v8
(0x40727E0, 0x4072B30, 0x4072C30), v10 queue users, v11-v19 icon atlas path, v21-v24 visual-clone users, v29/v31
displacement scans, v33 set-RT / executor, v34 RT descriptor, v35 clone base / camera controller vtables, v36
transform setters, v38/v39 StealthVFX clone, v40 0x3CC9700. Shader disassembly: `shaders\asm\CombineUI*.asm`.
Scripts: `py\sigscan.py` (wildcard byte scan), `py\hexdump.py`.

## Open leads

* Who sets PM entry `+0x79` (only reader: 0x13E1A40). Not in the entry class, the PM, LSPaperdoll or the
  TextureProvider (displacement scans v29/v31).
* 0x3CCAAC0 was not decompiled (assumed to mirror 0x3CC9700).
* The meaning of the atmosphere tweak `+0x364..+0x370` written by 0x1B740F0.
* Whether 0x1BD4CB0 (a level-scene clone of a player character) is a reusable photo-mode or preview object.
