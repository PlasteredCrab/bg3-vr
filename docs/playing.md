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
   - the game UI on the back of your left wrist (the left stick click moves it in front of you and back);
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
| Touch the UI panel with the right controller's tip | Click there (push in to press, pull back to release); handy on the wrist panel |
| Left trigger (hold) | Highlight items |
| X | Map |
| Y | Switch between first person and third person (the game's own camera on a big screen in front of you; the laser works as the mouse there) |
| Menu | The VR menu (B is the game's Escape / game menu) |

The key bindings assume BG3's default PC keys; change any of them in `[bindings]`. Every button press gives a
light buzz.

## Holsters

Reach over a shoulder or to a hip, squeeze the grip and pull your hand away: your character draws the weapons kept
there. Right shoulder and both hips: the melee set. Left shoulder: the ranged set (bow, crossbow); the game switches
sets first, and without a ranged weapon you get the melee set. Bring the hand back to a holster and let go to put the
weapon away; squeeze and let go in place to draw or put away without pulling. Squeeze low behind your back, between
the shoulder blades, to open the inventory. Squeeze at the middle of your chest to light a torch flame in that hand
(it lights the area around you and moves with your hand); squeeze there again to put it out. You feel a light tick when your hand is over a holster. What each holster
holds is set in `[holster]` (melee, ranged, inventory, light, none).

## The VR menu

Press the left menu button. A panel opens in front of you with the settings you'd want to change while playing:
first or third person, where the game UI sits (in front of you or on your wrist), snap or smooth turning, snap
angle, smooth turn speed, comfort vignette, stick movement, arms following the controllers, pointing to select,
grabbing, physical melee and eye height, plus Recentre, Calibrate height and Close buttons.

Point at a row with the right hand and pull the trigger to change it (the left arrow steps back), push the right
stick left or right while pointing at a row, or touch the panel with the tip of the controller. While it is open
your buttons and sticks don't reach the game. Changes take effect at once and are saved to `bing3vr\menu.ini`,
which overrides `bg3vr.ini` (delete it to go back to the ini's values).

## Settings worth knowing

| Setting | Default | What it does |
|---|---|---|
| `[vr] autostart` | true | start VR by itself (`!vr_go` / `!vr_stop` by hand) |
| `[vr] turn`, `snap_degrees` | snap, 30 | turning style |
| `[vr] comfort_vignette` | 0.5 | darken the view's edges while walking or turning with the sticks (0 = off) |
| `[vr] eye_offset` | 0.10 | eye height above the head bone, metres |
| `[ui] placement`, `width`, `distance` | wrist, 1.1, 1.0 | the UI panel (`world` puts it in front of you; the left stick click switches) |
| `[locomotion] invert_forward` | false | if pushing forward walks backwards |
| `[dialogue] first_person_types` | 1 | which conversation types stay first person |
| `[quality] texture_distance` | (game) | 1.5 to 2.0 sharpens textures at a distance in VR |
| `[ik] arms` | true | arms and the held weapon follow your hands |
| `[grab] probe` / `hold` | false (off) | pick items up with the grips; with `hold`, throw by letting go mid-swing and pull far items by flicking the hand back while aiming and holding the grip |
| `[combat] physical` | false (off) | swing the right hand through a character to make your normal weapon attack (the game rolls) |

## Console commands (Script Extender console, client context)

`!vr_go`, `!vr_stop`, `!vr_recenter`, `!vr_ik on|off`, `!vr_pick hand|off`, `!vr_move stick|off`, `!vr_hands [buzz]`,
`!vr_ray camera|hand|off`, `!vr_quality`, `!vr_skel`, `!vr_fps` (writes every module's report to the log).

## If something is wrong

Everything the mod does is logged in `<game>\bin\bg3vr\bg3vr.log`, by module:
`xr:`, `xr input:`, `vr:`, `ui:`, `views:`, `stereo:`, `skeleton:`, `ik:`, `pick:`, `locomotion:`, `dialogue:`,
`grab:`, `quality:`. `!vr_fps` adds a report from each. To turn the mod off without uninstalling, create an empty
file `<game>\bin\bg3vr\disabled`.
