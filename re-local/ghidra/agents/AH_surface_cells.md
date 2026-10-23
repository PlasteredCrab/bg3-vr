# Blocky gaps in blood / ground surfaces, different per eye (agent AH, 2026-10-23)

Static analysis of bg3_dx11.exe 4.1.1.7631656. Ghidra project BG3_A (read-only, headless, request server
`agents\AH\AHServer.java`, stopped). The project disassembler. The engine compute shader `EoCSurfaceMeshGenerate.bshd`,
extracted read-only with LSLib Divine from `Data\EngineShaders.pak` into `AH\shaders\` and disassembled with
`S\dis_bshd.py` into `AH\shaders_asm\`. The mod's `stereo.cpp`, `camera.cpp`, `headset.cpp`, `vr_math.cpp`. RVAs are
relative to 0x140000000. cbN[i] = constant buffer bN, row i. "main" = left eye view, "eye" = right eye view. Raw output is
in `re-local\ghidra\agents\AH\` (ah1..ah18).

## Fix table

| # | what | RVA / hook | how | confidence |
|---|------|-----------|-----|------------|
| 1 | **Correct the per-tile coverage pass (stage 0x3D, `EoCSurfaceMeshGenerate`) for each eye's projection** | new mid-hook at **0x1812942** in stage 0x3D job **0x18126A0** (`mov [rcx+0x20], edi`; both branches of the cb0[1] build join here). rcx = cb0 data, rbx = the view, `[rbp+0x08]` / `[rbp+0x0C]` = the view's viewport width / height (int32) | If `stereo::eye_of(rbx) >= 0` and the controller `[rbx+0x21D8]` byte `+0x12` (orthographic) is 0: read the view's unjittered eye projection from PerCamera block [0] (`view + kPerCamera0 + kProjection`): a = P[0], c = P[5], b = P[8], e = P[9]. Write 4 floats at `rcx+0x10`: `2/(w*a)`, `-2/(h*c)`, `(-1-b)/a`, `(1-e)/c`. | Mechanism Confirmed (code + shader). Effect: Unconfirmed (logging plan 1, A/B 4) |
| 2 | **Give each eye's proxy the eye's projection and its inverse** | `stereo.cpp on_view_begin` (render thread, 0x17F21E0 hook), right after the proxy copy | `P = eye_projection(controller+0x110, fov, 0, 0)` (keeps the game's z terms, the same matrix `rewrite_block` puts in block [0]). Write it to proxy **+0x110**. Write its inverse to proxy **+0x150** (general 4x4 inverse as the game's setter 0x408C030 does, or the closed form below). | Mechanism Confirmed (code). Effect: Unconfirmed (plan 2, A/B 4) |
| 3 | Ship 1 and 2 together, behind one switch (e.g. `[views] eye_surface_mesh = true`) | - | Either one alone is predicted to look **worse** than today (section 3.3). | Derived from Confirmed code |
| - | Race between the eyes' recordings: **not a cause** | - | No change. The zero-code A/B `[views] serialize_eyes = true` is predicted to change nothing. | Confirmed (section 2) |

Closed-form inverse for fix 2 (column-major, as stored by the game and by `vr::projection`):
`P = [a,0,0,0, 0,c,0,0, b,e,zs,1, 0,0,zo,0]` gives
`P^-1 = [1/a,0,0,0, 0,1/c,0,0, 0,0,0,1/zo, -b/a,-e/c,1,-zs/zo]` (zs = P[10], zo = P[14]). Use it only when P[11] == 1
and P[15] == 0; otherwise skip the eye.

## 1. Cause

### 1.1 How surfaces are drawn (Confirmed)

Per view the game builds four surface stages (0x588E50, `ah2`):

| stage | object | what it does |
|-------|--------|--------------|
| 0x3A | vt 0x55F35F8, job **0x17DE2A0** | Selects surfaces. CPU only. |
| 0x3D | vt 0x55F6E70, init 0x1812350, prepare 0x18125B0, job **0x18126A0** | Compute pass `EoCSurfaceMeshGenerate`. Writes a per-tile index buffer. |
| 0x3B | vt 0x55F40E0, job 0x17F1B40 (inside 0x46) | Draws each surface with 0x1B7AA80 (G-buffer). |
| 0x3C | vt 0x55F4AB0, job 0x1800B00 | Same draw for the emissive variant. |

* 0x3D's grid is the view's viewport in **16x16 pixel tiles**: prepare 0x18125B0 sets `+0x2178 = ceil(w/16)`,
  `+0x217C = ceil(h/16)` from the view rect 0x407B8C0. The compute shader runs 16x16 threads per group, one group per
  tile (`dcl_thread_group 16, 16, 1`).
* For each pixel the shader rebuilds the world position from linear depth (t2 = 0xFF193183) (`ah1`, asm lines
  163-187):
  `px = threadID + cb9[1].zw`; `z = depth * cb11[12].y`;
  `xy/z = (px - cb9[1].zw + 0.5) * cb0[1].xy + cb0[1].zw`;
  `world = cb12[3..5] * (xy, z) + cb12[6]`.
* It then tests up to 40 surfaces (t0 records, cell-type textures t4..t43 / t44..t83, heights t84..t123): bounds,
  `|world.y - height| < 1.0`, and the cell type over a 3x3 cell neighbourhood. If **any** pixel of the tile passes for a
  surface type, the tile gets two triangles for that type. Otherwise it gets a degenerate quad (asm 2453-2479). The
  output is the keyed index buffer **0x2A547A4B** (u0, created by 0x18135B0, `gx*gy*0x5A0` bytes = 120 types x 12
  bytes per tile).
* 0x1B7AA80 then draws `gx*gy*6` indices per surface type from that buffer over a grid of tile-corner vertices
  (vertex buffer from 0x1813230). It places the grid on a near plane with the controller's **+0x150 (inverse
  projection)** and **+0xD0 (inverse view)** (`S\s31`). The Surface_Default pixel shader then tests each pixel exactly,
  but it can only draw where a tile quad exists.

So a pool pixel is drawn only if its 16x16 tile was marked by the compute pass **and** the drawn grid lines up with
those tiles. A wrong tile mark shows as a missing 16x16 square.

### 1.2 Where the eye's projection is wrong (Confirmed)

* cb0[1] is built in 0x18126A0 at 0x1812855..0x181293E (`ah10`) from `[view+0x21D8]` = the proxy controller:
  `cb0[1] = (2/(w*P[0]), -2/(h*P[5]), (-1-P[2])/P[0], (1+P[6])/P[5])`, P = proxy **+0x110**, w/h = the view's viewport.
  The orthographic branch uses `1/P[0]`, `1/P[5]`, far.
* The proxy is a copy of the real controller with only +0x90 / +0xD0 / slot 9 replaced (`stereo.cpp on_view_begin`).
  So +0x110 / +0x150 are the real controller's.
* The real controller's +0x110 is the **head's union FOV**: `camera.cpp on_set_projection` writes
  `vr::projection(g_head->fov)` at the setter call 0x408541F, and `headset.cpp centre_view` makes that fov the union of
  both eyes. The setter **0x408C030** copies it to +0x110 and stores the full inverse at +0x150 (`ah16`).
* The eye's image (depth, G-buffer, cb11, cb12) uses the **eye's** asymmetric projection (`rewrite_block`, offsets in
  P[8] / P[9]).
* 0x18126A0 reads the off-centre terms from **P[2] and P[6]**, which are 0 in this layout. So even the game's own
  formula never applies an off-centre projection. With the union matrix it uses the union's scale and no offset.

Result: the compute pass marks tiles using wrong rays. The draw places the grid with the union inverse, then the eye's
projection moves it: a tile at eye NDC `n` is drawn at about `(a_eye/a_union)*n + b_eye`. For a Quest 2 eye that is a
scale of about 1.14 and a shift of about 0.14 NDC (around 100+ pixels), mirrored between the eyes.

### 1.3 Why it looks like the video (derived from 1.2)

The two errors partly cancel. The compute pass tests the rays of the place where the tile is actually drawn, but with
the depth of the original tile. On flat ground the depth changes slowly, so most tiles come out right. On a rock or
near an edge the depth at the two places differs, the height / cell test fails, and the tile drops out. The shift is
mirrored per eye, so each eye loses different squares. That matches "blood on a rock broken into patches with square
gaps, different in each eye". Fix 2 (CPU cull FOV) can't help: this is a GPU tile mask, not culling.

## 2. Uploads, threads and the race (question 1)

| data | produced by | per view? | recorded where | can one eye overwrite the other's? |
|------|-------------|-----------|----------------|-------------------------------------|
| surface list (records +0x40, textures +0x50, type list +0x60, render objects +0x70 of the 0x3A object) | 0x3A job **0x17DE2A0** -> 0x17DE4D0 / 0x17DE5C0 / 0x17DEBB0, worker thread, every frame | yes, own 0x3A object per view | CPU memory only, no GPU command | no (Confirmed) |
| t0 records (key **0x41CAEBA0**) and t1 type list (key **0x3D8ADBCB**) of the compute pass | 0x18126A0 (stage 0x3D job) | keyed = one resource for the frame | **inline copy into the stage's own command stream**: 0x407AE70 -> 0x40CA170 with mode 3 emits cmd 0x2E holding the data; 0x407AF40 emits cmd 0x2F (`ah9`, `ah11`, call sites 0x1812A00 / 0x1812AEA) | no: the data travels inside each view's stream (Confirmed) |
| per-tile index buffer (key **0x2A547A4B**) | 0x3D compute dispatch | keyed = shared | GPU write in each view's 0x3D stage | no: see below (Confirmed) |
| cb0 of the compute pass | 0x18126A0 (cmd 0x05 / 0x06) | per view | own stream | no |
| cell-type / height textures (t4..t123 here; t0 / t1 of Surface_Default) | not found in the per-view stages; pointers come from the level's surface data (0x17DE5C0: `lVar5+0x18`, `+0x20`, and 0x17E2010) | shared objects | **not uploaded by stages 0x3A / 0x3D / 0x3B / 0x3C** (Confirmed). Their writer: Unconfirmed | no per-view writer exists in these stages |
| t37 mask 0x5BE0FB7A, linear depth 0xFF193183 | stage 0x0B, per view (S) | per view | own stream | no |

Why no race (Confirmed): all command streams are translated to D3D11 on the render thread in view order (W 1: 0x4073040
closes the frame buffer, then each view's stages in order: main, eye). Within a view 0x3D runs before 0x46 (0x3B) and 0x17
(0x3C) (inserted with 0x407A560 in 0x588E50). So the GPU order is main dispatch, main draws, eye dispatch, eye draws.
The eye's dispatch can't reach main's draws. Parallel recording only builds the streams. The only CPU data shared at
record time is read, not written.

Camera-relative selection (question 2, Confirmed): 0x17DE2A0 takes the frustum planes from controller **+0x210** (6 or
10 planes; the proxy copies the head's, which the setter / 0x4086010 build from the widened +0x14 / +0x20) and the
position from **slot 9** (the eye). 0x17DE4D0 / 0x17DE5C0 keep octree nodes and 64x64-cell chunks within 60 m (distance^2
<= 3600) of that position and inside the planes, in a +/-60 m window. The eyes are 6 cm apart, so they select the same
surfaces except right at the 60 m edge. The shader's own gate is 60.5 m from cb11[13]. Neither can make square gaps
next to the player.

## 3. Fix details

### 3.1 Fix 1 (compute CB)

* Hook 0x1812942. Original bytes `89 79 20` (`mov [rcx+0x20], edi`) then `BA 57 00 00 00`. 0x1812942 is the target of
  the `jmp` at 0x18128FC, which is fine for a hook placed exactly there. No branch targets 0x1812945..0x1812949 in the
  function's listing (`ah10`).
* Registers at the hook: rcx = cb0 data (cb0[0] at +0x00, cb0[1] at +0x10, cb0[2] at +0x20), rbx = view, r13 = the
  job (stage+0x20), `[rbp+0x08]` / `[rbp+0x0C]` = viewport w / h ints (from 0x40814F0 -> 0x407B8C0).
* Derivation: eye NDC `x = a*x/z + b`, pixel `ndc_x = 2(px+0.5)/w - 1`, `ndc_y = 1 - 2(py+0.5)/h`. So
  `x/z = (px+0.5)*2/(w*a) + (-1-b)/a` and `y/z = -(py+0.5)*2/(h*c) + (1-e)/c`. With b = e = 0 this equals the game's.
* Use block [0] (unjittered). It is written in begin-recording (0x3CC6CB9 hook) before the stage jobs run.

### 3.2 Fix 2 (proxy +0x110 / +0x150)

* Other render readers of +0x110: 0x3CC7490 block [0] (the mod rewrites it with the same `eye_projection`, so the result
  is unchanged), 0x3E12240 non-TAA path (already hooked per eye; it now gets the eye's matrix directly), 0x18126A0 (fix
  1 overrides it). Reader of +0x150: 0x1B7AA80 only (Z 3.1).
* After fix 2 the drawn grid lands on the eye's own 16x16 tiles, the same tiles fix 1 marks.

### 3.3 Why both, not one

* Fix 2 alone: the grid is right, but the compute pass tests rays shifted by `b/a` (about 9 degrees for Quest 2) with
  no cancelling shift in the draw. Pool edges move on all ground, not just on rocks.
* Fix 1 alone: the marks are right, but the grid is still drawn scaled and shifted, so marked tiles land in the wrong
  place.

## 4. One-run logging plan

Load a save, stand next to a blood pool that lies on a rock and on flat ground.

1. **Compute CB** (in the 0x1812942 hook, log-only first, every 120th call per view plus the first 4): view role,
   w / h, the game's 4 cb0[1] floats, proxy +0x110 P[0], P[2], P[5], P[6], P[8], P[9], block [0] P[0], P[5], P[8], P[9],
   and the corrected 4 floats. Also cb0[0] (`rcx+0x00..+0x0F`, grid dims) and cb0[2] (surface / type counts).
   Prediction: P[2] = P[6] = 0; proxy P[0] = the union scale (same in both views); block [0] P[8] about +0.14 in one eye
   and -0.14 in the other; the game's cb0[1].z = -1/P[0].
2. **Proxy matrices** (on_view_begin, same rate): proxy +0x110 [0], [5], [8], [9] before and after fix 2, and
   `max|P * P^-1 - I|` after. Prediction: before = union, after = block [0] of the same view, residual < 1e-5.
3. **Counts per view** per 120 frames: calls of 0x18126A0, 0x17F1B40 and 0x1B7AA80 (key by `[rcx+0x10]` view),
   stage 0x3D +0x2178 / +0x217C. And the 0x3A object's +0x4C (records), +0x5C, +0x6C, +0x7C counts and the first
   3 texture pointers of +0x50, for both views. Prediction: same counts and the same texture pointers in both views.
4. **A/B** (hotkey or VR menu, 20 s each, looking at the rock pool):
   * off -> fix 1 + 2: prediction: the square gaps go away in both eyes.
   * only fix 1, only fix 2: prediction: worse than off (3.3). This confirms the two-part mechanism.
   * `[views] serialize_eyes = true` with the fix off (zero code): prediction: no change.
   * Mono mode (one view, union FOV in both +0x110 and the PerCamera block), if available: prediction: no gaps, because
     the matrices agree.

## 5. Open leads

* The writer of the per-surface cell-type / height textures and whether Surface_Default's t0 / t1 are those same
  textures (material binding 0x3CED1C0 from 0x1B7AA80). Not needed for the fix; plan 3 logs the pointers.
* The meaning of the game's P[2] / P[6] read in 0x18126A0 (always 0 in this layout). Fix 1 replaces the result anyway.

## Raw files (agents\AH\)

ah1 condensed compute shader; ah2 per-view surface stage setup 0x588E50; ah3 / ah5 stage and job vtables; ah4 stage
0x3A slots; ah6 0x3A job 0x17DE2A0; ah7 / ah8 surface selection 0x17DE4D0 / 0x17DEBB0 / 0x17DE5C0; ah9 keyed upload
0x407AE70 / 0x407AF40 / 0x40814F0 / 0x4081390; ah10 disassembly of 0x18126A0; ah11 0x4081620 / 0x40CA170 / 0x4081070 /
0x407B8C0; ah13 0x17E1560 / 0x17E2010; ah16 projection setter 0x408C030; ah17 its caller; ah18 stage 0x3D buffers
0x18135B0 / 0x1813230 / 0x1813B10 / 0x1814020. Shaders: `shaders\Shaders\DX11\EoCSurfaceMesh*.bshd`,
`shaders_asm\`.
