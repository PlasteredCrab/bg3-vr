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

## Other cameras in the frame

Four plain `rf::CameraController` objects (base vtable 0x58EDC60, transform stored in the object) go through the view update every frame, as often as the gameplay camera. Created in 0x3E01E00, 0x3E02440, 0x3E18CE0 and 0x3E1C940.

**Confirmed 2026-10-12: they are the sun's shadow cascades.** All four are orthographic, share one forward vector (0.87, −0.36, −0.33, the sun direction), sit about 108 m up, and are spaced at increasing distances along it (log of a session on the Nautiloid). They're fitted around the gameplay camera, so with the pose written at the source they follow our eye. For stereo, both eyes can share one set of cascades fitted to the head position.

## The Script Extender's own hooks

It patches `D3D11CreateDevice`, `CreateDXGIFactory1`, `CreateSwapChainForHwnd`, `Present` (pre-hook) and `ResizeBuffers` with Detours (ImGui overlay). For OpenXR submission we need the finished frame: prefer hooking later in the frame (the swapchain's vtable `Present` after it, or the game's own post-processing end) over inline-patching the same functions.
