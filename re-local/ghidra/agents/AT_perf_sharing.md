# Work the right eye repeats, and how to share it - agent AT, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656. Ghidra project BG3_A (read-only, -noanalysis, request server
`agents\AT\ATServer.java`, stopped). bg3vr_disasm. Builds on T, W, X, AN, AL, AI, AH, A, H, S. RVAs are relative to
0x140000000. Raw output: `re-local\ghidra\agents\AT\` (at1..at24). "main" = left eye view, "eye" = right eye view
(CreatePlayerView 0x17F82F0).

**No per-stage timing exists yet.** The 17:01-17:03 log has only `xr: frames:` lines (5 s windows: avg 29-74 ms between
Presents, max 200-495 ms) and job dumps. So every saving below is ranked by what the code repeats, not by measured
time. Section 4 is the run that measures each candidate. "Confirmed" = read in code at the cited address.

## 1. Ranked candidates

| # | candidate | what the eye repeats (saving) | how | risk | confidence |
|---|-----------|------------------------------|-----|------|------------|
| 1 | **Local-light shadow atlas: eye uses main's allocation and skips its atlas render** | GPU: every shadowed local light again (point light = 4 atlas tiles, each a caster pass, plus linearise). Render thread: D3D11 submission of those draws. Worker: recording (0x3CD87B0 per face). Large in torch-lit interiors, zero where no local light casts shadows. | (a) mid-hook **0x3DFB7C5** in the 0x35 job: eye -> `rax = [main_view+0x21D8]`, so priorities, tile sizes and fades equal main's. (b) inline hook **0x3E01E00** (eye's 0x1E job): wait main ctx+0x60, compare the two 0x35 lists, return at once when equal. | low-medium. Fallback renders as today. | atlas content viewer-independent: Confirmed. Records stay eye-relative: Confirmed. Match rate: Unconfirmed (4.3) |
| 2 | **Blend shapes: eye skips its BlendShapeCompute dispatches** | GPU: one compute dispatch per morph mesh per frame, plus its CB / weight uploads. The eye writes the **same values into the same per-mesh buffer** main already wrote. Most in dialogues and crowds. | inline hook **0x3E15C50** (rcx = 0x41 stage): `[rcx+8] == eye` -> return. | low. A mesh only the eye sees keeps its last morph result (one frame old). | view-independence and the shared per-mesh output: Confirmed (2.2) |
| 3 | **Cull feedback query: eye skips it** | Worker: a second full scene query per view (0x3CD27F0) plus the gameplay "visible" marks. It feeds counter `[0x6048688]`, which the game-thread release job waits for (W 1), so it is on the game thread's path. | inline hook **0x3F7A510** (job run): `[[rcx+0x10]+8] == eye` -> return. | low. Marks are idempotent; both views use the same planes and the head controller. | mechanism Confirmed (2.3); cost Unconfirmed |
| 4 | **Skinning: eye binds main's 0x41 output instead of skinning again** | GPU: one SkinningCompute dispatch per skinned mesh. Worker: the 0x41 job. Also removes a race (2.4). | Hook **0x3CCDAC0** (eye view -> main view), skip the eye's 0x3E16830 / 0x3E17490, and make main's 0x41 skin the union of both views' sets. Two job edges. | medium-high: needs the union, else an eye-only mesh (LOD edge) draws with a stale offset. | inputs view-independent: Confirmed. Union recipe: Unconfirmed |
| 5 | **Occlusion raster (MOC): eye reuses main's** | Worker: the eye's occluder raster, time-boxed to **1-20 ms** per view (clamp constants 0.001 / 0.02 s at 0x58D8014 / 0x58D8010, loop in 0x3F38A40). | Only if both views' raster inputs are equal (4.4): eye's CullStage uses main's MOC, the eye's raster job returns at once. | medium | budget Confirmed; input equality Unconfirmed (writer of job +0x40 not found) |
| 6 | One cull for both eyes (CullStage 0x08) | Worker: the eye's scene query, bucketing and 34 bucket sorts. | W row 7. Both views already cull with the same planes (camera entity +0x210) and mask; only the MOC and slot 9 (6 cm) differ. | high: many consumer pointers and the 0x47 list push. | recipe Unconfirmed |
| 7 | Ground-surface selection 0x3A (job 0x17DE2A0) | Worker: an octree walk within 60 m. Same result except at the 60 m edge (AH 2). | Point the eye's 0x3D / 0x3B / 0x3C at main's 0x3A object, skip the eye's job. | medium (ordering, pointer fields) | consumer fields Unconfirmed; saving small |
| 8 | Volumetric clouds 0x44 | GPU: a second ray-march. With parallel eye cameras, the eye's image of clouds at infinity is main's image shifted by a constant. | Needs a new GPU copy pass. Measure 0x44 first; else lower CloudsQuality `[0x604AA10]+0x49` (global). | high | engine support: none found |

Not shareable, each eye must run it (section 3): clustered lights 0x05, lighting and fog froxels 0x1B, SSAO 0x37,
sun resolve 0x03, G-buffer / decals / emissive, sky 0x36, velocity 0x40, room masks 0x33, outlines 0x1F, surface tiles
0x3D, per-object and particle fills 0x48, DLSS, post steps. No runtime probe capture, terrain or impostor cache exists to
share (T 4.2). Global knobs that cut both eyes: FogQuality +0x4A, CloudsQuality +0x49, SSAO +0x5A, GodRays +0x58,
Bloom +0x4C (X).

**Side finding (correctness, Confirmed code):** both eyes' 0x41 jobs write each skinned mesh's buffer offset into the
same mesh slot, indexed by `[view+0x226C]` (2.4). With parallel recording this is the same race eye_slot fixed for
0x47. Cheapest fix: give the eye view `+0x226C = 1`. That index is the split-screen player-2 slot (0x192ED80 sets 0 / 1).
Candidate 4 removes the race too.

## 2. Evidence

### 2.1 Local-light atlas (stages 0x35, 0x1E)

* 0x35 job 0x3DFB6A0 (AL al15): it loads the controller at **0x3DFB7BE** (`mov rax,[r14+0x21D8]`) and stores it at
  **0x3DFB7C5** (`mov [rbp-0x80],rax`, r14 = view). That pointer goes only to the priority pass 0x3DFBF80 (through
  `param_1[1]`, T t10 line 49 slot 9) and to the FOV read at 0x3DFB936. The record writer **0x3DFC660** re-reads the
  view's own controller (`[[stage+8]+0x21D8]` slot 9, al15 line 42). So the swap in (a) changes the sort, the tile size
  and the fade, but the eye's records stay relative to the eye. Confirmed.
* Allocation: the list is sorted by entry +0xC, then tiles come from the stage's quadtree in that order. Per entry it
  writes +0x10 = the node's first 8 bytes (tile position) and +0x18 = size / atlas size (0x3DFBD2C path). Equal lights,
  equal order and equal sizes give equal tiles. Confirmed.
* 0x1E job 0x3E01E00 (AN an16): per light it builds a CameraController from the light's transform (0x40855D0 on
  light +0x18) and records the casters with it. It renders into the atlas keys 0xC91746C5 / 0xFA76F838. So the content
  is the light's view only. Confirmed. Its only outputs are its own command stream. Main's stream runs on the GPU
  before the eye's (A 1.5, AH 2), so the eye's 0x1B / 0x00 read main's tiles.
* Skip test at 0x3E01E00 (rcx = stage+0x20, view = `[rcx-0x18]`, 0x35 stage = `[rcx+0x2120]`): main's 0x1E ran
  (`stage+0x18`), both lists (`stage35+0x2178`, count `+0x2184`) have the same lights with `+0x320 >= 0`, and the same
  +0x10 (8 bytes) and +0x18. Wait first: `0x410E880(main ctx+0x60, 1)`. Main's +0x60 depends on nothing of the eye's.
* Still repeated after the skip: the per-light caster culls. Job 0x3E00E30 (vtable 0x58C49A0, run on ctx+0x50) calls
  0x3E01710. That function also grows the cull-object array that 0x41 indexes (at1 lines 86-114), so don't skip it.

### 2.2 Blend shapes (0x41, 0x3E15C50)

* 0x3E15670 builds three de-duplicated sets from the view's CullStage (+0x90 / +0xA0 / +0xB0 lists), the 0x1E
  per-light culls and the 0x02 cascade culls (at1 lines 77-215). Then it calls 0x3E15C50 (blend shapes), 0x3E16830
  (skinning) and 0x3E17490 (offsets). De-duplication is within one view only.
* 0x3E15C50 (at17 lines 78-322): per mesh, weights from `[obj+0x98]` +0x20, base vertex SRVs from the mesh, and the
  UAV `*[obj+0x98]`, a buffer owned by the mesh. No camera read (no slot-9 call in the function). So both views write
  identical data into the same buffer. Confirmed.
* Skinning reads that buffer (0x3E16830, `[lVar35+0x98]`) in the eye's own stream, after main wrote it. Skipping the
  eye's dispatches changes nothing the eye reads.

### 2.3 Cull feedback query

* CullStage job 0x3E24970: **0x3E25697** `cmp byte [r15-0x48],0` (stage +0x88, set to 0 by the base ctor 0x3F7A530),
  then the params copy and **0x3E2575B** `call 0x410E5D0` with counter `[0x6048688]` and the job `[stage+0x80]`
  (vtable 0x58DC3D8, run 0x3F7A510 -> **0x3F7A700**).
* 0x3F7A700 (at8) calls **0x3CD27F0**(scene, params), a full octree query under the scene lock that runs 0x3CD0FA0 per
  node (at16). Then it marks objects in the stage's lists: `+0x38 = 1` and a time stamp within 256 m, a slot +0x78
  call with the time, and 0x3D27900 for items above a screen-size threshold.
* Params: +0x10 = the camera entity's controller (0x17F3A50(world, entity)+8: the head, not the proxy), planes from
  its +0x210, mask from `[view+0x2268]` (0x201 for both eyes), MOC = the view's (0x3E246B0, at1). So the eye's query
  differs from main's only by the MOC. Confirmed.

### 2.4 Skinning and the shared mesh slot

* 0x3E16830 writes per mesh `[mesh+(idx+0x44)*4]` (offset), `+0x46` (previous), `+0x48` (frame). 0x3E17490 writes
  per renderable `+0xF0 / +0x110 [idx]` (offset), `+0xF8 / +0x118` (previous), `+0x100 / +0x120` (frame). In both,
  idx = `[view+0x226C]` (0x3E1723A, 0x3E175DA). Output is the stage's own buffer `+0x2180 + (frame&1)*8` (0x3E16330).
* Draw-time readers **0x3D30E40** / **0x3DD21D0** read `+0xF0 / +0x110 [idx]` with idx = `[[ctx+0x10]+0x226C]` and
  write it into the draw's constants. The buffer is bound by **0x3CCDAC0**(view, ...), which finds stage 0x41 in the
  view's own group (16 callers, all draw jobs).
* Bone matrices come from the renderable (`plVar40[0x1f]` or `[plVar40[0x1c]+0x10]`), not from a view. So the
  skinned output is view-independent. Confirmed.
* `[view+0x226C]` is zeroed by the root ctor (0x407B180) and set to 0 / 1 only by the split-screen layout 0x192ED80.
  The eye's value is Unconfirmed (4.5). If it is 0, both eyes race on the same slot.
* Sharing recipe (Unconfirmed parts marked): (1) 0x3CCDAC0 for the eye -> call with main's view. (2) Eye's 0x41 job
  skips 0x3E16830 / 0x3E17490 (and 0x3E15C50, item 2). (3) The eye's draws must record after main's 0x3E17490: add
  edge eye ctx+0x58 <- main ctx+0x58 (0x41074A0; eye +0x58 has 2 of 3 predecessor slots used, log 17:02:28). (4) Main's
  0x41 must also skin the eye's own sets (eye CullStage, eye 0x1E culls), else a renderable only the eye draws (an LOD
  band crossed between the eyes' positions, 6 cm) reads a stale offset into main's buffer. That needs main +0x58 <- eye
  +0x50 and an insertion into main's list before its size count (at1 line 175). The insertion point: Unconfirmed.

### 2.5 MOC raster

* 0x3F38A40 (at1 319-953): sizes the view's MOC from `[view+0x2180/+0x2184]`, transforms occluders with the job's
  matrix at +0x40..+0x7C and position +0x80, rasterises sorted occluders until QPC passes clamp(job +0x34, 0.001,
  0.02) s. Entry made per view by Enter 0x3CF0080 -> 0x3F06CC0; ctx+0x40 waits for it.
* Who writes +0x34 / +0x40 each frame was not found (the list is an ECS component; 0x3B870C0 only removes it). If the
  matrix is the camera entity's (like the planes), both views' rasters are identical.

### 2.6 Per-view work that stays per eye (Confirmed reasons)

* 0x48 fills 0x3F59B10 / 0x3F5D2E0 (at15 lines 1823-1885): per particle, distance curves from the view's slot 9 and
  facing from slot 11, written into the view's ring at `+0xE0 + slot*4`. Camera-relative, so not shareable without
  shader changes (S 1.1).
* Fog: froxel grids are frame-wide, but each view fills its own tile window with its own rays (AI 3). Reuse would
  need a reprojection in the shader.
* SSAO, 0x03, 0x05, 0x33, 0x1F, 0x3D, 0x40: screen-space per view (T 4.1, AN 3.5, AH 1).
* LOD: chosen inside the cull query (H 2.2) from the shared FOV and each eye's position. Nothing else per view.

## 3. Implementation notes (top items)

1. **Atlas (item 1).** (a) MidHook 0x3DFB7C5 (bytes `48 89 45 80`): if `r14 == eye` set `rax = [main+0x21D8]`.
   (b) InlineHook 0x3E01E00 (prologue `48 8B C4 48 89 58 10`): eye only, after the wait and the compare, return. Count
   skips and mismatches; log the reason for a mismatch. Switch: `[views] share_light_shadows`.
2. **Blend shapes (item 2).** InlineHook 0x3E15C50 (prologue `48 8B C4 48 89 58 10`), `[rcx+8] == eye` -> return.
   Switch: `[views] share_blendshapes`.
3. **Cull feedback (item 3).** InlineHook 0x3F7A510 (`4C 8B 41 18 ...` then `jmp 0x3F7A700`; safetyhook relocates the
   jmp). `[[rcx+0x10]+8] == eye` -> return. The function is shared by every stage built on base 0x3F7A530, so test the
   view, not the stage. Switch: `[views] skip_eye_cull_feedback`.
4. Threads: (1b) and (3) run on workers, in parallel with main's jobs. They only read main's data after waiting on
   main's counter, or not at all. No writes to main's objects.

## 4. Measurement plan (one run)

Run: outdoors in sun 20 s, a torch-lit interior 20 s, a dialogue 20 s. Hotkey / VR menu A/B for each switch above,
20 s each, timers running. Log every 120 frames, per view (main / eye / special): mean, p95, max.

1. **GPU per stage per view.** Hook **0x407F240** (command-buffer close = D3D11 execution, render thread, immediate
   context `[cmd+0x48]`). Around the original: `End(timestamp)` before and after. Name it by `[cmd-0x30]` when that is a
   known stage vtable (ids from the job dump), else "sub-buffer of the last stage". The current view comes from a TLS
   set in view slot 4 **0x3CC7010**. One TIMESTAMP_DISJOINT per frame, begun in the Present hook. Read 3 frames later
   with DONOTFLUSH. Also QPC around the same call: render-thread D3D11 cost per stage. gpu_trace already hooks the
   context, so add draw and dispatch counts per (view, stage).
2. **CPU per job, keyed by view** (QPC on entry/exit, worker thread id): 0x3F38A40 (MOC), 0x3E24970 (cull), 0x3F7A700
   (feedback), 0x3DFB6A0 (0x35), 0x3E00E30 and 0x3E00310 (per-light culls), 0x3E01E00 (atlas), 0x3E15670 split into
   0x3E15C50 / 0x3E16830, 0x3E541B0 (0x48), 0x17DE2A0 (0x3A), 0x18126A0 (0x3D), 0x3E47BB0 (0x44).
3. **Atlas match.** At the eye's 0x3E01E00: light counts of both lists, lights in one list only, matched lights with a
   different +0x10 / +0x18. Histogram with and without hook (a). Prediction: near 100 % match with (a).
4. **MOC inputs.** At entry of 0x3F38A40, per view: job +0x34, +0x40..+0x7C, +0x80..+0x88, `[view+0x2180/+0x2184]`, and
   +0xA4 (occluder count). If equal between views every frame, item 5 is exact.
5. **Skin slot.** Once per second: `[view+0x226C]` of both eyes; count of meshes in each view's 0x41 sets; for 4
   renderables in both sets, `+0xF0..+0x104` / `+0x110..+0x124`. If the eye's index is 0 and the offsets differ between
   the views' jobs, the race is real.
6. **Counts**: `[[0x614FBC0]+0x20]` workers, and per frame the number of shadowed lights, skinned meshes and morph
   meshes per view.

Read it this way: a stage whose eye GPU time is large and whose A/B removes it is a win. If the render thread's
0x407F240 time is near the frame time, items 1, 2 and 4 (fewer eye commands) matter most. If workers are saturated,
items 3, 5 and 6 matter.

## Raw files (agents\AT\)

at1 0x3E15670 / 0x3CCDAC0 / 0x3F38A40 / 0x3E246B0 / 0x3B88E40; at2/at4/at10/at20/at21/at23 references; at3 0x3F06CC0;
at5 0x3B870C0; at6 0x3F7E920 / 0x3F7A530; at7/at8 0x3F7A510 / 0x3F7A700; at9/at13 failed writer searches; at11 0x3F06FC0;
at12 displacement scan +0x34/+0x8C/+0xA4; at14 0x3CC6C00 / 0x17F21E0; at15 0x3F59B10 / 0x3F5D2E0 / 0x3E541B0; at16
0x3CD27F0 / 0x3D27900 / 0x3D30BB0; at17 0x3E15C50 / 0x3E16830 / 0x3E17490 / 0x3E16330; at18 0x3D30E40 / 0x3DD21D0;
at19 0x3CF0080; at22 stage 0x3A slots; at24 0x192ED80. rd.py reads exe data.

## Open leads

* The per-frame writer of the MOC job's matrix (+0x40) and budget (+0x34).
* The exact insertion point for a two-view union in 0x3E15670 (item 4).
* The consumer fields that hold the 0x3A object in stages 0x3D / 0x3B / 0x3C (item 7).
