# Item outline for the gravity gloves, ToInventory amount, theft on stow - agent AC, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_G, read-only, no analysis pass, headless, request
server `agents\AC\ACServer.java`; disassembly with bg3vr_disasm). RVAs are relative to 0x140000000. Raw output is in
`re-local\ghidra\agents\AC\` (ac1..ac30). Story rules are quoted from the game's own goal files that agent I extracted
read-only (`agents\I\gamedata\...\_CRIME_CrimeTriggers.txt`). "Confirmed" = read in code (or game data) at the cited
place. "Unconfirmed" = not settled by the code read; section 6 says how one run settles it.

## Fix for the VR mod

| what | RVA / hook | how | risk | confidence |
|------|-----------|-----|------|------------|
| **Outline one entity of our choice (recommended)** | push a request into **ecl::OutlineSystem** (`sys+0x90`) with **0x126F650**`(sys+0x90, &req)`; do it inside a hook on the system's update **0x1A6DE40** entry (rcx = sys, rdx = ctx) | `sys = *(u64*)(*(u64*)(world+0x30) + (u32)[0x146059208]*0xF8)`, `world = [[0x145FDA240]+0x1A8]`. Request (0x20 bytes): `+0` u64 entity handle (= `ecl::Item+0x10`), `+8/+0xC/+0x10` float r,g,b (linear), `+0x14` u16 key, `+0x18` u32 op = 0 (add or recolour). Use a key that no real player uses (any non-zero u16, e.g. 0x7FF0). Re-push op 0 every frame while aimed (cheap: an existing entry is only recoloured). | low (unlocked array, so push only from the consumer's own update) | Mechanism Confirmed (1.2-1.4). Thread of 0x1A6DE40: Unconfirmed (6.1) |
| Remove our outline | same push, op **2**, same key | Push once when the aimed entity changes or the gloves release. Never use key 0: key 0 makes op 3 = remove every entry of the entity (also the mouse hover's). | low | Confirmed (0x1A6E130, 0x1A6E2A0) |
| Game colour instead of our RGB | **0x1A6E030**`(sys, u64 entity, u32 colourIndex, u16 key)` (add) / **0x1A6E130**`(sys, entity, s16 key)` (remove) | Colour = table `[0x14604B1B8] + 0xE80 + idx*0x10` (RGB, game does pow 2.2). Hover of an item uses idx 0x25. Same thread rule as above. | low | Confirmed |
| Alternative: the game's own hover highlight | highlight manager `[0x146007DC8]`, **0x1809260**`(mgr, u16 player, u64 entity, u32 state=1, u8 flag=0)` / clear **0x18092D0**`(mgr, s16 player, u64 entity)` | Same colour and filters as the mouse hover. Shares the (entity, player) slot with the mouse hover, so the hover's clear also clears ours: re-apply every frame. Filters can refuse some entities (1.5). | medium | Confirmed |
| Show only our outline while the gloves aim | detour **0x18090A0** (SetHighlight) | When gloves aim and the return address is **0x12E88ED** (the hover call at 0x12E88E8, state 1), skip the call and remember (player, entity). On release, call 0x1809260(mgr, player, entity, 1, 0) for it if it is still the hovered one. The hover's clear (0x18092D0 at 0x12E88B6) on an entity without a node is harmless. | low | Call sites Confirmed (disassembly) |
| **ToInventory amount** | Osiris `ToInventory` 0x368A110 -> 0x35F3380 | `0` = do nothing (returns success). `<0` (e.g. -1) = whole stack. `>= stack size` = whole stack (clamped). `0 < n < stack` = split n. | - | Confirmed (0x35F3380) |
| **Stow as theft, keep the normal crime path** | server: `Osi.ToInventory(item, char, -1, 0, 0)` (clearOriginalOwner **0**) | Goes through `esv::inventory::InteractionRequestSystem`, the same system that adds a picked-up item. That system tags every added item with `esv::inventory::StealingDataOneFrameComponent`, which the crime hook turns into the Osiris event **CharacterStoleItem**; the story registers crime "Steal" from that event. | low | Path Confirmed to the StealingData tag (5.2). That CharacterStoleItem fires with the right thief for a ToInventory add: Unconfirmed (6.4) |
| Fallback if CharacterStoleItem does not fire | SE Lua, story DBs | Reproduce the story rule exactly (5.3): `Osi.DB_CRIME_StealQueuedID(char, owner, x, y+0.5, z, "Steal")`, `Osi.DB_CRIME_StealQueued(char, item, template, owner, amount, value)`, `Osi.PROC_CRIME_PreventMoveForbiddenItem(item)`, `Osi.TimerCancel("CRIME_ProcessThefts")`, `Osi.TimerLaunch("CRIME_ProcessThefts", 0)`. Only for party members and items not taken from a container (the story's own conditions). | medium (story DB layout can change with patches) | Story text Confirmed |
| Moving an owned item in the world (gloves pull) | story rule on `MovedBy`, `OnThrown`, `OnStartCarrying` | The normal game makes it the crime **"MoveForbiddenItem"** (5.4). The gloves move the item client-side, so no server `MovedBy` exists. To match: when a pull starts on an item whose owner is an NPC, call `Osi.PROC_CharacterRegisterCrime(char, "MoveForbiddenItem", item, owner, 0)` with the same conditions as `QRY_GetMoveForbiddenItemInfo`. | medium | Story text Confirmed; that client moves raise no MovedBy: Unconfirmed (6.5) |

## 1. Who writes the outline (question 1)

### 1.1 Correction to R: which set CullStage reads (Confirmed)

In CullStage job 0x3E24970 (N/r14_culljobs.c 549-552, 590-592) the sub-job field `+0x18` (the set looked up in
0x3E21C70) is `local_258`, from **0x1A6F580**. 0x1A6F580 reads query **0x60B17D8** = `Spec<ls::OutlineEntitiesForRenderingSingletonComponent>`
(disassembly 0x1A6F5BC). The per-player block `0x1817500(...) + index*0x30` that R named is field `+0x20`. 0x1817500
reads query 0x6059474 = `ls::FadingEntitiesForRenderingSingletonComponent`, and 0x3E21C70 uses it only for the fade
value (0x3CFFEC0). So the outline is not per player at this stage.

0x3E21C70 walks each renderable to its root visual (`+0xC8` parent), takes the handle at root `+0x108`, looks it up in
the OutlineEntitiesForRendering set, and then reads that entity's **`ls::OutlineComponent`** (type id 0x60A48F0) with
0x3E280D0 / 0x3E28240. That component pointer is item+0x18 in R's description.

### 1.2 `ls::OutlineComponent` and its only writer (Confirmed)

* Layout (0x1A70150, 0x1A6F7F0, 0x1A6E8D0): an array `{ptr, cap, count}` of 0x10-byte entries
  `{float r, g, b; u16 key}` plus a byte at `+0x10`. The byte is set when the entity is an item with `item+0x18 & 6`
  (or 0x12AFC30 for others); meaning not traced.
* All readers of the type id 0x60A48F0 (ac1): 0x1A6E6B0, 0x1A6EB40, 0x1A6F710, 0x1A6FF90, 0x1A70150 (all in the
  OutlineSystem update), 0x1A70880/0x1A70DB0/0x1A712D0/0x17DDBB0 (query registrations), 0x3B48860 (type registration),
  0x3E280D0/0x3E28240 (render reads). So **only ecl::OutlineSystem changes the component**, and only through its
  request list.
* The singleton set is kept in sync by 0x1A6ED50 from the Added/Removed/Dead queries of OutlineComponent
  (0x60B17E8/0x60B17E0/0x60B17E4). Any entity that has the component is in the set.

### 1.3 ecl::OutlineSystem (type id 0x6059208) update 0x1A6DE40 (Confirmed)

Called by the ECS callback 0x7615A0 (registered in 0x747670), args `(sys, ctx)`. Each frame it:
1. Takes every request at `sys+0x90` (0x20-byte records, count `+0x9C`) through 0x1A6E2A0, ops 0/1 into an add map
   (`+0x10`), ops 2/3 into a remove map (`+0x50`), op 3 also into a remove-all set (`+0xA0`).
2. 0x1A6E6B0: remove-all entities lose the component (or get count 0 if re-added in the same frame).
3. 0x1A6E8D0: add map. Entity must be alive and `0x18A0310(world, entity)` non-null. No component yet: op 0 creates
   it (0x1A70150). Component present: the entry with the same key is recoloured; op 0 appends if there is none; op 1
   only recolours.
4. 0x1A6EB40: remove map. Entries with the key are erased; an empty component is removed.
5. 0x1A6ED50 syncs the singleton set, then all request maps are cleared (`+0x9C = 0`).

The component is **persistent**. Nothing clears it per frame; it changes only on requests.

Request layout (0x1A6E030 / 0x1A6E130 / 0x1A6E2A0): `+0x00` entity, `+0x08` r, `+0x0C` g, `+0x10` b, `+0x14` u16 key,
`+0x18` u32 op. Op meanings: 0 add/recolour, 1 recolour only if present (0x1A6E1A0), 2 remove this key, 3 remove all.
0x1A6E130 sends op `3 - (key != 0)`, so key 0 is "all keys".

Push helper 0x126F650 is a plain array append with no lock (Confirmed). Producers of OutlineSystem requests (ac6):
0x1A6E030, 0x1A6E130, 0x1A6E1A0, 0x180A060, 0x180A820, 0x1BCD0F0, 0x1BCE330, 0x1BD15D0.

### 1.4 The highlight manager decides colour (Confirmed)

Global `[0x146007DC8]` keeps per entity, per player a node `{state +0, colour +4, override colour +8, applied +0xC,
flags +0xD/+0xE}` (0x180B390). Calls:
* SetHighlight **0x18090A0**`(mgr, ctx*, u16 player, u64 entity, u32 state, u8 flag)`; wrappers 0x1809260 (builds ctx
  from `[[0x145FDA240]+0x1A8]`), 0x1F50780, 0x1A3B590 (state 3). Gate: `byte [mgr+0x78] != 0`.
* ClearHighlight **0x18092D0**`(mgr, s16 player, u64 entity)`; refresh 0x180A060`(mgr, s16 player or 0, entity)`;
  query 0x180A680 returns the applied colour index (0x1B = none).
* **0x180A820** maps state to colour index and sends add (0x1A6E030) / remove (0x1A6E130). Indices seen: state 1 and
  not a character -> **0x25**; own controlled character 0x13; state 2 -> 0x1C; state 5 -> 0x22; relation colours
  0x3F, 0x40..0x47; 0x1B = no outline. Refusals (return without outline): no visual, item flag bit 22 (`item+0x18 &
  0x400000`), 0x1172410, 0x1F5A770, 0xADD480 true.

### 1.5 Where the mouse-hover outline comes from (Confirmed)

The gui system `gui::tooltips::WorldTooltips` update **0x12E7F70** (query 0x6098434) compares, per player, the cached
entity at VM `+0x720` with the "CurrentTarget" view-model's entity (`[VM+0x728]+0xA8`). When it changes it calls
ClearHighlight (0x12E88B6) on the old one and **SetHighlight(state 1, flag 0) at 0x12E88E8** on the new one, and
sets the cursor shape (`[sys+0x10]+0x24`, ids 0x14..0x33). It also gives state 5 to world-tooltip items (left-alt
labels, list VM `+0x710`, calls 0x12E895D / 0x12E99F7). VM layout from ctor 0x141C960; "CurrentTarget" cleared by
0x141D850. Who fills CurrentTarget from picking: Unconfirmed (6.3). UI portrait hover uses 0x12F4940 (state 1,
flag 1). Other users of the OutlineSystem: the 0x1BCxxxx object (lists `+0x28/+0x38`, colours 0x1C/0x23/0x24) and the
ping highlight system (0x1A3BBB0, `ecl::highlight::DataComponent`, state 3).

## 2. Best place to add/remove our outline (question 2)

Direct OutlineSystem request with our own key (table row 1). Reasons (all Confirmed): it bypasses the highlight
manager's filters; our entry is keyed separately from the hover's (entity, player) entry, so the hover's op 2 never
touches it; and the renderer in single player (`view+0x21D0 = 0xFF`, R 1.1) draws an object with its first entry,
whatever the key. Op 3 for an entity can still come from 0x180A060(mgr, 0, entity) when that entity has no manager
nodes, and from 0x1BCD0F0. Re-pushing op 0 every frame covers that.

Handle: the game outlines the item's own handle `ecl::Item+0x10` (0x1BCCA00: `0x793890` item, then `*(item+0x10)` to
0x1A6E030). Same field the mod already has.

Threading: 0x1A6DE40 consumes the list in its ECS update; the push has no lock. Pushing at 0x1A6DE40 entry is safe
whatever thread runs it. Calling 0x1A6E030 from `on_frame` is safe only if on_frame never overlaps the client ECS
update (Unconfirmed, 6.1).

## 3. Interference with the mouse hover (question 3)

* Both coexist: different objects get their own outlines; on the same object the first entry's colour is drawn.
* The right-hand laser drives the picking ray, so the hover outline already follows the laser. If the gloves aim with
  the same hand at the same item, the game's hover (idx 0x25) is already there.
* To show only ours while aiming: detour 0x18090A0 and drop the state-1 call from 0x12E88E8 (row 5). Do not filter
  the OutlineSystem request list; that would also drop left-alt and party highlights.

## 4. Osiris ToInventory amount (question 4) (Confirmed)

0x368A110 reads `amount` from arg +0x28, `notify` +0x38 (param_5), `clearOriginalOwner` +0x48 (param_6), and calls
0x35F3380(out, item, target, amount, notify, clearOwner, 0):
* `amount == 0`: returns success at once, nothing moves.
* else `stack = 0x30C3530(item)` (0x114FB20 amount helper, the same one the inventory processor uses for a missing
  amount). `n = (amount >= 0 && amount < stack) ? amount : stack`. So -1 and any value >= stack mean the whole stack.
* Partial and the target inventory is the item's current inventory: `esv::inventory::StackSystem` split 0x34A5660,
  return.
* `clearOriginalOwner != 0`: 0x3600AC0 clears the item's record in `esv::ownership::TrackingSystem` (record `+0x20` =
  null, recursive into contents). That removes the owner, so no theft (5.2).
* Move: 0x35F9A60(item, target, opts, n, ...) with param_15 = 1. Inside it, amount 0xFFFF also means the whole stack.
* `notify != 0` and the previous holder differs: `esv::inventory::EntityReceivalNotificationSystem` 0x350B730.

## 5. Theft on stow (questions 5a-5c)

### 5.1 Owner components (Confirmed, 0x3600630)

The crime owner of an item is resolved by **0x3600630**`(out, ctx, &item)`:
1. item must have the tag `eoc::ownership::OwnedAsLootComponent` (0xA8A3F0);
2. owner = pending `esv::ownership::OwneeRequestComponent` (0x26E6790) if set, else
   **`eoc::ownership::OwneeCurrentComponent.Ownee`** (0xAFC7D0);
3. owner must be a character (`eoc::character::CharacterComponent`, 0xAC5050), else no owner.

`eoc::inventory::IsOwnedComponent.Owner` (0xB0D6D0) is used by the inventory code as "which inventory the item is in"
(0x35F9A60, 0x3474EE0), not as the crime owner. Type names from the init functions' GetTypeName strings (ac23).

### 5.2 Where the theft is raised (Confirmed up to the Osiris event)

* The pick-up systems (`esv::pickup::*`, updates 0x35B8330 Request, 0x35B68D0 Validation, 0x35BD350 Permission,
  0x35C4310 Execution, 0x35C7730 Finished; ac27-ac29) contain no ownership or crime call (callee lists in ac29).
  The theft is not in the pick-up pipeline.
* `esv::inventory::InteractionRequestSystem` (type 0x6051680, registered 0x2375C50, update 0x239C930 -> 0x3471AF0,
  **0x3473DF0**) processes the add/move requests (0xE0-byte records at `sys+0x20`, 0x3474EE0). For every item it
  added, 0x3473DF0 (0x34748xx) creates **`esv::inventory::StealingDataOneFrameComponent`** (0x34766C0) with
  `+0x00` = crime owner from 0x3600630, item name/template, position, amount, `+0x5C/+0x60` ints. Condition: the
  object from `[sys+0x88]->vf[1]` has type `DAT_145EED7C4` and byte `+0x570 == 0` (meaning Unconfirmed).
* ToInventory reaches the same system: 0x35F9A60 pushes the request with 0x34715C0 (item not in an inventory) or
  0x3471750/0x3471830 (item in an inventory) into `[0x6051680 system]+0x20`.
* Crime hook: 0x30C9480 (gets `esv::CrimeHandler`, subscribes to `esv::script::GameplayEventSystem+0x100`) registers
  handler **0x30D1520**. It looks the item up in the StealingData queries (0x6112F34/0x6112F38), compares the owner
  character with the new holder, and calls **0x30CBBD0**, which builds a 10-argument Osiris event and queues it
  (0x38252A0). Argument order (character, item, template, x, y, z, owner, container, int, int) matches the story
  event **`CharacterStoleItem(_Character,_Item,_ItemTemplate,_X,_Y,_Z,_Victim,_SrcContainer,_Amount,_Value)`**.
  The exact gameplay event that triggers 0x30D1520 and its thief argument: Unconfirmed (6.4).

### 5.3 The crime itself is story (Confirmed, Shared `_CRIME_CrimeTriggers.txt` 3783-3870)

On `CharacterStoleItem` for a party member: if `_SrcContainer` is NULL, queue crime "Steal" at the thief's position
(`DB_CRIME_StealQueuedID`, `DB_CRIME_StealQueued`, `PROC_CRIME_PreventMoveForbiddenItem`, timer
`CRIME_ProcessThefts`). From a container only in combat: "Steal_NoCrimescene". The timer registers the crime with
`PROC_CharacterRegisterCrimeWithPosition(char, "Steal", NULL, x, y, z, victim, crimeID)` after the per-item block
check `QRY_CRIME_Steal_BlockRegisterCrime`; the stolen items go to `DB_CRIME_Steal(crimeID, item, ...)`. Witnesses
and NPC reactions follow from the registered crime. Where the inventory UI's "stolen" mark comes from: Unconfirmed
(6.4 logs OwneeCurrent/OwneeHistory before and after).

### 5.4 Moving an owned item in the world (Confirmed, same file 5656-5790)

`QRY_GetMoveForbiddenItemInfo(mover, item, owner)`: mover is a party member, the item is an item, `CanSitOn 0`,
`IgnoreGenerics 0`, `QRY_CrimeIsNPCOwner(owner)` -> crime **"MoveForbiddenItem"**. Raised on `MovedBy(item, char)`
(pending 3 frames, cancelled if `CharacterStoleItem` follows, then
`PROC_CharacterRegisterCrime(player, "MoveForbiddenItem", item, owner, 0)`), on `OnThrown`, on `OnStartCarrying`,
and on the "teleportation" spell. So yes: dragging, throwing or carrying an owned item is itself a crime in the
normal game.

### 5.5 Recommendation for 5b

1. Stow with `Osi.ToInventory(item, char, -1, 0, 0)` (clearOriginalOwner 0). It uses the same inventory system as a
   pick-up, which creates the StealingData that feeds the theft event (5.2). It does not walk the character.
2. Listen to `CharacterStoleItem` in SE (after). If it fires with the controlled character as thief, nothing else is
   needed.
3. If it does not, replay the story rule of 5.3 yourself (table row 7).
4. `Osi.Pickup(char, item, "", 0)`: per E it uses the direct request list `[0x14603B358]+0x2A0` when the item lacks
   flag 0x4 and force is 0, else Osiris task 0xE. Whether that task walks: Unconfirmed, not needed if 1-2 work.
5. Gloves pull of an owned item: call `PROC_CharacterRegisterCrime(char, "MoveForbiddenItem", item, owner, 0)` under
   the conditions of `QRY_GetMoveForbiddenItemInfo`, once per pull, and skip it when the pull ends in a stow that
   raises CharacterStoleItem (the story cancels the pending move crime the same way).

## 6. One-run logging plan

Session: hover items with the laser, aim the gloves at a different item, hold left-alt, stow one owned item in a
shop or camp with an NPC watching, pull another owned item without stowing.

6.1 **Threads.** Log `GetCurrentThreadId()` at 0x1A6DE40 entry, at 0x1A6E030 entry (with return address), and in
`on_frame`. If 0x1A6E030's game callers and on_frame share a thread with 0x1A6DE40, either push place is fine.

6.2 **Our outline.** At 0x1A6DE40 entry: request count `[sys+0x9C]`; after our push, dump all requests (entity, rgb,
key, op). At exit: for our entity, OutlineComponent via 0x1A6FF90(ctx_world, entity) -> entries (rgb, key) and
count. Expect our key present every frame, the hover's key (player id) only on the hovered entity.

6.3 **Hover source.** At the 0x18090A0 detour: return address, player, entity, state, flag. Compare the entity of the
call returning to 0x12E88ED with the PlayerPickingHelper's Inner item handles (I 1.1, `+0x48`, `+0xB0`). That shows
whether CurrentTarget follows picking.

6.4 **Theft.** Server side, for the stowed item: before ToInventory and two frames later log OwneeCurrent.Ownee
(0xAFC7D0), OwnedAsLoot (0xA8A3F0), OwneeRequest (0x26E6790), and the result of 0x3600630. Detour 0x34766C0 (log
item, StealingData `+0` owner, `+0x5C`), 0x30D1520 entry (`*rdx`, `[r8+0x10]`, bytes `[r8+9]`, `[r8+0xA]`,
`[r8+0x18]`) and 0x30CBBD0 entry (rdx thief object `+0x10` handle, r9 owner). In SE log `CharacterStoleItem` and
`CrimeIsRegistered`. Also log at 0x3473DF0 the check object type `*vf[3]()` vs `[0x145EED7C4]` and byte `+0x570`.
Do the same once with a normal mouse pick-up of an owned item as the reference.

6.5 **Moves.** In SE log `MovedBy`, `OnThrown`, `OnStartCarrying` and `CrimeIsRegistered` during a gloves pull of an
owned item. Expect no MovedBy (client-side move). If none, row 8 applies.

## Raw files (agents\AC\)

ac1 type-id refs; ac2 ls outline functions; ac4 OutlineSystem update and request processor; ac5-ac6 request API and
highlight manager; ac7 0x1BCxxxx outline users; ac9 highlight manager; ac10/ac13 gui hover (0x12E7F70, 0x12EA790);
ac14 VM ctor; ac16 highlight DataComponent; ac17 op 1 wrapper and ECS callback; ac18 theft/ownership/pick-up refs;
ac19-ac21 StealingData, crime hook, inventory request processor; ac22-ac23 component getters and names; ac24-ac25
ToInventory and move core; ac26-ac30 system ids, pick-up updates, request pushers.
