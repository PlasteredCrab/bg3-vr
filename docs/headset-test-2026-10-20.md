# Headset test plan (2026-10-20)

First test of the headset path with same-frame stereo, controllers and the skeleton probe. Everything below
is deployed. Each step lists the `bg3vr.log` lines that answer its question, so one session gives all the
evidence.

## Before starting

1. Edit `<game>\bin\bg3vr\bg3vr.ini`, section `[xr]`: `enabled = true`, `mode = mono`. (Left off so SteamVR
   doesn't start with every launch while testing on the desktop.) `input` and `input_haptics` default to on.
2. Start SteamVR and connect the Quest 2 (Link or Air Link), then start the game and load the save.

Optional, without the game: `build\vs2026\bin\RelWithDebInfo\bg3vr_xr_test.exe --input --seconds 30` shows
the test pattern and prints the controller profile, button presses and hand poses. It checks the
controller code alone. `--panel` adds a 25 cm test panel on the left wrist (the layer the game UI will use):
it should stay on the controller with no lag, the pattern drawn over the scene.

Also possible on the desktop, without the headset: `!vr_ray camera` casts from the screen centre. Aim at
the same kinds of things and then run `!vr_ray off`.

## In the game

| Step | Do | Look for in bg3vr.log |
|---|---|---|
| 1 | Wait for the first frame | `xr: session open ...`, `xr input: controller actions attached` (or a warning with the reason) |
| 2 | Press buttons on both controllers | `xr input: ... profile /interaction_profiles/oculus/touch_controller`, `xr input: right trigger down ...`, a short buzz on each grip press |
| 3 | `!vr_go` | `skeleton: visual ... -> skeleton ...`, the bone list, `skeleton: Head_M is bone N`, then `stereo layout on` after 3 s |
| 4 | Put the headset on, look around, walk | both eyes show the scene; `xr: ... same-frame stereo` frame counts in reports (`!vr_fps`) |
| 5 | Hold the hands out in front, then `!vr_hands` | `xr input: left/right ... world pos (...)`: world hand positions near the character, about 0.5 m in front of the head |
| 6 | `!vr_hands buzz` | both controllers vibrate |
| 6b | `!vr_ray hand`, point the right controller at the floor, a wall, an item (crate, bottle) and a character; then `!vr_ray off` | `ray: [solid/broad/all] at (...) ... wrapper ... type N dynamic/static ... entity slot ...`: which mask set sees items and characters, and whether items carry an entity handle |
| 6c | `!vr_quality` (after `!vr_go`) | `quality: view 0/1 ... fov ... LOD band factor ... texture factor ...` and the texture cache budget and usage: tells whether `[quality] texture_distance` / `texture_budget_mb` are needed |
| 6d | Talk to an NPC (normal conversation), then trigger a cutscene if one is near (or a long rest) | `dialogue: player joins timeline ... type N ... (kept in first person)` per timeline: whether conversations stay first person, and which types the cutscenes have (to tune `[dialogue] first_person_types`) |
| 7 | `!vr_fps` | `skeleton: ... reads, ... failed, ... with a pose older than the last frame` |

Things to note while in the headset (just say them, the logs carry the numbers):
- Does the world look the right size, and is the horizon level?
- Is there a visible difference in brightness or sharpness between the eyes (shared TAA/exposure)?
- Any flicker or a one-frame lag in one eye?
- Head position vs. the character: too high, too low, inside the head?

## What each answer decides

- Hand world positions: whether `camera::tracked_to_world` places the hands correctly. Next comes the
  physics raycast from the aim ray (waiting on agent D's scene lookup).
- Bone list: the arm chain names for IK (`ik::solve_two_bone`) and the Skeleton slot 30 hook.
- `Head_M` world vs. the follow position: whether the camera can follow the real head bone instead of a
  fixed height.
- Fresh-pose counts: whether the head is read after this frame's animation or one frame late.
