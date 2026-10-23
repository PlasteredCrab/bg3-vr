# BG3 rendering system (DX11, 4.1.1.7631656) - agent A report

Static analysis only (Ghidra project `BG3_A`, headless; raw decompiles in `re-local\ghidra\agents\A\` and
`agents\A\dec\`). RVAs are relative to image base 0x140000000. "Confirmed" = read from decompiled code or
disassembly at the cited address. "Unconfirmed" items say what would confirm them.

Status (2026-10-19): section 1 answers the priority question as far as static analysis allows (the
remaining decision needs one of the runtime checks in 1.7); sections 2-13 are the reference manual.
Not yet covered in depth: individual post-process step internals, the Noesis UI renderer, the
splitscreen ECS systems' update functions, and shader-side behaviour (`.bshd` files are not in the exe).

Graphics settings object `DAT_14604aa10` (fields used by the code above): `+0x43` shadow quality index,
`+0x77` / `+0x80` anti-aliasing / upscaler modes (2,3,5 / 2,3,4 select the jittered PerCamera block and
the upscaled resource sizes), `+0x4B` frame CB flag, `+0x2C` and `+0x36` used by compositing/lighting.

Tools left in `agents\A\scripts\`: `AServer.java` (persistent headless query server, copy of agent B's),
`q.ps1` (send a request), `summarize.py` (keys/strings/opcodes per decompiled function),
`opscan.py` (command opcode emitters), `nearstr.py` (strings near an address).

---------------------------------------------------------------------------------------------------------

## 1. Priority question: why the split view's G-buffer draws produce no pixels

### 1.1 Short answer (what the code shows)

* The split view's culling runs **without occlusion culling** because it has no Masked Occlusion Culling
  object at `[view+0x48]`. That fully explains the larger draw count (359 vs 238) and nothing else:
  confirmed below (1.3). It does not explain missing pixels.
* Nothing in the CPU side of the G-buffer pass is per-view in a way that could reject every pixel:
  the split view's draws go through exactly the same recording code as the main view's, with the
  split view's own constant buffers, viewport and camera (1.4).
* The commands of all views are translated in a fixed order (frame -> main view -> its stages ->
  split view -> its stages -> special view -> its stages) on one context at command-buffer close time,
  so the split view's draws see the constants the split view uploaded itself; a shared constant buffer
  with the special view (if the split view object really is a byte copy of it, see 1.6) would not be
  overwritten before the split view's draws execute (1.5).
* The opaque G-buffer geometry, the depth-first objects and the linear depth are all produced by one
  stage, **0x0B** (sub-jobs 0x3E2BD90: `m_DepthFirstObjects` + `::Fade`; 0x3E2C3B0:
  `m_OpaqueObjects::Default/Fill`, `m_OpaqueCustomObjects::Default/Fill`; init loads
  `LinearizeDepth.bshd`; binds the four G-buffer targets 0x4C0D1B61, 0x3B0A2BF7, 0xA2037A4D, 0xD5044ADB,
  depth 0x22F3E110, 0xFF193183, 0x5BE0FB7A). Stage 0x46 then adds deferred decals (sub-renderers 0x0C /
  0x0D) and the game's objects (0x3B) into the same G-buffer. If the trace's R32F target is the linear
  depth (unconfirmed which key it is), its value 1.0 means the split view's depth has nothing in it
  either, i.e. the failure is common to all geometry, not specific to G-buffer state. The inputs common
  to all geometry passes are: the per-view constant
  buffers bound by the view's vtable slot 16 (b13 frame, b12 PerView, b11 PerCamera, b7 "focus"), the
  per-pass viewport constant buffer (b9, carries the split-screen index), the camera-relative instance
  transforms (camera position from `[view+0x21D8]` slot 9), the viewport, and the depth target.
* Of these, the only ones the split view computes differently from the main view are the **PerCamera
  blocks (b11)**, which the mod's stereo hook at 0x3CC6CB9 rewrites per eye, and the PerView/PerCamera
  inputs `RS+0x2180/+0x2184/+0x21A0` (render/upscaled viewport sizes) used by the jittered projection
  0x3CCCFD0. **Most likely cause (unconfirmed): the matrices in b11 (PerCamera, slot 11) of the split
  view are wrong (degenerate, NaN or with the eye's geometry behind the near plane), so every vertex is
  clipped.** The mod's stereo hook rewrites exactly these matrices for the view assigned to eye 1, from
  that eye's FOV tangents (`2/(r-l)`, `2/(u-d)`); a zero FOV for eye 1 yields inf/NaN (1.7 item 2).
  Decisive checks are in 1.7; the cheapest is to dump VS cbuffer slot 11 and 12 at one split
  G-buffer draw and at one main G-buffer draw and diff them, and to rerun with the stereo hook disabled
  for the split view (it should then render the main image in its half).

What I could not decide statically is listed in 1.8.

### 1.2 Pass order inside a normal view (confirmed stage roles used here)

Main-view stage list: `47 41 08 48 35 1E 3A 05 0B 3D 46 36 44 18 3C 17 40 37 02 03 1B 1A 00 1F 33 21 3E 3F 0A`.

| id | role (evidence) |
|----|-----------------|
| 0x47 / 0x48 | per-object GPU data ring buffer (3 x 0x20000 entries, allocated in init 0x3E53CB0; sizes per object 0x3E53FA0; upload 0x3E541B0) |
| 0x41 | GPU skinning / blend shapes compute (`BlendShapeCompute.bshd`, `SkinningCompute.bshd`, init 0x3E15070) - the ~45 dispatches at the start of end-recording |
| 0x08 | CullStage (`CullStage::m_OpaqueObjects::Default`, `m_DepthFirstObjects`, ... ctor 0x3E229C0) |
| 0x0B | **opaque G-buffer geometry** + depth-first objects + linear depth (`FadePattern.bshd`, `LinearizeDepth.bshd`, `DeferredDecal_ExtractNormal.bshd`; init 0x3E2B3E0 binds G-buffer keys 0x4C0D1B61, 0x3B0A2BF7, 0xA2037A4D, 0xD5044ADB, depth 0x22F3E110, 0xFF193183, 0x5BE0FB7A, 0x1AF4071A). Sub-job 0x3E2BD90 draws CullStage `m_DepthFirstObjects` (cull+0xBB8) and `::Fade` (+0xC28); sub-job 0x3E2C3B0 draws `m_OpaqueObjects::Default` (+0xCA0), `::Fill` (+0xD10), `m_OpaqueCustomObjects` (+0xD88, +0xDF8). RT set at stage+0x63E0/+0x63F0, viewport from the depth target's size and the view rect (0x407B8C0), per-view CBs via view slot 16 (cmd,1,0), depth pass CB at b10 |
| 0x46 | deferred decals into the G-buffer (sub-renderers id 0x0C vt 0x58D2520 job 0x3EBBB50: `m_DeferredDecalObjects` +0x2048; id 0x0D vt 0x58D25B0 job 0x3EBC0C0: `m_DeferredGameDecalObjects` +0x20B8) and the game's G-buffer objects (id 0x3B, job 0x17F1B40, inserted before 0x0D by 0x3AEC1F0) |
| 0x18 / 0x17 | emissive passes into key 0x2EDB4AC1 (0x18: `m_EmissiveObjects` +0x1EF8, `m_EmissiveDecalObjects` +0x1F68; 0x17: `m_EmissiveGameDecalObjects` +0x1FD8); job 0x3E4B680 is used below as the worked example of the draw path (same functions as 0x0B and 0x46) |

### 1.3 [view+0x48] = Masked Occlusion Culling (MOC) and the camera's per-view cull entry - confirmed

* The exe contains Intel's MOC (`.?AVMaskedOcclusionCulling@@` at 0x5F27980, SSE2/SSE41/AVX2 variants).
  `0x41A1BD0` constructs the per-view wrapper (vtable 0x59116E0): `FUN_144efa700(2, alloc, free)`
  (MaskedOcclusionCulling::Create), near clip 0.01 (`0x3c23d70a`), 100.0 (`0x42c80000`), a job counter
  (`0x4107090`), and per-thread tiles (`DumpToFile` jobs 0x5911738).
* Only creator: the base view constructor 0x3CC5AA0, at 0x3CC657F, and only when
  `[[view+0x10]+0x40] == 0 && [view+0x2BD0] != 0`:
  ```c
  if ((*(int *)(param_1[2] + 0x40) == 0) && (*(char *)(param_1 + 0x57a) != '\0')) {   // +0x2BD0
      ... plVar14 = FUN_1441a1bd0(alloc(0x160));
      param_1[9] = plVar14;                                                            // +0x48
      (**(code **)(*plVar14 + 8))(plVar14, frame+0x4308, frame+0x430C, width);          // slot 1: resolution
  }
  ```
  `+0x2BD0` is byte 0x60 of the 0x64-byte creation config copied to `+0x2B70..+0x2BD3` by the same
  constructor (`param_1[0x56e..0x57a] = param_7[...]`). The special view's config has it 0, so a copy of
  the special view has no MOC.
* Camera Enter (gameplay controller slot 1, 0x3CF0080) registers the view with the camera entity only if
  `[view+0x48] != 0`:
  ```c
  if (*(longlong *)(param_2 + 0x48) != 0) {
      plVar5 = FUN_143b88e40(world, camera_entity);          // per-camera list of view entries
      flags = view+0x2268 == 0 ? 0x201 : (== 1 ? 0x401 : 1);
      lVar7 = FUN_143f06cc0(alloc(0x18), view, flags);       // entry {view, cull job, flags}
      append(plVar5, lVar7);
  }
  ```
  `0x3F06CC0` creates a 0x180-byte cull job (vtable 0x58D7FF0, run 0x3F38A40 = rasterise occluders into
  the view's MOC) with its own counter, and links it into the view's job graph:
  `FUN_1441074a0(sched, [[view+0x40]+0x40], counter)` (view ctx+0x40 waits for the occluder job).
* CullStage's per-frame setup 0x3E246B0 looks the view up in that list and takes the MOC from it:
  ```c
  plVar14 = FUN_143b88e40(cam[100], cam[99]);
  for (entry in list) if (entry->view == stage->view) { uVar22 = *(entry->view + 0x48); break; }
  ...  uStack_190 = uVar22;            // MOC pointer in the cull parameters (0 for the split view)
       uStack_170 = 0x201/0x401/1;     // player visibility mask from [view+0x2268]
  FUN_143cd1e80(scene, &params);       // scene cull query (frustum + optional MOC)
  ```
  So the split view culls with frustum only -> more visible objects -> more draws. Consistent with the
  trace (359 vs 238).

### 1.4 What a geometry draw depends on (confirmed path; shown for stage 0x18's job 0x3E4B680 - the G-buffer sub-jobs 0x3E2BD90 / 0x3E2C3B0 (stage 0x0B) and 0x3EBBB50 / 0x3EBC0C0 (stage 0x46) build the same render context and call the same 0x3CEE160 / 0x3CD87B0 / 0x3CD43D0 / 0x3CD8640 sequence; 0x3E2BD90 calls view slot 16 with (cmd, 1, 0))

```
0x3E4B6D7  FUN_144097ab0(mgr, &srv, 0xFF193183)      // SRV of key 0xFF193183
0x3E4B6F7  FUN_144097ab0(mgr, &srv, 0x5BE0FB7A)      // SRV of key 0x5BE0FB7A
0x3E4B768  call [view_vtable+0x80](view, cmd, 1, 1)   // GameRenderView slot 16 = 0x17F26F0: bind per-view CBs
0x3E4B77A  0x4098180(mgr, 0x2EDB4AC1) -> 0x17DD720    // pass CB of the RT set at b10
           render context: cmd, view, stage, stage 0x47 of the view (0x407AE00(view+0x18,0x47)),
                           camera [view+0x21D8], CullStage lists (+0x1EF8 / +0x1F68)
           0x3CEE160 per object (sort key incl. fade level), 0x3CD9BA0 sort,
           0x3CD87B0 instance data, 0x3CD8640 draw (object vtable +0xA0)
           0x407F3C0 opcode 0x0B (set RTs, no clear: empty clear list), no viewport (rect = -1)
           0x40814F0 -> 0x4081620 -> 0x40816E0 (viewport CB b9) + 0x407FED0 (opcode 0x1F viewport)
           0x3CD43D0 (render list execution with per-bucket state descriptors 0x58C6FC8/0x58C6FE0)
           0x3CCDAC0 (binds stage 0x41's skinning output UAV/SRV)
```

Per-view bindings made by slot 16 (0x17F26F0), opcode 6 = bind constant buffer; the 8-byte slot field
encodes VS slot (+0x11), PS slot (+0x15), CS slot (+0x16), confirmed in the executor 0x40C7F20:

| CB | source | slot | uploaded by |
|----|--------|------|-------------|
| frame CB `[[frame+0x42A8]]` | 0x20 bytes from frame+0x4288.. | b13 (VS/PS/CS) | 0x4072CB0 (frame sync), frame's own command buffer |
| PerView `[view+0x2820]` or `[view+0x2838]` | 400 bytes from view+0x2340 / +0x24D0 | b12 | 0x3CCDD00 in begin-recording 0x3CC6C00 |
| PerCamera `[view+0x2850]` or `[view+0x2868]` | 0xE0 bytes from view+0x2660 / +0x2740 | b11 | 0x3B97160 in begin-recording |
| "focus" `[view+0x2C80]` | 0x80 bytes from view+0x2C00.. | b7 (VS/PS) | 0x17F2B80 (GameRenderView begin-recording) |
| scene SRV `+0x2B20..+0x2B30` (if `[view+0x2B18]`) | | t124 (VS/PS) | 0x17F26F0 itself (opcode 0x18) |
| pass CB of key 0x2EDB4AC1 | | b10 | 0x17DD720 |
| viewport CB | 0xB0 bytes, includes viewport rect, UV ranges and **split index `[view+0x2268]` at +0x38** | b9 | 0x40816E0 |

The second block of each pair (+0x2838 / +0x2868, the jittered projection) is used when
`[DAT_14604aa10+0x80] in {2,3,4}` or `[+0x77] in {2,3,5}` (upscaler / TAA modes), else the first.

Camera-relative instance data: 0x3CD87B0 writes 0x50 bytes per instance (3x4 world matrix rows + two
vec4 from object +0x1E8..+0x204) using the camera position from `[ctx+0x30]` = `[view+0x21D8]` vtable
slot 9 (`(**(code **)(*(longlong *)param_2[6] + 0x48))()`), so positions are relative to the view's
camera; the PerCamera block +0x80 holds projection x view-rotation (translation dropped, see 3.3).

### 1.5 Command execution order and the context (confirmed)

* Every command buffer (`0x407F110` / `0x40C8730`) records opcodes with 0x40F9B30. Closing it
  (`0x407F240`) takes the renderer lock `[cmd+0x38]+0x78`, walks the stream and executes each command
  with `0x40C8A10` on the context at `[cmd+0x48]` (set in 0x40C8730 from `[[renderer+0x478]+8]`), then
  resets the state cache (`0x40C82F0(cmd+0x58)`). Unconfirmed: that `[[renderer+0x478]+8]` is the
  immediate context (one context for all command buffers); a D3D11 trace shows it (one context used for
  all draws).
* Close order per frame (frame end 0x3DF97C0 -> 0x4073040): frame command buffer (`frame+0x50`), then for
  each active view in `frame+0x42E0` order: view slot 4 (0x3CC7010) closes the view's own buffer
  (`view+0x50`: begin markers + PerView/PerCamera/focus uploads), then calls every stage's slot 8, which
  waits for the stage job and closes the stage's buffer (base 0x3CD4330: `0x410E880(job counter)` then
  `[stage]+0x20`; e.g. 0x3E53E80 `FUN_14407f240(stage+0x30)`). So within one frame execution is
  sequential: main view's uploads and stages, then the split view's, then the special view's.
* Consequence: GPU state that a command buffer does not set is inherited from the previous buffer on
  the same context (the per-buffer cache only avoids redundant calls). The opcode scan (section 5.3)
  finds only one emitter of the "scissor = viewport" opcode 0x21 (0x2130166) and none of the explicit
  scissor opcode 0x20, so scene passes do not set scissor rectangles; they rely on rasterizer states
  with scissor disabled (unconfirmed: needs the RS descriptors).

### 1.6 Fields that differ (main vs split) and what reads them

| field | main / split | readers (confirmed) | effect |
|-------|--------------|---------------------|--------|
| `+0x48` MOC | set / null | 0x3CF0080 (Enter), 0x3E246B0 (cull params) | split: no occlusion culling, no camera cull entry, no cull job linked into its job graph |
| `+0x2BD0` | 1 / 0 | 0x3CC5AA0 only (config byte 0x60) | gate for creating `+0x48` |
| `+0x2BE8..+0x2BF4` (16-byte entity handle), `+0x2BF8` int, `+0x2BFC` byte, `+0x2AEA` byte | set / null handle | written by 0x17E7FD0 for the view returned by 0x3BAAE20 (player's view); read by stage 0x33's job 0x1816520 (room/region trigger masks), 0x17E3320 (object filter: `[view+0x2BF8] == object layer` when flag +0x250), 0x120xxxx/0x121xxxx/0x16xxxxx game code | the player character for room masks and per-view level/layer filtering; the split view has none because it isn't a player's view in the manager |
| `+0x2C80..+0x2C90` (3 CB objects), `+0x2CA8/+0x2CB0`, `+0x2CB8/+0x2CC0` (two hash sets) | | ctor 0x17F1E60 creates `+0x2C80` (0x80-byte CB) and inits the sets (0x17F3760); dtor 0x17F2060; readers 0x17F3360/0x17F3610/0x17E1CD0/0x17E2AA0/0x17E80F0 | per-view entity sets (GameRenderView); not on the draw path |
| `+0x2270` view index | 0 / 1 | render loop 0x4072B30 writes it; 0x3E3ABB0/0x3E3CCE0 compare it with globals `DAT_145ef2cb0`/`DAT_145ef2cb4` (debug view selection) | no effect unless a debug view index is set |
| `+0x22B8` | FFFFFFFF / 0 | not identified (none of the found `+0x22B8` users take a view) | unknown |

### 1.7 Decisive runtime checks (in order of cost)

1. **Diff b11/b12 at a G-buffer draw.** In the D3D11 trace, at the first split G-buffer draw and the first
   main one, read back VS constant buffers 11 and 12 (0xE0 and 400 bytes). Expected equal except for the
   eye offset. NaN/zero/huge values in the split's b11 (+0x80 rows = projection x view rotation, +0x40 =
   projection) identify the cause. Also compare b9 (viewport CB) and b13.
2. **Disable the stereo hook (0x3CC6CB9) for the split view.** If the split view then renders the main
   image in its half, the per-eye matrices written by the hook are the cause. From reading the mod's
   `native/core/src/stereo.cpp` (not modified): for the view assigned as eye 1, `rewrite_block` replaces
   PerCamera `+0x00` (view), `+0x40` (projection: `out[0] = 2/(r-l)`, `out[5] = 2/(u-d)`,
   `out[8]/[9]` from the eye's tangents) and `+0x80` (projection x view rotation) of **both** blocks with
   the FOV from `head->eye_fov[1]` or `eyes->fov[1]`. If that FOV is zero/uninitialised for eye 1
   (e.g. no headset session while tracing), `r - l = 0` gives inf/NaN in `+0x40/+0x80` and every vertex
   of every geometry pass (depth pre-pass included) is discarded - exactly the observed symptom. The
   hook's counters (`g_rewrites[1]`, `g_without_eyes`, `g_unmatched`) and the FOV values logged for
   eye 1 settle this.
3. **Check `RS+0x2180/+0x2184` (render viewport w/h ints) and `RS+0x21A0` (upscaled width) of the split
   view** at begin-recording: 0x3CCCFD0 divides by `(float)[view+0x2180] * 0.5` and
   `(float)[view+0x2184] * 0.5` when building the jittered block; 0 gives inf/NaN in PerCamera[1].
4. **Rasterizer / depth-stencil state at a split G-buffer draw** (ScissorEnable, scissor rect, StencilEnable,
   DepthFunc) vs main.
5. **Check that the split view's PerView/PerCamera/focus buffer objects are its own**: compare
   `[split+0x2820]`, `+0x2838`, `+0x2850`, `+0x2868`, `+0x2C80` with the special view's. If equal, the
   split view is a byte copy (1.8).

### 1.8 Not determined statically

* How the split view object came to share the special view's stage array and job context: every
  constructor of a render view (0x582BD0, 0x17F82F0, 0x1B75D50 -> 0x17F1E60; 0x3AC7500, 0x3BAAF90 ->
  0x3CC5AA0) allocates a fresh job context (0x407B080) and fresh CBs (0x40D0C30 in 0x3CC5AA0/0x17F1E60),
  and no code copies 0x2D90 bytes of a view (scalar scan for 0x2D90: only the three allocations and an
  unwind funclet). So a byte copy of a view does not come from the game. Check 5 above settles whether
  the CBs are shared.
* Which pixel-shader inputs could discard: the G-buffer pass binds SRVs of keys 0xFF193183 and
  0x5BE0FB7A and the per-player data (b7, b9 split index). Shaders were not analysed.

### 1.9 Giving the split view what it lacks, with the game's own functions

| missing | game function | notes |
|---------|---------------|-------|
| MOC `[view+0x48]` | `0x41A1BD0(alloc 0x160)` then its vtable slot 1 `(obj, [frame+0x4308], [frame+0x430C], height)` as 0x3CC5AA0 does; allocator `0x4170110(0x40EA160(), 0x160)` | then call SetCamera again (slot 10, 0x17F2B30; or Exit/Enter) so Enter (0x3CF0080) creates the camera cull entry and links the cull job into the view's job context. Only worth doing for the draw count; it does not change pixels. |
| player character / room data (`+0x2BE8..+0x2BFC`) | 0x17E7FD0 (fills the player's view returned by 0x3BAAE20) | only for a view registered as a player's view in the view manager (0x3BAA1A0 path). Copy the main view's 0x20 bytes otherwise. |
| proper view with its own CBs, job context, config (`+0x2BD0`) | view manager slot 1 -> 0x17F82F0 (constructs a GameRenderView with the main config), then 0x4073230(frame, view, 0), 0x588E50 | the game's add-player-view path 0x3BAA1A0 does exactly this |

---------------------------------------------------------------------------------------------------------

## 2. Frame structure (render thread)

| step | function | what it does (confirmed) |
|------|----------|---------------------------|
| frame sync | 0x4072CB0 | opens frame cmd buffer (opcode 3), frame vtable +0x58, 0x4073C50; for every view in `frame+0x42E0`: copies WriteState `+0x21E0..+0x2267` to ReadState `+0x2158..+0x21DF`; 0x4094E60 on the stage resource manager `frame+0x4250`; builds the 0x20-byte frame CB (`frame+0x4288..+0x42A7`: time `+0x4288`, frame number+fraction `+0x4290`, `+0x428C` = byte `+0x4393`, `+0x4294` = TAA/upscaler mode flag, `+0x4298` = `[DAT_14604aa10+0x4B]`, `+0x429C` = 1.0, `+0x42A0` = byte `+0x45`) and uploads it into `[frame+0x42A8]` (opcode 5) |
| render loop | 0x4072B30 | for each view with slot 13 true, ReadState.Enabled (`+0x2158`) and `frame+0x42F0`: `view+0x2270 = running index`, slot 1 (begin-recording); then arms+releases each active view's job counters (0x407B540: ctx +0x40,+0x60,+0x48,+0x50,+0x58,+0x08 ... ) |
| begin-recording | 0x17F21E0 -> 0x17F2B80 (focus CB) -> 0x3CC6C00 | slot 11, 0x3CC6DC0 (environment change flags `+0x2AED`), opcode 3, PerView/PerCamera build (0x3CC7490 x2), optional 0x3CC70B0 (exponential depth distribution for clustered/froxel lighting from `[view+0x21D8]` near/far and `[view+0x28A0]+0x234`), uploads (0x3CCDD00 x2 to `+0x2820/+0x2838`, 0x3B97160 x2 to `+0x2850/+0x2868`), opcode 4, then every stage's slot 6 (execute: submit job) |
| per-frame prepare | view slot 2 (0x17F2230) | focus data `+0x2CC8/+0x2D00` from local players, light list `+0x2D38` (5 entries), then every stage's slot 7 |
| frame end | 0x3DF97C0 | for frames with mode `[frame+0x40]==1` (offscreen) and `[frame+0x42F0]`: 0x4073040, close frame+0x50, each view's slot 5, frame slot 4/6 (+0x20/+0x30); signal DAT_146048680; then frames with mode 0: 0x4073040 and frame slot 4/6; then `[[DAT_14604aae8+0x1F0]]` slot 6 |
| end views | 0x4073040 | close frame cmd buffer; for each active view: slot 4 (0x3CC7010: close view cmd buffer `+0x50`, clear `+0x2150`, every stage's slot 8); then wait (0x410E880) on all 17 counters of each active view's context |

`frame+0x42F0`: per-frame "render enabled" byte (0x1B75D50 clears it for the offscreen BasicGameObjectRenderer
frame). `frame+0x40`: frame mode (0 = on-screen with compositing via the special view, 1 = offscreen).

## 3. Render views

### 3.1 Classes and vtables

| class | vtable | ctor | notes |
|-------|--------|------|-------|
| rf::RenderView (root) | 0x58EDB30 | 0x407B080 | `+0x10` frame, `+0x18` = `[frame+0x4250]` (stage resource manager), `+0x20` = self (stage-group owner), `+0x28/+0x30/+0x34` stage array, `+0x38` = 0xFFC0..., `+0x40` job context (0x88 bytes, 0x40878C0), `+0x50` command buffer (0x407F110, 0x4000 bytes), ReadState/WriteState |
| game render view base | 0x58B5230 | 0x3CC5AA0 | PerView/PerCamera blocks and CBs, environment, scene, MOC, creation config `+0x2B70..+0x2BD3` |
| ecl::GameRenderView | 0x55F42F8 | 0x17F1E60 | adds player handle `+0x2BE8`, focus CB `+0x2C80`, entity sets `+0x2CA8/+0x2CB8`, focus data `+0x2CC8..+0x2D84` (size 0x2D90) |

Vtable 0x55F42F8 (slots): 0 dtor 0x17F2060, 1 begin-recording 0x17F21E0, 2 prepare 0x17F2230,
3 0x3CC6FD0, 4 end-recording 0x3CC7010, 5 0x3CC7070 (stages slot 4), 6 0x3CC86D0 (tile counts),
7 0x17F2910, 8 0x3CC9190 (resize: `+0x2290` size, tiles `+0x29A8/+0x29B0`, stages slots 9/11),
9 0x17F2670 (bind per-view CBs + focus CB), 10 SetCamera 0x17F2B30, 11 0x407BEB0, 12 0x3CCCB10,
13 IsActive 0x17DF050, 14 0x17F2180, 15 0x17F3280, 16 bind per-view resources 0x17F26F0 (base 0x3CC83A0),
17 0x17F44A8, 18 0x5D92E0, 19 0x5D92C0, 20 0x3CCCAB0, 24 0x3CCCAC0.

Construction sites: 0x582BD0 and 0x17F82F0 (GameRenderView, size 0x2D90), 0x1B75D50
(BasicGameObjectRenderer offscreen view, own frame, config with `+0x2BD0 = 1`, `+0x2274` = param),
0x3AC7500 and 0x3BAAF90 (base class directly).

### 3.2 RenderViewState layout (confirmed from 0x407B800, 0x407BB80, 0x407B8C0, 0x407BE10, 0x4072CB0)

WriteState at `+0x21E0`, ReadState at `+0x2158` (= WriteState - 0x88), copied each frame by 0x4072CB0.
Offsets below are within the state (add 0x2158 for ReadState, 0x21E0 for WriteState):

| off | type | meaning |
|-----|------|---------|
| +0x00 | byte | Enabled |
| +0x04 | int | (must be <= 0 for prepare 0x17F2230) |
| +0x08 | float2 | Scale (x, y), clamped to [0,1] (ctor default 1,1) |
| +0x10 | float2 | Offset (x, y), clamped to [0, 1-scale] |
| +0x18 | float4 | render viewport, normalized {w, h, x, y} |
| +0x28 | int4 | render viewport in pixels {w, h, x, y} (ReadState `+0x2180` w, `+0x2184` h) |
| +0x38 | float4 | upscaled viewport normalized |
| +0x48 | int4 | upscaled viewport pixels (ReadState `+0x21A0` w) |
| +0x58 | float4 | display viewport normalized |
| +0x68 | int4 | display viewport pixels (WriteState `+0x2248/+0x224C` -> camera aspect in 0x407BE10) |
| +0x78 | 8 bytes | unknown |
| +0x80 | ptr | CameraController |

Pixel viewports come from the frame sizes (`frame+0x20/+0x24` render, `+0x28/+0x2C` upscaled, `+0x30/+0x34`
display) and frame-level rect fractions (`frame+0x42F8`, `+0x4318`, `+0x4338`), times the view's
Scale/Offset (0x407B8C0). The camera's aspect is set from the **display** viewport (0x407BE10).

After the states: `+0x2268` byte split-screen index, `+0x226C` qword, `+0x2270` int view index (render
loop), `+0x2274` byte (offscreen flag, read by stage 0x35), `+0x2290/+0x2294` view size, `+0x2298` byte
from settings `[DAT_14604b1b8+0x1BC]`, `+0x229C` tile size, `+0x22A0`.

### 3.3 Per-view GPU blocks (built in begin-recording by 0x3CC7490, uploaded by 0x3CCDD00 / 0x3B97160)

Two pairs: `[0]` = PerView `view+0x2340` (400 bytes) + PerCamera `view+0x2660` (0xE0), unjittered;
`[1]` = PerView `view+0x24D0` + PerCamera `view+0x2740`, with the projection rebuilt by 0x3CCCFD0
(jitter). Upload targets: `[view+0x2820]`, `[view+0x2838]` (PerView), `[view+0x2850]`, `[view+0x2868]`
(PerCamera), all created in the base ctor with 0x40D0C30 (dynamic buffers via device vtable +0x18).

PerCamera (0xE0, confirmed): `+0x00` view matrix (controller +0x90..+0xCF), `+0x40` projection (block
[0]: controller +0x110; block [1]: 0x3CCCFD0 from FOV `+0x14`, near `+0x18`, far `+0x1C`, aspect `+0x20`
and the jitter of 0x3F11710 on `RS+0x2180/+0x2184/+0x21A0` - ints - and the frame counter at
`[[view+0x2050]+8] + [[view+0x2050]+0xC]`), `+0x80` projection x view rotation (translation dropped:
geometry is camera-relative, see 1.4), `+0xC0` near (controller `+0x18`), `+0xC4` far (controller
`+0x1C`), `+0xD0..+0xDB` world position (controller slot 9), `+0xDC` = -1.

PerView (400 bytes, from 0x3CC7490; partially decoded): `+0x00..+0x2F` sun/light data from the active
lighting `[view+0x28D8]` and atmosphere `[view+0x28A0]` (direction `+0x00` from `view+0x2A40`, cos/sin of
`[+0x28D8]+0x260` degrees at `+0x0C/+0x1C`, colour x intensity `view+0x2A4C`), `+0x30..+0x6F` copied from
the controller `+0xD0..+0x10F`, `+0x70..+0x9F` three frustum-corner vectors (from controller forward /
right / up via 0x4084ED0/0x4084CB0/0x4084DC0, `tan(fov/2) * near`, aspect), `+0xA0..+0xDF` fog /
atmosphere colours, `+0xE0..+0x12F` fog distances from the lighting objects, `+0x130..+0x16F` sky and
cloud parameters, `+0x17C` = byte `view+0x2A50`, `+0x18C` = 0x3CDA5D0(). Only written when both
`[view+0x28A0]` and `[view+0x28D8]` are set (else stale).

Focus CB (0x80 bytes, b7): `view+0x2C00..+0x2C7F`, built by 0x17F2B80 from the per-player focus data
`+0x2CC8` (own player) / `+0x2D00` (other player) and five light entries `+0x2D38..+0x2D84`; uses global
defaults `DAT_14614eed0..` when `[view+0x2961]` is set.

## 4. Stages

### 4.1 Base stage (confirmed)

Stage object: `+0x00` vtable, `+0x08` view, `+0x10` &view->StageGroup (`+0x18`), `+0x18` byte run-this-frame,
`+0x19` byte init ok, `+0x20` job sub-object (vtable with `DumpToFile` name; slot 2 = job run),
`+0x30` stage command buffer (0x407F110), `+0x2138` job counter, `+0x2130`.. stage fields.
Common slots: 1 id (`mov al, id; ret`), 2 init, 3 release, 4 0x3CD42F0, 5 prepare (gate 0x17DA690 or
0x3D961A0 = gate + camera), 6 execute (base 0x3CD4300: submit job `0x410E5D0(counter, job)` if `+0x18`),
7 per-frame (from view slot 2), 8 wait+close (base 0x3CD4330), 9 0x3CD4370, 10/11 resize hooks.

Gate 0x17DA690: `+0x19 && view.ReadState.Enabled && [frame+0x42F0] && view slot13() && [view+0x2B10] &&
byte [[view+0x2B10]+8] && global flag ([[[DAT_146046f98+0x1D8]+0x30] + 0x12 + DAT_1460593cc*0xF8]) &&
[view+0x21D8]`.

### 4.2 Stage table

Vtables found by scanning .rdata for `slot1 = mov al,id; ret` with slot 4 = 0x3CD42F0, plus the factory
0x3AC51C0 and the inline constructions in 0x588E50. Purposes are from strings loaded in the init and
resource keys; "?" = not established.

| id | vtable | init (2) | prepare (5) | execute (6) | job run | wait (8) | purpose / evidence |
|----|--------|----------|-------------|-------------|---------|----------|--------------------|
| 0x00 | 0x58BE598 | 0x3D95620 | 0x3D961A0 | base | 0x3D961D0 | base | transparent / forward pass? binds depth 0x22F3E110, 0xFF193183, 0xDA734765, 0xBCE29AFA, 0xCBE5AA6C; sets RT 0x5EB1BE4E / 0xC7FDC9FB; reads cluster keys; `BlendSeparateAlpha.bshd` nearby; reads config byte `+0x2BD1` |
| 0x02 | 0x58C4E18 | 0x3E19910 | 0x3E1A410 | 0x3E1B0C0 | 0x3E1BB10 | 0x3E1B9D0 | `ShadowMapDownsample.bshd`; job uses split index |
| 0x03 | 0x58C5048 | 0x3E1EAC0 | 0x3E1F2F0 | base | 0x3E1F520 | base | directional-light (sun) cascaded shadow resolve: `/Shadows/Generated/DirLight_Cascaded`, `_PCSS`, `Engine_BlueNoise_Grey_64.dds`; output key 0x861C73EF; per-player keys 0xC0F3544B/0x447C4203 (index 0) or 0xB7F464DD/0x337B7295 (index 1) |
| 0x05 | 0x58C4B00 | 0x3E05D60 | 0x3D961A0 | base | 0x3E06930 | base | clustered light assignment (`ClusteredAssignment.bshd`, ortho variant), UAV clears + dispatches |
| 0x07 | 0x58C70C0 | 0x3E4C6F0 | 0x3E4D0F0 | base | 0x3E4D2A0 | base | final composite with UI (`CombineUI*.bshd`, `SoftCursor.bshd`) - special view; reads 0x0C419ABC / 0x684EE890 |
| 0x08 | 0x58C5AA0 | 0x17DE250 | 0x17DE260 | 0x3E245E0 | 0x3E24970 (+0x3E21C70, 0x3E21FD0 sub-jobs) | 0x3F7A6F0 | CullStage: per-view CPU culling into named buckets (`m_OpaqueObjects`, `m_DepthFirstObjects`, alpha/refraction/decal/emissive/outline/UI buckets) |
| 0x0A | 0x58C4810 | 0x3DFA600 | 0x3DFA9B0 | base | 0x3DFAA50 | base | debug-draw overlay (lines/triangles from `DAT_14604ab18`, 0x4080470) into 0x684EE890 + depth 0x1541F69A; main view only, built inline by 0x3AC4EA0; job counter = ctx+0x78 |
| 0x0B | 0x58C5CA8 | 0x3E2B3E0 | 0x3D961A0 | 0x3E2BA60 | sub-jobs 0x3E2BD90 (depth-first), 0x3E2C3B0 (opaque) | base | opaque G-buffer geometry + depth-first + linear depth (see 1.2) |
| 0x17 | 0x58C7030 | 0x3E4BD40 | 0x3D961A0 | base | 0x3E4C0E0 | base | emissive game decals into 0x2EDB4AC1 |
| 0x18 | 0x58C6F58 | 0x3E4B200 | 0x3D961A0 | base | 0x3E4B680 | base | emissive objects + emissive decals into 0x2EDB4AC1 (fmt code 0x5120002), depth 0x22F3E110; SRVs 0xFF193183, 0x5BE0FB7A |
| 0x19 | 0x58B4FC8 | 0x3CBFC30 | 0x3CC0490 | base | 0x3CC04F0 | base | special view: clear (job only emits opcode 9); key 0xFF6F1F7D |
| 0x1A | 0x58C6330 | 0x3E34140 | 0x17DA690 | 0x3E345B0 | 0x3E346A0 | 0x3E34620 | container of sub-jobs (?) |
| 0x1B | 0x58C6038 | 0x3E31E20 | 0x17DA690 | 0x3E32B50 | sub-jobs 0x3E33F10, 0x3E38940, 0x3E35C80, 0x3E34ED0, 0x3E348C0, 0x3E42130, 0x3E40820, 0x3E3F200, 0x3E3CCE0, 0x3E3ABB0, 0x3E39810 | 0x3E33E80 | clustered deferred lighting. Sub-stages (vtable, id, init): 0x58C62A0 id 0x04 0x3E34750 `ClusteredClear`; 0x58C60C8 id 0x12 0x3E383C0 `DepthMaxZx9/12/18/24` (key 0xCD0B15EE); 0x58C66B8 id 0x11 0x3E393D0 `DilateMax2` (0x230574C2, 0x54024454); 0x58C6210 id 0x0F 0x3E349D0 `ClusteredLightingClassification`; 0x58C6178 id 0x0E 0x3E353F0 deferred lighting from the G-buffer (`Engine_IBL_LUT.dds`); 0x58C6628 id 0x14 0x3E39C40 (0xE66299BB, 0x9165A92D); 0x58C6508 id 0x15 0x3E3C330 (0xBCE29AFA, 0xCBE5AA6C); 0x58C6598 id 0x13 0x3E3EDD0 creates 0xDA734765 (read by stage 0x00; possibly the volumetric fog volume - `VolumetricFogInitialize/Lighting` and `DeferredFog_*` strings are in this module, not tied to a sub-stage yet). Sub-stage inits read config byte `view+0x2BD1` |
| 0x1E | 0x58C49C0 | 0x3E01560 | 0x3E01870 | 0x3E018A0 | 0x3E01E00 | base | sun shadow cascades (creates the 4 cascade cameras; reads view index) |
| 0x1F | 0x55F3530 | 0x17DAA70 | 0x17DB4C0 | base | 0x17DB570 | base | EoC outline (`EoCOutlineObject_ST(I).bshd`), key 0x5AE0F2DF |
| 0x21 | 0x58A11A8 | 0x3B21E10 | 0x3B237E0 | 0x3B23890 | 0x3B239B0 | 0x3B23910 | `ls::PostProcessStage` (post chain; see 4.4) |
| 0x32 | 0x58C4EA8 | 0x3E18610 | 0x17DA690 | 0x3E18780 | 0x3E1BB10 / 0x3E18CE0 | 0x3E187B0 | shadow map renderer (helper stage used by 0x1E/0x02) |
| 0x33 | 0x55F7078 | 0x1814C70 | 0x18164F0 | base | 0x1816520 | base | EoC room / region trigger masks (`EoCRoomTrigger*`, `EoCRegionTrigger*`); uses player handle `+0x2BE8..+0x2BFC` |
| 0x35 | 0x58C48A8 | 0x3DFB410 | 0x3DFB670 | base | 0x3DFB6A0 | base | local-light shadow atlas allocation (`ShadowTileMap`); reads config `+0x2B7C..+0x2B8C`, `+0x2274` |
| 0x36 | 0x58C6C10 | 0x3E452B0 | 0x3D961A0 | base | 0x3E45750 | base | sky (`PPEmissiveSkydome*.bshd`) |
| 0x37 | 0x58C7248 | 0x3E4DBA0 | 0x3E4F430 | base | 0x3E4F490 | base | SSAO (`PPSAO*.bshd`), keys 0x361F94A4, 0xD3052CFC, 0x74EA5362, 0x8517349C |
| 0x3A | 0x55F35F8 | 0x17DE250 | 0x17DE260 | 0x17DE270 | - | 0x17DE290 | game-side object created in 0x588E50 (?) |
| 0x3B | 0x55F40E0 | 0x17F1A50 | 0x17F1AC0 | 0x3CEFD20 | 0x17F1B40 | 0x3CEFD60 | sub-stage of 0x46 created in 0x588E50 (?) |
| 0x3C | 0x55F4AB0 | 0x18005C0 | 0x1800A80 | base | 0x1800B00 | base | game-side G-buffer pass (same keys as 0x18) using 0x3A/0x3D |
| 0x3D | 0x55F6E70 | 0x1812350 | 0x18125B0 | base | 0x18126A0 | base | `EoCSurfaceMeshGenerate.bshd` (ground surfaces) |
| 0x3E | 0x58C7340 | 0x17DE250 | 0x17DE260 | 0x3E51D10 | 0x3E51DE0 | 0x3F7A6F0 | second cull stage (calls the scene cull 0x3CD1E80 twice) |
| 0x3F | 0x58C7548 | 0x3E548F0 | 0x17DA690 | base | 0x3E54C00 | base | post on the final colour 0x684EE890 (UAV/SRV passes; purpose ?) |
| 0x40 | 0x58C4C28 | 0x3E11B60 | 0x3E12110 | 0x3E12240 | 0x3E12770 | base | camera velocity (`VelocityBufferCamera.bshd`, `TAAVelocityBuffer`), key 0xC602F0F9 |
| 0x41 | 0x58C4D08 | 0x3E15070 | 0x17DA690 | base | 0x3E15670 | base | GPU skinning / blend shapes |
| 0x44 | 0x58C6DE8 | 0x3E470E0 | 0x3D961A0 | base | 0x3E47BB0 | base | volumetric clouds (`VolumetricClouds_*`); built only for the main view or split index 1 |
| 0x45 | 0x58C6EA8 | 0x3E4A4C0 | 0x3E4A670 | 0x3E4A6F0 | 0x3E4AA40 | 0x3E4A930 | `DownsampleResolver.bshd` (special view); releases 0xD4684117, 0x1AF4071A |
| 0x46 | 0x589D768 | 0x3AEC350 | 0x3AECC10 | base | 0x3AECC70 | base | G-buffer additions: binds the 4 G-buffer targets + depth (PSO 0x2140/0x2148), runs its sub-renderers (`+0x2150`, count `+0x215C`): deferred decals (ids 0x0C/0x0D) and the game's objects (0x3B) |
| 0x47 | 0x58C7498 | 0x3E53CB0 | 0x17DA690 | 0x3E53DE0 | 0x3E53FA0 | 0x3E53E80 | per-object GPU data ring (offsets stored per split index at object+0xE0+index*4) |
| 0x48 | 0x58C7408 | 0x3E54150 | 0x17DA690 | 0x3E54190 | 0x3E541B0 | 0x3E53E80 | uploads 0x47's buffer |

### 4.3 Resource keys (from the bind / setRT / lookup calls in each stage; roles partly inferred)

"RT of" below means the key passed to 0x40814F0 (viewport + viewport CB b9 computed from that key's
size), which is normally the pass's render target; for compute passes (0x3D) it is the dispatch size
source, not a render target.

| key | format code | bound / written by | read by | role |
|-----|-------------|--------------------|---------|------|
| 0x22F3E110 | 0x5230002 | 0x0B (dims), 0x18, 0x17, 0x3C, 0x36, 0x40, 0x44, 0x46, 0x00, 0x1F, 0x33 | many | scene depth-stencil (the depth pre-pass and opaque passes bind it as DSV) |
| 0x2EDB4AC1 | 0x5120002 | RT of 0x36 (sky), 0x44 (clouds), 0x18 / 0x17 (emissive), 0x3C (game) | - | emissive / sky colour accumulation (HDR) |
| 0x4C0D1B61, 0x3B0A2BF7, 0xA2037A4D, 0xD5044ADB | 0x5120002 | MRTs of 0x0B (opaque) and 0x46 (decals, game objects) | lighting sub-stage 0x0E of 0x1B (with `Engine_IBL_LUT.dds`), SSAO 0x37, 0x03 | **the G-buffer** |
| 0xFF193183 | 0x5410002 | bound by most scene stages; RT of 0x3D (surface meshes) | SRV in 0x18 | linear depth / depth-derived buffer (the trace's R32F "linear depth"; unconfirmed) |
| 0x5BE0FB7A | 0x5410002 | bound by 0x18, 0x17, 0x3C, 0x3D, 0x46 | SRV in 0x18 | ? |
| 0xC602F0F9 | | RT of 0x40 | TAA | camera velocity |
| 0x1115DE1, 0xFA1D09B3, 0x64959AC0, 0x5C86E4C0, 0x5CA3D9F0, 0x9331233, 0x976B0D74 | | written by 0x05 (clustered assignment, UAVs) | 0x1B, 0x00 | light clusters |
| 0x861C73EF, 0x94009344, 0x3E344F11 | | 0x03 | 0x00, 0x1B | sun shadow mask (and per-player 0xC0F3544B/0x447C4203 or 0xB7F464DD/0x337B7295) |
| 0x738395A7 | | 0x35 (`iVar*0x60` bytes) | 0x1B, 0x00 | local-light shadow atlas tile records |
| 0x361F94A4, 0xD3052CFC, 0x74EA5362, 0x8517349C | | RTs of 0x37 | 0x00 (0x74EA5362) | SSAO chain |
| 0x5EB1BE4E, 0xC7FDC9FB | | RTs of 0x00 | 0x1B (lookup), 0x33 (dims) | lit HDR colour (unconfirmed) |
| 0x5AE0F2DF | | 0x1F | | outline mask |
| 0x96395540, 0xB7D21B7B, 0xA2D14EA1, 0xBFA1FA28, 0x5D3751B0 | | 0x33 | | room / region trigger masks |
| 0x684EE890, 0x1541F69A | 0x5120002 / 0x5230002 | 0x0A (debug draw), 0x3F | 0x07 | final scene colour + its depth |
| 0x0C419ABC | | special view (0x19/0x45) | 0x07 | resolved scene colour for compositing |
| 0x3B9705AE, 0xC3C184CD | | 0x44 | | volumetric clouds |

`agents\A\stage_summary.txt` lists, per decompiled function, every key passed to the manager calls.

## 5. Recording and the command format

### 5.1 Command buffer

`0x407F110` / `0x40C8730`: `+0x08` stream (blocks via 0x40F9B30), `+0x20` second stream (callbacks run at
close, 0x40C5AE0), `+0x38` renderer, `+0x40` device wrapper, `+0x48` ID3D11DeviceContext, `+0x58` state
cache (reset by 0x40C82F0), `+0x122C` open flag, `+0x122D` dirty flag, `+0x1230` CB allocator,
`+0x2000` = DAT_14604aae8 (frame info: `+8` frame counter).

### 5.2 Opcodes (executor 0x40C8A10; D3D11 method from the context vtable offset)

| op | size | D3D11 / meaning |
|----|------|-----------------|
| 0x00 | 0x10+[+4] | inline data (skipped) |
| 0x01 / 0x02 | | callbacks |
| 0x03 / 0x04 | 1 | begin / end markers |
| 0x05 | 0x27+size | Map(WRITE_DISCARD) + memcpy + Unmap (dynamic buffer update) |
| 0x06 | 0x20 | VS/PS/CSSetConstantBuffers (slots at +0x11/+0x15/+0x16) |
| 0x07 / 0x08 | | 0x40C6A70 / 0x40C6B60 |
| 0x09 | 0x30 | ClearRenderTargetView or ClearUnorderedAccessViewFloat |
| 0x0A | 0x18 | ClearUnorderedAccessViewUint |
| 0x0B | | set render targets (0x40CA2E0; clears per load action: ClearRenderTargetView / ClearDepthStencilView / DiscardView) |
| 0x0C | 8 | begin pass (0x40CA700) |
| 0x0D | 1 | end pass (forget bound targets) |
| 0x0E | 0x18 | 0x40CA9C0 |
| 0x0F | 0x10 | RSSetState |
| 0x10 | 0x18 | OMSetDepthStencilState(state, stencil ref) |
| 0x11 | 0x10 | set pipeline state (0x40CAA90) |
| 0x12 / 0x13 / 0x15 | | 0x40CAB80 / 0x40CAC00 / 0x40CAD50 |
| 0x16 | 0x18 | IASetIndexBuffer |
| 0x18 | 0x28 | bind SRV (0x40CAED0) |
| 0x1A / 0x1B / 0x1C / 0x1D | | bind UAV / variants (0x40CB440, 0x40CB610, 0x40CB810) |
| 0x1E | 0x18 | bind sampler (cached, flushed before draws by 0x40CC710) |
| 0x1F | 0x14 | RSSetViewports {x,y,w,h, 0..1} |
| 0x20 | 0x14 | RSSetScissorRects (explicit) |
| 0x21 | 1 | RSSetScissorRects from the current viewport. **Bug: bottom = y + width** (0x40C7080: `lea r10d,[rax+rdx]` with eax = viewport width) |
| 0x25 / 0x26 | | AMD AGS BeginUAVOverlap / EndUAVOverlap |
| 0x27 | 0xC | Draw(count, start) |
| 0x28 | 0x10 | DrawIndexed |
| 0x29 | 0x18 | DrawIndexedInstanced |
| 0x2A | 0x10 | Dispatch |
| 0x2B | 0x18 | DispatchIndirect |
| 0x2E | | Map + memcpy |
| 0x31 / 0x36 / 0x37 | 0x18 | CopyResource |
| 0x38 / 0x39 / 0x3E | | CopySubresourceRegion |
| 0x3F | 0x10 | GenerateMips |
| 0x42 / 0x43 | 0x18 | GPU profiling markers |

### 5.3 Emitters

`agents\A\opcode_sites.txt` lists every call of 0x40F9B30 followed by `mov byte ptr [rax], op` (byte scan,
`scripts\opscan.py`). Single-site helpers: 0x0E 0x4080560, 0x0F 0x408053F, 0x10 0x408058D, 0x12 0x40805C5,
0x17 0x4080650 (pipeline-state apply helper around 0x4080500), 0x11 0x407F77C (0x407F700),
0x16 0x407F96A, 0x29 0x4082278, 0x21 0x2130166 (only emitter of scissor-from-viewport).

### 5.4 Job system (confirmed from 0x4107090, 0x41074A0, 0x4108550, 0x410E5D0, 0x410E880)

| function | meaning |
|----------|---------|
| 0x4107090(sched, desc) | create a counter (0x2C0 bytes, aligned 0x40): `+0x00` pending count, `+0x04` flags (1 armed, 2 released, 4 signalled, 0x10/0x20 event wait), `+0x250` successors (array, count `+0x25C`), `+0x268` predecessors (up to 3) |
| 0x41074A0(sched, A, B) | A waits for B: B is stored in A's predecessor slots, A appended to B's successors (under `sched+0x28` lock) |
| 0x4108550(sched, c) | release: if armed and every predecessor has (flags & 9) != 1, set flag 2 and flush pending jobs (0x410E6F0) |
| 0x410E5D0(c, job) | submit a job to a counter: `job+8 = c`, count+1; runs at once (0x41085D0) if released, else queued on `c+0x40` |
| 0x410E880(c, help) | wait: while count != 0, run queued jobs of this counter / steal other jobs (0x4108300) if `help`; event wait otherwise |
| 0x407B540(view) | arm (`+2`, flag 1), release, signal (flag 4) each of the view context's counters |
| 0x40878C0 | initialise a view's 17-counter context |

Per-view context (0x88 bytes, `[view+0x40]`): 17 counters at +0x00..+0x80; `+0x40` is the view's
"stages started" counter (camera cull jobs and the special view link to it), `+0x78`/`+0x80` are the
counters 0x3AC4EA0 links into the frame graph (special view's `+0x40` waits for `+0x78`;
`DAT_146048680` waits for `+0x80`).

## 6. Pipeline building (game side)

| function | what it does (confirmed) |
|----------|---------------------------|
| 0x588E50 (game pipeline vtable slot 88) | 0x3AC4EA0; then, unless the view is the special view, creates 0x3A (vt 0x55F35F8), 0x3D (vt 0x55F6E70, `+0x2140` = the 0x3A object), 0x3C (vt 0x55F4AB0, links 0x3A and 0x3D), inserts them before stages 0x05 / 0x46 / 0x17 via 0x407A560 (insert-before: the new stage is pushed when the loop reaches the given stage), creates 0x3B (vt 0x55F40E0, not in the list; attached to 0x46 by 0x3AEC1F0), 0x1F (0x17DA740, size 0x2280) and 0x33 (vt 0x55F7078) before 0x21; finally two output objects 0x180F4B0 and 0x17FCD00 attached to stage 0x21 (`view+0x2BE0` = the first) and inserted into 0x21's post-process step list after the step with id 0x23 (0x3B23A70) |
| 0x3AC4EA0 (choose stages) | special view (`pipeline+0xA8`): list `19 45 07`. Other views: `47 41 08 48 35 1E 05`, [`45` if `[frame+0x40]`], `0B 46 36 44 18 17 40`, `37`, `02 03 1B 1A 00 21 3E 3F`, [`07` if `[frame+0x40]`]; for the main view (`pipeline+0xA0`) also stage 0x0A inline (vt 0x58C4810, counter = ctx+0x78); then links the job graph (5.4) when `[frame+0x40] == 0` |
| 0x3AC51C0 (game pipeline slot 87, build list) | for each id: finds existing 0x08, 0x02, 0x03, 0x1B, 0x37, 0x05, 0x35, 0x1E, 0x47 in the view's array (passed to constructors), then constructs by id (switch) and adds with 0x407A440 (append) or 0x407A560 (insert before, e.g. 0x41 before 0x08); 0x44 only for the main view or split index 1 |
| 0x407A440 / 0x407A560 | stage group append / insert-before: registers the stage with the resource manager (0x409A4C0 under the owner view's record), calls the stage's init (slot 2); on failure unregisters (0x409AB20) |
| 0x407AE00(group, id) | find stage by id |

## 7. Stage resource manager (`[frame+0x4250]`, also `[view+0x18]`)

| function | meaning |
|----------|---------|
| 0x4094920(mgr, view, index) | create the view's record (0x20 bytes, 0x4092CC0) and insert it into `mgr+0x58` (count `+0x64`) |
| 0x409A4C0(mgr, owner, stage, ...) | add a stage record under the owner view's record (none -> stage not registered) |
| 0x409AB20 | remove a stage record |
| 0x40956C0(mgr, stage, key, fmt, fmt) | bind a resource key to the stage (the key must exist in the key table `+0x78/+0x80/+0x88/+0x98/+0xA8`; returns 0 if missing or if the stage has no record) |
| 0x4095830 / 0x4096BB0 | unbind (read / write binding) |
| 0x40955F0 | bind with a descriptor (creates the key if needed) |
| 0x409B0D0(mgr, key, divisor, desc, upscaled, ...) | create a key: `divisor == 0` -> fixed size from the descriptor, else **size = ceil(frame size / divisor)** using `mgr+0x594/+0x598` (render size) or `mgr+0x59C/+0x5A0` (upscaled size, when the upscaler setting `[DAT_14604aa10+0x77] > 0` and the flag is set); mip count from the size |
| 0x4095F50 / 0x4096520 | create / bind a pipeline state object from a descriptor (render-target keys, formats, load/store actions) - shared by hash (cache `mgr+0x148`) |
| 0x4097250 / 0x4097820 | create / release a compute or graphics shader binding (`.bshd`) |
| 0x4098180(mgr, key) | resource of a key (`+0x50` of the entry) |
| 0x4097AB0(mgr, out, ?, key, mip, ?, mode) | SRV/UAV view descriptor of a key |
| 0x4094E60(mgr) | per frame (from 0x4072CB0): for every view record whose view is enabled and `frame+0x42F0`, calls every stage's slot 5 (prepare) and collects the set of running stages; if the set (or the bindings) changed, rebuilds the resource allocation (0x4098470) |

Consequence for multiple views: every key is one resource sized from the **frame**, shared by all views
of the frame; each view draws into its own viewport rectangle of it. Per-player data needs separate keys
(e.g. stage 0x03 binds 0xC0F3544B/0x447C4203 for index 0 and 0xB7F464DD/0x337B7295 for index 1).

## 8. Post-processing (stage 0x21, `ls::PostProcessStage`)

0x3B21E10 (init) constructs the step objects in this order (step vtables; names from the strings stored
right after each vtable in .rdata - "(by strings)" = not confirmed by reading each step):

| order | step vtable | job run | step (by strings) |
|-------|-------------|---------|-------------------|
| 1 | 0x58D3100 | 0x3ECFD90 | god rays (`PPGodRaysBlur`, `ClearRTGraphics`) |
| 2 | 0x58D3270 | 0x3ED0BE0 | depth of field (`PPDOF*`) |
| 3 | (ctor 0x3ED2AD0) | 0x3ED3300 | ? |
| 4 | 0x58D3820 | 0x3EDD3D0 | `FadeBlending` |
| 5 | 0x58D3928 | 0x3EDE160 | exposure / eye adaptation (`PPCalculateEV`, `PPHistogram`, `PPExposure`, `ExposureEvBuffer`; old path `OldPPExposureCompute_*`, job 0x3EDFC40) |
| 6 | (ctor 0x3EDF4B0) | | ? |
| 7 | 0x58D3D90 | 0x3EE0D10 | `PPReExpose` / lens flare features |
| 8 | 0x58D4068 | 0x3EE1D40 | lens flare (`PPLensFlare_*`) |
| 9 | (ctor 0x3EE44C0), 0x58D4188 | 0x3EE46C0 | darken environment (`PPDarkenEnvironment*`) |
| 10 | 0x58D42B0 | 0x3EE6070 | radial blur |
| 11 | 0x58D4418 | 0x3EE71B0 | SMAA |
| 12 | 0x58D46C0 | 0x3EE9880 | TAA (`PPTAA`, `PPTAA_Screenshot`) |
| 13 | 0x58D47B8 | 0x3EEAFA0 | bloom pyramid (`PPBloom*`) |
| 14 | 0x58D4940 | 0x3EEB4E0 | bloom / chromatic aberration |
| 15 | 0x58D4A38 | 0x3EEFD20 | LUT compute (`PPComputeLUT*`, display LUT ACES/ZCAM) |
| 16 | 0x58D4C08 | 0x3EF0990 | colour grading apply LUT |
| 17 | 0x58D4D38 | 0x3EF2860 | ? |
| 18 | 0x58D4E30 | 0x3EF34B0 | FSR2 (`FSR2_*`), second part 0x58D50F0 / 0x3EF4170 |
| 19 | 0x58D5970 | 0x3EFD570 | FSR1 (`FSR_SLOW`, `FSR_RCAS_SLOW`, `FSR_Tonemap`) |
| 20 | 0x58D5A40 | 0x3EFF380 | inject texture (HDR calibration) |
| 21 | 0x58D5D10 | 0x3EFFDB0 | ? |
| 22 | 0x58D5FC0 | 0x3F00FA0 | tonemap (`PPToneMap_ApplyLUT_v1/v20/v21`, ZCAM LUTs) |
| 23 | 0x58D60C8 | 0x3F02B80 | CAS sharpening |
| 24 | 0x58D61E8 | 0x3F03530 | vignette |
| 25 | 0x58D6380 | 0x3F040F0 | HDR calibration max luminance |

The full chain is per view (each view has its own stage 0x21 and steps), but the targets are shared keys
(section 7), which is why cross-frame resources such as the exposure luminance textures are shared by
the main and split views (observed: the split view's exposure feeds the main view's next frame).

## 9. Shadows

* Sun: stage 0x1E creates the four cascade cameras (`CameraController`, vtable 0x58EDC60) - 0x3E01E00,
  0x3E02440 (job), 0x3E18CE0, 0x3E1C940 per notes; stage 0x32 (vt 0x58C4EA8) renders them; stage 0x02
  downsamples (`ShadowMapDownsample.bshd`); stage 0x03 resolves the cascaded shadow mask with PCSS and
  blue noise into key 0x861C73EF, with per-player keys selected by `[view+0x2268]`.
* Local lights: stage 0x35 allocates tiles in the shadow atlas (key 0x738395A7, 0x60 bytes per tile
  record) with a quadtree (0x3F77440(stage+0x2140, size, max tile, min tile)), parameters from the view's
  creation config: 3 quality records of 0x20 bytes at `+0x2B70`, `+0x2B90`, `+0x2BB0` indexed by the
  shadow quality setting `[DAT_14604aa10+0x43]`. Record fields used: `+0x00` atlas size, `+0x04`, `+0x08`
  (tile sizes), `+0x0C`, `+0x14`, `+0x18`, `+0x1C`. Example (offscreen BasicGameObjectRenderer view,
  0x1B75D50): {0x400, 0x200, 0x80, 0x200, 40.0, 55.0, 0x100, 8.0}, {0x200, 0x100, 0x40, 0x100, 20, 35,
  0x80, 16}, {0x100, 0x80, 0x20, 0x80, 20, 25, 0x40, 24}. The 8192 atlas of the main view comes from
  the main view's config (passed by 0x582BD0's caller; unconfirmed value). **A view copied from the
  special view has the special view's config here** (unknown values; check `+0x2B70..+0x2BCF`).

## 10. Compositing, UI and Present

* Special view stages: 0x19 (clear, key 0xFF6F1F7D), 0x45 (`DownsampleResolver.bshd`, the main view's
  output to display size), 0x07 (`CombineUI*.bshd`: composite scene key 0x0C419ABC with the UI texture,
  soft cursor quad; viewport from 0x40816E0/0x407FED0).
* Stage 0x0A (main view only, last): debug primitives (lines/triangles from `DAT_14604ab18`) drawn into
  key 0x684EE890 (scene colour) with depth key 0x1541F69A.
* Present: dx11 RenderFrame vtable 0x58EE490 slot 8 = `rf::dx11::RenderFrame::FlipBuffers` 0x40908A0:
  `swapchain = [frame+0x43A8]`; `Present(sync = ([frame+0x40]==0 && byte [frame+0x43B4]) ? 1 : 0,
  flags = (byte [frame+0x43B6] && sync==0 && ![frame+0x44]) ? DXGI_PRESENT_ALLOW_TEARING : 0)`.
  Slot 4 = 0x4090890 (`jmp 0x4072CB0`, the frame sync, called by the frame end 0x3DF97C0 after the views
  finished - so the WriteState -> ReadState copy and the frame CB for the next frame happen there);
  slot 6 = 0x4092400 (DXGI output / window checks); CreateFrameBufferSurfaces 0x4090FD0.

## 11. Game-thread side (views, cameras, scenes)

Re-verified here: Enter/Exit (0x3CF0080 / 0x3CF0210), player data 0x17E7FD0, focus data 0x17F2230.
The rest is from `docs/re-notes/stereo.md` (confirmed earlier from decompiles; addresses unchanged).

| function | role |
|----------|------|
| 0x17F2B30 (view slot 10) -> 0x3CC90B0 | SetCamera: old controller Exit, new Enter, WriteState controller `+0x2260`, aspect (0x407BE10), listeners; skipped if `+0x2C98` |
| 0x3CF0080 / 0x3CF0210 | gameplay controller (vtable 0x58B6750) Enter / Exit: per-view cull entries on the camera entity (1.3) |
| 0x3CCC550 | set the view's scene (`+0x2B10`); releases `+0x2968..+0x2988`, rebuilds 0x3CC9530 / 0x3CC92B0, `+0x2B08`, creates `+0x2B18` |
| 0x3CC9700 / 0x3CCAAC0 | atmosphere / lighting setters (`+0x2880/+0x2890/+0x28A0`, `+0x28B8/+0x28C8/+0x28D8`); requests from 0x1E1DF90 / 0x1E1E1D0 |
| 0x17F8160 | set-player-view-scene (scene + 0x3CC9D30 + 0x3CCAFA0) |
| 0x3BAA1A0 / 0x3BAA380 / 0x3BAA680 / 0x3BAAE20 | view manager: add player view (create, index 1, setup 0x588E50) / add local player / remove / player -> view |
| 0x17E7FD0 | per-player data into the player's view (`+0x2BE8..+0x2BFC`, `+0x2AEA`) |
| 0x17F2230 (view slot 2) | focus data from the local players (`+0x2CC8` own, `+0x2D00` other) and lights `+0x2D38`; then stages' slot 7 |
| 0x4072CB0 | WriteState -> ReadState copy (render thread, at frame end for the next frame) |

`ecl::splitscreen::{ViewSystem, RenderViewportSystem, RequestSystem, SplitReasonsSystem}` and
`ecl::player_management::RenderViewTrackingSystem` exist (type-name strings 0x54CB840, 0x54D1560); their
update functions were not decompiled in this pass.

## 12. Facts that matter for two eyes per frame

* All views of a frame share every keyed resource, sized from the frame (section 7); a view renders into
  its viewport rectangle. Cross-frame resources (exposure, TAA history) are therefore shared by the two
  eyes unless the stage uses per-player keys (only 0x03 and 0x44 select keys by split index).
* Occlusion culling per view needs the MOC (`+0x48`) and the camera Enter registration (1.3, 1.9).
* **Per-object GPU data offsets are stored per split index, not per view.** Stage 0x47's execute
  (0x3E53DE0) copies `[view+0x2268]` into the job (`stage+0x219C`); its job (0x3E53FA0) walks the visible
  objects and writes each object's offset into the view's ring buffer to
  `[object+0x2D0 or +0x2D8] + 0xE0 + index*4`, then stage 0x48 uploads the data. Two views with the same
  index (main and split both 0) write the same slot from two parallel jobs, so a view can record draws
  with the other view's offsets into its own buffer (a race; only objects that go through 0x47 are
  affected; where the slot is read back was not traced - unconfirmed that it is at draw recording). This is a reason to give the second eye index 1, or to serialise the two
  views' 0x47 -> draw sequences.
* The camera's aspect is taken from the **display** viewport of the view (0x407BE10); the jitter uses the
  render viewport size ints `RS+0x2180/+0x2184` and the upscaled width `RS+0x21A0` (0x3CCCFD0, 0x3F11710).
* Opcode 0x21 (scissor from viewport) computes `bottom = y + width` (0x40C7080); harmless for wide
  viewports, clips the bottom `height - width` rows when width < height (e.g. a 1832x1920 eye). Its only
  emitter is 0x2130166 (not identified; scene passes do not use it).
* Command buffers execute in close order on one context; GPU state not set by a pass is inherited from
  the previous pass/view (1.5).
* The special view's job context waits for every normal view's ctx+0x78 (0x3AC4EA0); stage 0x0A, which
  uses that counter in the main view, is only built for the main view.

## 13. Function index (this report)

| rva | name |
|-----|------|
| 0x407B080 | root RenderView ctor (job context 0x88 bytes + 0x40878C0, command buffer, states) |
| 0x3CC5AA0 | game render view base ctor (CBs, MOC, config `+0x2B70..+0x2BD3`) |
| 0x17F1E60 / 0x17F2060 / 0x1B76620 | GameRenderView ctor / dtor / deferred destroy |
| 0x582BD0, 0x17F82F0, 0x1B75D50, 0x3AC7500, 0x3BAAF90 | view construction sites |
| 0x41A1BD0 / 0x41A1F90 | per-view MOC wrapper ctor / dtor (vtable 0x59116E0) |
| 0x3CF0080 / 0x3CF0210 | gameplay camera Enter / Exit |
| 0x3F06CC0 / 0x3F06E70 | camera cull entry create / destroy (job vtable 0x58D7FF0, run 0x3F38A40) |
| 0x3B88E40 | camera entity's per-view entry list |
| 0x3E229C0 | CullStage ctor (bucket list offsets, section 1.2/4.2) |
| 0x3E246B0 | CullStage cull parameters + scene query |
| 0x3E24970 | CullStage job |
| 0x3CD1E80 | scene cull query |
| 0x3E2BD90 / 0x3E2C3B0 | G-buffer depth-first / opaque sub-jobs (stage 0x0B) |
| 0x3AECC70 | stage 0x46 job (G-buffer additions) |
| 0x3EBBB50 / 0x3EBC0C0 | deferred decal sub-renderers |
| 0x17F1B40 | game G-buffer objects (0x3B) |
| 0x3E4B680 | emissive pass job (stage 0x18) |
| 0x3CEE160, 0x3CD9BA0, 0x3CD87B0, 0x3CD43D0, 0x3CD8640 | render-list sort key, sort, instance data, execution, draw |
| 0x17F26F0 / 0x3CC83A0 | bind per-view resources (view slot 16) |
| 0x17F2670 | view slot 9 (slot 16 + focus CB) |
| 0x17F21E0 / 0x17F2B80 / 0x3CC6C00 | begin-recording (GameRenderView) / focus CB / base body |
| 0x3CC7490 | PerView/PerCamera build |
| 0x3CCCFD0 / 0x3F11710 | jittered projection / jitter sample |
| 0x3CCDD00 / 0x3B97160 | PerView / PerCamera upload (opcode 5) |
| 0x3CC7010 | end-recording |
| 0x3CC6DC0 | environment change flags (`+0x2AED`) |
| 0x3CC70B0 | depth slice distribution (froxels) |
| 0x40816E0 | viewport CB (b9) |
| 0x40814F0 / 0x4081620 | pass viewport + viewport CB from a key's size |
| 0x407F3C0 | set render targets (opcode 0x0B, optional 0x1F) |
| 0x407FED0 | viewport (opcode 0x1F) |
| 0x407F240 / 0x407F1E0 | close command buffer (execute) / discard |
| 0x407F110 / 0x40C8730 | command buffer init |
| 0x40F9B30 | command allocator |
| 0x40C8A10 | command executor (opcode switch) |
| 0x40C7E70 / 0x40C7F20 | opcode 5 (buffer update) / opcode 6 (bind CB) |
| 0x40CA2E0 | opcode 0x0B (set RTs + clears) |
| 0x40CBBC0 / 0x40C7080 | viewport / scissor-from-viewport |
| 0x4072CB0 | frame sync (also dx11 RenderFrame slot 4 via 0x4090890) |
| 0x4072B30 | render loop |
| 0x4073040 | end views |
| 0x3DF97C0 | frame end |
| 0x4073230 | add view to frame |
| 0x407B540 | arm/release a view's counters |
| 0x407B8C0 / 0x407BB80 / 0x407B800 | viewport rect / all viewports / scale+offset |
| 0x407BE10 | camera aspect from display viewport |
| 0x40908A0 | `rf::dx11::RenderFrame::FlipBuffers` (Present) |
| 0x4090FD0 | `CreateFrameBufferSurfaces` |
| 0x4092400 | dx11 RenderFrame slot 6 (output/window checks) |
| 0x588E50 | game pipeline full setup |
| 0x3AC4EA0 | choose stages |
| 0x3AC51C0 | build stage list (factory) |
| 0x407A440 / 0x407A560 / 0x407AE00 | stage group append / insert-before / find |
| 0x4094920, 0x409A4C0, 0x40956C0, 0x4095830, 0x4095F50, 0x4096520, 0x409B0D0, 0x4094E60, 0x4098180, 0x4097AB0 | stage resource manager (section 7) |
| 0x4107090, 0x41074A0, 0x4108550, 0x410E5D0, 0x410E880 | job system (5.4) |
| 0x3B21E10 | post-process stage init (step list, section 8) |
| 0x3DFB410 / 0x3DFB6A0 | shadow atlas stage init / job |
| 0x3E1EAC0 / 0x3E1F520 | sun shadow resolve init / job |

