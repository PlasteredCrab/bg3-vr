# Agent I: picking, targeting, actions and dice (bg3_dx11.exe 4.1.1.7631656)

Build: bg3_dx11.exe 4.1.1.7631656, image base 0x140000000; every address is an RVA.
Method: static only. Ghidra headless on project **BG3_A** (read-only) through a persistent query server
(`agents/I/IServer.java` = agent E's EServer command set + a `run <Script>` command; `agents/I/q.sh`).
Raw outputs: `agents/I/dec/*.c` (decompiles), `agents/I/req/*.out` (query results). Earlier agents' dumps
reused: `agents/E/dec/pickrange.c` (every function 0x1C3E180..0x1C4A000, decompiled by agent E).

Conventions: **Confirmed** = read in this build's decompile/disassembly (RVA given). **SE** = taken from
the Script Extender source in `downloads/bg3se-src` (names/layouts of a nearby build; not re-verified
unless stated). **Unconfirmed** = hypothesis, with what would confirm it.
Reports built on: A (rendering), B (split screen), C (input, character tasks), D (physics/raycasts),
E (items, net messages, Osiris table), F (UI), H (LOD).

(Report is written incrementally; sections marked "in progress" are not finished.)

---

## Hook points for the VR mod

| Need | RVA | How to use | Confidence |
|---|---|---|---|
| Replace the mouse picking ray with the controller ray | **0x4085960** `CameraController::ScreenToWorldRay(cam, u, v, float* outA, float* outB, bool cached)`; picking call sites return to **0x1C45CE1** and **0x1C45E21** (both in 0x1C45A90) | Hook 0x4085960; when the return address is one of the two picking sites, write the controller ray: `outA` (param_4, the point the raycasts start from) = ray origin, `outB` (param_5, raycast end) = origin + dir * L. Everything downstream (hovered character/item, ground position, path preview, highlight, default action) follows. | Confirmed (call + data flow); which of outA/outB is "near" with the game's projection: Unconfirmed (see 1.3) - irrelevant if the hook writes both |
| Same, without touching the camera function | 0x1C45A90 (picking ray build + jobs) | Alternative: overwrite the ray array entries `{end vec3, start vec3}` before the jobs are queued (stack array; mid-function hook) - more fragile than the 0x4085960 hook | Confirmed |
| Read what the game picked this frame | PlayerPickingHelper (per player) fields, filled by 0x1C45A90 | Inner[0/1] hovered character/item/etc. (+0x08..+0xD0), ground hit `Position` +0xF0 / `Normal` +0xFC, AiGrid tile +0x148, placeable +0xD8, scenery +0x108, ray points +0x120 / +0x12C | Confirmed (writes); names SE |
| Force the hovered target to a given entity (gesture/hand touch -> target) | **0x1C49310** `PlayerPickingHelper::SetSelection(helper, PlayerPickingInRangeEntity*)` | Fills Inner[] with that entity (character or item) and drops a ground ray under it for `Position`; used by controller-mode target cycling. Must be re-applied each frame after the helper update (0x1C43820 rebuilds the state) | Confirmed (body); per-frame persistence Unconfirmed |
| Controller-vs-mouse input mode | global byte **0x614FBC8** (1 = controller mode) set by 0x5EB2B0 / cleared by 0x5EB490 | Keep 0 (mouse mode) so the picking uses the ray path above; controller mode uses a facing-based target scorer instead (1.5) | Confirmed |

| Read what the game picked, per task | helper getter **0x1CC3260**(pickingMgr, playerId), hovered target **0x1C42190**(helper, &out, flag) | Same calls the tasks use | Confirmed |
| Click with the controller trigger | input event **Action1 = 4** (0x4161610); controller OnInputEvent **0x1B3F680** | Inject the raw mouse-left / pad-A that the user's bindings map to Action1 (C 3.2 InjectRaw 0x40F1F00); with the ray hook the click lands where the hand points | Confirmed (event id, dispatch); binding of the raw input Unconfirmed per profile |
| Start any character task from code | `ic = *(ecl::Character+0xA0)`; `ic->vf[0x68](type)` = Tasks[type]; `ic->vf[0x78](task, 0)` | E.g. MoveTo (0), Spell (4), PickUp (18); task types in 2.1 | Confirmed (game uses it in 0x159DB00) |
| Physical swing -> the game's own attack with dice | Osiris `Attack` **0x3654360** (SE Lua `Osi.Attack(attacker, target, 0)`) | Server-side; picks the equipped weapon attack spell (0x0FD1E00), Osiris task 0x12 -> CastRequestSystem 0x336E820 with no "ignore" options -> normal roll and action cost. `alwaysHit = 1` skips checks/rolls | Confirmed (code path); roll outcome visible at runtime only |
| Cast a named spell from code | Osiris `UseSpell` 0x36A82A0 -> 0x3671750 | Always adds options 0x1/0x2/0x4/0x200 (SE: IgnoreHasSpell/CastChecks/SpellRolls/TargetChecks) - may skip the roll | Code Confirmed; effect of 0x4 Unconfirmed |
| Observe/alter the attack result and the d20 | SE server events `ExecuteFunctor`, `BeforeDealDamage`, `DealDamage`, `DealtDamage` (hooks on 0x36D9630 / 0x36DD2E0 / 0x36E9F60 / 0x31C32F0) | Read `Hit` roll data (natural d20, total, critical) for VR presentation | Addresses Confirmed by SE signatures in this build; field layout SE |
| End the player's turn | client **0x1B3E3C0**(playerId) (sends 0x51 / 0xE8) or Osiris `EndTurn` 0x3688FB0 | Gesture "end turn" | Confirmed |
| Join combat | Osiris `EnterCombat` 0x369FA20 | e.g. after a physical hit on a neutral, if the game does not start combat by itself | Confirmed (request list); auto-start Unconfirmed |
| Turn state / movement budget | SE components `TurnBased`, `TurnOrder`, `ActionResources` (Movement GUID d6b2369d-84f0-4ca4-a3a7-62d2d192a185) | Constrain or charge physical walking in turn-based mode | Names Confirmed; spending code not located |

---

## 1. Picking (PlayerPickingHelper)

### 1.1 Objects and per-frame entry (Confirmed)

- `ecl::PickingHelperManager` system update 0x7588E0 -> **0x1CC2F40**, which calls the per-player
  update **0x1C43820** (call at 0x1CC3207). Helpers are created by **0x1C41890**
  `PlayerPickingHelper::ctor(helper, level, playerId)` (vtable **0x5656888**, dtor 0x1C41E00).
- The helper layout matches SE `ecl::PlayerPickingHelper` (`GameDefinitions/Picking.h`) at every offset
  checked here; offsets computed from the SE struct and confirmed by the code below:

| Off | SE name | Confirmed use in this build |
|---|---|---|
| +0x08 / +0x70 | `Inner.Inner[0]` / `Inner[1]` (0x68 each: CharacterPosition +0, DeadCharacterPosition +0xC, ItemPosition +0x18, TooltipPosition +0x24, Character +0x30, DeadCharacter +0x38, Item +0x40, Tooltip +0x48, GameObject +0x50, +0x58, Flags +0x60) | copied from the object-pick job (0x1C45A90) or set by SetSelection 0x1C49310 / UI hover 0x1C4A320 |
| +0xD8 / +0xE4 | `PlaceablePosition` / `PlaceableNormal` | from job +0x1E0 (+0xA0/+0xAC) |
| +0xF0 / +0xFC | `Position` / `Normal` (ground under cursor) | from job +0x1E8 (+0x40/+0x4C) |
| +0x108 / +0x114 | `SceneryPosition` / `SceneryNormal` | from job +0x1F0 (+0x38/+0x44) |
| +0x120 / +0x12C | SE `WorldPosition` / `WorldNormal` | **the two unprojected cursor-ray points** (see 1.3); not a hit |
| +0x140 / +0x148 | `PlaceableEntity` / `AiGridTile` | job +0x1E0 +0x98 / job +0x1E8 +0x38 (AI-grid tile looked up at the ground hit) |
| +0x150 / +0x154 / +0x158 | `PlaceableFraction` / `Fraction` / `SceneryFraction` | job fractions |
| +0x15C | flags (bit0 ground hit valid, bit1 "object set by selection/UI") | |
| +0x160 | `Level` (client level object; `+0x30` physics scene, `+0xB8` used by the object job) | |
| +0x168 | `PlayerId` (u16) | |
| +0x170 / +0x17C | `SelectableObjects` array (0x20 per entry) / count | controller-mode candidates (1.5) |
| +0x180..+0x1A0 | `Selection` optional (entity +0x180, flag +0x1A0) | written by 0x1C49310 |
| +0x1B8 | `PickingDirection` vec3 | = character steering yaw (C 4.2) |
| +0x1C4 / +0x1CC | `ControllerNudge` vec2 + flag | |
| +0x1D0 | `WindowCursorPos` vec2 | `= *cursorSource` every pick (0x1C45A90) |
| +0x1D8 / +0x1E0 / +0x1E8 / +0x1F0 | (SE hidden) the four raycast jobs | allocated in 0x1C41890, see 1.4 |
| +0x1F8 | (SE hidden) controller virtual-cursor object (vtable 0x5917C28, update 0x422EE70); its `+8` = cursor state when controller cursor mode is on | |
| +0x204 / +0x210 | `TargetOverride` optional<vec3> (value / has-value byte) | ctor clears +0x210; **no reader in 0x1C3E000..0x1C4E000** (FindInDecompiled `+ 0x204)` / `+ 0x210)` = 0 hits); readers elsewhere not found yet (Open lead) |
| +0x218 / +0x260 / +0x2A8 | `SpellConditionsCache` / `Cache` / `SightCheckCache` (hash maps) | invalidated by 0x1C47610 (1.6) |
| +0x258 / +0x2A0 / +0x2E8 | per-frame counters (cache-miss budget) | zeroed at the top of 0x1C43820 |
| +0x2EC | `IsMoving` | = 0x1C43820's `bVar25` |

### 1.2 Per-frame update order, 0x1C43820 (Confirmed, `agents/E/dec/pickrange.c` lines 3775-4075)

```
PlayerPickingHelper::Update(helper, GameTime*)                       0x1C43820
  counters +0x258/+0x2A0/+0x2E8 = 0; Inner (+0x08..+0xD0) reset from a default block (0x1C4B6A0)
  if (byte[0x614FBC8] == 0)          // keyboard+mouse mode
      0x1C45A90(helper)              // cursor-ray picking (1.3, 1.4)
      free the controller-cursor state (*(helper+0x1F8))+8, helper+0x200 = 0
  else                               // controller mode
      character = player map [0x60487A0]+0x138 lookup by PlayerId (+0x168) -> ecl::Character (0x7841A0)
      IsMoving = component(0x116E4A0)+0x44 > 0
      PickingDirection(+0x1B8) = (-sin yaw, 0, -cos yaw) from the steering component (agent C 4.2)
      ControllerNudge(+0x1C4) from 0x1C4BD20, flag +0x1CC
      0x1C47610(helper, character, IsMoving)     // cache invalidation only (1.6), not picking
      0x1C4B7D0 x2: refresh the two caches with condition callbacks 0x1C4CA50 / 0x1C4C9D0
      if (helper+0x200 == 0 && controller cursor active (*(helper+0x1F8))+8 != 0)
          SetSelection(0x1C49310) on the character itself, or clear (0x1C4B370)
      else
          0x1C48750(helper, time, character)      // controller target selection (1.5)
      0x422EE70(*(helper+0x1F8))                  // controller virtual cursor update
      if no Selection and the controller cursor is active: 0x1C45A90(helper)   // ray picking from the virtual cursor
      IsMoving -> +0x2EC; +0x200 = cursor active
  0x1C4B8A0(+0x218 / +0x2A8, time): expire cache entries
```

0x1C45A90 is also called directly by 25 other sites (0x1C41F60, 0x1C41FE0, 0x1C42060, 0x1C420E0,
0x1C42190, 0x1C422E0 x3, 0x1BCCA00, 0x1F7E750, 0x1FE5A50 x2, 0x200CAD0, 0x200CD10, 0x200E050,
0x200E450, 0x2064800, 0x2065360, 0x2066730, 0x20688D0, 0x206DD30, 0x4250910, 0x425D840; `req/q2.out`):
tasks force a fresh pick when they need one. A hook on the ray source (0x4085960) covers those too.

### 1.3 Where the ray comes from (Confirmed: 0x1C45A90 disassembly `req/q1.out`, decompile pickrange.c 5120-5495, `dec/p1_ray.c`)

1. **Cursor source.** Mouse mode: `cursor = *(vec2*)[0x614FB70]`, a 0x14-byte object allocated in
   0x3AA8EE0 (pointer stored at 0x3AAA2F6); the SDL pump 0x57AAA0 writes the window-space mouse position
   (floats) into `+0` at 0x57B32A and the global mouse state into `+8` at 0x57AB5C. Controller cursor mode:
   `cursor = *(*(helper+0x1F8)+8)`. Either way it is copied to `WindowCursorPos` (+0x1D0) at 0x1C45B3A.
2. **Which view/camera.** `view = 0x17F7330(playerManager [0x60487A0], PlayerId)` (the player's render
   view, with a fallback to the first active player; the same getter F found for world-to-screen).
   Camera = `[view+0x21D8]` (agent A: the view's camera controller), gated by `camera->vf[0x80]()`.
3. **Normalisation to the player's viewport** (disassembly 0x1C45DBB..0x1C45DF3):
   `u = (cursor.x - int[view+0x21A8]) / int[view+0x21A0]`, `v = (cursor.y - int[view+0x21AC]) / int[view+0x21A4]`
   (viewport x, y, width, height as ints; the same fields F found in WorldToScreen 0x407BF00).
4. **Unproject: 0x4085960** `(camera, u, v, float* outA /*R9*/, float* outB /*[rsp+0x20]*/, bool cached /*[rsp+0x28] = controller-mode byte*/)`:
   `ndc = (2u-1, 2(1-v)-1)`; refreshes the camera if `ViewDirty` (+0x10) / `ProjectionDirty` (+0x11)
   (0x4085150 / 0x40851F0); with `cached == 0` it rebuilds the view from the camera rotation quaternion
   (+0x58) and position (`vf[0x48]`) through 0x4084FF0 and inverts it; with `cached != 0` it uses the
   stored inverse at +0xD0..+0x10C; multiplies by the matrix at +0x150..+0x18C (inverse projection per
   SE's `rf::Camera` layout, not re-verified); then `outA = unproject(ndc.x, ndc.y, z = 1)` and
   `outB = unproject(ndc.x, ndc.y, z = 0)` (perspective divide). With the game's reversed-Z projection
   (docs/re-notes/camera.md: near maps to 1) **outA is on the near plane** and outB is the far point.
   Whether outB is finite with the measured infinite-far projection depends on the stored inverse
   (**Unconfirmed**: log outA/outB once at the hook).
5. **Ray record.** The picking builds an array of 0x18-byte records `{outB (end), outA (start)}`
   (stores at 0x1C45E45..0x1C45E6C). In controller-cursor mode it casts 8 more rays around the cursor
   (stack offset table `local_c8..local_84` in the decompile: (0,0), four (+-0.354, +-0.354) and four
   axis offsets of +-1.0); mouse mode casts one ray.
6. `helper+0x120 = outA`, `helper+0x12C = outB` (end of 0x1C45A90). SE calls these `WorldPosition` /
   `WorldNormal`; in this build they are **the ray's start and end**, not a hit and a normal (SE's doc
   text "world-space hit position" does not match this build).

### 1.4 The four picking jobs and what they write (Confirmed)

0x1C45A90 copies the ray into four job objects owned by the helper, runs them on the job system
(`0x410E5D0([0x6007BA8], job)` submit; `0x410E640` + `0x410E880([0x6007BA8], 1)` wait), then copies
the results into the helper. Each job's execute is vtable slot 2 (`req/q3.out`). All raycasts go
through the client physics scene `[helper.Level+0x30]` slot **+0x40** (D 5: the multi-hit raycast),
arguments `(scene, from, to, results, queryFlags, groupMask, excludeMask, context=1, -1, -1, filter)`:

| Job (helper field, size, vtable) | Execute | Ray (from -> to) | Flags / group / exclude | Results -> helper |
|---|---|---|---|---|
| Objects (+0x1D8, 0x168, 0x5656848) | **0x1C43EE0** | record+0xC (outA) -> record+0 (outB), every ray record | 0xF / **0x4CA20** / 0x210080 | sorts hits by distance; hit -> entity (`hit->vf[0xA0]`, D 8.4); per-view visibility filter 0x3CFFCA0 (view index job+0xA2); skips hits near the player's own characters (list 0x1C4BB60); resolves character (0x7841A0) / item (0x793890) / others (0x1C4BE80, 0x18D3950, 0x1C4C080); fills Inner[0], Inner[1] (+0x08..+0xD0) |
| Placeable (+0x1E0, 200, 0x5656868) | **0x1C44D70** | job+0x10 (outA) -> job+0x1C (outB) | 0xF / 0x8C00 / 0x194280 | PlaceablePosition/Normal +0xD8/+0xE4, PlaceableEntity +0x140, PlaceableFraction +0x150 |
| Ground (+0x1E8, 0x68, 0x5656808) | **0x1C452D0** | job+0x10 -> job+0x1C | 5 / 0x820 / 0x194280 | first accepted hit: Position +0xF0, Normal +0xFC, bit0 of +0x15C, AiGrid tile +0x148 (grid `[[0x6007A28]+0x90]+0x80`), Fraction +0x154 |
| Scenery (+0x1F0, 0x58, 0x5656828) | **0x1C457C0** | job+0x10 -> job+0x1C | 5 / 0xFFFFFFFF / 0x390388 | SceneryPosition/Normal +0x108/+0x114, SceneryFraction +0x158 |

UI hover override: in mouse mode, when `[[0x6047958]+0x130]->vf[0x50]()` is true (UI element under
the cursor) the object job is skipped and **0x1C4A320** fills Inner[] from the UI-referenced entity
(`helper+0x1B0`, SE `UIEntity`).

### 1.5 Controller mode (no ray): target scoring (Confirmed)

`0x1C48750(helper, time, character)` builds `SelectableObjects` (+0x170) from the combat participants
in turn order (0x1C46830, when 0x0ACA090 says the character is in turn-based combat) or from nearby
entities (0x1C473D0 -> 0x1C46E00, scored by **0x1C488F0**: distance and angle between `PickingDirection`
and the direction to the candidate; weights in the options object `[0x604B1B8]+0xDFC..+0xE7A`, acos
cut-off 1.658 rad), sorts them, keeps or replaces `Selection`, and applies it with **SetSelection
0x1C49310**: Inner[] = that entity, ground `Position` from a short vertical raycast (slot +0x38, flags 5,
group 0x20) under it, and the controller cursor `*(helper+0x1F8)+8` moved to it. For VR this is a
facing-based auto-target, not pointing.

### 1.6 0x1C47610 is cache invalidation, not controller picking (Confirmed; corrects C 4.2's label)

It walks six ECS change queries (type-id globals 0x60B8B50, 0x60B8B54, 0x60B7168, 0x60B8B58,
0x60B8B5C, 0x60B8B60) and marks entries of the three caches dirty (`SpellConditionsCache` +0x218,
`Cache` +0x260, `SightCheckCache` +0x2A8); clears all when `IsMoving` changes. No raycast, no cursor.

### 1.7 Substituting a VR ray (recommendation from 1.2-1.4)

* **Best: hook 0x4085960**; when the return address is 0x1C45CE1 or 0x1C45E21 (the two picking calls),
  write the controller ray: `outA = origin` (start), `outB = origin + dir * maxDist` (end). The four jobs,
  Inner[], Position, the default target action and every task that reads the helper then follow the
  hand. Keep `byte[0x614FBC8] = 0` (mouse mode). Other callers of 0x4085960 (0x1E14480 camera behaviour,
  0x422E6E0, 0x4261630) pass through unchanged.
* The UI-hover branch (1.4) must not fire while pointing at the world: it depends on the mouse being over
  a Noesis element; with the laser UI (F 2.6) keep the mouse off UI elements when the laser is on the world.
* Writing `WindowCursorPos` / the cursor object is not a substitute: the cursor is unprojected through one
  view's camera and viewport (1.3), which is not the hand's ray in VR.
* `TargetOverride` (+0x204/+0x210) is **an output of the Spell task, not an input**: 0x1FB8AB0 (Spell
  task, helper from 0x1CC3260 in r15) writes a target position to `[helper+0x204]` and sets
  `[helper+0x210] = 1` while a spell's targeting state needs it (0x1FB8D3E..0x1FB8D58), and clears it
  otherwise (0x1FB8D7D, 0x1FB8DE5); the Spell task's ExitPreview 0x1FB5990 clears it (0x1FB5A2F). No reader
  of +0x204 was found among the 55 functions that fetch the helper (callers of 0x1CC3260 / 0x1C42190 /
  0x1C420E0 scanned with `bg3vr_disasm --func`). Writing it from the mod would be overwritten. Not a VR input.
* Highlights, tooltips and the cursor shape follow Inner[] (tasks read it, section 2), so they follow the hand.

---

## 2. From click to action (client)

### 2.1 Objects (Confirmed)

* Per player character: `ecl::Character` (0x7841A0 lookup) `+0xA0` = its **InputController** (also
  agent E's 0x159DB00). The player's controller is `CharacterInputController`, vtable **0x5918678**
  (created in 0x1FAA960 on top of the base ctor 0x1B3E730, base vtable 0x5643038, name strings
  "InputController" 0x56430D0 / "CharacterInputController" 0x5918710).
* Fields used below (match SE `ecl::InputController`): `+0x08/+0x10` owner EntityRef, `+0x18` Active,
  `+0x1C` MouseHoldTime (-1 = none), `+0x20..+0x23` pressed/hold flags, `+0x24` MousePos (copied from
  the cursor object `[0x614FB70]`), **`+0x30` Tasks array indexed by task type** (count `+0x3C`),
  `+0x40` DragDropEntity, `+0x48` flags (bit0 = controller mode), **`+0x50` RunningTask**,
  **`+0x58` PreviewTask**, `+0x60` request flags.
* Vtable slots (0x5918678): +0x20 Update **0x1B3F9F0**, +0x58 OnInputEvent **0x1B3F680**,
  +0x68 GetCharacterTask **0x1B3EF70** (`return Tasks[type]`), +0x78 SetRunningTask 0x4245940,
  +0x88 FetchPressedObject 0x1B3F910, +0x90 InitializeTasks **0x4244E20**.
* Task objects (InitializeTasks 0x4244E20; slot = task type, C 4.1's numbering confirmed by the slot
  each object is stored in): 0 MoveTo (ctor 0x4262C40, vtable **0x591A400**), 1 ControllerSelection
  (0x5919878), 2 MoveController (0x591A158), 3 MoveInDirection (0x591A288), **4 Spell (ctor 0x1FB2730,
  vtable 0x56A4CC0, 0x598 bytes)**, 5 Dialog 0x1FE6610, 6 ItemUseRemotely 0x206E4C0, 7 ItemUse 0x1F7BCA0,
  8 ItemDrop 0x206A0A0, 9 ItemInteractArea 0x425A710, 10 ItemMove 0x20665D0, 11 ItemMoveDummy 0x425D7F0,
  12 ItemCombine 0x2068700, 13 ItemCombineDummy 0x2065D30, 14 SheathUnsheath (0x591A5D0), 15 Lockpick
  0x200C9D0, 16 DisarmTrap 0x200DFE0, 17 Pickpocket 0x20647B0, 18 PickUp 0x206C020, 19 Listen 0x2064150,
  20 Loot 0x1FE53E0, **22 DefaultTargetAction (ctor 0x4259530, vtable 0x59199D0)**, 23 ClimbTo 0x4258470
  (slot 21 CharacterMove is not filled here).
* Task vtable slots used by the controller (SE `CharacterTask` order holds): +0x28 Start, +0x30 Stop,
  +0x38 CanExecute, +0x48 GetPriority(preview), +0x50 GetExecutePriority, +0x68 GetError,
  +0x88 OnInputEvent, +0x90 EnterPreview, +0x98 UpdatePreview, +0xA0 ExitPreview, +0xC8 Enter,
  +0xD0 Update, +0xD8 Exit, +0x118 IsReady. Task fields: +0x68 `ecl::Character*`, +0x80 TaskType,
  +0x84 Flags, +0xE9 "execute requested" byte.

### 2.2 Input event ids (Confirmed, 0x4161610)

Registered with 0x40F36F0 like C's table: 0 Unknown, 1 UILeftMouse, 2 UIRightMouse, 3 UIMiddleMouse,
**4 Action1** (the world click / controller A), 5..0xA ControllerUnlinked*, 0xB KeyboardAny,
0xC.. DefaultCamera*, Widget* (C's 0x61+ table continues from 0x1B8E510). 0xAD Interact and 0xAE
ActionCancel are handled by the controller too.

### 2.3 The click path (Confirmed)

1. InputManager dispatches the event to the controller: **OnInputEvent 0x1B3F680(ic, &result, event)**.
   It first forwards the event to the RunningTask and to every task (`task->vf[0x88]`). For
   **Action1 (4)** press it records MousePos = `*[0x614FB70]`, DragDropEntity, and calls every task's
   Start (+0x28); on release it reports "handled" if a task took it. For ActionCancel (0xAE) it stops all
   tasks (Stop + ExitPreview) and exits the running task.
2. A task accepts the click in its own OnInputEvent. Example **MoveTo 0x4265950**: on Action1 release
   (`event+0x10` set, `event+0x1C` clear), not dragging, picking ground hit valid (`helper+0x15C` bit0)
   and the mouse not over UI (`[[0x6047958]+0x130]->vf[0x50]()` false) -> `task+0xE9 = 1` and notifies
   the listeners of `ecl::CharacterManager` (0x6051B60).
3. Each frame **Update 0x1B3F9F0** -> 0x1B410C0 (mouse-hold handling), **0x1B40730** (preview: choose the
   PreviewTask, `UpdatePreview`, cursor shape through `ecl::gCursorControl` [0x6007DD0] +0x24/+0x28
   (SE `CursorControl` layout), action-cost preview to `ecl::ActionResourceSystem` 0x6058F98 listeners),
   **0x1B41320** (execution: among Tasks, the highest `GetExecutePriority` with `CanExecute` and
   `IsReady` (+0x118, e.g. MoveTo 0x42639D0 = `task+0xE9`) becomes RunningTask: GetError (+0x68) must be 0,
   then Enter (+0xC8), Update (+0xD0) each frame until it returns false, Exit (+0xD8); task-change
   listeners at `[0x6007988]+0x88`).
4. Preview priorities read the picking helper (`0x1CC3260(pickingMgr, playerId)` = helper getter,
   `0x1C42190(helper, &out, flag)` = hovered target entity):
   * **MoveTo GetPriority 0x4263090**: ground hit valid and not over UI -> HoverPosition (+0x330) =
     `helper.Position` (+0xF0), HoverTarget (+0x340) = hovered entity, priority 3 (0x14 in some
     player states).
   * **Spell GetPriority 0x1FB3420**: when the hovered entity is a character that passes the
     hostility test 0x1132F90 (or an item flagged attackable), it picks **the character's weapon attack
     spell with 0x0FD1E00** (the same selector Osiris `Attack` uses, 3.2) and arms the Spell task with it
     through **SetSpell 0x1FB2BF0(task, SpellId*, sourceItem, ...)** (SpellId stored at task+0x278,
     prototype FixedString +0x2A8, source item +0x2B0) and returns priority 0x12 (0x19 when forced).
     So **a mouse click on an enemy = Spell task with Target_MainHandAttack (or the ranged/offhand
     variant)**, ordinary spell-cast flow afterwards.
   * **DefaultTargetAction GetPriority 0x4259640** returns -1 unless the player's input device is a
     controller (`[[0x604B1A8]+0x388+player*4] == 1`); it builds target actions through the task's
     TargetActionManager (+0x108) for the hovered target (+0x100) and its Update 0x4259B10 executes the
     first one (0x1FD93C0). Controller-only.
5. Starting a task from code (Confirmed mechanism, used by the game in 0x159DB00):
   `ic = *(ecl::Character*+0xA0); t = ic->vf[0x68](ic, type); /*fill task fields*/; ic->vf[0x78](ic, t, 0)`.

### 2.4 Spell targeting and the network (Confirmed)

* Client system `ecl::spell_cast::CastSystem` (type id 0x6059194, registered in 0x743140 from
  0x70EC90, update callback 0x75F670, object ctor 0x1EB0530) owns a request interface (vtable
  **0x54E3640**, created by 0x715C20) whose slots send the cast messages:
  +0x40 0x1F4C7C0 -> **StartPreview 0x11C** (builder 0x1FF95E0: SpellId at msg+0x28, caster +0x60,
  16 bytes +0x68, two bytes +0x78/+0x79, entity +0x80, target array +0x88 (0x60-byte entries), byte +0x98);
  +0x48 0x1F4C870 -> **UpdatePreview 0x11D** (0x1FF99A0; per-target variant records, types 0..0xE);
  +0x50 0x1F4C8C0 -> **Cancel 0x11F** (msg+0x28 caster, +0x30 16-byte cast guid, +0x40 bool);
  +0x58 0x1F4C9A0 -> **Confirm 0x11E** (0x1FF9FE0); +0x68 0x1F4CD30 -> 0x1F714C0, which turns the
  picking helper into spell targets with **0x1C422E0** (Inner[1] character +0xA0, dead character +0xA8,
  item +0xB0, or ground `Position` +0xF0).
  The direct **Cast 0x120** (0x1FFA6E0) is sent from 0x1FE4150 and 0x1FA6970 (not traced which UI).
  Message names from the adjacent strings `NETMSG_SPELL_CAST_START_PREVIEW / UPDATE_PREVIEW / CONFIRM /
  CANCEL / CAST` (0x554F698..0x554F550), matching the server handling below.
* Server `esv::spell_cast::Protocol` ProcessMsg **0x4283DF0** -> `esv::spell_cast::CastRequestSystem`
  (type id 0x6051628): 0x11C -> 0x4283B10 converts -> **0x336E950** (network start request); 0x11D ->
  preview-update list `+0x138` (0xA0 entries); 0x11E -> **0x336EB10** (confirm); 0x11F -> cancel list
  `+0xD8` {caster, guid, flag}; 0x120 -> a CastStartRequest with options `|= 0x10000` (`|0x80` when the
  message flag is set) appended to `+0x38` via 0x3371B30.

---

## 3. Server: making the game's own attack happen, and the dice

### 3.1 Osiris built-ins (handlers Confirmed; arguments from usage in the game's own goal scripts,
extracted read-only to `agents/I/gamedata/`)

| Call (usage in game scripts) | Handler | What it does (Confirmed) |
|---|---|---|
| `Attack(attacker, target, alwaysHit)` e.g. `Attack(_Executioner,_Victim,1)` | **0x3654360** | resolves attacker (character) and target (any object); refuses dead attackers (0x784340); picks the attacker's **weapon attack spell from its spellbook with 0x0FD1E00** (same as the mouse click, 2.3); creates Osiris task **type 0x12 (UseSpell)** via factory 0x30E7AA0, target `+0x80` (entity) / `+0x88` (position), option bits `+0xBD` = alwaysHit ? 0b0111 : 0; queues it on `char+0x120` with 0x35F4DB0 |
| `UseSpell(caster, "Spell_Id", target, target2, ...)` e.g. `UseSpell(_, "Projectile_MainHandAttack", target, NULL_...)` | 0x36A82A0 -> **0x3671750** | character caster: same task type 0x12 with option bits **`+0xBD` = 0x0F** (`or bl,0xF` at 0x3671C66), WithoutMove `+0xBB`; non-character caster: builds a CastStartRequest directly with options `| 0x207` (`| 0x217`) and submits **0x336E820** (Osiris list of CastRequestSystem) |
| `EnterCombat(a, b)` | **0x369FA20** | appends {a, b, 1, null} to `esv::combat::System` (0x6050310) `+0x308` |
| `SetHostileAndEnterCombat(f1, f2, a, b)` | 0x36A0EA0 | `esv::script::RequestSystem` (0x60D9710) 0x2EA01D0 |
| `EndTurn(char)` | **0x3688FB0** -> 0x36012F0 | end-turn request into `esv::TurnOrderSystem` (0x6050320) `+0x88` {handle, bool}, or, in force-turn-based mode, `esv::ftb::ZoneSystem` (0x60517A4) `+0x40` |
| `ForceTurnBasedMode`, `IsInCombat`, `CombatGetActiveEntity`, `AddActionPoints`, `GetActionResourceValuePersonal` | 0x36A3470, 0x3689150, 0x3687E50, 0x3673B80, 0x3660420 | not traced further |

**Osiris UseSpell task (type 0x12, ctor 0x36198B0, vtable 0x58600A8)**: Start/continue 0x3619D10 builds a
CastStartRequest (spell +0x38, caster, targets from +0x80/+0x88/+0x98/+0xA0 via 0x361A420, options from
**0x361A2B0**) and submits **0x336E820(CastRequestSystem, request)**. Option mapping in 0x361A2B0
(Confirmed; names from SE `SpellCastOptions`): bit0 -> 0x1 IgnoreHasSpell, bit1 -> 0x2 IgnoreCastChecks,
bit2 -> 0x4 IgnoreSpellRolls, bit3 -> 0x200 IgnoreTargetChecks, WithoutMove or caster already at the
cast position -> 0x10 NoMovement, `+0xBC` -> 0x20 AvoidAoO, consumable item -> 0x40, always | 0x80000.

Consequences for a rolled VR attack:
* **`Osi.Attack(attacker, target, 0)` gives options 0x80000 only** (plus NoMovement when already
  positioned): the normal checks and the normal spell roll (`SpellRoll Attack(AttackType.MeleeWeaponAttack)`
  of `Target_MainHandAttack`, Shared `Spell_Target.txt`: UseCosts ActionPoint:1, SpellSuccess DealDamage
  main-hand weapon). `Attack(…, 1)` sets 0x1|0x2|0x4.
* `Osi.UseSpell(...)` on a character always sets 0x1|0x2|0x4|0x200. If SE's name for 0x4
  ("IgnoreSpellRolls") is right, Osiris UseSpell casts skip the attack roll. **Unconfirmed**: compare the
  combat log / `DealtDamage` event of `Osi.Attack(a,t,0)` and `Osi.UseSpell(a,"Target_MainHandAttack",t)`
  on a high-AC target (miss chance must appear only for Attack).
* Recommended: **`Osi.Attack(player, target, 0)`** from SE server Lua when the hand's swing hits a
  character (target from the client-side hit test, D 8). It chooses the equipped weapon's attack spell
  itself and goes through the same CastRequestSystem as a player cast (action cost, `SpellAnimation`
  and roll then come from the spell; the server cast pipeline after 0x336E820 was not traced here).

### 3.2 Client-side alternative (same as a mouse click)

Arm the Spell task exactly like 0x1FB3420 does: `task = ic->Tasks[4]`, SetSpell 0x1FB2BF0 with the
weapon attack SpellId from 0x0FD1E00, picking pointed at the target (1.7), then the normal
preview -> confirm (0x11C / 0x11E) path. More moving parts (argument list of 0x1FB2BF0 beyond the first
three is Unconfirmed); prefer 3.1.

### 3.3 Where the roll and damage are resolved (server)

* Functor execution entry points (located by SE signatures in this build, `sigfind`):
  `esv::ExecuteStatsFunctor_*`: AttackTarget **0x36D9630**, AttackPosition 0x36DB7E0, Move 0x36DC140,
  Target **0x36DD2E0**, NearbyAttacked 0x36DF2D0, NearbyAttacking 0x36E0370, Equip 0x36E1050,
  Source 0x36E2970, Interrupt 0x36E4D40 (dispatch switch at 0x35D92E8, SE "UnitTestHelper::ExecuteStatsFunctors");
  `DealDamageFunctor` execute **0x36E9F60** (call at 0x2F56FF8, also reached by Osiris `ApplyDamage`
  0x36A8B40, E 6); `StatsSystem::ThrowDamageEvent` **0x31C32F0** (call at 0x36EB1A3).
  Signature (SE): `void(HitResult* hit, Functors* self, ContextData* ctx)`; the HitDesc carries the
  attack roll (SE `Hit.h`: `AttackRollAbility`, `ConditionRolls`, `StatsRoll{Roll, Result{Total,
  NaturalRoll, Critical}, Metadata}`).
* SE already hooks these: server Lua events `ExecuteFunctor`, `BeforeDealDamage`, `DealDamage`,
  `DealtDamage` (SE `Lua/Server/FunctorEvents.inl`) - the mod can read the natural d20 and the outcome of
  the attack it triggered there (and present its own VR dice).
* The d20 itself comes from the server roll streams (`esv::roll::stream::{StreamsComponent, UpdateSystem,
  SaveSystem}`, SE: per-combat/per-player PRNG streams with karmic "FailDebt/SuccessDebt"); the exact
  roll function was **not located** (Open lead).
* Dice UI: attack rolls are not animated dice; the 3D dice belong to ability checks ("active rolls":
  `eoc::RequestedRollComponent`, `esv::RollSystem`, messages RequestedRoll 0xEA, SelectDice 0x131 (client
  sender 0x1A40820), ActiveRollModifier 0x113 (0x19D5980); Osiris `RequestActiveRoll` 0x36ABDE0 /
  `RequestPassiveRoll` 0x36ABDD0). Not traced further.

### 3.4 Attacks outside turn-based combat, starting combat

* Neither the Osiris Attack path (3.1) nor the client Spell priority (0x1FB3420) tests a turn or combat
  state (Confirmed by their bodies), so attacks are cast in real time; the action cost (ActionPoint) is a
  normal spell use cost.
* Joining combat: Osiris `EnterCombat` -> `esv::combat::System +0x308` request list (Confirmed); how a
  hostile cast against a neutral starts combat automatically was not traced (**Unconfirmed**; runtime: log
  `eoc::combat::ParticipantComponent` / Osiris `CombatStarted` after `Osi.Attack` on a neutral).

---

## 4. Turn-based essentials

* Components (SE, names confirmed by `GetTypeName` strings): `eoc::TurnBasedComponent` (0x54C9330;
  SE: IsActiveCombatTurn, RequestedEndTurn, CanActInCombat, Timeout, CombatTeam),
  `eoc::TurnOrderComponent` (0x54A9540; SE: CurrentTurnOrder, groups, Round, TimeRemaining),
  `eoc::combat::{StateComponent, ParticipantComponent, IsInCombatComponent}`,
  `eoc::ActionResourcesComponent` (0x54AD440; SE: `HashMap<Guid, Array<{ResourceUUID, Level, Amount,
  MaxAmount, ...}>>`).
* Resource GUIDs (game data, Shared `ActionResourceDefinitions.lsx`, read-only extract): ActionPoint
  `734cbcfb-8922-4b6d-8330-b2a7e4c14b6a`, BonusActionPoint `420c8df5-45c2-4253-93c2-7ec44e127930`,
  ReactionActionPoint `45ff0f48-b210-4024-972f-b64a44dc6985`, **Movement
  `d6b2369d-84f0-4ca4-a3a7-62d2d192a185`**; all replenish per Turn.
* Whose turn: SE `entity.TurnBased.IsActiveCombatTurn`; Osiris `CombatGetActiveEntity` 0x3687E50.
* End turn: client **0x1B3E3C0(playerId)** - if the character is in turn-based combat sends
  EndTurnRequest **0x51** (payload +0x28 = character, via 0x1B3E1F0 / 0x1B3E130 for each group member),
  (0x1B3E1F0 with callback 0x1B3E130; 0x1B3E1F0 itself not decompiled),
  in force-turn-based mode ForceTurnBasedEndTurnForPlayerRequest **0xE8**; cancel 0x52 (0x1B3E190,
  0x159FC30). Server: Osiris `EndTurn` 0x3688FB0 -> 0x36012F0 (3.1).
* Movement budget: the MoveTo task carries SE `CharacterMoveTask::{PathDistance, PathLimitDistance,
  HasLimitPath}` (preview path clipped to the remaining Movement); the code that subtracts Movement while
  walking was **not located** (the stats string "Movement" 0x54EB840 is the character speed stat, FixedString
  0x5EEC368, not the resource). For physical walking: read the Movement entry of ActionResources per frame
  (SE), stop/charge the player when the walked distance exceeds it, and write the remaining amount
  (SE component write + replicate, or a server request; **Unconfirmed** which one the game accepts without
  being reset) - Open lead.

---

## Open leads

1. **Option 0x4 semantics** (3.1): does Osiris UseSpell skip the attack roll? Runtime: compare
   `DealtDamage` / combat log for `Osi.Attack(a,t,0)` vs `Osi.UseSpell(a,"Target_MainHandAttack",t)`.
2. **Far point of 0x4085960** (1.3): log outA/outB for one call (finite or inf) - only matters if the mod
   keeps any game-computed ray.
3. **Where the d20 is drawn** (roll streams) - find users of `esv::roll::stream::StreamsComponent`
   (TypeIdRefs found no initializer pattern for its type id; search the UpdateSystem body instead).
4. **Movement consumption in turn-based** - find the system that spends the Movement resource while a
   character paths (start from `eoc::ActionResourceSpendEventOneFrameComponent` 0x55113F0 writers, or the
   CharacterUseMovement message 0x2F handler).
5. **UI-hover predicate** `[[0x6047958]+0x130]->vf[0x50]` (1.4, 2.3) - identify the object (likely the
   Noesis/UI manager, F) so the VR laser can keep it false while pointing at the world.
6. **Cast 0x120 senders** 0x1FE4150 / 0x1FA6970 (UI hotbar?) and the CastSystem update 0x75F670 body.
7. **Automatic combat start** on a hostile cast against a neutral (3.4).
8. **SetSpell 0x1FB2BF0** remaining arguments (only needed for the client-side attack route 3.2).

