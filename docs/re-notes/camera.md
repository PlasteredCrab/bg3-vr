# Camera notes

Goal for Phase 0: find the structure that holds the gameplay camera's transform and projection, and the function that updates it each frame.

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
