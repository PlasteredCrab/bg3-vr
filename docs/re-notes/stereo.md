# Stereo rendering notes (Phase 1 prep)

Static analysis of bg3_dx11.exe 4.1.1.7631656 with `bg3vr_disasm`, 2026-10-12. Addresses are in [../offsets.md](../offsets.md).

## Per-eye camera: what we can already control

- **Pose:** written into the camera entity's Transform at the start of the view update (`camera_view_update`, working since Phase 0). One eye pose per render is a write of a different position and rotation.
- **Projection:** the projection update (0x40851F0) mirrors the view update: build, then store via a setter call (`camera_set_projection_call`), clear `ProjectionDirty`, rebuild the frustum (0x4086010). A mid-hook on that call can swap in a per-eye matrix, and the frustum and culling follow it.
- The perspective builder is **symmetric only**; `OffCenter` is honoured just for orthographic cameras. VR eyes need asymmetric frusta, so the projection hook builds them itself, in the game's convention:

```latex
P = \begin{pmatrix} \frac{2}{t_R - t_L} & 0 & -\frac{t_R + t_L}{t_R - t_L} & 0 \\ 0 & \frac{2}{t_U - t_D} & -\frac{t_U + t_D}{t_U - t_D} & 0 \\ 0 & 0 & \frac{n}{n - f} & -\frac{f n}{n - f} \\ 0 & 0 & 1 & 0 \end{pmatrix}
```

  where t_L, t_R, t_D, t_U are the tangents of the eye's field-of-view angles (OpenXR `XrFovf`, left and down negative), n = 0.1 and f = 10000 as the game uses. Rows/columns as written; store column-major. Left-handed, +Z forward, reversed-Z (near maps to 1).
- **Handedness:** OpenXR spaces are right-handed with −Z forward; the game's camera space is left-handed with +Z forward. Converting a pose: mirror Z (negate the z position and the x/y quaternion components), then check that the eye separation comes out on the correct side.
- Re-dirty after writing: setting `ViewDirty` (+0x10) and `ProjectionDirty` (+0x11) forces the game to rebuild on the next use. The camera system calls `if (ViewDirty) UpdateView(); if (ProjectionDirty) UpdateProjection();` before reading matrices (seen in 0x4085960 and 0x4086130).

## Two ways to get two eyes

1. **Alternate-eye (prototype):** each frame, write the next eye's pose and projection, and submit the frame to that eye's swapchain. Needs nothing new from the engine. Cost: ghosting and per-eye temporal effects (TAA history) mixing between eyes.
2. **Split screen (target):** the PC build contains the full split-screen pipeline: `ecl::splitscreen::{ViewSystem, RenderViewportSystem, RequestSystem, ...}`, `ecl::player_management::RenderViewTrackingSystem`, render views with `SplitScreenIndex` and `ViewIndex`, and per-view `PerCamera` blocks. Two render views with their own cameras in one frame is synchronized stereo. Leads:
   - Input event `ToggleForceSplitscreen` (id 0x5E0). Try injecting it via the Script Extender, or binding it.
   - Named global `ForceSplitscreen` (registered at 0x40CE0). Possibly a config or command-line switch.
   - Open question: does split screen require a second local player entity, and can its camera be driven by our Transform hook like the first?
   - 2026-10-13, static: both references to the string `ForceSplitscreen` are registrations (0x40CE0 registers a name; 0x2137110 builds a property map). It's a field of the Script Extender's `GlobalSwitches` (`Misc.h`), so Lua can flip it: `Ext.Utils.GetGlobalSwitches().ForceSplitscreen` (`!vr_split on|off`). Its readers go through the struct offset, so they weren't traced.
   - `Debug_Client_Splitscreen_Profile_%d` (used in 0x4062A00) names the profile of a secondary local player when it's added, and the strings `DisconnectSecondaryLocalPlayersCommand` and `LocalPlayers` point the same way: split screen most likely needs a **second local player**, not just the switch. The next in-game test answers it: `!vr_split on`, then `!vr_cam` (a second gameplay camera?) and `!vr_views find` (a second render view, its `SplitScreenIndex` and viewport?).
   - Render view layout, per the Script Extender (`RenderFramework.h`): `rf::RenderView` holds two `RenderViewState`s (ReadState at +0x2148, WriteState right after, 0x88 each). Each has viewports (render, upscaled, display: width, height, offset) and ends in a `CameraController*` at +0x80. After them come `SplitScreenIndex`, `ViewIndex` and a debug name. `ecl::GameRenderView` adds `PerView[2]` and `PerCamera[2]`, and `PerCamera` starts with the view matrix. So **each render view names its own camera controller and its own viewport**. If a second view exists, pointing it at a second controller driven to the other eye, with viewports side by side, is synchronized stereo. `native/core/src/views.cpp` (`!vr_views find`) finds the views of our controller by memory scan and logs all of this, including where the view matrix sits inside the view.
## In-game results, 2026-10-13 (no headset)

- `!vr_views find` with the camera held: **one render view** draws our camera (Read/WriteState both enabled, render and upscaled 1920×1080, display 1920×1200, owner player 255). The camera's view matrix appears 4 times inside it and its inverse 4 times (view+0x2360 … +0x3300 if ReadState is at +0x2148), in two groups 0xB60–0xBB0 apart: likely `PerView`/`PerCamera` for this frame and the previous one.
- **After `!vr_split on`, a second render view exists**, with the same (heap) object at its start, the same states and the same controller, ours. So `ForceSplitscreen` makes the game create a second view with only one local player. Whether it is drawn, and where to, is the next question (frame rate stayed at 30, which looks like a cap, so it proves nothing).
- The word at ReadState−0x2148 is a heap pointer, not an exe vtable, so the Script Extender's ReadState offset is off for this build; the name read as '?'. `!vr_views` now lists the exe vtables found before the states, to place the view's start.
- The back buffer is **1920×1200, format 24 (R10G10B10A2_UNORM)**, 3 buffers, flip-discard. A 10-bit format has no sRGB form, so the headset copy is now a shader blit (`blit.cpp`): back buffer → sampleable copy → decode sRGB → 8-bit sRGB eye image, under `SwapDeviceContextState` so the game's cached D3D state is untouched (unit-tested on WARP).
- The game ran at **about 30 fps** throughout, before any toggles.
- `!vr_follow`: 4,836 reads, 0 rejected over 21 s.
Second session, 2026-10-13:
- **The 30 fps is a background throttle**: in game it was exactly 30.0 fps in every report (normal camera and first person alike) while menus ran at 97 fps, the game is on the RTX A4000, and it presents with sync interval 0 and the tearing flag (vsync off). The console window had focus the whole time; BG3 limits its frame rate when its window isn't focused. Measure with the game window focused.
- The game render view's vtable is **exe+0x55F42F8, at ReadState−0x2158** (its methods use +0x2150, +0x21D8, +0x2298, +0x2961, +0x2C98), so views start 0x10 earlier than the Script Extender layout says; the command buffer's vtable exe+0x590A4D0 is at view+0x50. No RTTI in the exe (`/GR-`), so `bg3vr_disasm --rtti` finds nothing.
- The second view created by `ForceSplitscreen` has the command buffer at the same place but **no exe vtable at ReadState−0x2158**: a different class, or a different layout. `!vr_views` now logs the first word of every view.
- Slot 6 of the render view's vtable (0x3CC86D0) copies a byte from the big settings object at [0x604B1B8]+0x1BC into view+0x2298 and recomputes tile counts from the view size (+0x2290/+0x2294, tile +0x229C).
- Next experiment: `!vr_views sbs` writes Scale (0.5, 1) and Offset (0 / 0.5, 0) into both states of the first two views every frame; `!vr_views full` restores (1, 1) / (0, 0). If the game composites both views, the screen splits.
**Layout result, 2026-10-13:** with two views (after `!vr_split on` and a few seconds), `!vr_views sbs` drew view 0 (the original) into the **left half of the screen: the whole scene, scaled to half width**, with the UI still full screen. The game honours RenderViewState Scale/Offset. The right half, where view 1 should be, was **black**. Either view 1 isn't drawn, or it's drawn and then cleared by view 0's pass. `!vr_views swap` tells them apart; `!vr_views set|enable` place or disable one view.
**Swap and disable, 2026-10-13:** `!vr_views swap` moved the scene to the **right** half and left the left half black; with `sbs` plus `!vr_views enable 0 0` the whole screen was black (UI still drawn). So **only view 0 is ever drawn**; the view that `ForceSplitscreen` creates is set up (states, matrices, our controller) but nothing renders it with one local player. What we have: per-view placement on screen works (Scale/Offset), and disabling a view works. What we don't: a second view that renders. Most likely the split-screen systems only render a view that belongs to a local player, so the next test is real split screen with a second local player (a second gamepad).
- **Recording:** render view vtable slot 1 (0x17F21E0) begins recording: if `IsRecording` (view+0x2150) is clear it opens the command buffer (view+0x58), sets view+0x127C and IsRecording, then records (0x17F2B80, 0x3CC6C00). Slot 4 (0x3CC7010) ends it: closes the command buffer (view+0x50), clears IsRecording and calls slot 8 of every app stage in the array at view+0x28 (count at +0x34). Both are virtual calls from the render loop, which calls them for view 0 only. `!vr_views refs` lists what points at each view (a list holding view 0 but not view 1 is the candidate draw list) and `!vr_views diff` the small fields that differ.
- PC split screen: one gamepad becomes player 1's input; a second local player needs a second pad (not available). The exe has a UI flag `IsSplitScreenSupported`.
**refs / diff, 2026-10-13 (image base 0x7FF67BF20000 that session):**
- The game keeps **two game render views** (vtable exe+0x55F42F8, 0xADF0 apart in memory). This time view 1's controller was null; earlier it pointed at ours.
- **View 1 has never been drawn**: a frame counter that view 0 stores in many places (view+0x4C, +0x21DC, +0x2264 … = 554) is 0 in view 1.
- **View 1 has 3 app stages, view 0 has 29** (view+0x28 array, count at +0x34). The full pipeline (G-buffer, lighting, post) only exists for view 0, so adding view 1 to a draw loop would not be enough.
- Most pointers to view 0 are its stage objects: `{exe vtable, view, view+0x18}`. Their constructors are called from two **pipeline builders**, both virtual (no direct callers): **0x588E50** (game side; pointer at .rdata 0x549A218, next to 0x3AC51C0 at 0x549A210) and **0x3B21E10** (renderer side; pointer at 0x58A11B8). 0x588E50(this, view) returns at once if `view == this+0xA8` (the view it was last set up for), then gets or creates stages by id through 0x407AE00(view+0x18, id) (ids 5, 0x46, 0x17, 0x21, 8 …). So **a pipeline is built for one view at a time**.
- Two lists hold `[view 0, view 1]` side by side (all render views); the draw loop that picks only view 0 is not found yet.
- Next: find the render loop over views (calls view vtable slots 1 and 4) and what makes it skip view 1; then whether calling the builders for view 1 gives it a working pipeline.
**Start-up trace, 2026-10-13:** the game pipeline (one object; vtable has setup 0x588E50 and the stage-list builder 0x3AC51C0 called through slot +0x2B8) runs its setup once per view at start-up, from 0x3AC1CDF (view 0) and 0x3AC1CF2 (view 1). The setup first calls **0x3AC4EA0(pipeline, view)**, which decides the stages:
- `view == pipeline+0xA8` (the **special view**): one stage list, [0x5899BDC, 0x5899BDF): 3 stages. That's view 1, so view 1 isn't a split-screen view at all.
- otherwise: lists [0x5899BE0,+7), [0x5899BE7,+1) (only if [view+0x10]+0x40 != 0), [0x5899BE8,+7), [0x5899BEF,+1), [0x5899BF0,+8), the full pipeline; and if `view == pipeline+0xA0` (the **main view**), extra main-view setup.
The view that appears with ForceSplitscreen after a load never gets the setup (nothing calls it without a second player). `!vr_views build` finds the pipeline at runtime (the object holding two found views at +0xA0/+0xA8 with the setup in its vtable) and calls the setup for a view that is neither, on the render thread, under an access-violation guard; a view without a camera gets ours.
**First build attempt, 2026-10-13: crash.** The full setup (pipeline vtable slot 88) for the split view returned and took it from 3 to 30 stages, then the game crashed on the next frame (dump read with `tools/dumpinfo`): access violation at 0x3CC6D8B in the special view's begin-recording, calling a stage whose vtable word was garbage. The tail of 0x3AC4EA0, for any view other than the special one when [[view+0x10]+0x40] is 0, calls 0x41074A0 twice to **link the special view's output to the view being set up**; doing that for a second view freed a stage the special view still ran.
- Stage-id lists (bytes in .rdata): special view `19 45 07`; normal view `47 08 48 35 1E 05 0B`, [`45`], `46 36 44 18 17 40 37`, `02`, `41 03 1B 1A 00 21 3E 3F`, [`07`]. The bracketed two are built into a normal view only when that flag is set; otherwise the special view runs 0x45 and 0x07 for the main view, i.e. it's what puts the main view on screen.
- Second attempt: `!vr_views build` now calls only the stage-list builder (slot 87, 0x3AC51C0) with all six normal lists, including 0x45 and 0x07, so the new view composites itself; no game-side passes, no rewiring.
- Second attempt crashed the same way (special view, dead stage at its begin-recording), so it isn't the rewiring tail. The builder (0x3AC51C0) looks stages up by id (stage vtable slot 1) only in the target view's own array, so the link to the special view is elsewhere. Third attempt: `!vr_views build` leaves out 0x45 and 0x07 (`build full` includes them) and logs every view's stage ids before and after.
- **Cause found (third attempt's stage log):** before building, the split view and the special view had the **same stage array buffer** (the split view is a shallow copy of the special view). The builder grew the split view's array, which reallocated the buffer and freed the old one; the special view kept pointing at it (its stage ids read as garbage right after the build) and crashed the next frame. Fourth attempt: before building, the split view gets an empty array of its own (stage group at view+0x18: owner view +0x20, stages +0x28, capacity +0x30, count +0x34), and its owner pointer is corrected if it still names the special view.
- Fourth attempt: with its own empty array, the special view stayed intact (`19 45 7`) and the game kept running, but the builder faulted (caught) after 6 stages of the first list (`47 8 48 35 1E 5`, before 0x0B). With the shared array it had got through every list, so it likely needs one of the special view's stages (19/45/07) or something linked from them. The guard now logs the fault's code and data address and the exe return addresses on the stack, captured in the exception filter.
- The fault: exe+0x4096593 (null object) under 0x407A440 = stage group add-stage (rcx = view+0x18; calls 0x409A4C0([group], [group+8], stage), then the stage's slot 2), from the builder at 0x3AC6AF0, adding stage 0x0B. The stage group's first word (view+0x18) is the scene graph; the split view carries the special view's. Fifth attempt gives it the main view's scene graph and logs all three views' +0x18/+0x20.
- Fifth attempt: all three views already share one scene graph, so that wasn't it; same fault. The faulting function 0x4096520 (dozens of callers) dereferences its third argument, a null object, looks like subscribing to a signal. Sixth attempt: `build full` builds 0x45 and 0x07 first (the special view's stages the split view had before the detach), and the log lists pointer fields set in the main view but null in the target.
- Sixth attempt (no crash): building 0x45 first didn't help; 0x07 wasn't created. Exact chain (RtlVirtualUnwind): builder 0x3AC6AF5 → add-stage 0x407A47D → stage 0x0B's init 0x3E4DBA0 (returns to 0x3E4EB95) → 0x4096520 with r8 = [stage+0x2168] = null. +0x2168 is set at 0x3E4E0C0 from `0x4095F50([[stage+0x10]], desc)` (stage+0x10 = &view->StageGroup, so [[…]] = view+0x18's object, shared by all views), with the descriptor from 0x17DCB10; it returned null for the split view's stage. Likely a resource keyed by name or hash that already exists for the main view (shader/target permutations by hash 0x361F94A4 variants 0..3 follow). Next: decompile 0x4095F50 and 0x17DCB10 in Ghidra. The main view has many pointer fields the split view lacks (view+0x48, +0x21D8, +0x2260, +0x22B0, +0x2338 … +0x2B20).
**Decompiled (Ghidra, `tools/ghidra/decompile.ps1`), confirmed from code, 2026-10-13:**
- `0x4095F50(manager, …, desc)` returns null only if `0x409DB80` flags the descriptor (or an allocation fails, which logs a debug string). Otherwise it looks the descriptor's hash up in a cache at manager+0x148 and **shares** an existing entry (refcount +1) or creates one. A duplicate is shared, not refused.
- `0x409DB80(manager, out, desc)`: for every 44-byte entry of the descriptor's first array, looks the entry's first u32 (a key) up in the manager's key table (buckets +0x78, count +0x80, next +0x88, keys +0x98, values +0xA8); **any key not found sets the error flag**.
- `0x17DCB10` only moves three arrays into the descriptor.
- Stage 0x0B's init (0x3E4DBA0): calls `0x40956C0(manager, stage, key)` for keys 0xFF193183, 0x4C0D1B61, 0x3B0A2BF7, **0x361F94A4**, 0xD3052CFC, 0x8517349C, 0x74EA5362 (on any failure it clears the stage's ok flag +0x19 and calls `0x4095830` for the ones that succeeded); then builds a descriptor of 4 entries, all with key **0x361F94A4** (a constant), and `[stage+0x2168] = 0x4095F50([[stage+0x10]], …)`.
- `0x40956C0(manager, stage, key)` creates nothing: if the key is in the same key table it increments that resource's count (+0x40) and appends a binding to the stage's record in the manager's list at +0x58.
- So for the split view's stage 0x0B, key 0x361F94A4 was **not in its manager's key table**. Not yet known why (the main view's 0x0B uses the same constant key). The main view has stages the split view lacks ahead of 0x0B (0x41, 0x3A, 0x3D, and game-side 0x3C, 0x1F, 0x33, 0x0A). Next run logs both managers and whether the key is present.
- Run 2026-10-13 17:09 (logged facts): the split view's stage 0x0B uses the **same manager** as the main view's (0x…8E00), and key 0x361F94A4 **is present**, before and after. So the missing key wasn't it.
- From the decompile: `[stage+0x2168]` is only assigned inside a block guarded by the stage's ok flag (+0x19); the code that crashes (two more lookups, then `0x4096520(…, [stage+0x2168], …)`) runs regardless of the flag. So either the first lookup returned null, or an earlier step cleared the flag: the three key registrations (`0x40956C0`), the checks `0x3E50720`/`0x3E50A50`/`0x3E50D80`/`0x3E510B0`, or the loads `0x3CD56D0` into +0x2140…+0x2160. Not known which yet. Next run logs the failing stage's flag and fields (stage = rdi in the init's frame, recovered by the unwind) next to the main view's working stage 0x0B.
**Why stage 0x0B fails (decompiled, 2026-10-13 evening, no test yet):**
- The four checks `0x3E50720`/`0x3E50A50`/`0x3E50D80`/`0x3E510B0` each create their key if it's missing, bind it, and always return 1. `0x3CD56D0` returns nothing (stores a handle).
- `0x40956C0(manager, stage, key)` returns 0 if the key is missing **or if the manager has no record for the stage** (assembly: `xor al, al` at 0x409572B; `mov al, 1` at 0x4095826 only after finding the stage's record).
- `0x409A4C0(manager, owner view, stage, …)` (called by add-stage with [view+0x18], [view+0x20]) finds the manager's record for the owner view (list +0x58, count +0x64) and appends a stage record under it; **no record for the owner → returns 0, stage not registered.**
- In attempts 4-6 the split view's owner (+0x20) was set to the split view itself, which has no record, so its stages weren't registered, stage 0x0B's three registrations (`0xFF193183`, `0x4C0D1B61`, `0x3B0A2BF7`) failed, the ok flag was cleared, +0x2168 stayed null, and the unconditional `0x4096520(…, [stage+0x2168])` faulted. While the owner was still the special view (attempts 1-3) the builder went through every list.
- Next test: owner left as the special view, stage array still detached. What owning the stages through the special view's record means for drawing is not known; the cleaner fix (a record for the split view) needs the function that creates view records, not found yet; look for writers of manager+0x58/+0x64 once the full Ghidra analysis is done.
**How views get records (full Ghidra analysis, 2026-10-13 night):**
- `0x4094920(manager, view, index)` creates a manager record for a view (32 bytes, built by `0x4092CC0(record, view)`) and inserts it into the manager's list (+0x58/+0x64) before entry `index` (only inserts if index < count).
- Its only caller, `0x4073230(frame, view, is_special)` (5 callers), adds a view to a RenderFrame: the frame's view list is at +0x42E0 (count +0x42EC, capacity +0x42E8; the same list the render loop walks), its stage resource manager at +0x4250, its special view at +0x4388. A special view is stored and appended; a normal view is inserted just before the special view in the list and gets its record at that index.
- So the game's way to make a view drawable is `0x4073230(frame, view, 0)`. `!vr_views build` now logs whether each view is in the frame's list and has a record, calls `0x4073230` for the split view if it isn't in the list (new signature `render_frame_add_view`), and makes the split view its own stage-group owner only once it has a record (otherwise the owner stays the special view).
- Tools: `tools/ghidra/FindInDecompiled.java` finds functions in an RVA range whose decompiled C contains given strings (`_` = space, `~` = `)` in arguments).
- **Run 2026-10-13 21:45:** the split view was not in the frame's list; `0x4073230(frame, split view, 0)` returned and gave it a list entry and a manager record; with itself as owner the stage build **completed: 24 stages** (`45 7 47 41 8 48 35 1E 5 B 46 36 18 17 40 37 2 3 1B 1A 0 21 3E 3F`, no fault). Then the game **froze** (log stops; process still responding to Windows, CPU climbing). Where it hangs is unknown; `watchdog.cpp` now logs every thread's stack once if no frame is presented for 3 s.
## Making alternate-eye smooth (until step 5 lands)

Alternate-eye is how several commercial-quality VR conversions of flat games work. What decides how smooth it is:
- **Frame rate.** Each eye updates at half the game's rate, and the compositor reprojects the stale eye by its pose. The game must hold the headset's rate (set the Quest 2 to 72 Hz in the first tests). The desktop's vsync must not cap it: during a headset session `render.cpp` forces Present's sync interval to 0 (`[xr] desktop_vsync = false`), and `xrWaitFrame` paces the game.
- **TAA.** Temporal anti-aliasing and the upscalers (DLSS/FSR/XeSS) blend each frame with the previous one, which was the *other* eye: strong ghosting and smearing. Use FXAA or no AA, and no upscaler, in the game's graphics options. Also turn off motion blur and depth of field.
- **Correct pairing.** Each image must be submitted with the pose it was rendered with (`headset.cpp`: the applied-view history, matched in the RenderFrame or render views, else `!vr_xr_lag`). A wrong pairing makes the world swim when you turn your head; a wrong pairing in alternate mode puts each eye's image in the other eye.
- **Resolution.** Every eye image is the whole back buffer (1920×1080 now), stretched over about 94°×93°: about 20 px/° across but 12 px/° down. A window closer to square (the per-eye size, about 1832×1920, or less) evens it out. BG3 stores the size in `graphicSettings.lsx` (`ScreenWidth`, `ScreenHeight`); whether it accepts a non-standard size in windowed mode is to be tried.

## Other cameras in the frame

Four plain `rf::CameraController` objects (base vtable 0x58EDC60, transform stored in the object) go through the view update every frame, as often as the gameplay camera. Created in 0x3E01E00, 0x3E02440, 0x3E18CE0 and 0x3E1C940.

**Confirmed 2026-10-12: they are the sun's shadow cascades.** All four are orthographic, share one forward vector (0.87, −0.36, −0.33, the sun direction), sit about 108 m up, and are spaced at increasing distances along it (log of a session on the Nautiloid). They're fitted around the gameplay camera, so with the pose written at the source they follow our eye. For stereo, both eyes can share one set of cascades fitted to the head position.

## The Script Extender's own hooks

It patches `D3D11CreateDevice`, `CreateDXGIFactory1`, `CreateSwapChainForHwnd`, `Present` (pre-hook) and `ResizeBuffers` with Detours (ImGui overlay). For OpenXR submission we need the finished frame: prefer hooking later in the frame (the swapchain's vtable `Present` after it, or the game's own post-processing end) over inline-patching the same functions.

## Per-view camera blocks and the split view's camera (static, 2026-10-14)

All from decompiles of 4.1.1.7631656 (`re-local\ghidra\percamera.c`, `jitterproj.c`, `setcamera*.c`,
`controller_enter.c`) and an instruction scan for the displacements 0x21D8 / 0x2260
(`tools\ghidra\FindDisplacement.java`, `re-local\ghidra\disp_controller.txt`).

- **Begin-recording:** vtable slot 1 (0x17F21E0) tail-jumps to 0x3CC6C00. That calls
  `0x3CC7490(view, view+0x2340, view+0x2660, 0)` and `0x3CC7490(view, view+0x24D0, view+0x2740, 1)`,
  optionally `0x3CC70B0(view+0x2290, ...)`, then uploads copies of the four blocks (0x3CC5830 / 0x3CCDD00
  for the PerView blocks, 0x3B915D0 / 0x3B97160 for the PerCamera blocks).
- **0x3CC7490 reads everything camera-related from ReadState's controller (view+0x21D8).** With no
  controller it writes neither PerCamera block. The split view's controller was null in the last build log
  (it is a copy of the compositing-only special view).
- **PerCamera layout:**
  - +0x00 view matrix: the controller's ViewMatrix, +0x90..+0xCF.
  - +0x40 projection:
    - block [0]: the controller's projection (+0x110);
    - block [1]: rebuilt by 0x3CCCFD0 from the controller's FOV (+0x14), near (+0x18), far (+0x1C) and
      aspect (+0x20), plus the frame's jitter sample (0x3F11710 on view+0x2180/+0x2184/+0x21A0). It is
      always symmetric for perspective cameras, so [8] and [9] are the jitter alone; for orthographic
      cameras it copies the controller's matrix.
  - +0x80: projection × the view matrix's rotation (translation dropped); column 3 = the projection's.
  - +0xC0 near; +0xC4 far.
  - +0xD0 world position, from controller vtable slot 9 (GetWorldTranslate); +0xDC = -1.
- **Consequence for the existing per-eye projection:** the projection override at the controller's setter
  (camera module, set_eye_fov) only reaches block [0]. Block [1], what the frame is drawn with, keeps the
  symmetric FOV/aspect projection. The stereo hook writes both blocks.
- **Assigning a camera:**
  - The game render view's vtable (exe+0x55F42F8) slot 10 (0x17F2B30) is SetCamera(view, controller,
    keep). Unless view+0x2C98 is set, it calls 0x3CC90B0, which:
    - calls the old controller's Exit (vtable +0x10) and the new one's Enter (+0x08);
    - writes WriteState's controller (+0x2260);
    - updates the controller's aspect from the view's size (0x407BE10);
    - passes the camera to the view's listeners.
  - The base RenderView (vtable 0x58EDB30) has 0x407BD90 in the same slot.
  - The frame sync 0x4072CB0 copies WriteState (+0x21E0..+0x2267) into ReadState (+0x2158..+0x21DF) for
    every view in the RenderFrame's list.
  - Game-side callers such as 0x17F7CD0 call SetCamera directly when the view exists, or else queue a
    command under a lock.
- **One controller can serve several views.** The gameplay controller (vtable 0x58B6750):
  - Enter (0x3CF0080) appends an entry for the view to a per-camera list, if view+0x48 is set;
  - Exit (0x3CF0210) removes it.
- **What changed:** !vr_views build now calls SetCamera on the game thread instead of writing both states'
  controller pointers directly, and the stereo hook sits at 0x3CC6CB9 (signature
  `render_view_camera_built`).

## The split-view freeze (watchdog stacks, 2026-10-19)

The `!vr_views build full` run froze about 0.3 s after SetCamera returned. The render thread (all others idle;
the busy main thread was stepping the Lua GC) was spinning in the job-counter wait 0x410E880. The call path:

- the frame end 0x3DF97C0 calls 0x4073040, which calls each view's end-recording (vtable slot 4, 0x3CC7010);
- end-recording calls each stage's slot 8;
- here that was 0x3E4A930, the wait of stage **0x45** (vtable 0x58C6EA8; its init loads
  `DownsampleResolver.bshd`; its id comes from slot 1 at 0x3E4A450, `mov al, 45h`). Its execute (slot 6,
  0x3E4A6F0) submits the stage's job (stage+0x20) to the counter `[stage+0x2138]` (0x410E5D0).

The job system, from decompiles:

- **Counters:** created by 0x4107090. Predecessors are at +0x268 (up to 3) and successors at +0x250.
- **Flags at +4:** 1 = armed, 2 = released, 4 = signalled. Release (0x4108550) needs every predecessor
  done. Before release, jobs wait on the counter's pending list; release flushes them (0x410E6F0).
- **0x41074A0(sched, A, B)** makes A wait for B.
- **Per-view job context:** each render view gets a 17-counter context at [view+0x40] (root constructor
  0x407B080, then 0x40878C0). The render loop 0x4072B30 arms and releases each active view's context
  (0x407B540). The frame end 0x4073040 waits on all of them.
- **0x3AC4EA0, the game's stage setup for a normal view:**
  - it builds 0x45 and 0x07 only when [[view+0x10]+0x40] is set;
  - otherwise it links the view into the frame's graph: the special view's [ctx+0x40] waits for the view's
    [ctx+0x78], and DAT_146048680 waits for its [ctx+0x80];
  - the main view in the log has neither 0x45 nor 0x07; the special view has 19 45 07.
- **What changed:** our hand-built `full` variant gave the split view a stage 0x45 the game never gives a
  normal view in this mode, and its job never finished. `!vr_views build` now calls 0x3AC4EA0 itself
  (signature `pipeline_choose_stages`), and `full` is refused.

## Root cause of the split-view hang: a shared job context (job dumps, 2026-10-19)

- **The split view shares the special view's job context.** Its [view+0x40] is the special view's
  (0x192B08A74D0 in both before the build): it starts as a copy of the special view, pointers included.
  The root constructor 0x407B080 normally gives every view its own context: 0x88 bytes from the game
  allocator (0x4188700 on arena 0x40EA160()+0xE8, align 0x10), zeroed, then filled by 0x40878C0.
- **The game's stage setup then closes a loop.** 0x3AC4EA0 made that shared context's +0x40 counter wait
  for its own +0x78.
- **The deadlock.** In the first frame, the split view's end-recording called stage 0x08's wait
  (0x3F7A6F0), which tail-calls 0x410E880 on ctx+0x40. That counter was armed and signalled (n4 f5) but
  never released, because its predecessor ctx+0x78 (n2 f5) can't complete before this frame's stages do.
- **How it was pinned down.** The watchdog's frame registers (the wait's r14, end-recording's rbx) were
  matched against the job dump, and the event log shows the same order.
- **Fix:** `!vr_views build` gives the split view its own context, made the constructor's way, before
  calling 0x3AC4EA0. The view destructor (0x407B320) frees it with the same allocator.

## Correction and next step (2026-10-19)

- **The old explanation was wrong.** The 2026-10-13 note above said 0x3AC4EA0's two 0x41074A0 calls "link the
  special view's output" and freed a stage. In fact 0x41074A0 only adds a job-graph edge (A waits for B).
- **What really caused that crash:** a stage with a garbage vtable in the special view's begin-recording.
  That matches the shared stage array: the split view grew it to 30 stages, and the special view kept the
  freed buffer. That was found and fixed afterwards.
- **What works now.** With the stage array detached, a frame record from 0x4073230, its own job context
  and 0x3AC4EA0, the split view runs every frame without hanging. But it draws nothing (right half black
  with the stereo layout).
- **What it's missing:** the main view's 0x3A, 0x3D, 0x44, 0x3C, 0x1F, 0x33 and 0x0A. The full setup
  0x588E50 creates 0x3D, 0x3C and 0x33 inline (stage vtables 0x55F6E70, 0x55F4AB0, 0x55F7078) and hooks two
  output objects into the view's stage 0x21.
- **Next step:** `!vr_views build` now runs 0x588E50 ([views] full_setup = true). A crash watch (vectored
  handler) logs any fault with its registers, stack and a job dump.

## Full setup works; split-screen index (2026-10-19)

- **The full setup now runs cleanly.** 0x588E50 on the split view, after the fixes above, ran with no fault
  or hang: 27 stages, all of the main view's except 0x44 and 0x0A. The right half was still black.
- **0x0A** (vtable 0x58C4810) is built inline by 0x3AC4EA0 only for the pipeline's main view
  (pipeline+0xA0), so a second player's view wouldn't have it either.
- **0x44** (vtable 0x58C6DE8) is built by 0x3AC51C0 only for the main view or a view whose byte
  [view+0x2268] is 1. That byte is the split-screen index after WriteState.
- **How the game adds a player view.** In 0x3BAA1A0(manager, player), the game's own add-player-view:
  1. the manager's vtable slot 1 creates a view;
  2. [view+0x2268] is set to 1;
  3. the view is disabled if this isn't the active player;
  4. the pipeline setup (DAT_146046f98 slot 88) runs;
  5. the view is stored as player 1's or 2's.

  0x3BAA380 (add local player, called from 0x17F5CB0 with an input device) uses it, or a spare view from
  the manager's pool (+0x80, count +0x8C).
- **The index matters.** About 60 places read [view+0x2268], most of them stage code. The split view (a
  copy of the special view) had index 0, so it probably renders as player 1. The build now sets it to 1
  before 0x588E50, the game's order.
