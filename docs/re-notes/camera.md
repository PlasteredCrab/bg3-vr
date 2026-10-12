# Camera notes

Goal for Phase 0: find the structure that holds the gameplay camera's transform and projection, and the function that updates it each frame.

## World conventions (from `!vr_probe`, 2026-10-12)

- Y is up: Tav on the Nautiloid at `(50.759, 17.318, -427.084)`.
- Rotations are quaternions, printed `(x, y, z, w)`: `(0, 0.608, 0, 0.794)` is a pure yaw of about 75°.
- Scale `(1, 1, 1)` for a human. Units are probably metres; confirm by walking a known distance.
- OpenXR is also Y-up and right-handed. Check BG3's handedness (sign of Z when walking "forward") before mapping poses.

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
