# VR feature plan: hands, body, conversations, inventory (draft, 2026-10-23)

These are plans, not built features. Each one is a VR menu setting with its own on/off switch, and each logs heavily
so one test session shows whether it works. **Known** marks what earlier research (the agent reports in
`re-local/ghidra/agents/`) or the mod's working code has confirmed. **To find out** marks what must be decompiled
before building, so nothing rests on a guess. Performance work (fixed foveated rendering, sharing the right eye's
repeated work) comes first and has its own research running: `AT_perf_sharing.md` and `AU_foveated.md` when they land.

The order is at the end.

---

## 4. Physical doors, chests and levers

**How it feels**
- **Doors:** put a hand on a door and push or pull. It swings with your hand, past its catch it opens fully, and it
  closes if you push it back. Locked doors don't move: you feel a short thud, and the game's normal "locked" answer
  appears (lockpicking stays the game's).
- **Chests:** grab the lid and lift it. Past about 30° the chest opens, and the game's loot window comes up in front
  of you (with the UI hidden by default, a window opening already brings the panel up: `gameui.cpp`).
- **Levers and buttons:** grab a lever and pull it through its arc; at the end it fires. Press a button with your
  fingertip.

**Known**
- Opening and using go through the server: `Osi.Open` / `Osi.Close` / `Osi.Use`, and natively
  `esv::item::UseSystem` (0x35E6820, 0x33D8D30) (agent E). Doing it this way keeps traps, locks, ownership and crime
  checks the game's own.
- The mod already finds the item under a hand (gloves: a ray past the player's own body, entity to `ecl::Item`) and
  moves client items (`SetTranslate` / `SetRotate`, on the main thread since the 14:54 crash fix).
- Item flags at `ecl::Item+0x18` (agent AD): 0x2 = can be picked up, 0x4 = can be moved. Doors and chests aren't
  pickable, so the gloves already leave them alone.

**To find out**
- **Animation state:** how a door's open and close animation is driven on the client (an animation timeline on the
  visual, or a transform the server replicates). It decides whether the mod can play the door to a chosen angle
  (follow the hand) or can only trigger "open" at a threshold.
- **Hinges:** where a door's hinge axis and rotation range are stored (template or physics joint). Without them, the
  hand drags a door on a guessed arc.
- **Item kinds:** how to tell door, chest, lever and button apart (item template flags, the `Use` actions list).
- **Lid:** whether a chest lid is a separate bone or animation, so the lid can follow the hand.

**Plan**
1. **First version (threshold):** grab, a pull or push past a set distance, then `Osi.Use` / `Open` through the
   existing bridge to server Lua, with a haptic click. This needs only "which kind is it", from the templates.
2. **Second version (follow the hand):** drive the door or lid angle from the hand while it's held. Release past the
   threshold calls the game's open; otherwise it springs back. This depends on the animation research.

**Risks:** doors that open by script (quest doors) must keep their game logic, so the mod always triggers the game's
own Use and never just moves the mesh. Multiplayer is out of scope.

---

## 5. Drinking potions by bringing them to your mouth

**How it feels:** hold a potion with the gloves and raise it to your mouth. After about half a second there you hear
it, the bottle tilts, it empties, and the game applies its effect to your character. Bringing it to a companion's
mouth gives it to them, like the game's "throw potion / give" action.

**Known**
- The gloves hold the item, and the mod knows the head position (the headset) and the hand's.
- Using an item on a target goes through the server (`Osi.Use`, `esv::item::UseSystem`); turn costs in combat stay the
  game's (a bonus action for potions).

**To find out**
- Which items are drinkable or consumable: the template or stats type (Potion, Food, Drink), readable from the client
  item or server Lua.
- The exact server call that consumes a potion as the character's own "drink" action, with the right action cost in
  turn-based combat. Compare `Osi.Use(char, item, "")` with casting the item's use spell, and check what the game's
  own "use from hotbar" sends.
- How a held world item (not in the inventory) is used. The game may need it in the inventory first; if so, stow it,
  then use it.

**Plan:** a mouth zone (a sphere about 12 cm below and 8 cm in front of the eyes). A held consumable inside it for
0.5 s triggers the use through the bridge; a fill bar, sound and haptics while it's there. Only for the controlled
character at first. Setting: "Drink by mouth".

---

## 6. Reading books, letters and scrolls in your hand

**How it feels:** hold a book, letter or scroll and its text shows on the item itself (or on a page panel attached to
it), at reading size, and turns with your hand. Bring it closer and it gets bigger. Turn pages with a flick of the
other hand or the stick. Opening it also marks it read and gives its journal entry or quest update, as the game does.

**Known**
- The game shows books in its own UI page (`Book.xaml`, event `ShowBook`, agent AB 1.5). The VR panel shows the game's
  UI texture, and the mod can crop it (AB 2).
- The 3D UI characters already show something attached to a place in the world (the doll quad layers), so the same
  mechanism can carry a page.

**To find out**
- How the game opens a book for an item: the event and its arguments, and the server or quest side effects that must
  still happen.
- Where the book page sits in the UI texture, so the mod can crop just the page and draw it on a quad attached to the
  held item, instead of showing the whole UI panel.
- Whether page turns can be fired as UI commands (the Book view-model's commands, like the dice commands found in
  `AQ_dice_rolls.md`).

**Plan:** holding a readable item opens the game's own book (so its effects happen). While open, the mod hides the
normal UI panel and draws the cropped page on a quad at the item, about 35 cm from the eyes. Releasing or stowing the
item closes the book. Setting: "Read in your hand".

---

## 7. Legs and walking body

**How it feels:** look down and your legs walk with your stick movement and your own room-scale steps. Standing still,
your feet stay planted while you turn your head. Lean or crouch and the body follows within limits.

**Known**
- The arms already follow the controllers by rewriting bones in Skeleton slot 30 `UpdateMeshBindings` (0x3A20590)
  (`bodyik.cpp`, agent C). The same hook can rewrite the leg chains.
- The game's own walk animation runs on the character when it moves.
- Steering yaw is at `eoc::SteeringComponent+0xC`; the game's forward is (-sin yaw, 0, -cos yaw) (agent C).

**To find out**
- The leg bone names and chain for the player's skeleton (logged by `!vr_skel`; already in the logs from earlier runs).
- How the walk animation's speed and stride relate to the stick (the locomotion system's speed parameter, agent J), so
  the feet don't slide.
- A ground raycast per foot. The mod's raycast works; the cost per frame needs measuring.

**Plan:**
1. **Head-driven body:** the body turns to follow the head when it's more than about 45° off, with a short delay, not
   on every head turn. Crouch from head height: the hips go down by the head's drop.
2. **Feet:** two-bone IK on each leg to ground raycasts during idle, keeping planted feet until the hips move more
   than about 25 cm (then a procedural step). While walking with the stick, the game's walk cycle plays, with a speed
   factor so its stride matches.

The camera never moves with the body (the head-only camera rule). This changes only what you see when you look down.
Setting: "Full body (legs)".

---

## 8. Hands that don't pass through walls and tables

**How it feels:** your virtual hand stops at a wall or table and slides along it while your real hand keeps going,
with a buzz on contact. Let go and it settles back onto your real hand within a moment.

**Known**
- The mod's raycast against the game's physics scene works and can skip the player's own body
  (`raycast_past_self`).
- The arms are posed from the hand targets (`bodyik.cpp`), so moving the target moves the visible hand and arm. The
  gloves and laser use the real controller pose, so they're unaffected.

**To find out:** only cost and tuning. A sphere or capsule cast would be better than one ray. Check whether the
game's PhysX scene exposes a sweep, like `RaycastClosest` (agent D).

**Plan:** each frame, cast from the shoulder (or last frame's virtual hand) to the real hand. If it hits, put the
virtual hand at the hit point minus about 2 cm along the normal, then smooth back to the real hand at about 0.1 s.
Haptics on first contact, scaled by speed. The hand target feeds IK and held items. Weapons in the hand stop too,
which also helps swings look right. Setting: "Solid hands".

---

## 9. More haptics

**How it feels**
- **Weapons:** a sharp thud when your weapon hits, a dull thud on a miss or a block.
- **Being hit:** a jolt, stronger with more damage.
- **Bow:** a creak as you draw, a snap on release (already partly there).
- **Spells:** a hum in the casting hand while a spell is held ready, and a burst when it fires.
- **Feet:** a light tick per footstep while walking.
- **Other:** small ticks on UI hover, item highlight and holster entry (already there).

**Known**
- The haptics path works: `headset::haptic` and the per-hand queues; the gloves and holsters use it.
- Attack results reach Lua through the Script Extender's `DealDamage` / `DealtDamage` events (agent I).
- The animation system has text keys and footstep events (agent C, `AnimationTextKeyEvents`).

**To find out**
- The client-side event for "my character was hit" (damage or HP change on the controlled character) that's cheapest
  to watch: an HP component change in Lua, or a native event.
- Where footstep events fire on the client (animation text key events with a footstep tag), filtered to the
  controlled character.
- A "spell prepared" state: the character's prepared spell or casting state, as the combat module reads now.

**Plan:** a small haptics module with named patterns (thud, jolt, hum, tick), triggered by bridge events from Lua
(hits, damage) and by native hooks (footsteps). Each pattern has its own strength setting, plus one "Haptics
strength" slider in the VR menu.

---

## 11. Backpack inventory

**How it feels:** reach over your shoulder and squeeze. A 3D grid of your inventory opens around your hand, items
shown as small 3D objects on shelves. Point or reach to pick one; it comes out physically into your hand (a held
item, like a pulled one). Put an item back by letting go over your shoulder (stowing already works this way).

**Known**
- The shoulder zone and stowing already exist (holsters, `hold.cpp`, `Osi.ToInventory` through the theft rules).
- The 3D UI already renders item and character models into textures (the paperdoll renderer, agent P), and the mod
  gives them stereo views (`dolls.cpp`). Agent V found the item-tooltip models too (`V_stereo_dolls_items.md` 3.3).
- The inventory data is readable from Lua (inventory components) and from the inventory UI's view-models.

**To find out**
- How to render many small item models at once. The paperdoll renderer does one model per render frame, so we need
  either many renderers (cost) or a cheap per-item thumbnail path (the game's own inventory icons, from the UI
  texture atlas).
- Taking an item out into the world: drop it at the hand's position (`Osi.Drop` / `DropTo`), then hold it with the
  gloves. Check the latency, since the drop goes through the server.

**Plan:**
1. **First version:** the shelf shows the game's inventory icons (flat, from the UI) on a curved grid around the hand.
   Pointing and squeezing takes the item out (`DropTo` at the hand, then grab it).
2. **Later:** 3D thumbnails for the most-used items only. The game's own inventory window stays available with A.
Setting: "Backpack inventory".

---

## 12. Conversations

**How it feels:** the NPC you talk to looks at your eyes, not at your character's head height or the old camera.
Answers float as cards below their face; point at one and pull the trigger, or use the stick and A, as now. The
camera stays your own; only their gaze and the answers change.

**Known**
- Answers are in the dialogue view-model: VMDialogue `+0xF8` answers, `+0x2E0` highlighted, `+0x300` shown; each
  answer's text at `+0x60` and its index at `+0x98`. Picking an answer can go through the DCDialogue handler 0x1741EC0
  or the answer's bound input event (agent AB). The mod already picks answers with the sticks and number keys.
- The UI panel comes up in conversations, and dialogue windows can be cropped from the UI texture (AB 2).

**To find out**
- **NPC gaze:** the look-at target the dialogue system gives the speaking NPC (an attach to the player character's
  head bone or the camera), and whether the mod can set it to the player's eye position each frame (a look-at
  component, or overriding the head bone's look-at IK).
- **Cards:** the answers' text is available as LocaStrings in the view-model. The mod needs either to render text
  itself (a small text renderer on quads) or to crop each answer's rectangle from the UI texture
  (`PointToScreen` 0x45FC6B0 gives element rectangles).

**Plan:**
1. **Cards:** crop each answer button's rectangle from the UI texture and show it as its own small panel in an arc
   at chest height, about 70 cm away. The laser picks it, and the click goes to the real button, so the game's own
   logic runs. Dice checks in answers then lead into the physical dice.
2. **Gaze:** after the look-at research, override the NPC's look-at target to the player's head pose while they
   speak.
Settings: "Answer cards", "NPCs look at you".

---

## Order of work

1. **Performance (now):** fixed foveated rendering and more right-eye sharing, from the AT and AU research, with
   GPU and CPU timing in the log so the first test measures each change.
2. **Physical dice:** the research is done (AQ, AR when it lands). Build behind "Physical dice", on by default.
3. **Small and self-contained:** potions by mouth (5), solid hands (8), haptics (9). Each needs one short research
   item.
4. **Reading in the hand (6) and conversation cards (12):** these share the UI-cropping work.
5. **Doors, chests and levers (4):** threshold version first.
6. **Legs (7) and backpack (11):** the largest; after the rest has been tested.
