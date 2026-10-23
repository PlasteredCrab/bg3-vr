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
   - the game UI on the back of your left wrist (left grip + left stick click, or the VR menu's View page, moves it in front of you and back);
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
| Left stick click | Open / close the VR menu |
| Left grip + left stick click | Move the UI panel to your left wrist and back |
| Touch the UI panel with the right controller's tip | Click there (push in to press, pull back to release); handy on the wrist panel |
| Left trigger (hold) | Highlight items |
| X | Map |
| Y | Switch between first person and third person (the game's own camera on a big screen in front of you; the laser works as the mouse there) |
| Menu | The VR menu (B is the game's Escape / game menu) |

The key bindings assume BG3's default PC keys; change any of them in `[bindings]`. Every button press gives a
light buzz.

## Hand tracking

If your headset and runtime offer hand tracking (OpenXR XR_EXT_hand_tracking), put a controller down and that hand
is tracked instead:

- Point with your index finger (the other fingers curled): the laser follows your hand.
- Close your hand after pointing: a click (select, walk, talk, cast, attack). Open it to release.
- Close an open hand: a grab (holsters, items, bodies), the same as squeezing a grip.
- Touch the wrist UI or the VR menu with your index fingertip.
- Turn your other palm toward your face and pinch thumb and index finger: the VR menu.

The log says at start-up whether hand tracking is available (`xr input: ... hand tracking`).

## Dead bodies

Close a hand (squeeze the grip, or close a tracked hand from open) on a body part of a corpse: it is grabbed and the
whole body follows through the game's own ragdoll joints; let go mid-swing to throw it. For now a body stays within
about a metre of where it fell (the game ties every ragdoll to the character's position); carrying bodies away comes
next. On / off: VR menu Body page, Grab bodies.

## Fighting

Every attack is the game's own click, so its rules decide everything: attack rolls, range, action costs, the
weapon ability or spell you prepared.

- Melee: with a melee weapon drawn, look at an enemy within reach (2.5 m) and swing your pointing hand quickly. That
  clicks on who you are looking at: your weapon attack, or the ability you prepared. Swinging at nothing does nothing.
  (VR menu Body page, Swing attacks: off, at who you look at, or blade contact, where the blade has to pass through
  the enemy.)
- Bows: with a bow drawn, bring your pointing hand's grip to the bow hand to nock, pull it back (the controller
  creaks as you draw), aim along the arrow and let go. The shot is a click on the character the arrow points at.
- Crossbows and spells: aim with your pointing hand and click (the trigger, or a closed hand after pointing).

## Holsters

Reach over a shoulder or to a hip, squeeze the grip and pull your hand away: your character draws the weapons kept
there. Right shoulder and both hips: the melee set. Left shoulder: the ranged set (bow, crossbow); the game switches
sets first, and without a ranged weapon you get the melee set. Bring the hand back to a holster and let go to put the
weapon away; squeeze and let go in place to draw or put away without pulling. Squeeze low behind your back, between
the shoulder blades, to open the inventory. Squeeze at the middle of your chest to light a torch flame in that hand
(it lights the area around you and moves with your hand); squeeze there again to put it out. You feel a light tick when your hand is over a holster. What each holster
holds is set in `[holster]` (melee, ranged, inventory, light, none).

## The VR menu

Click the left stick to open and close it (the menu button on the left controller also works where the runtime
passes it on; a Quest 2 through SteamVR opens its own menu with it instead). The first time VR starts it opens by
itself on the Guide. A panel opens in front of you with tabs along the top. Push either stick up or down to move
through the tabs, settings and buttons, left / right to change the highlighted one, A to change it or press a
button, B to close; or point at it and pull the trigger:

- View: first or third person, the render resolution (a share of the headset's own resolution per eye; 80% by
  default, "Game's own" uses the game's setting), where the game UI sits (in front of you or on your wrist), the
  wrist UI size, and the aiming beam (off, solid, dashed, flowing dots or pulse; its colour and opacity).
- Comfort: snap or smooth turning, snap angle, smooth turn speed, comfort vignette, stick movement.
- Body: dominant hand (which hand points; the wrist UI goes on the other), arms following the controllers, weapon angle, pointing to select, grabbing, physical melee, holsters.
- Scale: IPD (the headset's own, or a fixed 56-72 mm), world scale (100% is true scale), eye height.
- Combos: hold one button and press another (left grip + A character sheet, + B journal, + X spellbook, + Y party
  view, + right trigger end turn, + right stick click sneak by default); each one's action is yours to pick.
- Buttons: what every controller button does, named for the controllers in your hands (Touch, Index, Vive) and
  your dominant hand; pick from the game's actions (click, inventory, map, journal, spellbook, end turn, sneak,
  jump, shove, weapon set, draw/sheathe, ...) or the VR ones (first/third person, wrist UI, VR menu, recentre).
- Guide: animated lessons for pointing, walking, turning, the wrist, weapons, light and bags, grabbing, fighting,
  your buttons and this menu, written for your current settings and bindings.

Recentre, Calibrate height and Close buttons sit at the bottom.

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
| `[vr] eye_offset` | 0 | eye height above the character's eyes, metres |
| `[ui] placement`, `width`, `distance` | wrist, 1.1, 1.0 | the UI panel (`world` puts it in front of you; left grip + left stick click switches) |
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
