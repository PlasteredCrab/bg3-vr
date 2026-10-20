# Playing BG3 VR

Status: everything below is built and deployed, but the headset path hasn't been tested in the game yet (see
[headset-test-2026-10-20.md](headset-test-2026-10-20.md)). Some pieces are off by default until that test confirms
them; they are marked "(off)".

## Setup (once)

1. Deploy the mod (README) and enable **BG3 VR** in the game's Mod Manager. The Script Extender must be installed.
2. In `<game>\bin\bg3vr\bg3vr.ini`, section `[xr]`: `enabled = true`, `mode = mono`.
3. Optional: copy the sections you want to change from the repo's `config/bg3vr.ini` (`[vr]`, `[ui]`,
   `[bindings]`, `[locomotion]`, `[dialogue]`, `[quality]`, `[ik]`, `[grab]`). The deployed ini is only written on
   the first deploy, so new sections aren't in it; every setting has a built-in default.

## Every session

1. Start SteamVR with the headset connected (Quest: Link or Air Link).
2. Start the game (DirectX 11) and load a save.
3. Put the headset on. VR starts by itself once the game is running:
   - first-person view at your character's eye height;
   - stereo three seconds later;
   - your own head hidden;
   - the game UI on a panel in front of you;
   - hand pointing, thumbstick walking and the controller bindings.

   After a loading screen it starts again, and switching characters moves it to the new one.

## Controls (Quest Touch; defaults)

| Control | Does |
|---|---|
| Right trigger | Click: move, talk, loot or attack wherever the right hand points; press UI buttons on the panel |
| Right grip | Right-click (context menu); picks things up instead when grabbing is on (off) |
| A | Inventory |
| B | Cancel / back |
| Right stick left/right | Snap turn 30 degrees (or smooth, `[vr] turn = smooth`) |
| Right stick up/down | Scroll (like the mouse wheel) |
| Hold right stick click | Recentre: you face forward again, and the UI panel comes in front of you |
| Left stick | Walk where you look (half push strolls, full push runs) |
| Left stick click | Move the UI panel to your left wrist and back |
| Left trigger (hold) | Highlight items |
| X | Map |
| Y | Journal |
| Menu | Game menu |

The key bindings assume BG3's default PC keys; change any of them in `[bindings]`. Every button press gives a
light buzz.

## Settings worth knowing

| Setting | Default | What it does |
|---|---|---|
| `[vr] autostart` | true | start VR by itself (`!vr_go` / `!vr_stop` by hand) |
| `[vr] turn`, `snap_degrees` | snap, 30 | turning style |
| `[vr] comfort_vignette` | 0.5 | darken the view's edges while walking or turning with the sticks (0 = off) |
| `[vr] eye_offset` | 0.10 | eye height above the head bone, metres |
| `[ui] placement`, `width`, `distance` | world, 1.1, 1.0 | the UI panel |
| `[locomotion] invert_forward` | false | if pushing forward walks backwards |
| `[dialogue] first_person_types` | 1 | which conversation types stay first person |
| `[quality] texture_distance` | (game) | 1.5 to 2.0 sharpens textures at a distance in VR |
| `[ik] arms` | false (off) | arms and the held weapon follow your hands |
| `[grab] probe` / `hold` | false (off) | pick items up with the grips |
| `[combat] physical` | false (off) | swing the right hand through a character to make your normal weapon attack (the game rolls) |
| `[ui] placement` | world | `wrist` puts the UI on your left wrist (the left stick click switches) |

## Console commands (Script Extender console, client context)

`!vr_go`, `!vr_stop`, `!vr_recenter`, `!vr_ik on|off`, `!vr_pick hand|off`, `!vr_move stick|off`, `!vr_hands [buzz]`,
`!vr_ray camera|hand|off`, `!vr_quality`, `!vr_skel`, `!vr_fps` (writes every module's report to the log).

## If something is wrong

Everything the mod does is logged in `<game>\bin\bg3vr\bg3vr.log`, by module:
`xr:`, `xr input:`, `vr:`, `ui:`, `views:`, `stereo:`, `skeleton:`, `ik:`, `pick:`, `locomotion:`, `dialogue:`,
`grab:`, `quality:`. `!vr_fps` adds a report from each. To turn the mod off without uninstalling, create an empty
file `<game>\bin\bg3vr\disabled`.
