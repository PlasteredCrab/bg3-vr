# Stereo lighting mismatch and one-eye outlines (agent N, 2026-10-20)

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_A, read-only, headless) plus the game's own
DX11 shader bytecode. RVAs relative to 0x140000000. Raw material in `re-local\ghidra\agents\N\`:
r1_begin.c (0x3CC6C00 / 0x3CC7490 / 0x3CC70B0), r2_cb.c (0x3F3CF00), r8_ctrl.c (controller axes, frustum,
0x1B matrix helpers), r10_mat.c (controller matrix setters), r12_outline.c / r15_outpost.c / r17_outstep.c
(outline stage and post step), r13_cull.c / r14_culljobs.c (CullStage), r20_2961.c, r21_create.c,
r22_setup.c (0x588E50). Shaders: `N\shaders\` (extracted read-only from Data\EngineShaders.pak with the
project's LSLib Divine) and `N\shaders_asm\*.asm` (disassembled with d3dcompiler_47 D3DDisassemble; the
bytecode has no reflection, so registers are cbN[i] = constant buffer bN, 16-byte row i).
Agent A's decompiles are cited as A/dec/<file>.

## Hook points for the VR mod

| need | RVA | how to use | confidence |
|------|-----|-----------|------------|
| Per-eye PerView (the lighting/fog camera) | **0x3CC6CB9** (existing `render_view_camera_built`, rdi = view) | In the same hook, for each block i in {0,1}: PerView_i = view+0x2340 (i=0) / view+0x24D0 (i=1). Write +0x30..+0x6F = inverse of the eye view matrix (4x4, same column-major layout as the view at PerCamera+0x00), and the three corner rays +0x70/+0x80/+0x90 (w = 0) = `far * (F + R*(nx - P[8])/P[0] + U*(ny - P[9])/P[5])` for (nx,ny) = (-1,1), (3,1), (-1,-3), with P = the rewritten PerCamera_i +0x40, far = PerCamera_i +0xC4, R/U/F = rows 0/1/2 of the eye view's rotation. Do it in the mono / alternate path too. | Confirmed (CPU layout and shader use); Unconfirmed that it removes the whole artefact (run test below) |
| Stage 0x1B temporal reprojection | **0x3E32C44** (rdi = stage, rbx = view; just after `call 0x3E33030`) | Overwrite the 16 doubles at [rsp+0x20..+0x9F] (the current VP the stage built from the controller) with `P x V` from the view's rewritten PerCamera[1] (+0x40 times +0x00, full view with translation, in doubles). The reprojection `stage+0x2580 = VP_prev x VP_cur^-1` and the stored previous VP (+0x24F8) then follow the eye, per view. | Confirmed (code); register/stack layout Confirmed from disassembly |
| Stage 0x40 camera-velocity blocks (TAA, outline TAA) | **0x3E12730** (rbx = stage, view = [rbx+8]; before the job submit) | Replace the stage's current block: +0x2230 view (0x40), +0x2270 projection (0x40), +0x22B0 rotation-only VP (0x40), +0x22F0 position (12 B) with the view's rewritten PerCamera (block [1] if `[DAT_14604AA10+0x80]` in {2,3,4} or `+0x77` in {2,3,5}, else block [0]). On the first frame (`[rbx+0x2148]` was 0) also copy it into the previous block +0x2150..+0x221B. Next frame's previous = this block, so the history becomes per-eye. | Confirmed (code + VelocityBufferCamera / PPTAA use cb8) |
| Symmetric camera FOV / aspect at the source | controller +0x14 (vertical FOV, rad), +0x20 (aspect); used by 0x4086010 (CPU frustum), 0x3CC7490 (PerView corners), 0x3CCCFD0 (jittered projection) | Write a symmetric frustum that encloses both eyes (`fov = 2*atan(max(u,-d))`, `aspect = max(r,-l)/max(u,-d)` over both eyes' tangents) together with the projection override, then set ProjectionDirty (+0x11). 0x407BE10 resets the aspect from the display viewport when a camera is set, so re-assert each frame in `on_view_update`. Fixes culling and cascade fitting at the eye edges; not a substitute for the per-eye rewrite. | Confirmed (readers); value effect Unconfirmed |
| Jittered projection for an eye view | 0x3CCCFD0 (rcx = view; callers 0x3CC7E4F, 0x3E122E9, 0x3E32C2A) | Alternative to the two stage hooks above for the projection part only: at return, if the view is an eye view, set out[0], [5] and the non-jitter parts of [8], [9] to the eye's terms. The view matrix those stages read (controller +0x90) still needs the hooks above. | Confirmed |
| Outline post step gate (one-eye outlines) | **view+0x2961** read at 0x17FCF49 (outline post-step prepare 0x17FCF10); setter 0x3CCCC20 | Log it for both eye views. If the eye view's byte differs from the main view's, copy `[main+0x2961]` into `[eye+0x2961]` every frame (a plain byte store; don't call 0x3CCCC20, which also rebuilds the tile grid). | Gate Confirmed; that it differs: Unconfirmed |
| Outline object pass gate | stage 0x1F prepare 0x17DB4C0 (`stage+0x18`), job 0x17DB570 | Log `stage+0x18` (runs this frame), `stage+0x2140` (= [level+0xB8]) and the view's CullStage `m_OutlineObjects` count `[cull+0x2164]` for both eye views. | Confirmed (code) |

## 1. Camera-dependent inputs and when they're captured

### 1.1 Begin-recording order (Confirmed, disassembly 0x3CC6C54..0x3CC6D2C, decompile r1_begin.c)

0x3CC6C00(view), rdi = view, on the render thread:
1. view vtable +0x58, 0x3CC6DC0 (environment flags), open the command buffer.
2. 0x3CC6C6F `0x3CC7490(view, view+0x2340 PerView[0], view+0x2660 PerCamera[0], 0)`.
3. 0x3CC6C88 `0x3CC7490(view, view+0x24D0 PerView[1], view+0x2740 PerCamera[1], 1)`.
4. 0x3CC6CB4 `0x3CC70B0(view+0x2290, [view+0x21D8], [[view+0x28A0]+0x234])`.
5. **0x3CC6CB9, the mod's stereo hook** (`lea rdx,[rdi+0x2340]`).
6. Copy and upload: PerView[0] -> [view+0x2820], PerView[1] -> [view+0x2838] (0x3CC5830 + 0x3CCDD00),
   PerCamera[0] -> [view+0x2850], PerCamera[1] -> [view+0x2868] (0x3B915D0 + 0x3B97160).
7. Every stage's vtable +0x30 (slot 6, execute) in view+0x28 order.

Steps 6 and 7 run after the hook, but any stage that reads the controller `[view+0x21D8]` directly in step 7
(0x1B, 0x40) never sees the rewrite. The PerCamera blocks have no CPU readers outside the constructor
0x3CC5AA0 and 0x3CC6C00 (FindDisplacement 0x2660 / 0x2740, r11_d*.txt).

### 1.2 PerView block (400 B, b12): built by 0x3CC7490, NOT rewritten by the mod (Confirmed)

From r1_begin.c (`param_2` = PerView, `plVar5` = controller):
* +0x00..+0x2F sun/light, +0xA0..+0x16F fog/atmosphere/sky (A 3.3): camera-independent.
* **+0x30..+0x6F = controller +0xD0..+0x10F = inverse view (camera to world) of the head.** Controller
  +0x90 is a matrix set: 0x408BD60 (the view setter, called from 0x4085150 on `controller+0x90`) stores the
  view at +0x00 and its inverse at +0x40 (r10_mat.c 74-96); 0x408C030 stores the projection at +0x80
  (controller +0x110) and its inverse at +0xC0 (controller +0x150).
* **+0x70 / +0x80 / +0x90 = far-plane rays to the three vertices of the oversized full-screen triangle**,
  w = 0 (r1_begin.c 520-570):
  `far = ctrl+0x1C`, `hY = tan(ctrl+0x14 / 2) * far`, `hX = hY * ctrl+0x20`;
  jitter, block [1] only: `jx = hX/(w/2) * jit.x`, `jy = hY/(h/2) * jit.y` (0x3F11710 sample;
  w, h = view+0x2180 / +0x2184);
  F, R, U = the controller's rotation applied to +Z (0x4084ED0), +X (0x4084CB0), +Y (0x4084DC0) (r8_ctrl.c:
  the constant 1.0 is added to z, x, y respectively);
  `base = F*far + R*jx + U*jy`, `top = base + U*hY`, `W = R*hX`;
  +0x70 = top - W (NDC (-1, 1)), +0x80 = top + 3W (NDC (3, 1)), +0x90 = base - 3hY*U - W (NDC (-1, -3)).
  These use the controller's **FOV field +0x14 and aspect +0x20**. The mod never writes either:
  camera.cpp only overrides the matrix passed to the projection setter, and `kFov` / `kAspectRatio` are
  only read (camera.cpp:338/340, 562). So the corners describe the game's own FOV setting, symmetric,
  around the head.

### 1.3 PerCamera blocks (0xE0, b11): rewritten by the mod (Confirmed, A 3.3)

View (+0x00), projection (+0x40), projection x view rotation (+0x80), near/far (+0xC0/+0xC4), position
(+0xD0, controller slot 9). The view bind 0x17F26F0 (A/dec/viewbind.c) binds b13 frame, b12 PerView [0] or
[1], b11 PerCamera [0] or [1] (the jittered pair when `[DAT_14604AA10+0x80]` in {2,3,4} or `+0x77` in
{2,3,5}), and b7 focus.

### 1.4 What the shaders read (Confirmed from the game's DX11 bytecode, N\shaders_asm)

cb11 = PerCamera rows (0-3 view, 4-7 projection, 8-11 rotation VP, 12 near/far, 13 position);
cb12 = PerView rows (3-6 inverse view, 7/8/9 the corner rays).

| shader (stage) | camera inputs | consequence today |
|----------------|---------------|-------------------|
| ClusteredLighting_N (0x1B lighting sub-jobs) | ray = barycentric mix of cb12[7], cb12[8], cb12[9] from the pixel's viewport UV (lines 106-115); world position = ray * linear depth + **cb11[13]** (eye position) (line 1282); G-buffer normals to world with cb12[3..5]; no cb11[0..11] at all | rays from the head's game-FOV frustum, position offset from the eye: every light, shadow-cascade and SSR lookup is done at the wrong world point |
| ClusteredAssignment (0x05) | cluster bounds from cb12[7..9] + cb11[12]; light volumes projected with **cb11[8..11]** (eye rotation VP); cb12[3], cb12[4] (head right/up) | light-to-cluster binning mixes the eye projection with the head/game-FOV cluster frustums |
| ClusteredLightingClassification | cb0, cb9 only | none |
| VolumetricFogInitialize / Lighting / Gather | froxel rays from cb12[7..9], position from cb11[13], cb11[12] | haze volume built in the head's game-FOV frustum |
| DeferredFog_Trilinear / _Cubic (sub-job 0x3E42130) | cb6 (cluster CB), cb9 only; draws the 3-vertex triangle | samples the mis-placed froxel volume |
| EoCFog | cb11[13] | eye position (ok) |
| VelocityBufferCamera (0x40) | VS: ray = cb12[7 + vertex index] (`mov o1.xyz, cb12[r0.x + 7]`); PS: `p = depth*ray + (cb11[13] - cb8[12])`, projected with cb8[8..11] | cb8 is the stage-0x40 block (head, game FOV): wrong velocity even with a still head (eye/head offset) and wrongly scaled during rotation, so TAA history is misaligned |
| PPTAA, EoCOutlineTAA | cb8 (stage 0x40 block) jitter | as above |
| PPSAO | cb11[5] (P[5] only) and cb11[12] | ignores the eye's off-centre terms P[8], P[9]: small AO error, no slide |
| EoCOutlineObject_ST / _STI | cb11[0..2], cb11[8..12] | eye (ok) |

### 1.5 Stage 0x1B execute 0x3E32B50: its own VP and temporal reprojection (Confirmed)

A/dec/s1B.c 581-760, r8_ctrl.c, disassembly 0x3E32C0E..0x3E32C7E:
* `P = 0x3CCCFD0(view)` (symmetric, from the controller's FOV / aspect / near / far plus jitter, always
  jittered), then `0x3E33030(P, ctrl+0x90, rsp+0x20)` = P x V_head as 16 doubles at [rsp+0x20..+0x9F].
* `stage+0x24F8..+0x2577` = the previous frame's VP, per stage, i.e. per view (initialised from the current
  one when `stage+0x2411` is clear).
* 0x3E33540 = 4x4 inverse; 0x3E33A70 = product: **`stage+0x2580 = VP_prev x VP_cur^-1`**, then prev = cur.
* `stage+0x25C0/+0x25C4` = n/(n-f), n/(f-n); copied with the reprojection into the CB staging area
  `+0x2420..+0x24DF`, which sub-jobs 0x3E3CCE0 and 0x3E3ABB0 upload (A/dec/s1B.c 5655-5680, 7233-7260).
  0x3E3CCE0 also passes the controller's slot-9 position (head) to 0x3E1D830 (s1B.c 5425, 6018).
* Runs in step 7, after the hook, from the controller: head pose and game FOV, not the eye.

### 1.6 Stage 0x40 execute 0x3E12240: camera velocity / TAA (Confirmed)

A/dec/st_40.c 294-600, disassembly 0x3E12718..0x3E12765. It builds PerCamera-shaped blocks inside the
stage from the controller: current at +0x2230 (view +0x2230, projection +0x2270, rotation VP +0x22B0,
position +0x22F0, jitter +0x2308/+0x230C) and previous at +0x2150 (= last frame's current, or the current
one when `stage+0x2148` is clear). The projection is 0x3CCCFD0 in TAA/upscaler modes, else controller
+0x110. Per stage, so per view. Head pose, never the eye. The outline job 0x17DB570 also reads it
(`0x14407AE00(group, 0x40) + 0x2150` through 0x17DA570).

### 1.7 Clustered lighting constant buffer 0x3F3CF00 (0x220 B, cb6) (Confirmed)

Callers (r3_callers.txt): 0x3D961D0 (stage 0x00), 0x3E06930 (0x05), 0x3E1FF80, every 0x1B sub-job
(0x3E33F10, 0x3E35C80, 0x3E38940, 0x3E3ABB0, 0x3E3CCE0, 0x3E3F200, 0x3E42130), 0x3E54C00. Contents relevant
here (r2_cb.c; disassembly 0x3F3D16C..0x3F3D3A5):
* Z slicing: +0x1F0 = 72 (tile px), +0x1F4 = 45 slices, +0x1E0 = 45 / (log2 far - log2 max(near, 1)),
  +0x1E4 = 1 - log2(near) * scale; near/far from controller +0x18 / +0x1C (the mod doesn't change them).
* The view's tile window: +0x150..+0x15C = (render viewport x/y/x+w-1/y+h-1 (view+0x2188/+0x218C/
  +0x2180/+0x2184) / tile size +0x229C + 0.5 [+ split index [view+0x2268] for x]) / grid (+0x29B0/+0x29B4).
* +0x170..+0x19F = view+0x22C0..+0x22EF (set by the constructors 0x3CC5AA0 / 0x3AC51C0).
* Sun-shadow cascade matrices from the shadow stage (`param_3+0x2490..`).
No camera matrices: the cluster shaders take the camera from b11/b12 (1.4).

### 1.8 Stage 0x05 clustered light assignment 0x3E06930 (Confirmed)

Binds view slot 16 (b13/b12/b11), uploads the 0x3F3CF00 CB (cb6), a 0x20 CB and a 0x10 CB, and dispatches
`ceil(viewport/72)` groups. Its shader mixes b11 and b12 (1.4).

### 1.9 Froxel depth distribution 0x3CC70B0 (Confirmed)

It reads only near/far (controller +0x18 / +0x1C), `[view+0x28A0]+0x234` and the view's slice settings
(view+0x2290..). It doesn't depend on pose or FOV, so stereo doesn't affect it.

### 1.10 Sun shadow cascades 0x1E

The cascade cameras are fitted on the CPU (0x3E02440 uses the cascade controller's axes 0x4084CB0/DC0/ED0;
which gameplay-camera data feeds the fit was not traced, Unconfirmed). The cascade matrices are world-space,
so they don't depend on the eye. The lookup happens in clustered lighting at the reconstructed position, so
shadows slide with the lighting until the PerView fix is in. stereo.md: the cascades follow the camera that
is driven at the source (the head), which is fine for both eyes.

## 2. Why the lighting slides opposite to the head rotation

| data | source | eye or head? | FOV |
|------|--------|--------------|-----|
| geometry (G-buffer, forward, outlines) | PerCamera +0x80/+0x00/+0x40 (b11) | eye (hook) | eye, asymmetric |
| lighting / fog / velocity view rays | PerView +0x70/+0x80/+0x90 (b12) | head rotation | **game FOV field ctrl+0x14, aspect ctrl+0x20**, symmetric |
| world position base | PerCamera +0xD0 (b11) | eye | - |
| normals to world | PerView +0x30 (b12) | head | - |
| 0x1B temporal reprojection | stage+0x2420 | head | game FOV |
| 0x40 velocity "previous camera" | stage+0x2150 (cb8) | head | game FOV (TAA modes) |

Clustered lighting computes a pixel's world point as `eyePos + depth * ray(uv)`, where `ray(uv)` comes
from the game-FOV head corners. With NDC x, the ray's tangent is `x * tH` (`tH = hX/far`, the game's
horizontal half-angle tangent). The depth at that pixel, though, belongs to the eye ray with tangent
`cx + x * sx` (`sx = (r-l)/2`, `cx = (r+l)/2`). A world point at camera angle a appears in the geometry at
`x_g = (tan a - cx)/sx`, but its lighting appears at `x_l = tan a / tH`. Whenever the game's FOV setting is
narrower than the headset's (`tH < sx`; the eye is about 90-100 degrees wide), the lighting image is a
magnified copy of the true one. Turn the head right by d: `tan a` drops for every point, the geometry moves
left by about `d/sx`, and the lighting moves left by the larger `d/tH`. Relative to the world, the lighting
slides left, against the turn. Vertically it's the same with `hY/far` against `(u-d)/2`: looking up makes
it slide down. The eye's off-centre terms `cx`, `cy` add a constant offset per eye, opposite for the two
eyes, which is why the two eyes disagree.

Hard-edged rectangles: lights are binned per 72-px tile and 45 log-depth slices (0x3F3CF00). The
assignment shader builds each cluster's bounds from the head/game-FOV corners but projects light volumes
with the eye's rotation VP (1.4). Shading then evaluates each tile's lights at the mis-reconstructed
positions. A light ends up lighting whole 72-px tiles wherever the mismatched binning put it, and nothing
outside those tiles, so it shows as a block of tiles that doesn't follow the geometry. Volumetric haze
uses the same froxel/tile grid and rays (VolumetricFog* use cb12[7..9]), so the same happens to it.

Runtime confirmation (one run, log everything): at 0x3CC6CB9 for each eye view, log the controller's +0x14,
+0x20, +0x18, +0x1C; PerView[0]/[1] +0x30..+0x9F before the rewrite; PerCamera[0]/[1] +0x40 after the
rewrite; and the eye tangents. Prediction: `tan(fov/2)` and `tan(fov/2)*aspect` are smaller than
`(u-d)/2` and `(r-l)/2`, by the factor the lighting overshoots. Then turn on the PerView rewrite: the
slide and the rectangles should go away. camera.cpp:338 already logs FOV and aspect once at attach.

## 3. What's inconsistent and the fix

Inconsistent today (all Confirmed above):
1. PerView[0]/[1] +0x70/+0x80/+0x90 (corner rays): head rotation, game FOV, symmetric. **Main cause.**
2. PerView[0]/[1] +0x30..+0x6F (inverse view): head. It only rotates normals (the eye rotation is the
   head's on Quest 2), but rewrite it for consistency and for future canted displays.
3. Stage 0x1B's reprojection (stage+0x2420 / +0x2580 / +0x24F8): head, game FOV.
4. Stage 0x40's camera blocks (stage+0x2150 / +0x2230) = cb8 of the velocity, TAA and outline-TAA passes:
   head, game FOV. The velocity is wrong even with a still head, so TAA mixes in misaligned history.
5. The controller's FOV/aspect fields are the game's setting, so the CPU frustum (0x4086010, culling),
   the PerView corners and 0x3CCCFD0 are all narrower than the eye.

Fix, in order:
1. **Extend the existing hook at 0x3CC6CB9** (stereo.cpp `on_camera_built`, both the per-eye path and the
   mono path) to rewrite PerView_i (view+0x2340, view+0x24D0) after rewriting PerCamera_i:
   ```
   V = PerCamera_i+0x00 (rewritten eye view, column-major, V[c*4+r]); P = PerCamera_i+0x40; far = PerCamera_i+0xC4
   R = (V[0],V[4],V[8]); U = (V[1],V[5],V[9]); F = (V[2],V[6],V[10])
   ray(nx,ny) = far * ( F + R*(nx - P[8])/P[0] + U*(ny - P[9])/P[5] ),  w = 0
   PerView_i+0x70 = ray(-1, 1); +0x80 = ray(3, 1); +0x90 = ray(-1, -3)
   PerView_i+0x30..+0x6F = inverse(V)   (rigid: [R^T | position], same storage as controller +0xD0)
   ```
   Check: with the game's own symmetric P (P[8] = -jx_near/hX_near, P[0] = n/hX_near) this formula gives
   exactly 0x3CC7490's corners, jitter included (block [0] has no jitter, matching PerView[0]).
2. **Stage 0x1B**: mid-hook 0x3E32C44, overwrite the doubles at [rsp+0x20..+0x9F] with
   `PerCamera[1].P x PerCamera[1].V` (block [1], because 0x1B always uses the jittered 0x3CCCFD0).
3. **Stage 0x40**: mid-hook 0x3E12730, overwrite stage+0x2230 (view), +0x2270 (projection), +0x22B0
   (rotation VP), +0x22F0 (position) from the view's PerCamera (block [1] in TAA/upscaler modes, else [0]);
   on the first frame also +0x2150.. (previous).
4. **Source FOV** (culling and edges): write the controller's +0x14 / +0x20 to a symmetric frustum
   enclosing both eyes whenever the projection is overridden, and re-assert it each frame (0x407BE10
   resets the aspect).
5. A better single hook point? The two eye views share one controller, and four separate consumers read it
   directly (0x3CC7490, 0x3E32B50, 0x3E12240, slot-9 position in the instance/draw code 0x3CD87B0). The
   alternative is one CameraController per eye view (SetCamera 0x17F2B30 on each view, both driven from the
   headset at the view update 0x4085150). That makes every downstream consumer consistent, including CPU
   culling per eye. The FOV/aspect fields are still symmetric only, so items 1 (corners) and the
   0x3CCCFD0 projection still need the asymmetric terms. Items 1-3 are the smaller change and fix
   everything this report found.

## 4. One-eye outlines

Rendering path (Confirmed from code):
* **Stage 0x1F** (vtable 0x55F3530; constructor 0x17DA740, size 0x2280; created for every non-special
  view by 0x588E50 with the view's own CullStage 0x08 as +0x2130, r22_setup.c 206). Prepare 0x17DB4C0
  sets `stage+0x18` only if gate 0x17DA690 passes, and `[view+0x2B10]+0x48 == [DAT_146007A28]+0x90`
  (level), and `stage+0x2140 = [level+0xB8]` is non-null, and `[DAT_14604B1B8+0xE]`, and
  `0x17DD430([DAT_145FDA240+0x1A8])`, and the view has a camera. Job 0x17DB570: if the view's CullStage
  bucket `m_OutlineObjects` (CullStage+0x2150, count +0x2164; name set at r13_cull.c 290) is empty, it
  clears the outline mask (key 0x5AE0F2DF). Otherwise it draws those objects into the mask in the view's
  viewport (EoCOutlineObject_ST/STI, cb11 = eye) and runs the ClearRT/blur compute (ceil(w/32) x
  ceil(h/32)).
* **Outline post step** (vtable 0x55F4900, constructor 0x17FCD00; attached to stage 0x21's post chain by
  0x588E50): prepare **0x17FCF10** enables it (`+0x18 = 1`) only if `[view+0x2961] == 0`, the same
  level/scene check, `[DAT_14604B1B8+0xE]` and 0x17DD430. Shader setup 0x17FE400 (EoCOutlineTAA when
  `[DAT_14604AA10+0x80] == 2`, with keyed resources 0x1C84A002 and 0xDC7D4A87 via 0x4096520, shared by
  all views like every keyed resource), 0x17FE800 (Combine / BlurH / BlurV).
* Per-player selection: CullStage params 0x3E246B0 pick the visibility mask from the split index
  (0x201 for index 0, 0x401 for 1, r14_culljobs.c 1048-1056), and the cull job 0x3E24970 hands its
  sub-jobs a per-player block `0x1817500(...) + index*0x30` (r14 400-460). SE has per-player highlight sets
  (`UpdateCharacterHighlights[3]`, Visual.h:1121). Both eye views render with index 0, so object
  selection is identical for both.

What differs for the created eye view (Confirmed from code):
* It never goes through the player manager's add-player-view 0x3BAA1A0, which registers the view in the
  manager's player->view map (`0x3BAB270(mgr+0x30, player)+8 = view`, mgr+0xD8/+0xE0). So
  0x3BAAE20(player) never returns it, and per-player-view updates skip it: 0x1D11580 (writes
  `view+0x2961` through 0x3CCCC20 on the player's view, or the pipeline main view), 0x1F90B00 (the same
  for the manager's view list), 0x17E7FD0 (player data +0x2BE8..+0x2BFC), and 0x17F2230 (focus data).
* `view+0x2961` is written only by 0x3CCCC20 (FindDisplacement r18_d2961.txt: the one write at
  0x3CCCC29). The constructors 0x17F1E60 / 0x3CC5AA0 don't write it, and neither does CreatePlayerView
  0x17F82F0 (r21_create.c). 0x3CCCC20 is called with 0 by view vtable slot 14 (0x17F2180 / 0x3CC6BB0),
  which the mod's create path doesn't call. So the eye view's byte is whatever the allocation held, or
  never follows the game's per-player updates.

Most likely cause (Unconfirmed): `[eye+0x2961] != 0`, which disables the outline post step (combine) for the
eye view. Next candidates: the eye view's stage 0x1F not running (`stage+0x18` = 0 because of the scene
check), or an empty `m_OutlineObjects`.

Fix: each frame, before begin-recording, copy `[main+0x2961]` into `[eye+0x2961]` (byte store only). If
the log shows stage 0x1F off, make the eye view's `[view+0x2B10]` the same scene as the main view's.

Runtime log that settles it (one run, hover an item and hold left-alt): every frame for both eye views, log
`[view+0x2961]`, `[view+0x2268]`, `[view+0x2B10]` and its +0x48, `[[DAT_146007A28]+0x90]`; for stage 0x1F
(find it in the view's stage array by id 0x1F) `+0x18` and `+0x2140`; the CullStage's `[+0x2164]`
(outline object count); and the outline post step's `+0x18` (the step object on stage 0x21 whose vtable
is exe+0x55F4900).

## Open leads

* Which component 0x3B967F0 reads FOV from, and whether writing it is cleaner than
  writing controller +0x14/+0x20 (both are re-derived each frame?). Confirm with a log of controller +0x14
  before and after the view update 0x4085150.
* The gameplay-camera inputs of the cascade fit (0x3E01E00 / 0x3E02440): not traced.
* 0x3E1D830 (called from 0x1B sub-job 0x3E3CCE0 with the controller's position): what it selects by
  camera distance (local-light shadow LOD?). It uses the head position for both eyes, which is harmless.
* 0x3DFC660 / 0x3DFD980 read +0x22C0 / +0x2340 of some object (rsi/rbx): stage 0x35 fields or the view's
  PerView sun direction? Decompile if the shadow atlas misbehaves per eye.
* The outline TAA keyed resources 0x1C84A002 / 0xDC7D4A87 are shared between views. Check for
  ghosting between eyes once outlines show in both.
* SSR is in ClusteredLighting_*_SSR and uses the same cb12 rays: covered by fix 1, not separately checked.
