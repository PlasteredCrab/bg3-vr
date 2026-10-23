# Playing BG3 VR

Status: what has been seen working in a headset and what is only built so far is listed in the
[README](../README.md#status). Some pieces are off by default until a test confirms them; they are marked "(off)".

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
   - the game UI hidden: X shows it in front of you (and hides it again); it comes up by itself in combat and in
     conversations (VR menu, Game interface: always in front of you, or on your wrist, instead);
   - hand pointing, thumbstick walking and the controller bindings.

   After a loading screen it starts again, and switching characters moves it to the new one.

## Controls (Quest Touch; defaults)

| Control | Does |
|---|---|
| Right trigger | Click: move, talk, loot or attack wherever the right hand points; press UI buttons on the panel |
| Right grip | Right-click (context menu); while the gravity gloves highlight an item, grabs it instead |
| A | Inventory |
| B | Cancel / back |
| Right stick left/right | Snap turn 30 degrees (or smooth, `[vr] turn = smooth`) |
| Right stick up/down | Scroll (like the mouse wheel) |
| Hold right stick click | Recentre: you face forward again, and the UI panel comes in front of you |
| Left stick | Walk where you look (half push strolls, full push runs) |
| Left stick click | Open / close the VR menu |
| Left grip + left stick click | Map |
| Touch the UI panel with the right controller's tip | Click there (push in to press, pull back to release); handy on the wrist panel |
| Left trigger (hold) | Highlight items |
| X | Show / hide the game UI (in front of you) |
| Y | Switch between first person and third person (the game's own camera on a big screen in front of you; the laser works as the mouse there) |
| Menu (☰) | The VR menu too, where the runtime passes the button on (a Quest 2 through SteamVR keeps it for its own menu) |

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

## Conversations

When someone talks to you, the game UI with the answers comes off your wrist to a panel in front of you until the
conversation ends. Push either stick up or down to move through the answers and press A to pick one (or point at it
and pull the trigger); B skips the line being spoken. VR menu Comfort page: "Answers in front of you" and "Sticks
and A / B in conversations".

## Characters in UI screens

The character in the inventory, character sheet, party and trade screens is shown in 3D: each eye looks through the
picture's place on the UI panel as through a window, so the character stands behind the panel with real depth.
VR menu Scale page: "3D characters in menus" (off brings back the flat picture).

## Gravity gloves (items)

Point either hand at an item up to 6 m away. A small cone around the ray helps with small things. The item the hand
finds is highlighted: a tick in that hand, and the right hand's laser turns cyan.

- Squeeze the grip. Close by (1.5 m), the item is in your hand at once. Further away, the hand locks on: a firmer pulse,
  and the laser turns amber.
- Flick the hand back toward you. The item flies on an arc and steers into your hand over the last metre. Keep the grip
  closed, or close it as the item arrives, to catch it.
- Hold it and turn your hand to look at it from every side.
- Let go to drop it where it is. Let go mid-swing to throw it.
- Let go over a shoulder or behind your back to put it in your inventory.

A grip only grabs when something is highlighted, held or flying to that hand; otherwise it does what it is bound to
(the right grip right-clicks). On / off: VR menu Body page, Gravity gloves. Settings: `[grab]` (`reach`,
`pull_range`, `catch_radius`, `throw_min_speed`).

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
  upscaling used in VR (DLSS Balanced or Performance, or the game's own setting) and depth of field, the
  wrist UI size, and the aiming beam (off, solid, dashed, flowing dots or pulse; its colour and opacity).
- Comfort: snap or smooth turning, snap angle, smooth turn speed, comfort vignette, stick movement, where the camera
  sits (steady by default: only your head moves the view; or at the character's eyes), and the conversation controls.
- Body: dominant hand (which hand points; the wrist UI goes on the other), arms following the controllers, weapon angle, pointing to select, grabbing, physical melee, holsters.
- Scale: IPD (the headset's own, or a fixed 56-72 mm), 3D characters in menus, 3D depth (each eye its own view, or
  flat), world scale (100% is true scale), eye height.
- Combos: hold one button and press another (left grip + A character sheet, + B journal, + X spellbook, + Y party
  view, + right trigger end turn, + right stick click sneak by default); each one's action is yours to pick.
- Buttons: what every controller button does, named for the controllers in your hands (Touch, Index, Vive) and
  your dominant hand; pick from the game's actions (click, inventory, map, journal, spellbook, end turn, sneak,
  jump, shove, weapon set, draw/sheathe, ...) or the VR ones (first/third person, wrist UI, VR menu, recentre).
- Guide: animated lessons for pointing, walking, turning, the wrist, weapons, light and bags, grabbing, fighting,
  your buttons and this menu, written for your current settings and bindings.

Recentre, Calibrate height and Close buttons sit at the bottom.

Pointing works too: point at a row with the right hand and pull the trigger to change it (the left arrow steps
back), or touch the panel with the tip of the controller; the laser stops on the panel where it points. While it is
open your buttons and sticks don't reach the game. Changes take effect at once and are saved to
`<game>/bin/bg3vr/menu.ini`, which overrides `bg3vr.ini` (delete it to go back to the ini's values).

## Settings worth knowing

| Setting | Default | What it does |
|---|---|---|
| `[vr] autostart` | true | start VR by itself (`!vr_go` / `!vr_stop` by hand) |
| `[vr] turn`, `snap_degrees` | snap, 30 | turning style |
| `[vr] comfort_vignette` | 0.5 | darken the view's edges while walking or turning with the sticks (0 = off) |
| `[vr] eye_offset` | 0 | eye height above the character's eyes, metres |
| `[ui] placement`, `width`, `distance`, `wrist_width` | hidden, 1.1, 1.0, 0.8 | the UI panel: `hidden` until X shows it (and in combat and conversations), `world` always in front of you, `wrist` on the back of your wrist |
| `[locomotion] invert_forward` | false | if pushing forward walks backwards |
| `[dialogue] first_person_types` | 1 | which conversation types stay first person |
| `[quality] texture_distance` | (game) | 1.5 to 2.0 sharpens textures at a distance in VR |
| `[ik] arms` | true | arms and the held weapon follow your hands |
| `[grab] probe` / `hold` | false / true | the gravity gloves (see above); `probe` only logs what a grip would grab |
| `[combat] physical` | false (off) | swing the right hand through a character to make your normal weapon attack (the game rolls) |
| `[vr] camera_anchor` | body | `body`: the view moves only with your head (and the character walking); `head` / `head_exact`: at the character's eyes, smoothed / as animated |
| `[xr] head_prediction_ms` | 0 | extrapolate the head pose (0: the pose as it is now) |
| `[vr] teleport_turn` | false | turn your view to the body's facing after a cutscene moves it |
| `[render] vr_upscaler` | dlss_balanced | upscaling while in VR (`dlss_performance`, or `game` for the graphics menu's); put back when VR stops |
| `[render] vr_resolution` | 80 | per-eye render size, percent of the headset's |
| `[views] eye_origin` | proxy | `proxy`: each eye drawn from its own position (stereo depth); `head`: both from the head (flat) |
| `[views] share_shadows` | true | the right eye reuses the left eye's sun shadows (checked over the first 60 frames) |
| `[xr] async_wait` | false | wait for the headset's frame on its own thread, not in the game's Present |
| `[perf] worker_threads` | 0 | the game's job worker threads (0 = the game's 6; try 10-12 on an 8-core CPU) |
| `[ui] stereo_dolls` | true | characters in UI screens in 3D |

## Console commands (Script Extender console, client context)

`!vr_go`, `!vr_stop`, `!vr_recenter`, `!vr_ik on|off`, `!vr_pick hand|off`, `!vr_move stick|off`, `!vr_hands [buzz]`,
`!vr_ray camera|hand|off`, `!vr_quality`, `!vr_skel`, `!vr_fps` (writes every module's report to the log).

## If something is wrong

Everything the mod does is logged in `<game>\bin\bg3vr\bg3vr.log`, by module:
`xr:`, `xr input:`, `vr:`, `ui:`, `views:`, `stereo:`, `skeleton:`, `ik:`, `pick:`, `locomotion:`, `dialogue:`,
`grab:`, `quality:`. `!vr_fps` adds a report from each. To turn the mod off without uninstalling, create an empty
file `<game>\bin\bg3vr\disabled`.
