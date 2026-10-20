# VR UI design (draft, 2026-10-19)

Source: agent F's static RE (`re-local/ghidra/agents/F_ui.md`). Nothing here has run in the game yet.

## What the game does (confirmed in code)

- **One UI texture per frame.** Noesis draws the whole UI once per frame into its own frame-sized target (resource
  key 0x0C419ABC, depth/stencil 0xFF6F1F7D). This happens in the special (compositing) view's stage 0x19 (job
  0x3CC04F0, GameUI render 0x120FD10). Bink videos go into the same target.
- **Composite.** Stage 0x07 (job 0x3E4D2A0) draws the scene plus that UI texture over the whole back buffer, and the
  software cursor. With our stereo layout the UI is therefore stretched across both eye halves. The UI texture is
  bound only when `[stage+0x2188]` and byte `[[0x604B1B8]+0x0D]` are set; otherwise a no-UI shader is used.
- **Size.** GameUI `SetSize` 0x120E0E0 (re-applied on every resize by 0x585510). The layout is authored at
  3840x2160 and scaled by height/2160.
- **Pointer.** A float pair at `*(float**)0x614FB70`, in frame pixels, goes to Noesis `MouseMove` on the
  UIMouseMove event (0x4E). Clicks are events 1/2 and the wheel 0x5E/0x5F (→ 0x20C3B70). "Pointer over UI" state
  is at NUIM +0x180/+0x181/+0x190, with hit test 0x2106F90.
- **Controller mode.** Any input from a controller device (ids 3-10) switches the game into controller UI mode
  (0x5E3C60, flag 0x614FBC8). Noesis then ignores the mouse.
- **World-anchored UI.** Nameplates, overhead text, world tooltips and dialogue bubbles all go through
  `RenderView::WorldToScreen` 0x407BF00, which projects with the player's view (the left eye with our layout).

## Plan

1. **UI on a panel.** Copy the UI texture into the XR panel layer (`xr::Session::create_panel`, done; test:
   `bg3vr_xr_test --panel`). The UI's D3D texture is identified from the GPU trace of the stage 0x07 composite
   draw (its second shader resource, logged by `gpu_trace` when stereo turns on). Then copy it at Present.
   Placements:
   - default: a world panel in front of the player that follows the body's yaw (dialogue, inventory, menus);
   - a wrist panel on the left hand (hotbar, quick status);
   - optional: switch by gesture (look at the wrist).
2. **No UI in the eye images.** Stop stage 0x07 binding the UI texture: patch the bind at 0x3E4D2FE, or clear
   `[stage+0x2188]` before its init so the game picks `CombineUI_NoUI*`.
3. **Laser pointer.**
   - Every frame: intersect the right hand's aim ray with the panel quad, convert the hit to UI pixels, write
     them into the pointer float pair, then inject raw mouse motion (id 0x80, device 1) through `InjectRaw`
     0x40F1F00.
   - Trigger: inject left-click; grip or B: inject right-click; stick: wheel.
   - When the ray misses the panel, the pointer is hidden, and clicks go to the world (picking, agent I).
4. **Stay in mouse mode.** Never inject controller-device input. VR locomotion therefore uses keyboard raw input or
   direct component writes (agent J), or 0x5E3C60 gets patched. This decides the locomotion design.
5. **World labels.** Hook 0x407BF00 to project from the centre eye into the panel's pixel space, or turn the labels
   off. Real per-eye labels would need a separate world-space renderer (later).
6. **Dialogue.** Show the dialogue UI on the world panel near the speaker. Agent G is working on keeping the
   camera first-person during dialogues.

## Status (2026-10-20, built, untested in game)

- **Panel:**
  - `gpu_trace::watch_ui` records PS slot 1 of draws into the back buffer (the composite), which gives the UI
    texture;
  - the headset copies it with alpha into the panel layer, 1.1 m wide, 1 m ahead and 15 cm below the eyes, placed
    when VR starts and on recentre;
  - while the panel is shown, GlobalSwitches+0x0D = 0 keeps the UI out of the eye images (restored after);
  - after a window resize the UI goes back into the composite for 5 frames, so the new target is found.
- **Laser:** the right aim ray is intersected with the panel. A hit draws a dot and moves the real mouse cursor to
  that spot, and the trigger's injected left click acts there. A miss parks the cursor at the window centre.
- **Settings:** `[ui]` panel, remove_from_eyes, laser, width, distance, below_eyes.
- **Not yet:** wrist placement, re-projection of world labels (0x407BF00), keeping the software cursor out of the
  eye images.

## Open

- The D3D object behind key 0x0C419ABC (runtime: GPU trace or resource manager 0x4098180).
- Where Noesis draws when the UI size is smaller than its target.
- What writes `GlobalSwitches+0x0D`.
- The widget XAML is in `Data\Game.pak`, not the exe; read it with LSLib if specific widgets need moving.
