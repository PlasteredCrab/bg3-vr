# Which items the gloves may pull or grab: pick-up flag, weight and lift limits (agent AD, 2026-10-23)

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_H, read-only, -noanalysis, headless request
server `agents\AD\ADServer.java`; disassembly with bg3vr_disasm). RVAs are relative to 0x140000000. Raw output is in
`re-local\ghidra\agents\AD\` (d1..d20 decompiles, x1..x12 xrefs, q1..q3 vtable dumps). The helper scripts are
`typename.py` (type-id global -> `GetTypeName<...>` name), `fnids.py` (getter -> component type) and `riprefs.py`.
The thoth scripts were extracted read-only from Shared.pak/Gustav.pak into `AD\gamedata\` (`CommonConditions.khn`).
Stats text is agent E's extract. Earlier decompiles of agent E (E/dec/*.c) are cited where used.
"Confirmed" = read in code at the cited address. "Unconfirmed" = not settled by the code; section 7 says how one run
settles it. The game was not launched.

## Fix for the VR mod

| what | RVA | how | risk | confidence |
|------|-----|-----|------|------------|
| Client "can be picked up" flag | `ecl::Item+0x18` bit **0x2** | `(*(u64*)(item+0x18) & 0x2) != 0` | none (plain read) | Confirmed. It mirrors the server `CanBePickedUp` 0x4 (server 0x426F5F0 -> msg bit 0x2 -> client 0x1FF25E0 case 0x44 -> 0x1FF3E60). The client PickUp task reads it through 0x1F5A030 mode 1. |
| Client "can be moved" flag (thoth `IsMovable`) | `ecl::Item+0x18` bit **0x4** | `(flags & 0x4) != 0` | none | Confirmed. It mirrors the server `CanBeMoved` 0x8 (same chain). The client thoth IsMovable 0x1BA80E0 reads it. The ItemMove tasks read it through 0x1F5A030 mode 2. |
| "Can go into an inventory" tag | **0xAB1DB0** `bool HasCanBeIn(EntityWorld* w, u64* handle)` | `0xAB1DB0(world, &h)` | low (read-only, game thread) | Confirmed. This is `HasComponent<eoc::inventory::CanBeInComponent>`. The thoth `CanPickup` and the client PickUp task require it. |
| Item weight in grams, as the game uses it for lifting | **0x41D6FE0** `int* Weight(int* out, Ctx* c, u64* handle, int amount, u8 oneUnit)` | `Ctx c = {world, world, *(u64*)(world+0x210), world+0x198, world+0x308}`. Pick-up rule: `0x41D6FE0(&w, &c, &h, 1, 1)`. Whole stack: pass the stack amount and `oneUnit = 0`, or call **0x1BA9130** `int GetLiftingWeight(u64* ctx /*[0]=world*/, u64 handle, u32 checkStacks)` with `checkStacks = 1` | low (pure reads) | Confirmed. Weight = `eoc::DataComponent.Weight` (grams) x amount, plus the socket items (recursive), plus `eoc::inventory::WeightComponent` (contents of containers). |
| Lift limit for picking up (game's own pick-up) | **0x1FAB030** `bool CanLift(ecl::Character* c, int* grams)` | `0x1FAB030(playerChar, &w)` | low | Confirmed. True when `grams <= roundf(Str * PickupObjectMultiplier * 1000)`. `PickupObjectMultiplier` = 5 in Data.txt, so the limit is Strength x 5 kg. Used by the PickUp task's error check 0x206E160 and by 0x1FAB150. |
| Drag / move limit | **0x1FAAF10** (same signature) | `0x1FAAF10(playerChar, &w)` | low | Confirmed. Same formula with `MoveObjectMultiplier` = 12 (Strength x 12 kg). Used by the client world drag code 0x1BCEBE0 and the thoth `CanMove` 0x1BA7D50. |
| Throw limit | game data only (thoth script) | `Str*Str*0.2 >= stackGrams/1000`, `Str = *(int*)(0xAF2FE0(world, playerHandle) + 8)` | low | Formula Confirmed (`CommonConditions.khn` CanThrowWeight). Strength at StatsComponent+8: Confirmed by use (0x1FAB030 / 0x1FAAF10 multiply it by the Strength multipliers), name from SE. |
| Remove the `actor_type != 5` filter in hold.cpp | `hold.cpp` aim_ray | Use the flag + weight rule below. Keep the actor type only in the log. | low | The server makes an item body static unless it is a door, movable or pickable, which it makes kinematic (type 5) (0x30C0D50, Confirmed). The client rule is Unconfirmed (7). Either way type 5 does not mean "light". |

Recommended check (game thread, the same place hold.cpp resolves the item):

```
world  = *(u64*)(*(u64*)(exe+0x5FDA240) + 0x1A8)       // client ECS world, as the PickUp task reads it (0x206E268)
item   = 0x704BD0(*(u64*)(exe+0x6007F68), h)            // as today; item+0x10 == h
flags  = *(u64*)(item+0x18)
pc     = 0x7841A0(world, playerHandle)                  // ecl::Character* of the player
inInv  = *(u64*)(item+0x60) != 0xFFC0000000000000       // parent set: in an inventory / attached
busy   = (flags & 0x100480100) != 0                     // 0x1F5A030 refuses any interaction (bits not named)
Ctx c  = {world, world, *(u64*)(world+0x210), world+0x198, world+0x308}
int unitG;  0x41D6FE0(&unitG, &c, &h, 1, 1)            // grams, one unit
grabbable = (flags & 0x2) && 0xAB1DB0(world,&h) && !inInv && !busy && 0x1FAB030(pc, &unitG)
pullable  = grabbable                                    // the gloves pull only what the hand may hold
// optional, for the throw on release:
u64 wc[1] = {world};  int stackG = 0x1BA9130(wc, h, 1)  // = thoth GetLiftingWeight(h, true)
str = *(int*)(0xAF2FE0(world, playerHandle) + 8)
throwable = ((flags & 0x4) || grabbable) && str*str*0.2f >= stackG/1000.0f
```

All calls are plain Microsoft x64 functions. Every one reads ECS storage without locks, so call them on the client
game thread only (hold.cpp's on_frame path, which already raycasts and calls 0x704BD0 there). Cost: a few hash
lookups per call, fine for a few items per frame. Cache the result per entity for a second to be safe.

## 1. The client pick-up rule (the game's own PickUp task) - Confirmed

PickUp task (type 18, ctor 0x206C020, vtable **0x56AF0D8**, q1): GetPriority +0x48 = 0x206C890, GetExecutePriority
+0x50 = **0x206C5D0**, GetError +0x68 = **0x206E160** (d7). Target = `task+0x260`, character = `task+0x68`,
world = `*(*(0x5FDA240)+0x1A8)`. 0x206C5D0 returns a priority only when all hold:

1. the target is an `ecl::Item` (0x793890) or `ecl::Character` (0x7841A0), it has no `eoc::DetachedComponent`
   (0xACEE90), and it **has `eoc::inventory::CanBeInComponent` (0xAB1DB0)**;
2. for an item: `*(u8*)(*(item+0x40)+0x108) == 0` (meaning Unconfirmed; E saw the same byte gate the physics
   suspend in 0x1FF3E60), **`item+0x60` (parent) is null**, and no entry of the list from `(*(item+0x40))->vf[0x100]`
   has type byte 0x0B at +0x28 (types 1,3,8,0xE,0xF,0x11,0x18,0x1C give priority 7 instead);
3. the target is not a character (0xAC5050), and **0x1F5A030(character, item, 0, 1, mode 1, 0, 0)** is true
   (disassembly 0x206C827..0x206C841). With mode 1 that function ends with `return (item+0x18 >> 1) & 1`, i.e.
   **client flag 0x2**, after its range/ownership/state gates (it refuses when `flags & 0x100480100`, or when the
   item's physics-suspend count `item+0x6A > 0` and arg 3 is 0).

GetError 0x206E160 then computes `w = 0x41D6FE0(&w, ctx, &target, amount = 0x1C793F0(item), oneUnit = 1)`
(disassembly 0x206E2B1..0x206E2F3) and sets error 0x800000 when **0x1FAB030(character, &w)** fails and error
0x200000000000 when **0x41D7140(world, &charHandle, w)** fails (carry capacity, section 3).

0x1FAB150 `(ecl::Character*, u64* item, u64* targetInventory)` is the same check bundled (d9): CanBeIn, inventory/camp
checks, then 0x41D6FE0 (one unit), 0x1FAB030, 0x41D7140 and the character's `eoc::CanInteractComponent` bit 0x10. It
includes the carry limit, so it is not the right call for "hold in the hand".

The thoth scripts see the same data: client thoth `CanPickup` (registration 0x1832135, closure 0x18359F0 -> native
0x1834D60 -> 0x18378A0 -> 0x183FCE0/0x183FFF0) is exactly `0xAB1DB0(world, &entity)` (d1, d2). The server thoth
`CanPickup` (0x2AB9E45 -> 0x2ABBD40 -> 0x2ABD2B0) ends in the same 0x183FFF0.

The UI view-model property `HasCanPickUpFlag` (registration 0x1454A5F) only returns a cached byte (`vm+0x860`,
getter 0x1456B60); its writer was not traced. Not needed.

## 2. The two item flags and where they come from - Confirmed

| server `esv::Item+0x18` | server packs into ItemUpdate (0x426F5F0, E/dec/itemsync.c 665-690) | client unpacks (0x1FF25E0 case 0x44, E/dec/proto_pm.c 372-376) | client applies (0x1FF3E60, E/dec/apply2.c 308-333) | `ecl::Item+0x18` |
|---|---|---|---|---|
| 0x8 CanBeMoved | changed-mask 0x10, value bit 0x4 | record byte0 bit 4 | `SetFlags(4)` / clear 0x4 | **0x4** |
| 0x4 CanBePickedUp | changed-mask 0x20, value bit 0x2 | record byte0 bit 5 | `SetFlags(2)` / clear 0x2 | **0x2** |

ItemCreate (0x3F) fills the same record bits from create byte +0x5A (`<<2`, proto_pm.c 52-62); the server side of the
create packing was not traced. The server Osiris `SetCanPickUp` 0x362F490 sets/clears server 0x4 (d4), so story
changes reach the client through this path.

Both thoth `IsMovable` implementations read the matching bit: client **0x1BA80E0** `item = 0x793890(world, e);
movable = item ? (item+0x18 >> 2) & 1 : 1` (d4; characters count as movable), server 0x3007500 reads server bit 0x8
(d13, reached from registration 0x2ABA14B -> 0x2ABBE60 -> 0x2ABD7D0 -> 0x2AC7120).

Who uses which bit through 0x1F5A030 (mode = byte at [rsp+0x20] of the call): mode 1 (flag 0x2) - PickUp task
0x206C5D0, 0x206C890, 0x206DE30; mode 2 (flag 0x4) - ItemMove task GetPriority 0x2066950 (vtable 0x56AEB98 +0x48),
ItemMoveDummy 0x425D840 / 0x425E480 (vtable 0x5919F70 +0x48 / +0x88); mode 3 - item use (0x1F7BFB0, 0x1F7E750,
0x206E560).

## 3. Weight - Confirmed

**0x41D6FE0** `int* (int* out, Ctx* c, u64* handle, int amount, u8 oneUnit)` (d5, prologue disassembly):
```
if handle invalid: *out = 0
w  = (oneUnit ? 1 : amount) * DataComponent.Weight         // 0xB083A0(world, h) -> eoc::DataComponent*, Weight int at +0
w += sum over eoc::use::SocketComponent entries (0xAF9670; 0x68-byte entries, handle at +0x60) of 0x41D6FE0(..., 1, 0)
if (no shapeshift-state flag at +0x9F (0xB04340)) and (not a character (0xAC5050) or has CanBeIn):
    w += inventory::WeightComponent.Weight                  // 0xB0D120(world, h) -> int* (contents of a container)
*out = w
```
Only `c[0]` (the world) is read by the function; pass all five qwords as the callers do (0x1FAB150, 0x206E160).
Units are grams: Data.txt `ItemWeightLight 100`, `ItemWeightMedium 1000`, `ItemWeightHeavy 50000`, the scripts divide
by 1000 and use 500/10000/50000 as light/medium/heavy thrown-object bounds.

**0x1BA9130** `int GetLiftingWeight(u64* ctx, u64 handle, u32 checkStacks)` = thoth `GetLiftingWeight`
(registration 0x18327C2 -> 0x1835B90 -> 0x1834FD0 -> 0x1838210 -> 0x1839020 / 0x1844A60, d1/d3). With a valid
handle it reads only `ctx[0]` (world). It takes the amount from `eoc::inventory::StackComponent` (0xB0E030, sum of the
+4 ints of its entries) or from the stack an item is a member of (`StackMemberComponent` 0xB0E230), else 1, and calls
0x41D6FE0 with `oneUnit = !checkStacks`.

Getters used above (all `T* f(EntityWorld*, u64 handle)`, null when missing; type from the type-id global they read,
resolved with typename.py):

| getter | component | type-id global |
|---|---|---|
| 0xAB1DB0 (bool, `u64*` handle) | eoc::inventory::CanBeInComponent | 0x607D36C |
| 0xB083A0 | eoc::DataComponent (Weight +0, SE: StatsId +4, StepsType +8) | 0x607D434 |
| 0xB0D120 | eoc::inventory::WeightComponent (int Weight +0) | 0x607D398 |
| 0xAF2FE0 | eoc::StatsComponent (Strength = int +8) | 0x607D6D8 |
| 0xB22920 | eoc::encumbrance::StatsComponent (int +8 used as the carry limit) | 0x607D0DC |
| 0xB0E030 / 0xB0E230 | inventory::StackComponent / StackMemberComponent | 0x607D378 / 0x607D374 |
| 0x793890 / 0x7841A0 | ecl::Item / ecl::Character | 0x60531B4 / 0x6052E3C |

## 4. How the game decides "too heavy" - Confirmed

| rule | code | formula |
|---|---|---|
| pick up into the hand/inventory | 0x1FAB030 (callers: PickUp 0x206E160, 0x1FAB150, 0x13F0500, 0x15A3BD0, 0x1FCF6A0, 0x1FD1760, 0x1FD51D0, 0x1FD68F0) | `grams <= roundf(Str * Data[PickupObjectMultiplier] * 1000)` |
| move / drag in the world | 0x1FAAF10 (callers 0x1BCEBE0, 0x1FCF6A0, 0x1FD1760, 0x1FD51D0) and thoth `CanMove` 0x1BA7D50 (registration 0x18323BA -> 0x1835A90 -> 0x1834E50 -> 0x1837CD0 -> 0x1841FB0) | `grams <= roundf(Str * Data[MoveObjectMultiplier] * 1000)` |
| carry capacity | 0x41D7140 `(world, u64* char, int grams)` | `EncumbranceStats(+8) - WeightComponent(char) >= grams` (0 when the character has no WeightComponent) |
| throw (spell `Throw_Throw`, `ThrowableTargetConditions`) | thoth script `CanThrowWeight()` | `Str*Str*0.2 >= GetLiftingWeight(target, not IsInInventory(target)) / 1000` |
| shove | `CanShoveWeight()` | `Str * 12 (ENLARGE 28.2, REDUCE 5.1) >= GetLiftingWeight(target)/1000` |
| improvised weapon | `CanImprovisedWeaponWeight()` | same as throw |

`Str` = `StatsComponent+8`; 0x1FAB030 first asks 0x1106580(world, out, &h) (same StatsComponent type id, likely a
cached/overridden copy) and falls back to 0xAF2FE0. The multiplier is read with
`0xFEF8F0(*(*(u64*)(exe+0x5FFE828)+0x2A8), key)` (1.0 when the table is missing); the key globals are written by
the Data.txt key initialisers: `PickupObjectMultiplier` -> **0x5FC2128** (0x192080), `MoveObjectMultiplier` ->
**0x5FC20D8** (0x191860), `ThrowWeightMultiplier` -> 0x5FC2170 (0x192730; its only reader is 0x104D4D0, not
traced). Data.txt values: PickupObjectMultiplier 5, MoveObjectMultiplier 12, ThrowWeightMultiplier 2,
CarryLimitMultiplier 10, CarryWeightPerStr 10000.

Examples (Strength 10 / 16): hand pick-up 50 / 80 kg, drag 120 / 192 kg, throw 20 / 51.2 kg.
The script `CanPickUpWeight()` (Str*Str*0.2) exists in CommonConditions.khn but no stats entry uses it; the native
pick-up uses the Strength x 5 rule above.

## 5. Physics: actor type and mass

* Actor types (agent D, Confirmed there): `*(u16*)(actor+8)`: 5 PxRigidDynamic (dynamic **or kinematic**), 6
  PxRigidStatic, 13 articulation link. Kinematic test: wrapper vf +0xD0 IsKinematic 0x3A7D3D0 (or the actor's
  rigid-body flags, vf +0x1A8, `& 1` = eKINEMATIC). Mass: PxRigidDynamic vf +0xE8 getMass (template mass x scale, D 4.1).
  No game rule found reads the physics mass for pick-up, drag or throw; all of them use DataComponent weight.
* Server item bodies (Confirmed, 0x30C0D50, called from server SetFlags 0x30BC760 when a flag in 0x800E changes):
  `if ((flags & 0x800C) == 0) container MakeStatic (0x3C96240) else MakeKinematic (0x3C96280)`, i.e. doors,
  movable and pickable items are kinematic (type 5), everything else static (type 6).
* Client item bodies: Unconfirmed. The only client-side MakeStatic/MakeKinematic call is the generic physics loader
  0x3F7EE10 (request byte +0x34: 1 static, 2 kinematic); its producer was not traced. 0x2D6D200/0x2D6D670 (D's
  "static item bodies") are only reached from server code (0x307E0A0, 0x307E210).
* Consequence for hold.cpp: `actor_type == 5` is at best "movable or pickable or door" (if the client copies the
  server rule) and says nothing about weight; a chest or barrel that is movable would pass it. Use section 0.

## 6. Containers and chests

* A container's contents weigh in through `inventory::WeightComponent` (0x41D6FE0), so a full backpack is heavy.
* No separate "is a chest" test is needed for the gloves: the game's pick-up gate is flag 0x2 + CanBeIn + weight.
  Whether chests clear flag 0x2 / lack CanBeIn: Unconfirmed (7). The anubis `Item.GetIsContainer` and server flag
  `IsContainer` exist; their client mirror was not traced.

## 7. One-run logging plan

Log once per entity (first time any aim ray of either hand hits it, before any filtering), on the game thread:

```
h, item = 0x704BD0(...), item+0x10, flags = *(u64*)(item+0x18) (hex; print bits 0x2, 0x4, 0x100480100 separately),
parent = *(u64*)(item+0x60), physByte = item+0x40 ? *(u8*)(*(u64*)(item+0x40)+0x108) : -1, suspend = *(i8*)(item+0x6A)
worldA = *(u64*)(*(u64*)(exe+0x5FDA240)+0x1A8); worldB = camera ctrl+0x320; worldC = *(u64*)(exe+0x6007F68)
         -> print all three and whether A == B, A == C
canBeIn   = 0xAB1DB0(worldA, &h)
dataW     = p = 0xB083A0(worldA, h) ? *(int*)p : -1 ;  invW = p = 0xB0D120(worldA, h) ? *(int*)p : -1
unitG     = 0x41D6FE0(&x, ctx, &h, 1, 1)
stackG    = 0x1BA9130(&worldA, h, 1)
pc = 0x7841A0(worldA, playerHandle); str = *(int*)(0xAF2FE0(worldA, playerHandle)+8)
lift = 0x1FAB030(pc, &unitG); drag = 0x1FAAF10(pc, &stackG); carry = 0x41D7140(worldA, &playerHandle, unitG)
throwOK = str*str*0.2 >= stackG/1000
physics: actor type (u16 at actor+8), wrapper vf+0xD0 IsKinematic, vf+0xD8 IsStatic, mass = vf+0xE8 if type 5
```

Test objects in one area (e.g. the Nautiloid or the first beach): a chest, a barrel, a crate, a cup or bottle, a
sword, a book, a door, a backpack/pouch, a stack (arrows or gold), and a corpse (expect item = 0). This settles:
(a) flag 0x2/0x4 and CanBeIn per object class, (b) gram values (cup ~100-500, sword ~1000-3000, barrel tens of kg),
(c) the client actor type of movable vs fixed items, (d) which world pointer is valid (the skeleton code already saw
0x6007F68 hold a bad value once), (e) whether `physByte` is non-zero for items the game will not let you pick up.

## 8. Function index (new)

| RVA | what |
|---|---|
| 0xAB1DB0 | HasComponent<inventory::CanBeInComponent>(world, &h) |
| 0x41D6FE0 | entity lifting weight (grams) |
| 0x1BA9130 | thoth GetLiftingWeight core (amount from the stack) |
| 0x1FAB030 / 0x1FAAF10 | weight <= Str x PickupObjectMultiplier / MoveObjectMultiplier |
| 0x41D7140 | carry-capacity check |
| 0x1FAB150 | client "can pick up into inventory" (all checks) |
| 0x1F5A030 | client "character may interact with item" (mode byte: 1 pick up -> flag 0x2, 2 move -> flag 0x4, 3 use, 4 other) |
| 0x1BA80E0 / 0x1BA7D50 | client thoth IsMovable / CanMove |
| 0x1834D60 / 0x1834E80 / 0x1834E50 / 0x1834FD0 | client thoth natives CanPickup / IsMovable / CanMove / GetLiftingWeight |
| 0x3007500 | server thoth IsMovable (server flag 0x8) |
| 0x206C5D0 / 0x206C890 / 0x206E160 | PickUp task exec priority / priority / error |
| 0x2066950 | ItemMove task priority (mode 2) |
| 0x1BCEBE0 | client world drag update (weight via 0x1FAAF10) |
| 0x30C0D50 | server: item body static vs kinematic from flags 0x800C |
| 0xFEF8F0 | Data.txt value lookup (float) |
| 0x5FC2128 / 0x5FC20D8 / 0x5FC2170 | FixedString keys PickupObjectMultiplier / MoveObjectMultiplier / ThrowWeightMultiplier |
