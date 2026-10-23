# Agent AG: corrupted UI texture on the VR panel (bg3_dx11.exe 4.1.1.7631656)

Static RE only. Raw output: `re-local\ghidra\agents\AG\` (ag1..ag14). Inputs read: F_ui.md, AB_ui_system.md,
O_render_resolution.md, A_rendering.md 1.5/7, resolution.cpp, gpu_trace.cpp, headset.cpp (UI panel), the run log
`bin\bg3vr\bg3vr.log` (13:18-13:29) and the snapshots in `scratch\ui_shots_1320\`.

## Short answer

The game is not drawing Noesis offscreen tiles into its UI target. **The mod is copying a texture that is no longer
the UI target.** The game's stage resource manager hands out physical textures to resource keys, and every
allocation rebuild can move them around. It aliases keys that have the same descriptor and don't overlap in time,
and it reuses old textures from a pool. gpu_trace grabs the UI texture once, from composite PS slot 1. Then
headset.cpp sets GlobalSwitches+0x0D to 0, so the composite never binds the UI again and the watch never sees a
newer texture. After the next rebuild, the held texture can belong to another key with the same descriptor. Here
that was one of Noesis' "Offscreen%d" render targets, which have the same size and format (3572x1834 RGBA8)
after the resize. Noesis then draws its opacity-group and effect tiles into it. What the snapshot shows is an old
UI frame partly covered by tile rectangles (each tile area is cleared, then drawn), or a page that is nothing but
tiles.

## Fix table (first)

| # | Fix | Where / exact change | Thread | Status |
|---|-----|----------------------|--------|--------|
| 1 | **Find the UI texture again every frame. Never hold one pointer across frames.** Leave GlobalSwitches+0x0D at the game's value (1), so the composite binds the real UI SRV in PS slot 1 every frame. At the composite draw (RT0 = back buffer, slot-1 texture at least 640x360), treat that texture as this frame's UI. Update `g_ui_texture` whenever the pointer changes, and log the change. | gpu_trace.cpp `ui_on_draw` (already runs per draw while the watch is on). headset.cpp: stop calling `ui_set_in_eyes(false)` (lines ~1636/1653/1725 and wherever it is set to false). | D3D executor thread, inside the Draw hook | Mechanism Confirmed (below). Fix itself not tested |
| 2 | **Keep the UI out of the eye images some other way:** in that same composite draw, rebind slot 1 to a substitute SRV, call the original Draw, then rebind the UI SRV, so the game's state cache stays correct. Best substitute: the game's own "default" SRV, the 16x16 placeholder the composite binds when 0x0D = 0 (gpu_trace comment line 379). Capture it once by setting 0x0D = 0 for one frame and AddRef'ing the slot-1 view, then set 0x0D back to 1. A mod-made 16x16 RGBA8 texture of zeros is the fallback. | gpu_trace.cpp draw hook (`draw` -> `count_draw` -> `ui_on_draw`), around `g_hooks[3].call` for the composite `Draw(3)` | same | The game itself binds a "default" view with the same CombineUI shader when 0x0D = 0: Confirmed (stage 0x07 job, F d1_stage07.c l.577). That the placeholder's contents match a zero texture: Unconfirmed |
| 3 | Copy the UI into the mod's own texture **at the composite draw** (CopyResource / CopySubresourceRegion on the same context, before the Draw). The panel blit, alpha probe and snapshots then read only that copy. | gpu_trace.cpp `ui_on_draw` + headset.cpp `update_ui_panel` / `snapshot_ui` / alpha probe | same | Recommended; at that point the UI for this frame is complete (stage 0x19 runs before 0x07 in the special view, F 1.2/1.3) |
| - | Not needed: SetSize, recreating Noesis targets, changing the Noesis offscreen size, resizing at a different point. The sizes already agree (section 3). | | | Confirmed by code; runtime values to be logged |
| - | Not recommended: reading key 0x0C419ABC through the resource manager (`0x40979D0` -> entry+0x38) from the mod thread. This races with the rebuild 0x4098470, and the game's wrapper-to-ID3D11 layout isn't decoded. | | | - |

## Evidence

### 1. The held texture changes role (log and snapshots, Confirmed)

* `gpu: UI texture (composite PS slot 1)` was logged only at 13:20:42.012 (0x15EA0183DE0). 53 ms later came
  `ui: UI taken out of the eye images (GlobalSwitches+0x0D was 1)`. From then on the composite binds the default
  view (stage 0x07 l.577), so the watch cannot see changes.
* The next time the UI stayed in the eyes for a while (13:27:31, after a resize), slot 1 changed **three times in
  83 ms**: 0x15EA99A0160, then 0x15F33EDDF60, then 0x15F26646520. So the UI key's physical texture does move
  around after a resize.
* 0x15EA0183DE0 was kept from 13:20:42 through two more resizes (3572->1920->3572 at 13:26:23/24 and
  13:26:26/29). It showed:
  * 13:22:46 `ui_132246_292`: HUD plus tile rectangles at the bottom right;
  * 13:26:08 `ui_132608_035`: almost entirely a Noesis tile page (hotbar rows, portraits, item icons, tab labels,
    names with no frames, nothing cleared);
  * 13:26:29 `ui_132629_768`: a perfect inventory UI.

  So the same D3D texture was the UI target at one time and a Noesis offscreen page at another.
* 0x15F26646520 shows the same pattern: `ui_132746` and `ui_132806` are mixed.

### 2. The resource manager reassigns physical textures (code, Confirmed)

* Per frame, `0x4094E60` runs every stage's prepare (vtable +0x28), collects the running stages, and calls the
  allocation **0x4098470** at **0x40954E3** (A section 7; ag9 lines 1593-1985).
* 0x4098470 builds per-key stage-range bitsets. It then places keys into slots by descriptor hash and reuses a
  slot whose range is free (the `== -1` tests, ag9 ~l.833-850 and ~l.977-993), which is aliasing. It also matches
  old physical textures in `mgr+0xB0` by descriptor for reuse (from ~0x4099198, ag9 l.1095+).
* On a rebuild it releases each affected PSO's resolved views: **0x40A5750** zeroes PSO+0x20 (called at
  0x409878B). It also clears the key entry's resource **0x409BDA0** (entry+0x38 = 0, called at 0x409879F). The game
  then re-resolves lazily: **0x40A5380** fills PSO+0x20 from `0x40979D0(mgr, .., key)` = entry+0x38. So the game
  always uses the current texture, but a pointer the mod holds goes stale.
* Changes in which stages run (a new view, the character preview, dialogue) and resizes both trigger a rebuild.
  The resize only makes rebuilds happen right after VR starts. It is not the root cause.

### 3. Noesis device, offscreen targets, sizes (Confirmed unless marked)

* RenderDevice (game class, NUIM+0x40; ctor 0x2128530, dtor 0x2128D10), **vtable 0x56C26E0**:
  +0x28 0x21281D0 GetCaps (thunk), **+0x30 0x21296D0 CreateRenderTarget** and **+0x38 0x2129700 CloneRenderTarget**
  (both -> **0x21302F0**), +0x40 0x2129730 CreateTexture (`"Ramps"` call in 0x47E98E0), +0x48 0x212A110,
  **+0x50 0x2130080 BeginOffscreenRender** (+0xCD = 1, depth key 0x179E264), +0x58 0x2130110 EndOffscreenRender,
  **+0x60 0x21301A0 BeginOnscreenRender** (PSO +0xA8 = UI key 0x0C419ABC + depth 0xFF6F1F7D),
  +0x68 0x2130280 EndOnscreenRender, **+0x70 0x212A270 SetRenderTarget**, +0x78 0x579D90 (empty stub),
  +0x80 0x212A320, +0x88 0x212A3D0 ... Names from the slot order of Noesis 3.1 RenderDevice.h and from what each
  function does. The names from +0x78 on are Unconfirmed.
* Device init **0x2128E90** -> **0x212EB70(dev, w, h)** (w/h from GameUIParams +0x2C/+0x30). It registers 3
  stage-manager keys `"Offscreen0..2"` (name hash, fixed size `(w+1)&~1 x (h+1)&~1`, divisor 0, `0x409B0D0`) bound
  to the UI stage with 0x5410002 (`0x40956C0`), the depth key **0x179E264**, and the `Pyramid_*` blur chain. They
  go in the all-keys list `dev+0x58` (count `+0x64`, entries {w,h,key}) and the free list `dev+0x68` (count `+0x74`).
* **0x212F6D0** (resize; called by GameUI SetSize 0x120E0E0 at 0x120E117, 0x120DCE0, view create 0x20C2A23) is
  **grow-only**: max(old, new), resizes the keys through the pending map **0x4095E50** (`mgr+0x360/+0x390`), and
  updates the `+0x58` list. The free list is not touched. Released wrappers (dtor **0x2128390**) copy the entry
  back from `+0x58`, so the free list gets the new size once the old pages are released.
* **CreateRenderTarget 0x21302F0** takes a free key with exactly the requested (w,h), plus the depth key if
  stencil is needed. If none matches, it creates a standalone texture through a temporary command buffer
  (`0x40C9EF0`, views `0x40BEC50`, a shared stencil at `dev+0x78`).
* Noesis side: Renderer::Init **0x476A120** reads the device settings `+0x10..+0x28` (getters 0x468CF90..0x468D050;
  the game sets `+0x24/+0x28` = 2048 GlyphCache and `+0x1C` = 3 OffscreenDefaultNumSurfaces in 0x20C1780; the
  field names come from the Noesis 3.1 member order and are Unconfirmed). It then creates the VGL context
  **0x47E98E0** (offscreen size `+0xF8/+0xFC`, samples `+0x100`, default count `+0x104`, max `+0x108`; pages
  `+0xB0` count `+0xB8`, free pages `+0xC0` count `+0xC8`).
  * Pages come from **0x47F3970**: label `"Offscreen%d"`, the first via device +0x30 at the context's offscreen
    size, the rest via +0x38 Clone. The error string "Max number of offscreen surfaces reached" is in it.
  * **0x47EA5D0** grows the offscreen size to max(current, w, h) and recreates the pages. It is called from
    0x47FF7C0, which also stores the renderer's surface size (+0x174/+0x178). The call chain from IView::SetSize
    0x4703EF0 to 0x47FF7C0 is Unconfirmed (no direct call found).
* With the mod's resize: 0x585510 -> GameUI SetSize(frame+0x4328/+0x432C = 3572x1834) -> 0x212F6D0 grows the
  Offscreen keys to 3572x1834. The NUIM update calls IView::SetSize(3572,1834) (F). The UI key 0x0C419ABC is
  frame-sized (divisor 1) and was logged at 3572x1834. **So the view, the UI target and the offscreen targets all
  get the same size.** No stale viewport or scissor is involved. A resize from the options menu goes through the
  same 0x585510 path (O). The only extra thing the mod does is keep the window and swapchain at 1920x1200, and
  nothing in this chain reads the swapchain size. Because 0x212F6D0 is grow-only, after 3572->1920->3572 the
  Offscreen keys stay at 3572x1834, the same descriptor as the UI key at the VR size. That makes it possible for
  them to share physical textures. Which key exactly got 0x15EA0183DE0 is Unconfirmed. The tile contents prove a
  Noesis offscreen page.
* Composite: stage 0x07 job (F d1_stage07.c l.577): `if (stage+0x2188 == 0 || [GlobalSwitches]+0x0D == 0)` it
  uses the default view descriptor (DAT_145EE0438..48), else `0x4097AB0(mgr, .., 0x0C419ABC, 0xFFFF)`. The shader
  is chosen once at init by +0x2188, so both cases use the same CombineUI shader.

## Logging plan (one run)

Turn it on with the existing UI watch. Log the first 300 frames after every resize, then only changes.

1. **gpu_trace, per frame** (frame counter `render::frame_count()`):
   * the composite slot-1 texture (pointer, size, format) and whether it differs from last frame;
   * every RT0 bound between the UI clear and the composite that has the UI's size and format, with its draw count;
   * a flag for when the texture the mod held last frame is RT0 for draws in a frame where slot 1 is a different
     texture ("held texture reused as RT, N draws").

   This proves the change of role directly.
2. **Hook 0x4098470** (entry/exit; caller 0x4094E60): rebuild count and frame number. At exit, read key
   0x0C419ABC's entry+0x38 through the key table (`mgr+0x78/+0x80/+0x88/+0x98/+0xA8`, entry `+0x38`) and log the
   pointer. Match each rebuild against the slot-1 changes from item 1.
3. **Hook 0x120E0E0** (GameUI SetSize): w, h. After the NUIM update, log NUIM `+0xD4/+0xD8`, IView `+0x120/+0x124`
   (view = NUIM+0x70) and the scale `+0xDC`.
4. **RenderDevice** (NUIM+0x40), after each SetSize and 60 frames later:
   * `+0x10..+0x28` (offscreen and glyph settings);
   * the list `+0x58`/`+0x64` (w, h, key);
   * the free list `+0x68`/`+0x74`;
   * the VGL context `[dev+0x30]`: `+0xF8/+0xFC` size, `+0x104` default count, `+0xB8` pages, `+0xC8` free pages.
5. **Hook 0x21302F0** (CreateRenderTarget/Clone): requested w, h, stencil, whether a key matched (free-list hit)
   or the fallback ran, and the returned wrapper.
6. With fix 1/2 in place: log `ui texture changed a->b at frame N` and that the substitute SRV was bound. Keep
   snapshots on; all of them should be clean.

## Open points

* Contents of the game's default slot-1 view (the 16x16 placeholder): capture it rather than assume.
* Which key got 0x15EA0183DE0 (Offscreen0..2, or a Pyramid level of the same size): log item 2/5 settles it.
* Names of RenderDevice slots from +0x78 on, and of the device settings fields (taken from the Noesis 3.1 header
  order, not from strings).
