# Camera notes

Goal for Phase 0: find the structure that holds the gameplay camera's transform and projection, and the function that updates it each frame.

## World conventions (from `!vr_probe`, 2026-10-12)

- Y is up: Tav on the Nautiloid at `(50.759, 17.318, -427.084)`.
- Rotations are quaternions, printed `(x, y, z, w)`: `(0, 0.608, 0, 0.794)` is a pure yaw of about 75°.
- Scale `(1, 1, 1)` for a human. Units are probably metres; confirm by walking a known distance.
- OpenXR is also Y-up and right-handed. Check BG3's handedness (sign of Z when walking "forward") before mapping poses.

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
