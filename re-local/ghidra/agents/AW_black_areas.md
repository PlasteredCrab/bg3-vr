# Black overhead geometry in first person - agent AW, 2026-10-24

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_G read-only, request server `agents\AW\AWServer.java`;
bg3vr_disasm). All addresses are RVAs (base 0x140000000). Raw output in `re-local\ghidra\agents\AW\` (w1..w18).
New for this report: the DX11 shader bytecode of the stage 0x33 shaders, read from the game's own
`Data\EngineShaders.pak` (LSPK v18, LZ4) with `AW\pak.py` / `AW\pakx.py` and disassembled with the Windows
`d3dcompiler_47.dll` `D3DDisassemble` (`AW\dxbc.py`). Output: `AW\shaders\*.bshd.asm`. Nothing was downloaded and
nothing was run in the game.
"Confirmed" = read in code or shader bytecode at the cited place. "Unconfirmed" = section 4 settles it in one run.

## Fix for the VR mod

| # | mechanism | RVA | exact hook / patch | what it changes | risk | confidence |
|---|-----------|-----|--------------------|-----------------|------|------------|
| A | **Room-trigger black-out** (stage 0x33, EoCRoomTriggerMask + EoCRoomTriggerBlur). Every room-trigger volume whose room is not active (no party member inside, not joined by an open portal) is drawn into a screen mask; the blur compute then lerps the lit image toward **(0,0,0,1)** by that mask, except where linear depth == 1 (sky). | job 0x1816520; branch at **0x18167A5** | safetyhook **MidHook at 0x18167A5** (bytes `83 B8 EC 21 00 00 00`, `cmp dword ptr [rax+0x21EC],0`, followed by `0F 8E 9F 0C 00 00` `jle 0x1817451`). In the hook: if first person, `ctx.rax = (uintptr_t)&g_zero_dword - 0x21EC` (a static 4-byte 0). Nothing else. | The job takes the game's own "no room triggers" path 0x1817451. The game takes that same path in every dialogue (`[view+0x2961] != 0`, test at 0x18167B2) and whenever no room-trigger visual is in view. The scene colour goes through unchanged, so overhead geometry is shown lit. Per job call, so it applies to main and eye alike and stops at once when FP ends. | Low. Register-only, no shared write, render-worker thread, the game's own branch. Rooms the game would black out (unentered rooms) become visible in FP. Picking and gameplay are not involved. | Mechanism **Confirmed** (code + shader). That it is the 15:09 black: **Unconfirmed**, high (4.1). |
| A' | same, if a MidHook is not wanted | 0x3CFFEC0, 0x3D000A0 | InlineHook both; if first person and `_ReturnAddress() == base+0x1816980` (0x3CFFEC0) or `base+0x1816A11` (0x3D000A0), return 0.0f; otherwise forward. | Every room visual gets opacity 0 and is skipped at 0x1816983 / 0x1816A14. The mask stays empty, so the blur pass writes the colour back unchanged. | Low to medium: 0x3CFFEC0 is called per renderable in CullStage (one extra compare per call). | Confirmed |
| B | **Region-trigger black-out** (same job, part 1, EoCRegionTriggerMask + EoCRegionTriggerResolve). When the player's region object has its flag set (`[view+0x2BFC]`), every pixel whose surface is **outside** that region volume is overwritten with (0,0,0,1). It has no sky exception. | 0x1816520; **0x18165F0** | MidHook at **0x18165F0** (bytes `44 88 6C 24 50`, `mov [rsp+0x50], r13b`): if first person, `ctx.r13 = 0`. `test r13b,r13b` / `jz 0x181679A` then skips part 1. That is the path the game takes when `[view+0x2BFC] == 0`. | Removes the region black-out in FP. Footage (sky normal) says part 1 was most likely not active at 15:09 (3.2), so this is a guard, not the main fix. Note: stereo.cpp:640 copies main's +0x2BE8..+0x2BFF, including +0x2BFC, into the eye, so the eye runs part 1 too. | Low (same kind of hook as A). | Mechanism **Confirmed**. Active at 15:09: **Unconfirmed** (4.2). |

Don't do it by writing view fields (`+0x2BFC`, `+0x2961`) from the game thread. 0x17E7FD0, 0x1C56E30 and the mod's stereo copy
(stereo.cpp:640) write them too, and the render worker reads them during the job.

Not the cause (section 3): the see-through capsule, the Fading switch itself (it only gates obstruction fades), the overlay post
object `+0x2BE0`, shroud, room-restricted lighting, the floor/room filter 0x17E3320, and a "fade-to-black" use of FadingView.

## 1. What draws the black: stage 0x33 (Confirmed)

Stage 0x33 (vtable 0x55F7078, ctor 0x1814C70, init 0x18164F0, job **0x1816520**) runs after the forward pass 0x00 and the
outlines 0x1F, before post 0x21 (A 6: inserted before 0x21; R: GPU order 0x1F, 0x33, 0x21). Raw: `w1_stage33.c`,
`w2_stage33_helpers.c`, `w7_ctor33.asm`, `w11_job33.asm`.

### 1.1 Shaders (ctor 0x1814C70, names from the FixedString table 0x55F6F70..0x55F7030)

| stage field | shader | DX11 bytecode (`AW\shaders\*.asm`) |
|-------------|--------|------------------------------------|
| +0x2140 | EoCRegionTriggerMask | VS: `pos = viewProj(cb11[8..11]) * world(cb0[0..3]) * v0`; PS: `return (1,0,0,1)` |
| +0x2148 | EoCRegionTriggerResolve | VS: fullscreen, `o0.zw = (1,1)`; PS: **`return (0,0,0,1)`** |
| +0x2150 | EoCRoomTriggerMask | VS as the region mask; PS: `o = (op*(1-fx), op*fx, 0, op)` with `op = cb0[4].x`, `fx = cb0[4].y` |
| +0x2158 / +0x2160 | EoCRoomTriggerDownsample / ...LinearDepthDownsample | compute (mask mips, depth mips) |
| +0x2168 | **EoCRoomTriggerBlur** | compute 8x8, see 1.4 |

### 1.2 Render states (ctor calls at 0x18151AE..0x1815321; enum maps 0x40C38A0 compare, 0x40C2FC0 stencil op, 0x40C3550 blend; D3D11 device vtable +0xA0 blend, +0xA8 depth-stencil, +0xB0 rasterizer)

The game uses reversed Z (mod vr_math.hpp:53), so GREATER_EQUAL means "nearer or equal".

| field | state |
|-------|-------|
| +0x2180 / +0x2188 | rasterizer cull NONE / cull FRONT (0x40BBA00 arg 0 / 2) |
| +0x2190 | depth off; stencil on, mask FF/FF; front: func EQUAL, fail ZERO, pass KEEP; back: ALWAYS (constants 0x55F70F4 = 0x02000001, 0x7000000) |
| +0x2198 | depth test GEQUAL, no write; stencil z-fail: front depth-fail DECR, back depth-fail INCR, func ALWAYS ([0x55F710C] = 0x07000500, [0x55F70F0] = 0x07000400) |
| +0x21A0 | depth test LEQUAL, no write; stencil func NOT_EQUAL, pass ZERO ([0x55F7108] = 0x05010000) |
| +0x21A8 | blend off, colour write mask 0 (stencil-only pass) |
| +0x21B0 | blend MAX (colour and alpha), write mask 0xF |
| global [0x5EE0570] | depth GEQUAL, no write, no stencil (table builder 0x4076BB0, parsed in `AW\dsparse.py`) |

Pipelines (desc → pipeline object 0x4097250 / 0x40BEDD0; the object keeps raster +0x18, DS +0x20, **stencil ref byte +0x28**, blend
+0x30, applied by command 0x22 → 0x40C6F30 → OMSetDepthStencilState(DS, ref) at 0x40CAA30). Every stage 0x33 pipeline is built
with ref bytes 0 (`uStack_1d0 & ~0xFFFF`):

| pipeline | shader | raster / DS / blend | role |
|----------|--------|---------------------|------|
| +0x21C8 | RoomTriggerMask | none / +0x2198 / +0x21A8 | z-fail stencil count of the room volume (stencil != 0 where the visible surface is inside the volume) |
| +0x21D0 | RoomTriggerMask | front-cull / +0x21A0 / +0x21B0 | writes the mask colour where stencil != 0, then clears stencil |
| +0x21D8 | RoomTriggerMask | global / [0x5EE0570] / +0x21B0 | RenderMethod 0 only: writes the mask wherever a volume face is in front of the scene, i.e. **everything seen through the volume** |
| +0x21B8 | RegionTriggerMask | none / +0x2198 / +0x21A8 | z-fail stencil count of the player's region volume |
| +0x21C0 | RegionTriggerResolve | global / +0x2190 / default | fullscreen: black where stencil == 0 (outside the region), resets stencil elsewhere |

### 1.3 The job 0x1816520

Part 1, region (0x18165E8..0x1816797):
`r13b = [view+0x2BFC]`. If set, and `[view+0x2BE8]` is a live handle, and `0x1818AA0([view+0x2BF0],[view+0x2BE8])` and `0x3C11A00(obj)`
(the object's visual, via its handle at +0x20) are non-null: begin render pass [stage+0x21E0] on framebuffer [stage+0x21F8] =
{**0x5EB1BE4E** colour, 0x22F3E110 depth-stencil}; draw the region visual's meshes with +0x21B8 (0x1817540); then the fullscreen
+0x21C0 draw (0x4084180, command 0x27 with 3 vertices). Result: (0,0,0,1) in 0x5EB1BE4E outside the region volume.

Part 2, rooms (0x181679A..0x1817449), only if `[[stage+0x2130]+0x21EC] >= 1` (room-trigger visuals in this view's list
`[[stage+0x2130]+0x21E0]`) and `[view+0x2961] == 0`; otherwise 0x1817451 (the skip path, 4 resource touches, nothing drawn):
* `fadeSet = per-player block of ls::FadingEntitiesForRenderingSingletonComponent ([view+0x2268] picks it, 0x18187B0)`.
* For each visual v: skip if `[v+0x118] == 2`. `op = [view+0x2268]==0 ? 0x3CFFEC0(v, fadeSet, 0) : 0x3D000A0(v, fadeSet, idx)`;
  skip if `op == 0` (call sites 0x181697B → ret 0x1816980, 0x1816A0C → ret 0x1816A11).
  `fx = ([v+0x119] == 1) ? 1.0 : 0.0`. Draw with +0x21C8 then +0x21D0 (0x18177C0); if `[v+0x118] == 0` also +0x21D8 (0x1817B60).
  The CB is the visual's world matrix (vfunc +0xC0) + `op` (+0x40) + `fx` (+0x44) → the PS above. The target is **0x96395540**.
* Then the downsample and linear-depth downsample computes, then the blur compute (1.4).

SE names `[v+0x118]` / `[v+0x119]`: ecl::RoomTrigger has `RenderMethod`, `RenderEffect`, `Border` (SE Trigger.h:765). The visual is
SE's SceneObjectType `RoomTriggerVisual` (10). Mapping +0x118 = RenderMethod, +0x119 = RenderEffect: Unconfirmed (4.1 logs both).

### 1.4 EoCRoomTriggerBlur (bindings at 0x181718A..0x18172E6, descriptors 0x55F3500 / 0x55F7158 / 0x55F71A8 / 0x55F7148 / 0x55F7168)

u0 = **0xB7D21B7B** (output), t0 = **0x5EB1BE4E** (lit scene colour), t1 = 0x96395540 (room mask, sampled with mips),
t2 = 0xA2D14EA1 (linear depth), s0 = [0x5EE0478]. Shader (`EoCRoomTriggerBlur.bshd.asm`):
```
c = t0[p];  d = t2[p]
if (d == 1.0) { u0[p] = c; return; }            // far plane / sky: untouched
m = t1 at p (x = black channel, y = desaturate channel); 8-tap disk blur of m, depth-weighted (|d - d_i| * cb11[12].y * 4)
target = (mask.y > 0) ? grey(luma((c-0.5)*1.5+0.7)) : (0,0,0,1)
u0[p] = lerp(c, target, m)                        // m = 1 -> pure black
```
So, for any pixel whose visible surface is inside (RenderMethod 1) or behind (RenderMethod 0) an inactive room volume with
`op` = 1 and `fx` = 0, the output is exactly (0,0,0,1). Edges are the volume faces; the disk radius shrinks with depth, so near
geometry gets hard edges. The sky passes unchanged because of the `d == 1.0` test. This matches the footage exactly: "everything
above a certain height pure black with a hard, roughly horizontal edge" (the bottom face of a room volume above the player,
RenderMethod 0), "ribs pure black silhouettes against the bright sky", same in both eyes.

## 2. How it decides (Confirmed unless marked)

* **Opacity of a room visual** = `ls::FadingView0/1` of the RoomTrigger entity (0x3CFFEC0/0x3D000A0, AJ 1). FadingView is the
  **product** of the current values of all fade types on the entity (0x18DEB20: `value *= rec[+0xC]` per type, lines 232/307/382 of
  `w12_combine.c`).
* **RoomTrigger fade** (ecl::room_portal::RoomPortalFadeSystem, cb 0x756210 → 0x18BED80, AJ 2):
  query 0x60A71AC (RoomTrigger, RoomState, Area, Transform, without FadeActive): if `RoomState byte +0 != 0` → add a fade request
  `{0.0, 4.0}` (fade **out**, speed 4) for view 0 and view 1; if 0 → no request (stays 1.0). Query 0x60A71B0 (with FadeActive):
  `RoomState == 0` → remove request (fade back **in** to 1.0). Query 0x60A71B4 (FadeActive, no RoomState) → remove. So an
  **active room is invisible in the mask and an inactive room is black**.
* **RoomState byte** (`eoc::room_portal::RoomStateComponent`, 0x18 bytes: byte +0, `DynamicArray<Connection>` at +8; replicated,
  `ls::model::game::room_portal::v0`). Server system **0x2F1EE30**: builds the set of rooms that contain a party member (the
  trigger's inside-list matched against party entities) and floods it through connections whose portal is open
  (0x2F20880/0x2F20720, portal byte +0x10). Then for each room, from 0x2F1F947: if `RoomState.byte0 != (room in set)` it writes the new value
  through 0x2D47140 (the replicated-field setter). This is the user's theory, in code: a room you (the party) are not in, and that no open door joins to yours,
  is rendered pitch black.
* **Per view**: yes. Each view has its own stage 0x33 and its own room-visual list (`[stage+0x2130]`), picks the fade set by
  `[view+0x2268]`, and skips the whole pass when `[view+0x2961]` is set (cinematic). Region part: per view by `[view+0x2BFC]`
  (written by 0x17E7FD0 for player views only; the mod copies it to the eye).
* **Region part**: `[view+0x2BE8]` handle, `[view+0x2BF8]` id, `[view+0x2BFC]` flag come from the player character's object
  `[char+0x100]` (handle at +0x20, flag at +0x7A) and `[char+0x108]` (0x17E7FD0; cleared when 0x1D0FFE0 finds the player in its
  list). Which game data that object is (region / floor trigger): Unconfirmed; its volume is the visual of `0x3C11A00(obj)`.
  Other readers of +0x2BFC: only 0x1C56E30 (compare). +0x2AEA (same flag) is read by the sky stage 0x3E45750 (not traced).

### 2.1 Why it started with the 14:42 build (Unconfirmed, 4.3 settles it)

Fading=0 itself only gates the obstruction raycast (AJ: only 0x18CF450 reads GS+0x1CE). Two ways it can expose the black, both
consistent with the code:
1. Geometry over the player that the obstruction system faded to ~0 was **dropped at cull** (0x3E21C70: `f <= 0` or
   `floor(f*16+0.5) == 0` → not drawn; `w13_cullfade.c`). It wrote no depth, so the room mask had nothing there but sky
   (exempt) or geometry outside the inactive volume. With Fading=0 it is drawn, writes depth, and lies inside/behind the
   inactive room volume → black. This fits "the mod now draws that geometry - and something draws it black".
2. If an obstruction fade sat on the RoomTrigger entity itself, the product rule (2) would make `op` < 1 or 0 before 14:42
   and 1 after. No code path was found that puts obstruction fades on RoomTrigger entities (obstruction fades come from raycast hits
   and FadeTrigger/FadeGroup expansion, AJ 3), so this is the less likely one.
Either way the black itself is stage 0x33, and fix A removes it independent of the reason.

## 3. Candidates ruled out (from code)

| candidate | finding |
|-----------|---------|
| See-through capsule (b7, 0x17F2B80) | b7 is 0x80 bytes: c0/c1 = capsule ends + radius (the only fields that depend on +0x2CF8/+0x2D30), c2 = player position +0x2CEC (written every time, no matter the active bytes), c3..c7 = five more position+enable records (+0x2D38..+0x2D84). With the bytes at 0 the game itself writes a 0.0001 sphere (`aj16_focus.c`). The mod's change therefore only removes a cut; it cannot add a height plane or a black cap (any plane from c2 existed before 14:42 too). Material shaders are not in EngineShaders.pak, so the material side of b7 was not read; not needed for the black. |
| Fading switch GS+0x1CE as a darkening factor | only read by 0x18CF450 (AJ 3). The fade value reaches draws only as entry +0x28 (0x3E21C70) and as the list choice for render-flag bit 0xB in 0x3E220C0 (`f >= 1` → list +0xBB8, else +0xC28). With Fading=0 obstruction fades go to 1.0, so nothing is darkened by it. |
| Overlay post object `+0x2BE0` (0x180FB20) | per-entry draws filled only by explicit calls 0x1810800/0x18109C0/0x1810B90/0x1810D60 (AP 4). Each entry has two positions, radii and a GlobalSwitches colour `[GS + (type+0xE8)*0x10]` gamma 2.2 with alpha; it is a shape indicator in the post chain, not a volume mask. Count logged in 4.4. |
| Shroud (GS+0x1CB) | client reader 0x17E00D0 only toggles `[view+0x2BFD]` / `+0x2AEA` (from 0x17E01C1; write at 0x17E0201). +0x2AEA feeds the sky stage 0x3E45750, not geometry. Sky is normal in the footage. Logged in 4.2. |
| Room-restricted lighting / probes | not needed: 0x33 runs after lighting and fog and replaces the lit value with (0,0,0,1); a lighting cause would not exempt the sky through depth == 1 or give volume-shaped edges. Not traced further. |
| Floor / room filter 0x17E3320, IsRoof, FloorTrigger | outline/picking filter only (R 1.2, AJ "Not needed"); no draw-colour path. |

## 4. One-run logging plan

One session: nautiloid near the helm, first person, look straight up for 5 s, ahead 5 s, walk through one door. A hotkey cycles
four modes, 5 s each, while looking up: (a) no fixes, (b) fix A, (c) fix A + B, (d) fix A off with fading.cpp's suppression off
(Fading as the game has it). Log every frame for 2 s after each switch, else every 30th frame. Each line gets the view pointer
and the thread id.

4.1 Room part. MidHook 0x18167A5 (before fix A acts): `rdi` = view, `[view+0x2268]`, `[view+0x2961]`, `rax` = list owner,
`[rax+0x21EC]` count. MidHooks at **0x1816980** and **0x1816A11** (after the opacity call; `rdi` = visual, `xmm0` = op):
visual, op, `[v+0x118]`, `[v+0x119]`, `[v+0xF0]`, `[v+0x108]` (entity), `[v+0x110]`. Expect at 15:09's spot: at least one
visual with op = 1.0, +0x119 != 1, and with +0x118 = 0 for the "everything above a plane" look. Fix A makes the count path skip
(log "skipped").

4.2 Region part. MidHook 0x18165F0: `r13b` (= +0x2BFC), `[view+0x2BE8]`, `[view+0x2BF0]`, `[view+0x2BF8]`, `[view+0x2BFD]`,
`[view+0x2AEA]`, `GS[0x1CB]`. MidHook 0x181663C: `rax` (region visual, non-null = part 1 draws). Expect `r13b == 0` (sky normal).

4.3 Why after 14:42. In mode (d) vs (b): AJ 5.3's counts from 0x3CFFEC0/0x3D000A0 for return addresses 0x3E21D62/0x3E21D69
(CullStage): renderables dropped (`f <= 1/32`) per frame, split by IsWall 0x20 / IsScenery 0x400, plus the room-visual ops
from 4.1. Leads 2.1-1 are confirmed if the dropped count is > 0 only with Fading on and the room op is 1.0 in both modes; 2.1-2 if
the room op drops below 1 only with Fading on.

4.4 Overlay. `[[view+0x2BE0]+0x78]`, `[[view+0x2BE0]+0xF0]` (AP plan) once per second. Expect no change between modes.

4.5 Screen check. In mode (a), a frame capture or screenshot of each eye. In (b) the ribs must be lit. If they are still black in
(b) with the 4.1 log showing "skipped", the cause is elsewhere and 4.2/4.4 decide.

## 5. Open leads

* Meaning of RoomTrigger `+0x118` / `+0x119` values (RenderMethod 0/1/2, RenderEffect 1 = desaturate) from the editor data; logged
  in 4.1.
* The object at `[char+0x100]` (region part) and the meaning of its +0x7A flag; and the sky stage's use of `+0x2AEA`
  (0x3E45872).
* Who fills the room-visual list `[[stage+0x2130]+0x21E0]` (which cull stage, which frustum). For the eye it is the eye's own
  cull, so a volume visible in only one eye is blacked in that eye only.
* Resources 0x5EB1BE4E / 0xB7D21B7B: here 0x33 reads 0x5EB1BE4E and writes 0xB7D21B7B; the skip path 0x1817451 touches both
  with 0x40979D0 (not decompiled). The game uses that path in every dialogue, so the output is valid there.
* The material side of the SeeThrough b7 constants (material shaders are not in EngineShaders.pak).
