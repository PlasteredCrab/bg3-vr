# Camera notes

Goal for Phase 0: find the structure that holds the gameplay camera's transform and projection, and the function that updates it each frame.

## World conventions (from `!vr_probe`, 2026-10-12)

- Y is up: Tav on the Nautiloid at `(50.759, 17.318, -427.084)`.
- Rotations are quaternions, printed `(x, y, z, w)`: `(0, 0.608, 0, 0.794)` is a pure yaw of about 75°.
- Scale `(1, 1, 1)` for a human. Units are probably metres; confirm by walking a known distance.
- OpenXR is also Y-up and right-handed. Check BG3's handedness (sign of Z when walking "forward") before mapping poses.

## Gameplay camera, measured (`!vr_cam`, 2026-10-12)

- The active gameplay camera (`MasterBehaviorType` 8, component `ecl::EocCameraBehavior`) **looks along its +Z axis**. Camera at `(60.342, 27.073, -429.755)`; +Z column of `InvViewMatrix` `(-0.646, -0.652, 0.396)` points about 12 units to the orbit pivot.
- `WorldTransform` on this controller is the **orbit pivot**, not the eye: `(52.586, 19.346, -425.002)`, yaw-only rotation; `LookAt` is the pivot 0.1 lower.
- **Projection is reversed-Z with an infinite far plane.** Column-major elements: `[1]=1.201 [6]=1.921 [11]=0 [12]=1 [15]=0.1` gives `z_clip = near`, `w_clip = z_view` (left-handed, +Z forward). `FarPlane` reads 10000 but isn't used in the matrix.
- **FOV is vertical, in radians:** 0.9599 (55°); `1.921 = 1/tan(27.5°)`; `1.201 = 1.921 / aspect 1.6`.
- Writing `FOV` sticks for a moment, but the game eases it back to its own target (read 54.0 a few seconds after setting 30): per-frame state is owned by the camera update.
- Script Extender proxies (`Controller`, components) expire at the end of the tick that fetched them; keep `EntityHandle`s across ticks and re-read proxies.
- The second camera entity (behavior 3, only `ls::SoundCameraComponent`) is an inactive sound camera at the origin.

## Where the view is set (`!vr_cam_probe`, 2026-10-12)

- Writing pose fields from Lua's `Tick` has no visible effect: the camera update recomputes them before rendering.
- Vtable slot 5 (`Update` per the Script Extender) is never called for the gameplay camera. In 3 s, slot 9 (`GetWorldTranslate`) ran 43,953 times, slots 11 and 13 (shared function) 7,417 times; slots 3, 16, 19 and 23 also ran.
- A hardware write watch on the view and inverse-view translation caught exactly one writer per frame: the matrix setter 0x408BD60, called from the controller view update 0x4085150 at 0x40851CA. See [../offsets.md](../offsets.md).
- **Hook:** a mid-hook on that call replaces the view matrix in `rdx` before the setter runs, so the inverse, the frustum and culling are rebuilt from our pose by the game itself.

## What the Script Extender already maps (source at commit af85fd2, read 2026-10-12)

Files: `BG3Extender/GameDefinitions/Components/Camera.h`, `GameDefinitions/RenderFramework.h`, `Extender/Client/IMGUI/DX11.inl` in [Norbyte/bg3se](https://github.com/Norbyte/bg3se). Licence: MIT with the Commons Clause (no selling; a free fork is allowed).

**Camera objects**

- `ls::CameraComponent` (ECS component `Camera`): `Active`, `AcceptsInput`, `MasterBehaviorType`, `UseSplitScreenFov`, post-process settings, and `rf::CameraController* Controller`.
- `rf::CameraController` is a virtual class. Its vtable order: dtor, `Enter`, `Exit`, `GetFov`, `GetAspectRatio`, **`Update(GameTime const&)`** (slot 5), `DoLookAt`, `EditorLookAt`, `EditorZoomExtents`, `GetWorldTranslate`, `SetWorldTranslate`, `GetWorldRotate`, `SetWorldRotate`, `GetWorldTransform`, **`SetWorldTransform`**, `WarpMouse`, `ClearInputState`, `OnInputEvent`, `StopMotion`, `SetOrientation`, `SetMoveSpeed`, `SetLookAtShake`, `SetWobble`, `GetDistance_M`.
- Controller fields: `ViewDirty`, `ProjectionDirty`, `IsOrthographic`, **`OffCenter`**, `FOV`, `NearPlane`, `FarPlane`, `AspectRatio`, `WorldTransform`, `LookAt`, ortho bounds, `ID`, then an `rf::Camera` holding `ViewMatrix`, `InvViewMatrix`, `ProjectionMatrix`, `InvProjectionMatrix` and 10 culling planes, then 8 frustum points.
- `ecl::GameCameraBehavior`: the gameplay camera's state (target, distance, direction, pitch, tactical mode, collision avoidance). This is what we turn off for first person.
- `ecl::camera::PhotoModeCameraTransformRequestsSingletonComponent` takes `SetTransform` requests per entity. Photo mode is a free camera driven by transforms, so it's a possible shortcut.
- `CameraGlobalSwitches` (distances, FOVs, collision, auto-rotate) are reachable from Lua via stats `CameraSwitches`.

**Render views**

- `rf::RenderView` has `SplitScreenIndex`, `ViewIndex`, read and write `RenderViewState` (render, upscaled and display resolution, `CameraController*`).
- `GameRenderView` adds `PerView[2]` and `PerCamera[2]`; each `PerCamera` has `ViewMatrix`, `JitteredProjectionMatrix`, near and far planes, `WorldTranslate`. It also has `IsCinematicMode`, `IsInCombatCameraMode` and `CameraController*`.
- **Split screen exists in the engine.** Two render views, each with its own camera, may be the cheapest route to synchronized stereo. To investigate in Phase 1.

**The Script Extender's own DX11 hooks** (ImGui overlay): Microsoft Detours on `D3D11CreateDevice`, `CreateDXGIFactory1`, `IDXGIFactory::CreateSwapChainForHwnd`, `IDXGISwapChain::Present` (pre-hook) and `ResizeBuffers`. Our hooks must chain with these; prefer different hook points (e.g. vtable or later in the frame) over inline-hooking the same functions.

**Lua access.** All controller fields above are readable from Lua (`entity.Camera.Controller`). None of the 83 systems hookable through `Ext.Entity.OnSystemPostUpdate` is camera-related, so Lua writes go through `Ext.Events.Tick`, with no guarantee they land after the camera update. `Client/CameraProbe.lua` tests this (`!vr_cam`, `!vr_cam_fov`, `!vr_cam_follow`).

## Method

1. Load a save, stand still, and pan the camera. Search memory for a float triplet that tracks the camera position.
2. Find what writes it (x64dbg hardware breakpoint, or Cheat Engine "find out what writes").
3. Walk up to the per-frame update function and the owning object (singleton or manager).
4. Write signatures for both into `signatures.txt` until the log reports one unique hit each.
5. Record them in [../offsets.md](../offsets.md).

## Findings

| What | Address / RVA | Layout notes | Build |
| --- | --- | --- | --- |
| Camera position | | | |
| Camera rotation | | | |
| View matrix | | | |
| Projection / FOV | | | |
| Update function | | | |

## Systems to neutralise later

Tactical camera, collision push, occluder fading, roof hiding, depth of field, cinematic cameras.
