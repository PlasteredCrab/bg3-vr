# BG3 items, inventory, interaction, throwing and client/server authority (agent E, static RE)

Build: bg3_dx11.exe 4.1.1.7631656, image base 0x140000000; every address below is an RVA.
Method: Ghidra headless on project **BG3_B** (read-only) through a query server
(`agents/E/scripts/EServer.java`, a copy of agent B's BServer plus a `before <rva> <n>` command),
`names.sh` (type-id global → `ls::GetTypeName<…>` name) and `typerefs.py` (type name → type-id
globals → functions using them). Raw outputs: `agents/E/dec/*.c` (decompiles), `agents/E/req/*.out`
(query results), `agents/E/strings_all.txt` (all exe strings with RVAs), `agents/E/osiris_reg_all.txt`
(Osiris name → handler table), `agents/E/netmsg_ids.txt` (message id table), `agents/E/msg_senders.txt`
(every `GetFreeMessage(id)` call site), `agents/E/typerefs_*.txt`, `agents/E/gamedata/` (stats .txt
extracted read-only from Shared.pak / Gustav.pak with LSLib Divine).

Conventions: **confirmed** = read in decompiled code/disassembly of this build (RVA given).
**SE** = taken from the Script Extender source in `downloads/bg3se-src` (a different, newer-or-older
build; layout not re-verified here unless stated). **Unconfirmed** = hypothesis; the "how to confirm"
is given. No game run was made.

Agents: physics engine / raycasts / ragdolls = agent D; animation / skeleton / input = agent C;
player manager / message 0x23 = agent B (`B_splitscreen.md`); rendering = agent A.

---------------------------------------------------------------------------------------------------

## 0. Short answer

### 0.1 Authority model (confirmed)

* The game always runs a server (esv) and a client (ecl) world, also in single player; they talk
  through `eocnet::*Message` objects (ids in §4). Item state lives in **both** a legacy object
  (`esv::Item` / `ecl::Item`, flags at +0x18) and ECS components (`ls::TransformComponent` etc.).
* **The server is authoritative in general, but item transforms have an explicit client → server
  path that the server accepts without an ownership check:** the client's `ecl::ItemProtocol`
  post-update (0x1FF37D0) sends `ItemUpdateMessage` (0x44) for every active client item whose
  client flag bit 18 (0x40000) is set, carrying the client item's translate + rotate (0x1FF3C10).
  The server's `esv::ItemProtocol::ProcessMsg` (0x426DC40, case 0x44) looks the item up and calls
  `SetTranslate` 0x30BCE40, `SetRotate` 0x30BD0C0 and `SetFlags(0x400 TransformChanged)` 0x30BC760.
  The only checks are that the handle resolves to an `esv::Item`. Physics on the server item is
  suspended around the write (0x30C64F0 / 0x30C6610) when `item+0x48 … +0x108` is set.
* The same server sends item transforms back down (`ItemUpdateMessage` 0x44 built in 0x426F5F0,
  transform included when the server item has flag 0x400) and the client stores them as *pending*
  records (`ecl::ItemProtocol` 0x1FF25E0 case 0x44) that are applied once per frame by 0x1FF3E60
  (client physics suspend 0x1C7DD00 → SetTranslate/SetRotate → resume 0x1C7DE60); so a client-side
  write is only overwritten when the server itself moves the item.
* Characters are different: the server's `CharacterProtocol` accepts client steering/updates only from
  the character's controlling peer (case 0x24 compares the peer id, else `CharacterErrorMessage` 0x69).
* The client itself uses that path today: the client item update loop (0x1CCD960) runs a per-item
  settle routine (0x1C7BD70) that raycasts down, re-orients the item to the ground normal
  (0x1C7BFF0 → client `SetRotate` 0x1C77130) and then raises bit 0x40000 so the result is reported
  to the server. Client items can also have dynamic physics (bit 45, 0x200000000000; 0x1FC46E0).
* Two more client → server item requests carry an explicit world position and rotation:
  `ItemMoveToWorldMessage` 0x46 (server calls the same "move to world" core 0x35FAA60 that
  `ItemMoveToPosition` uses) and `CharacterDropEntityMessage` 0x114 (→ `esv::drop::DropEntityRequestSystem`).

### 0.2 Recommended approach for hand-held items (design, based on the confirmed paths)

1. **While held (every frame, client thread):** do what the client itself does when it applies a
   server transform (0x1FF3E60): on grab call the client **physics suspend 0x1C7DD00(ecl::Item*)**
   (refcounted, sets physics-object flag 0x100000 and resets the item's settle state), then every
   frame write the pose with the game's client setters **`SetTranslate` 0x1C77050(ecl::Item*, vec3*)**
   and **`SetRotate` 0x1C77130(ecl::Item*, quat*)** (both → `ls::TransformSystem` 0x3C77BD0 /
   0x3C77D20, which update `ls::TransformComponent` and fire its change listeners), and on release
   call **resume 0x1C7DE60**. Do **not** set bit 0x40000 every frame (it would stream one
   `ItemUpdateMessage` per frame). Keep the server item still (no server-side mover active) so nothing
   overwrites the client value. What physics-object flag 0x100000 means inside phx and how the PhysX
   actor follows the transform belong to agent D (unconfirmed here).
2. **On release without a throw:** either (a) set the final client transform and raise client flag
   0x40000 through `ecl::Item::SetFlags` 0x1C76D50(item, 0x40000) so the next client
   `ItemProtocol` post-update sends the authoritative position (cheapest, same path the game uses for
   settled items), or (b) from Lua on the server: `Osi.TeleportToPosition(item, x,y,z, "", 0,0,0,0,0)`
   (handler 0x36740A0 → item path 0x35F2740) or `Osi.ItemMoveToPosition(item,x,y,z,speed,accel,event)`
   (0x3626A20 → 0x35FAF90 → `esv::ItemMover` 0x36EEC00).
   Note: resume 0x1C7DE60 flags the item for a settle re-check (`+0x3C |= 4`). The settle routine
   0x1C7BD70 then either starts the item state-machine state 0xF with the current position (when the
   dynamic-physics test 0x1FC46E0 succeeds — candidate "falling" state, **unconfirmed**) or, if the
   item is marked `+0x3C&2`, re-orients it to the ground normal (0x1C7BFF0, rotation only) and reports
   it with bit 0x40000. Whether a release in mid-air makes the item fall by itself must be checked in
   game.
3. **Throw:** BG3's own throw is a spell (`Throw_Throw`, `SpellType Throw`, §9) that flies an
   `esv::Projectile` with the thrown entity attached as its *moving object* (`proj+0x4E8`, attach
   0x2AE3AD0, per-update move 0x2AE2B20, landing/ground snap 0x2ADE7A0 — confirmed) and resolves damage
   with the spell's `SpellSuccess` functors. A physical VR throw can (a) fly the object itself
   client-side and on landing place it (2a/2b) and apply effects with `Osi.ApplyDamage` (0x36A8B40) /
   statuses, or (b) cast the game's throw through the spell system (`UseSpell*` → 0x3671750) — how to
   pass the thrown object and destination was not traced (open item §14.4).
4. **Bodies:** a dead character stays an `esv::Character` (the throw rules address it with
   `Dead()`); it is moved with the character branch of `TeleportTo/TeleportToPosition` (0x35F09A0)
   and is a valid target of `Throw_Throw` (`(IsMovable() or Character() or Dead())`, §10). Ragdoll
   physics = agent D.
5. **Using things with the hand:** the server-side entry that the game's remote-use message ends in is
   `esv::item::UseSystem` (type id DAT_1460D95F0): `UseItem core` 0x35E6820 + `UseSystem::AddRequest`
   0x33D8D30 / 0x33D8DB0 (§8). From Lua: `Osi.Use(char, item, …)` queues an Osiris task (walks the
   character); `Osi.Open/Close(item)` drive doors/containers directly; `Osi.Pickup`, `Osi.Equip`,
   `Osi.ToInventory` for inventory (§6).

---------------------------------------------------------------------------------------------------

## 1. Objects and globals

| Global / helper | Meaning | Evidence |
|---|---|---|
| `DAT_145FF03C0` | esv server object; `+0x2B0` = server ECS world (used everywhere as `*(DAT_145ff03c0+0x2b0)`), `+0xA8` = net server (`+0x430` = handle remapper used by protocols), `+0x2C8` passed to use checks | 0x30BCE40, 0x426DC40, 0x306BBB0 |
| `DAT_145FDA240` | ecl client (EoCClient): `+0x1A8` client ECS world, `+0x98` game state / net client (`+8` → net peer, `+0x238` message factory, `+0x418` peer id) | 0x1FF37D0, 0x1BF74E0, agent B |
| `DAT_146007A28` | ecl level manager: `+0x90` current level (`level+0x30` = physics scene used for raycasts); `level+0x90` = client item manager object (`+0x68` client item mover, `+0x80` active-item snapshot, count `+0x8C`; `+0x90` move-to-world request list, count `+0x9C`, 0x50-byte entries). Whether that object is the ECS system instance `ecl::ItemManager` (DAT_146051B64) is **unconfirmed** | 0x1FF37D0, 0x1FF3E60, 0x1C7BFF0 |
| `DAT_14603B2B0` | esv level manager (`+0x90` current level; `level+0x18` → level desc, `+0xA0` → use/... managers, `+0xA0 → +0x88` item mover) | 0x35FAF90, 0x3625FE0, 0x426DC40 |
| `DAT_14603B360` | esv entity helper: `FUN_14281A1E0(*DAT_14603B360, handle)` → `esv::Item*`; `FUN_1428198F0(…)` → `esv::Character*`; `[1]` = net-handle remapper | 0x2F60BA0, 0x2F609A0, 0x426DC40 |
| `DAT_146007F68` | ecl entity helper: `FUN_140704BD0(*p, handle)` → `ecl::Item*`; `FUN_140702C40(*p, handle)` → game object used for parents/targets (type **unconfirmed**); `[1]` = net-handle remapper (0x4014CB0 / 0x4014C00). With the client world as first argument: `FUN_140793890(world, h)` → `ecl::Item*` (its result is passed to ecl::Item methods 0x1C793F0 / 0x1FC46E0), `FUN_1407841A0(world, h)` → `ecl::Character*` (result used with the user's peer id at +300 in 0x159DB00) | 0x1FF25E0, 0x1FF37D0, 0x18937F0, 0x206ACC0 |
| ECS system table | `*(world+0x30) + typeId*0xF8` → system instance (pattern used for every `DAT_146xxxxx * 0xf8`) | all |
| Message factory | `GetFreeMessage(factory, id)` = 0x40395F0; send (client→server) `0x4031600(peer, peerId, msg)`; broadcast (server) `0x40317A0(peer, list, msg, -1)`; release unused `0x40396B0` | 0x1FF37D0, 0x35E6700 |

ECS type ids resolved for this report (all with `names.sh`, i.e. from their `GetTypeName` initialiser):

| Id global | Name |
|---|---|
| DAT_146058CBC | ls::TransformSystem |
| DAT_146051798 | esv::ItemManager |
| DAT_146051B64 | ecl::ItemManager |
| DAT_1460F0050 / DAT_1460F00C0 | esv::Character / esv::Item (object type ids returned by vtable+0x40 of a game object) |
| DAT_1460516D8 | esv::inventory::EquipmentSystem |
| DAT_146051680 | esv::inventory::InteractionRequestSystem |
| DAT_1460D9758 | esv::inventory::ManagementSystem |
| DAT_1460D9770 | esv::inventory::StackSystem |
| DAT_1460D9794 | esv::inventory::LockingSystem |
| DAT_1460D97A4 | esv::inventory::EntityReceivalNotificationSystem |
| DAT_1460D96B8 | esv::drop::DropEntityRequestSystem |
| DAT_1460D9948 | esv::pickup::PickUpLegacyRequestProxySystem |
| DAT_1460D95F0 | esv::item::UseSystem |
| DAT_1460D98C0 | esv::item::animation::RequestsSystem |
| DAT_1460D9750 | esv::GravitySystem |
| DAT_1460D9674 | esv::invisibility::RevealingSystem |
| DAT_1460D92F8 | esv::GameActionManager |
| DAT_1460D9380 | esv::DisplayNameSystem |
| DAT_146050310 | esv::combat::System |
| DAT_146058FCC | ecl::item::BoundsSystem |
| DAT_146058FD4 | ecl::inventory::ProtocolRequestSystem |
| DAT_146058FE8 | ecl::container::LEGACY_ContainerExplorationSystem |
| DAT_1460591A8 | ecl::hotbar::System |
| DAT_146058D24 | ecl::GameActionManager |
| DAT_146058CEC | phx::PhysXScene (client) |
| DAT_146058F94 | ecl::PickingHelperManager |
| DAT_1460D96F4 / D96F8 | esv::thrown::ThrowAnimationSystem / AttachmentSystem |
| DAT_146059168 / 916C / 9170 | ecl::thrown::ThrowAnimationSystem / AttachmentSystem / SoundSystem |

---------------------------------------------------------------------------------------------------

## 2. Item objects

### 2.1 esv::Item (server legacy object) — fields confirmed by use

| Offset | Meaning | Evidence |
|---|---|---|
| +0x00 | vtable | game-object calls below |
| +0x10 | EntityHandle (u64) | used as the item handle everywhere (0x30BCE40, 0x35E6700) |
| +0x18 | ItemFlags u64 (`SetFlags` 0x30BC760 ORs, `ClearFlags` 0x30BCB40 clears; both fire side effects and notify listeners in `DAT_14603CB50`) | 0x30BC760 |
| +0x20 / +0x28 | EntityRef (handle, world) passed to `ls::TransformSystem` setters | 0x30BCE40, 0x30BD0C0 |
| +0x30 | level id (compared with current level `*(*(level+0x18))+8` in the mover) | 0x36EEC00 |
| +0x48 | sub-object; `+0x108` byte = "physics present" test before suspend/resume; `+0x270` used by flag 0x4000000 | 0x426DC40, 0x30BCB40 |
| +0x58 | item state machine (`FUN_1436E7190(machine, stateId)` creates a state; ItemRotateY uses state 0xE; Open/Close use states 0xD,0xB,0xC,0xA; `+0x38` current state) | 0x3626D60, 0x362EC00 |
| +0x90 | **user character** EntityHandle: written only by `esv::Item::SetUser` 0x30C5D90(item, charHandle), which also sets/clears bit 0x4 of the character's byte `+0x1A5`. Callers: the use code around 0x33A9560 / 0x33AAEA0 (next to the remote-use handler 0x33A47F0), legacy action code 0x41BCC00 / 0x41BD080, 0x307FC70, 0x3086E60, 0x3076D80, 0x3078930/0x3078F60, 0x372F9F0, and the item protocol (cleared on parent change). While set: `MoveToWorld` 0x35FAA60 returns 5, `ItemMoveToPosition` does not move the item, `UseItem` 0x35E6820 fails for any other character | 0x30C5D90, 0x35FAA60, 0x35FAF90, 0x35E6820 |
| +0xA8 | signed byte: physics-suspend refcount (`0x30C64F0` ++, `0x30C6610` --) and mover count (teleport stops the mover when >0) | 0x30C64F0, 0x35F2740, 0x3625FE0 |
| +0x120 (character) | Osiris task controller of an `esv::Character` (`FUN_1435F4DB0(+0x120, task, 0)` queues) | 0x364F240, 0x364F100 |

SE lists the same object as `esv::Item { VMT, VMT2, field_10, Flags(+0x18), MyHandle(+0x20 EntityRef),
Level, ItemType, UpdatePeerIds, Template, OriginalTemplate, ItemMachine(+0x58), … }` (SE `Item.h`);
the +0x18 / +0x20 / +0x58 offsets agree with the code above.

Server flag bits seen in code (names from SE `ServerItemFlags`; the *use* is confirmed):

| Bit | SE name | Confirmed use |
|---|---|---|
| 0x4 | CanBePickedUp | `SetCanPickUp` 0x362F490 sets/clears it; `Pickup` 0x364EFB0 tests it |
| 0x400 | TransformChanged | set after every accepted transform write (0x426DC40); server `ItemUpdateMessage` includes translate+rotate when set (0x426F5F0, test `flags>>10&1`) |
| 0x8000 | IsDoor | `Open` 0x362EC00 branches on it |
| 0x40000 | ClientSync1 | set by the server mover when it teleports an item off-level (0x36EEC00) and by physics suspend 0x30C64F0 |
| 0x80000 | WakePhysics | set/cleared by several systems (0x2ADE7A0, 0x36EE550) |
| 0x4000000 | (SE: Known) | set in `UseItem` 0x35E6820 |
| 0x800000000000 | (SE: InPartyInventory) | client `ItemUpdate` bit 13 of word +4 clears it (0x426DC40) |
| 0x200000000 | (SE: DontAddToHotbar) | cleared by MoveToWorld 0x35FAA60 |

### 2.2 ecl::Item (client legacy object) — fields confirmed by use

| Offset | Meaning | Evidence |
|---|---|---|
| +0x10 | EntityHandle | 0x1FF3C10 |
| +0x18 | client ItemFlags u64 (`ecl::Item::SetFlags` 0x1C76D50) | 0x1C76D50 |
| +0x20 / +0x28 | EntityRef → `ls::TransformSystem` / physics: `GetPhysicsObject` 0x1C785D0 = first qword of the entity's **`ls::PhysicsComponent`** (component getter 0x1857C80, type id DAT_146080BF0; SE: `phx::PhysicsObject* Physics`). The same helper is called with `esv::Item*` on the server (0x30C64F0), so the EntityRef sits at +0x20 in both classes | 0x1C77050, 0x1C785D0, 0x1857C80 |
| +0x30 | float settle timer (accumulates dt in 0x1C7BD70; ≥ 5.0 → 0x1C7B3A0 stops) | 0x1C7BD70 |
| +0x3C | byte: bit0 "settled/at rest", bit1 "needs ground snap", bit2 "re-check after resume" | 0x1C7BD70, 0x1C7BFB0, 0x1C7DE60 |
| +0x40 | sub-object; `+0x108` byte tested before the client suspends physics around a server transform write | 0x1FF3E60 |
| +0x48 | item state machine (state 0xF compared in 0x1C7BD70 / ItemAction) | 0x1C7BD70, 0x1FF25E0 |
| +0x60 | parent EntityHandle (set from S2C ItemUpdate `flags&2`) | 0x1FF3E60, 0x1FF3C10 |
| +0x6A | signed byte: client physics-suspend refcount (0x1C7DD00 ++, 0x1C7DE60 --) | 0x1C7DD00 |
| vtable +0xD0 | GetTransform → struct with rotation quat at +0x00 and translate at +0x10 (same layout as `ls::TransformComponent`) | 0x1FF3C10, 0x1C7BFF0 |

Client flag bits (meaning confirmed by code, names mine):

| Bit | Mask | Behaviour |
|---|---|---|
| 18 | 0x40000 | "report transform": the next `ecl::ItemProtocol` post-update sends `ItemUpdate` (flags \|= 1, translate+rotate) and clears the bit (0x1FF3C10) |
| 19 | 0x80000 | passed to the dynamic-physics routine (0x1FC46E0) |
| 22 | 0x400000 | set/cleared by S2C ItemActivate/ItemAction handling (0x1FF25E0) |
| 38 | 0x4000000000 | mirrored into ItemUpdate word bit 0x2000 (0x1FF3C10) |
| 43 | 0x80000000000 | set together with a call to 0x1C7E010 whenever bit 18 or 0x4000041000 is raised (0x1C76D50) |
| 45 | 0x200000000000 | item uses client dynamic physics (0x1FC46E0) |
| 20 | 0x100000 | set by the client physics suspend 0x1C7DD00 (together with physics-object flag 0x100000) |
| 10 / 46 | 0x400 / 0x400000000000 | either bit makes the client item update call 0x1BB7C40, which re-poses the item's physics shapes from its transform (physics object vtbl+0x70, per-shape vtbl+8) |

### 2.3a Common game-object vtable (esv::Item / esv::Character / other objects; slots confirmed by use)

| Slot | Use | Evidence |
|---|---|---|
| +0x10 | flags getter (TeleportToPosition tests `& 0x900000` for "other" objects) | 0x36740A0 |
| +0x28 | called before a generic teleport | 0x36740A0 |
| +0x40 | object type id (compared with `esv::Character` DAT_1460F0050 / `esv::Item` DAT_1460F00C0) | 0x36740A0 |
| +0x50 | get EntityHandle (out param) | 0x3673DC0, 0x368A110 |
| +0xB8 | get translate (vec3*) | 0x36266C0, 0x3626600 |
| +0xC0 | get rotation (quat*) | 0x36266C0, 0x36740A0 |
| +0xD0 | get transform (rotation +0x00, translate +0x10) | 0x35FAF90, 0x426F5F0 |
| +0xD8 | set translate | 0x36740A0, 0x2AE3AD0 |
| +0xE0 | set rotation | 0x2AE3AD0 |
| +0x100 | level id pointer | 0x36740A0, 0x33A47F0 |

### 2.3 ECS components relevant to items (names from `GetTypeName` strings; SE layouts noted)

`eoc::item::ItemComponent` (tag), `eoc::item::DoorComponent`, `eoc::item::HasMovedComponent`,
`eoc::item::InUseComponent`, `eoc::item::ISFallingComponent`, `eoc::item::ISRotateComponent`,
`eoc::item::ISTeleportFallComponent`, `eoc::item::ISDoor*Component`, `eoc::item_template::CanMoveComponent`,
`eoc::item_template::UseActionComponent`, `eoc::item_template::InteractionDisabledComponent`,
`eoc::inventory::{Data,Owner,IsOwned,TopOwner,Container,Member,Stack,StackMember,Weight,Wielded,MemberTransform,CanBeIn,CannotBeTakenOut}Component`,
`eoc::pickup::{PickUpRequest,PickUpExecuting}Component`, `eoc::drop::{Animation,OriginalDropTarget,DropEntityState}Component`,
`eoc::thrown::{IsThrown,Attach,RotationParameters,ThrownAnimationRequestOneFrame}Component`,
`eoc::improvised_weapon::{Wielded,Wielding,CanBeWielded}Component`, `eoc::projectile::SourceInfoComponent`,
`eoc::death::{Death,State,Downed,DeathType,DeadByDefault}Component`, `eoc::CanInteractComponent`,
`eoc::ObjectInteractionComponent`, `eoc::GravityDisabledUntilMovedComponent`,
`ls::TransformComponent` (SE: `{ Transform }` = rotation quat +0x00, translate +0x10, scale; the +0x00/+0x10
offsets are confirmed by `TransformSystem` setters 0x3C77BD0 / 0x3C77D20),
`ls::PhysicsComponent` (SE: `phx::PhysicsObject* Physics; PhysicsGroup; CollidesWith; ExtraFlags; HasPhysics; IsClustered`),
`ecl::item::RetryLinkDynPhysicsRequestComponent`, `ecl::death::ReloadDynamicPhysicsVisualRequestOneFrameComponent`.

---------------------------------------------------------------------------------------------------

## 3. Writing a transform

| Function | Signature (as used) | Notes |
|---|---|---|
| 0x3C77BD0 | `TransformSystem::SetTranslate(sys, EntityRef*, vec3*, bool notify, bool recordPrev)` | no-op if |Δ| < FLT_EPSILON on all axes; writes `TransformComponent+0x10`; when `notify`, calls listeners at `sys+0x18` (count `+0x24`, 0x48-byte functors) with (EntityRef, old pos); `recordPrev` → 0x3C78980 |
| 0x3C77D20 | `TransformSystem::SetRotate(sys, EntityRef*, quat*, notify, recordPrev)` | writes `+0x00`; listeners at `sys+0x30` (count `+0x3C`) |
| 0x3C79010 | get mutable TransformComponent for an EntityRef | called by both |
| 0x30BCE40 | `esv::Item::SetTranslate(item, vec3*, EntityHandle)` | stores the handle into the `esv::ItemManager` per-item record (0x33A7C70) then calls 0x3C77BD0(…,1,1) |
| 0x30BD0C0 | `esv::Item::SetRotate(item, quat*)` | 0x3C77D20(…,1,1) |
| 0x1C77050 | `ecl::Item::SetTranslate(item, vec3*)` | client world, 0x3C77BD0(…,1,1) |
| 0x1C77130 | `ecl::Item::SetRotate(item, quat*)` | 0x3C77D20(…,1,1) |
| 0x30BC760 / 0x30BCB40 | `esv::Item::SetFlags / ClearFlags(item, mask)` | side effects per mask (activation 0x2, 0x800E, 0x40180000010012, 0x50000000000B10 …) and change listeners |
| 0x1C76D50 | `ecl::Item::SetFlags(item, mask)` | 0x40000 → (if bit 43 clear) also sets 0x80000000000 and calls 0x1C7E010 |
| 0x30C64F0 / 0x30C6610 | server: suspend / resume item physics (refcount `+0xA8`; sets 0x100000 on the phys object flags via vtable+0x30; sets server flag 0x40000) | used around client-reported writes |
| 0x1C7DD00 / 0x1C7DE60 | **client: suspend / resume item physics** (refcount `+0x6A`; on first suspend: stops the settle timer, sets physics-object flag 0x100000 (vtbl+0x30) and client flag 0x100000, bumps a counter at `+0x7A` and sets bit 0x2000 at `+0x8A` of the object returned by 0x0E2CC10(world, entity); resume reverses it and marks the item for a settle re-check `+0x3C|=4`) | the client wraps every server-driven transform write in this pair (0x1FF3E60) |
| 0x1BB7C40 | client: push the item transform into its physics shapes | called from the item update when flag 0x400 / 0x400000000000 is set |

---------------------------------------------------------------------------------------------------

## 4. Network messages

### 4.1 Ids (registration function 0x119A320 → `RegisterMessage` 0x4039250(factory, id, msg, channel, name))

Item / inventory / interaction subset (full table in `agents/E/netmsg_ids.txt`):

| Id | Message | Dir | Serialize (vtbl+8) | Payload (confirmed from Serialize + handlers) |
|---|---|---|---|---|
| 0x38 | CharacterItemUsedMessage | S→C | – | {+0x28 user, +0x30 item, +0x38 bool success} (built in 0x35E6700) |
| 0x3F | ItemCreateMessage | S→C | – | – |
| 0x41 | ItemActivateMessage | S→C | – | client sets translate/rotate (0x1C77050 / 0x1C77130) on activation (0x1FF25E0) |
| 0x43 | ItemDestinationMessage | S→C | 0x11D8A40 | array of 0x78 entries: quat +0x00, vec3 +0x10, u32 +0x1C, handle +0x20, path/movement settings +0x28 (0x1154990) — the mover's target |
| 0x44 | **ItemUpdateMessage** | **both** | 0x11DA4B0 | array of 0x38 entries: u32 flags +0, u32 bits +4, handle +8; if `flags&1`: vec3 +0x10, quat +0x1C; if `flags&2`: handle +0x30 (parent/owner) |
| 0x45 | ItemUseRemotelyMessage | C→S | – | {+0x28 character, +0x30 item, +0x38 bool} (client 0x159DB00; server 0x426DC40 case 0x45) |
| 0x46 | **ItemMoveToWorldMessage** | C→S | 0x11D9350 | array of 0x38 entries: handle +0 (item), handle +8 (mover/character), byte +0x2C, vec3 +0x10, quat +0x1C, u32 +0x30 |
| 0x47 | ItemActionMessage | both | 0x11D7830 | array of 0x20 entries: handle +0, byte +8, state-machine data +0x10, byte +0x18 |
| 0x48 / 0x49 | ItemStatus / ItemStatusLifeTime | S→C | – | – |
| 0x4A | ItemTransformMessage | S→C | 0x11DA050 | **template** transform (not spatial): entries {handle, FixedString, byte, u32} |
| 0x4D | ItemMovedInformMessage | S→C | 0x11D8F60 | {handle +0x28, entries {handle, handle, u32, byte}} |
| 0x4E / 0x4F | ProjectileCreate / ProjectileUpdate | S→C | – | – |
| 0x114 | **CharacterDropEntityMessage** | C→S | 0x11ED140 | {dropper +0x28, handles[] +0x30, u32 +0x40 amount, vec3 +0x44, quat +0x50} |
| 0x115 | CharacterCancelDropEntityMessage | C→S | – | {handle} |
| 0x116 | **CharacterPickUpMessage** | C→S | 0x11ED250 | {item +0x28, handle +0x30, picker +0x38, u32 +0x40 amount, 16 bytes +0x48, u16 +0x58, u16 +0x5A, byte +0x5C} |
| 0x117 | CharacterCancelPickUpMessage | C→S | – | – |
| 0x120 | spell_cast::CastMessage | C→S | – | sender 0x1FFA6E0 |
| 0x121 | UseItemActionMessage | C→S | 0x11A4160 | {handle +0x28, byte +0x30, handles[] +0x38}; sent from a UI view-model ("ls.VMLearnableScroll", 0x14F9170), not the world "use" |
| 0x122…0x12A | inventory::{Create, ?, Add, MoveToSlot, Transfer, Sort, Equip, Unequip, Lock} | C→S | – | handled by `esv::InventoryProtocol` 0x4276270 (§7.3) |
| 0x25 / 0x26 | CharacterAction / CharacterActionData | both | – | client sender 0x1FE0520; server sender 0x306F590 |

Senders found by scanning every call to `GetFreeMessage` 0x40395F0 (`msg_senders.txt`): client
0x1FF37D0 (0x46, 0x44, 0x47), 0x1BF74E0 (0x116), 0x206ACC0 (0x114), 0x206ABE0 (0x115), 0x159DB00 (0x45),
0x14F9170 (0x121); server 0x426F5F0 (0x43, 0x44, 0x47, 0x48, 0x49, 0x4B), 0x4271F40 (0x44), 0x4270890
(0x3F, 0x41), 0x426F180 (0x4A), 0x41C3820 / 0x2F64070 / 0x3053860 / 0x3628B60 / 0x3629620 (0x4D),
0x35E6700 (0x38).

### 4.2 Protocol objects

| Object | vtable | ProcessMsg (vtbl+8) | Other |
|---|---|---|---|
| esv::ItemProtocol | 0x591AF20 | 0x426DC40 (cases 0x44, 0x45, 0x46, 0x47, 0x48, 0x4B) | ctor 0x426D480; vtbl+0x18 0x426EB60; S2C sync builder 0x426F5F0 |
| ecl::ItemProtocol | 0x56A7190 | 0x1FF25E0 (cases 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x47, 0x48, …) | vtbl+0x18 0x1FF37C0 → **0x1FF37D0 post-update (sends 0x46/0x44/0x47)**; created in 0x1808570 |
| esv::CharacterProtocol | 0x57EA6A0 | 0x306BBB0 (cases 0x20…0x11A incl. 0x114/0x115/0x116/0x117/0x118/0x119) | ctor 0x306ADA0 |
| esv::InventoryProtocol | 0x591B458 | 0x4276270 (cases 0x122…0x12B, 0x135) | – |
| esv::spell_cast::Protocol | 0x591BCD0 | 0x4283DF0 | – |
| Registration of all server protocols | – | – | 0x30A0760 (type ids `DAT_146111FB0` Character, `DAT_1461122A4` Item, `DAT_1461122AC` Inventory, `DAT_1461122F0` spell_cast, `DAT_1460F5EF0` Projectile) |

---------------------------------------------------------------------------------------------------

## 5. Client ↔ server item transform sync in detail

### 5.1 Server → client

`esv::ItemProtocol` sync 0x426F5F0 (per peer) allocates 0x43/0x44/0x47/0x48/0x49 messages, walks the
items relevant to that peer and, for an item with server flag 0x400, fills an `ItemUpdate` entry
with `vtable+0xD0` (transform) translate + rotate (`flags |= 1`). Moving items (server `ItemMover`)
are described with `ItemDestination` (0x43) instead.

Client `ecl::ItemProtocol::ProcessMsg` 0x1FF25E0 case 0x44: for each entry it gets/creates a 0x108-byte
pending record keyed by handle (0x1FF4CE0), copies translate to `rec+4`, rotation to `rec+0x68`,
sets `rec[0] |= 2`, copies parent handle (`rec+0x78`, bit 4), mirrors flag bits into `rec[0..2]`,
and appends the handle to `this+0x58` (pending list). Case 0x43 fills `rec+0x80…+0xB0` (destination).
Case 0x41 (activate) writes the transform immediately through 0x1C77050 / 0x1C77130.

The pending records are applied once per client frame by `ecl::ItemProtocol` vtbl+0x10 0x1FF37B0 →
**0x1FF3E60**: for each pending handle → `ecl::Item`; if `rec[0]&2`: when `item+0x40 … +0x108` is set,
**client physics suspend 0x1C7DD00**, then **SetTranslate 0x1C77050(rec+4)**, **SetRotate
0x1C77130(rec+0x68)**, clear the bit, **resume 0x1C7DE60**; `rec[0]&4` → parent handle (`item+0x60`);
`rec+0x80` (destination from 0x43) → client item mover (`itemMgr+0x68`, 0x201D810) which
interpolates the movement locally; other bits → flags / state machine / physics re-link (0x1FC39A0).
So a server value reaches the client item only when the server sent one.

`ecl::ItemProtocol` vtbl+0x40 0x1FF52D0 handles **ProjectileCreate 0x4E** (0x1C8-byte entries) and
**ProjectileUpdate 0x4F** (0x50-byte entries: handle +0, vec3 +8, quat +0x14, byte +0x3C, handle +0x40,
byte +0x48) and feeds the client projectile object through 0x18937F0, which keeps a *moving object*
handle at `projectile+0x1D0` and resolves it as an `ecl::Item` (0x793890, then dynamic-physics test
0x1FC46E0) or an `ecl::Character` (0x7841A0) — the thrown entity rides the projectile on the client.

Character messages, for comparison: the server's `CharacterProtocol` case 0x24 (Steering) and parts
of case 0x2B accept the request only if the sender's peer id equals the character's controlling peer
(`ownerId >> 16`), otherwise it answers `CharacterErrorMessage` 0x69 — characters have an ownership
check, items (case 0x44) do not.

### 5.2 Client → server

`ecl::ItemProtocol` post-update 0x1FF37D0, every client frame:
1. Builds `ItemMoveToWorld` (0x46) from the item manager's request list (`mgr+0x90`, count `+0x9C`,
   0x50-byte entries: item handle +0x00, target handle +0x18, u32 +0x24, vec3 +0x2C, quat +0x38),
   then clears the list. Producer of that list: **unconfirmed** (the client `ItemMove`/`ItemMoveDummy`
   character tasks of `ecl::ECharacterTask` are the candidates; confirm by finding writers of
   `mgr+0x9C`).
2. For every item in the active snapshot (`mgr+0x80`, count `+0x8C`): ItemAction (0x47) if its
   state machine changed, then **0x1FF3C10**: parent change → `flags|=2`; **client flag bit 18 →
   `flags|=1`, translate = `GetTransform()+0x10`, rotate = `GetTransform()+0x00`, clear bit 18**;
   bit 38 change → word bit 0x2000.
3. Sends each message that has entries to the server peer (0x4031600), otherwise releases it.

Server `esv::ItemProtocol::ProcessMsg` 0x426DC40:
* **case 0x44**: `item = FUN_14281A1E0(handle)`; per-peer record (0x4272​8D0 / 0x42723B0); if
  `flags&2` and new parent is null and the current `+0x90` character exists → 0x30C5D90(item, null);
  if `flags&1`: optional physics suspend (0x30C64F0) → **SetTranslate 0x30BCE40 → SetRotate
  0x30BD0C0** → resume (0x30C6610) → **SetFlags(0x400)**; bit 13 of word +4 toggles server flag
  0x800000000000. No ownership/permission check is present in this function.
* **case 0x46**: resolves item and the second handle (a character); if both exist →
  **0x35FAA60(item, level, &pos, &handle, …)** = the same core as Osiris `ItemMoveToPosition`.
* case 0x47: applies a state-machine action to `item+0x58` (state create 0x36E7190, vtbl+0x68 check).
* case 0x45: `UseItemRemotely` → 0x33A47F0 (§8).

### 5.3 The client "settle on ground" routine (the game's own client-authoritative example)

`ecl::ItemManager` update 0x1CCD960: for every active item: state-machine update (`item+0x48`),
if `flags & 0x400000000400` → 0x1BB7C40, then **0x1C7BD70(item, dt)**:
* only if `item+0x18 & 1` and the item has a physics object (0x1C785D0);
* accumulates `+0x30` (dt clamped to 1.0) and stops after 5 s (0x1C7B3A0);
* if the item's state machine is already in state 0xF → 0x1C7BFB0 (mark `+0x3C|=2`);
* otherwise, if the dynamic-physics test 0x1FC46E0 succeeds → mark and start state 0xF with the
  current position (0x2006E20);
* if it fails while `+0x3C & 2` → **0x1C7BFF0 (raycast down from `pos.y + 0.1` to the bottom of the
  physics bounds − 0.05 through the scene `*(clientLevel+0x30)` vtbl+0x40, keeps the hit closest in
  height that is not the item itself, builds an "up = hit normal" rotation keeping the yaw, and calls
  client SetRotate 0x1C77130 — rotation only)** → **SetFlags(0x40000)** → reported to the server.

---------------------------------------------------------------------------------------------------

## 6. Server-side movement / inventory API reachable from Osiris (and therefore from SE Lua `Osi.*`)

Osiris built-ins are registered by 0x3631830 (item), 0x3663710 (character), 0x36B0C00 (general):
`new 0x50 obj; ctor 0x3637450(obj, 1, list, name, …, handler)`; the handler is the `LEA RCX` before
the name. Table generated by `py/osiris_table.py` (`osiris_reg_all.txt`). Arguments arrive as an
array of 0x10-byte Osiris values (`param_2 + 0x10*i`; `+0x08` string pointer for GUID strings,
`+0x18/+0x28…` scalars). Object args are resolved by 0x2F60BA0 (GUID string → `esv::Item*`),
0x2F609A0 (→ `esv::Character*`), 0x2F60F50 (→ any game object).

| Osiris call | Handler | What it does (confirmed) |
|---|---|---|
| `ItemMoveToPosition(item, x,y,z, speed, accel, event)` | 0x3626A20 | → **0x35FAF90**(item, &pos, speed, accel, event): if `item+0x90` holder set → no move (holder check); else builds movement settings (0x1153DB0/0x1153E90) and either `ItemMover::MoveTo` **0x36EEC00** (item not in an inventory) or `MoveToWorld` **0x35FAA60** (item in an inventory: takes it out through the equipment / inventory systems) |
| `ItemMoveTo(item, target, speed, accel, matchRotation, event)` | 0x36266C0 | same core with the target's translate (vtbl+0xB8) and optional rotation (vtbl+0xC0) |
| `ItemDragToPosition(item, x,y,z)` | 0x3626670 | → **0x3625FE0**: unit-speed version: path from current transform to target (0x1154130), MoveToWorld 0x35FAA60 if the item has a parent entity, else `ItemMover::MoveTo` 0x36EEC00 |
| `ItemDragTo(item, target)` | 0x3626600 | same with target position |
| `ItemRotateY(item, deg, speed)` / `ItemRotateYToAngle` | 0x3626D60 / 0x3626E00 | state machine state 0xE on `item+0x58` (radians = deg·0.0174533) |
| `TeleportToPosition(obj, x,y,z, event, f1, f2, f3, f4, f5)` (10 args at `+0x08…+0x98`; the strings `TeleportLinkedCharacters`, `TeleportPartyFollowers`, `TeleportSummons` follow the name in the binary; f5 is the ground-snap flag in the item path) | 0x36740A0 | type switch on vtbl+0x40: **character → 0x35F09A0**(request struct with f1…f5 packed as bytes), **item → 0x35F2740**(item, pos, rot, level, event, f5), other objects with flags 0x900000 → vtbl+0x28, vtbl+0xD8(pos) |
| `TeleportTo(src, target, event, …)` | 0x3673DC0 | same switch with the target's translate/rotation/level |
| item teleport core | 0x35F2740 | stops an active mover (`ItemMover` 0x36EFB90), cancels a pending `esv::GameActionManager` action of type 8 on the item, optional ground snap (AI grid 0x140E4AE40 / 0x1411969D0), if the item is in an inventory → takes it out via EquipmentSystem, then places it |
| `ToInventory(obj, target, amount, notify, clearOriginalOwner)` | 0x368A110 | item → 0x35F3380; character-as-object → 0x35F8760 |
| `Drop(item)` / `DropTo(item, x,y,z)` | 0x3627A90 / 0x3627AD0 | → 0x35FF3D0 (creates a drop task 0x35FF230 and, when the item is in a character's inventory, queues it on that character's Osiris task controller `char+0x120` via 0x35F4DB0) |
| `MoveItemTo(char, item, target, amount, event)` | 0x364F240 | Osiris task type 0x10 (`OsirisMoveItemTask`) on `char+0x120` |
| `Pickup(char, item, event, force)` | 0x364EFB0 | if the item lacks flag 0x4 and `force==0` → direct request list `DAT_14603B358+0x2A0` (0x13D2D70); else task type 0xE |
| `Use(char, item, a, b, event)` | 0x364F100 | task type 0xF on `char+0x120` (the character walks/acts) |
| `Equip(char, item, a, b, c)` / `Unequip(char, item)` | 0x3652D20 / 0x3652EA0 | `esv::inventory::EquipmentSystem` 0x34F73C0 / 0x34F74E0 (same calls as the network Equip/Unequip, §7.3) |
| `Open(item)` / `Close(item)` | 0x362EC00 / 0x362ED20 | doors: item state machine; non-doors: `esv::item::animation::RequestsSystem` request 6 |
| `SetCanPickUp(item, bool)` | 0x362F490 | set/clear flag 0x4 |
| `SetGravity(item, mode)` | 0x362E3C0 | `esv::GravitySystem` 0x2EBD3D0 / 0x2EBD4D0 |
| `SetCanInteract`, `SetOnStage`, `SetVisible` | 0x362EF80, 0x367A550, 0x367DF50 | – (not decompiled) |
| `Die(char, …)` | 0x36A6F30 | 0x35F3F90 / 0x35F1A10 |
| `ApplyDamage(target, amount, type, source)` | 0x36A8B40 | builds a HitDesc and applies it through 0x36E9F60 |
| `UseSpell(…)` / `UseSpellAtPosition(…)` | 0x36A82A0 / 0x36A82B0 | both tail-jump to **0x3671750** with a bool (1 / 0): resolves caster (arg0), spell id string (arg1), target object(s) or position; if the caster is **not** a character it submits directly to **`esv::spell_cast::CastRequestSystem`** (DAT_146051628, 0x336E820) — the system the client's spell-cast messages 0x11C…0x11F feed (`esv::spell_cast::Protocol` 0x4283DF0 → 0x336E950); for a character caster it creates Osiris task type **0x12** (`OsirisUseSpellTask`) on the character |
| `CreateProjectileStrikeAt(Position)` | 0x368B230 / 0x368B3B0 | – (not decompiled) |
| `IsFreshCorpse`, `IsDead`, `Resurrect` | 0x36AE430, 0x364DEF0, 0x364DC70 | – |

Osiris events of interest (names in the region 0x5869000…): `OnThrown(ThrownObject, ThrownObjectTemplate,
Thrower, throwPosX/Y/Z)` (0x5869EB8), `OnStartCarrying(…, pickupPosX/Y/Z)` (0x5869F28),
`ItemTeleported`, `MovedFromTo`, `DroppedBy`, `UseStarted`/`UseFinished`, `PickupFailed`, `Died`.

`esv::ItemMover` (`*(*(level+0xA0)+0x88)` / `*(esv::ItemManager+0x88)`), `MoveTo` 0x36EEC00:
if the item is not on the current level → direct SetTranslate + SetRotate + SetFlags(0x40000);
otherwise creates/updates a 0x1A8-byte movement record (start 0x36EE170) and registers it with
`esv::invisibility::RevealingSystem`. Stop: 0x36EFB90.

---------------------------------------------------------------------------------------------------

## 7. Inventory, pick-up, drop, equip

### 7.1 Pick-up

Client: the pick-up task (0x1BF74E0; `ecl::ECharacterTask` "PickUp") sends `CharacterPickUpMessage`
0x116 with item, picker, amount and a 16-byte value (the item GUID kept at `task+0x100`).
Server: `esv::CharacterProtocol` 0x306BBB0 case 0x116 → `esv::pickup::PickUpLegacyRequestProxySystem`
0x35B4800(sys, picker, item, target, …); cancel 0x117 → 0x35B4920. The ECS pipeline after that is
`esv::pickup::{PickUpRequestSystem, PickUpRequestValidationSystem, PickUpPermissionSystem,
PickUpSplitRequestSystem, PickUpExecutionSystem, PickUpExecutionFinishedSystem}` with the components
listed in §2.3 (system bodies not decompiled; component users in `typerefs_*.txt`).

### 7.2 Drop

Client: the drop task 0x206ACC0 sends `CharacterDropEntityMessage` 0x114 {dropper, items[], amount,
**position, rotation**}. Server: case 0x114 → `esv::drop::DropEntityRequestSystem`
0x2E2F5D0(sys+0x10, dropper, &pos(+0x44), &rot(+0x50), list) — then `DropEntitySplitSystem`
(update 0x236F840), `DropEntityExecuteSystem` (0x236FA40), `DropEntityEndUpSystem` (0x236FC70)
with `esv::drop::*` components.

### 7.3 Inventory messages (`esv::InventoryProtocol` 0x4276270)

| Msg | Server call |
|---|---|
| 0x122 Create | `inventory::ManagementSystem` 0x3469F10 |
| 0x124 Add | `InteractionRequestSystem` 0x3471500(sys, item, target, slot…) |
| 0x125 MoveToSlot | `InteractionRequestSystem` 0x3471830 |
| 0x126 Transfer | `InteractionRequestSystem` 0x3471670 |
| 0x127 Sort | `InteractionRequestSystem` 0x3471480 |
| 0x128 Equip | `EquipmentSystem` 0x34F73C0(sys, &character, &item, bool, opts) — packs {item, bool, opts} into a 0x108-byte request (0x34F5FE0) and queues it on `sys+0x10` (0x34FE880); executed in the system update |
| 0x129 Unequip | `EquipmentSystem` 0x34F74E0(sys, &character, &item, opts) — same queue |
| 0x12A Lock | `inventory::LockingSystem` map write |

---------------------------------------------------------------------------------------------------

## 8. Using / interacting

* Client character tasks: the strings stored next to the type name `ecl::ECharacterTask` (0x55C0288)
  are `Loot, PickUp, DefaultTargetAction, CharacterMove, ItemCombine, ItemMoveDummy, SheathUnsheath,
  ItemCombineDummy, ItemDrop, ItemUse, ItemMove, ItemIteractArea, MoveToController, ItemUseRemotely,
  MoveInDirection, ClimbTo` (0x55C01B4…0x55C03A8); their numeric values are **unconfirmed** (no value
  table found). A task is created on a character's client task manager with `mgr->vtbl+0x68(type)`
  and queued with `mgr->vtbl+0x78(task, 0)` (seen in 0x159DB00 with types 5 and 0x14).
* `ItemUseRemotelyMessage` 0x45 (client 0x159DB00, from the `ls.VMGameObject` UI view-model) →
  server 0x426DC40 case 0x45 → **0x33A47F0(useMgr, character, item)**:
  `result = 0x35E6700(character, item, …, 0x13)` (runs the use core **0x35E6820** and broadcasts
  `CharacterItemUsed` 0x38) → `esv::item::UseSystem` (DAT_1460D95F0): `AddUseRequest` 0x33D8D30
  (entry {0, item, character}) and `AddUseResult` 0x33D8DB0 (entry {1, item, character, bool}); result 2
  queues a pick-up instead (`DAT_14603B358+0x2D0`).
* Use core 0x35E6820(character, item, …): fails when `item+0x90` holds another character or server
  flag bit 37 is set; handles locked items (key check 0x140FECD90, lockpick paths), sets flag 0x4000000,
  runs the item's use actions (rest of function, not traced).
* `UseItemActionMessage` 0x121 is a UI action (learnable scrolls), not world interaction.
* SE Lua route: `Osi.Use(char, item, …)` (task, may path-find), `Osi.Open/Close(item)` (direct),
  `Osi.Pickup(char, item, event, 1)`, `Osi.ToInventory`, `Osi.Equip`.

---------------------------------------------------------------------------------------------------

## 9. Throwing

### 9.1 Game data (Shared.pak `Public/Shared/Stats/Generated/Data/Spell_Throw.txt`, read-only extract)

`Throw_Throw`: `SpellType Throw`, `TargetRadius ThrownObjectRange`, `AreaRadius 1`,
`SpellRoll Attack(AttackType.RangedUnarmedAttack)`, `SpellSuccess` deals 1 / 1d4 / 2d4 bludgeoning for
light / medium / heavy thrown objects, `ThrownWeapon` damage for weapons with the Thrown property,
`Force(2)` if the object weighs more than half the target, `PRONE` if it weighs more than the target;
`ThrowableSpellSuccess IF(Character()):ApplyStatus(PRONE_AFTER_THROW,100,1)`;
**`ThrowableTargetConditions CanThrowWeight() and not Grounded() and not IsItemDisabled() and not
HasAttribute('InventoryBound') and (IsMovable() or Character() or Dead()) and CanMove(…) and (not
IsEquipped() or …)`**; `Template a82d0147-9ecd-455a-a9ef-c6d2b8119777`; `Trajectories rules:…`;
`SpellFlags … AddFallDamageOnLand …`; `UseCosts ActionPoint:1`.
`Throw_ImprovisedWeapon`: `SpellFlags … PickupEntityAndMove`, `SpellRoll Attack(MeleeUnarmedAttack)`.

### 9.2 ECS (names + functions; bodies only partly decompiled)

* Components: `esv::thrown::{ThrownParametersComponent (0x10 bytes, AddComponent 0x2AFC650),
  EntityThrownRequestOneFrameComponent, AttachmentRequestOneFrame, DetachmentRequestOneFrame,
  LandedOneFrameComponent (1 byte, AddComponent 0x2AFBBB0)}`, `eoc::thrown::{IsThrown, Attach,
  RotationParameters, ThrownAnimationRequestOneFrame}`, `esv::projectile::{InitializationComponent,
  SpellComponent, AttachmentComponent, ImpactEventOneFrame, SplitThrowableObjectRequestOneFrame}`,
  `esv::improvised_weapon::{PickUpData, PickUpRequestOneFrame, SetPositionOneFrame,
  SetVisibilityOneFrame, PreparingToBeReleasedEventOneFrame, CancelRequestOneFrame}`.
* Systems: `esv::thrown::ThrowAnimationSystem` (registration 0x2371F40, update wrapper 0x239B5E0 →
  **0x2AF9AB0**, which calls 0x2AFB900 (adds ThrownParameters), 0x2AFA1D0, 0x2AFB250, 0x2AFB720,
  0x2AFAEA0 (adds Landed)); `esv::thrown::AttachmentSystem` (registration 0x23721E0, update 0x239B630 →
  **0x2E97280**); `esv::improvised_weapon::PickUpSystem` 0x2373EA0; client `ecl::thrown::*` systems
  registered at 0x71D820 / 0x741350 / 0x71A4A0.
* SE describes the server projectile as carrying the thrown entity: `esv::projectile::InitializationData
  { …, EntityHandle ThrownObject, bool DamageMovingObjectOnLand, optional ProjectileResult Path,
  optional path::Settings MovementSettings, EntityHandle MovingObject, … }` and `esv::Projectile
  { …, PathMover Path, EntityHandle MovingObject, Transform MovingObjectTransform, bool IsThrown, … }`
  (SE `Components/Projectile.h`). **Confirmed in this build** (offsets of the legacy `esv::Projectile`):

  | Function | What it does |
  |---|---|
  | 0x2E8DE30 | projectile initialisation from an `InitializationData` block `d`: start position `d+0x0C` → `SetPosition`, target `d+0x18`, handles `d+0x78/+0x80/+0x88/+0x90` → `proj+0x18/+0x20/+0x40/+0x48`; **if `d+0x398` (moving object) is set → `AttachMovingObject(proj, d+0x398)`**, otherwise, when `d+0x2B8` is set, 0x2E8EEC0 (which also attaches); other path builders 0x2E8E7F0 / 0x2E8EB60 |
  | 0x2AE3AD0 | **`esv::Projectile::AttachMovingObject(proj, &handle, a, b)`**: stores the handle at **`proj+0x4E8`**; for an item: server physics suspend 0x30C64F0, 0x35FB760 (Osiris "moved by" events), aborts item state 0xF; then puts the object at `proj+0x500` (item: game-object vtbl+0xD8; character: 0x35E37B0) and rotation `proj+0x4F0` (vtbl+0xE0), and notifies `esv::FallingSystem` (DAT_1460D95B0). Called with a null handle it detaches: item → physics resume 0x30C6610 and 0x35FB8E0; character → listeners `DAT_14603B358+0x110` |
  | 0x2AE2B20 | **`esv::Projectile::SetPosition(proj, vec3*)`**: `proj+0x500 = pos`; moves the moving object (character → `esv::CharacterManager` 0x35E37B0(char, pos, 1, 1, 0); item → `ls::TransformSystem` 0x3C77BD0 directly, i.e. **without** setting item flag 0x400) and the projectile's own entity (`proj+0x5B0` EntityRef) |
  | 0x2ADE7A0 | landing / ground handling: for an item moving object `SetFlags(0x80000 WakePhysics)`; finds the ground height on the AI grid (0x1117FB0, 0x0E4FB80, 0x1178DB0) and re-places through 0x2AE2DC0 → SetPosition; item → `SetFlags(0x400 TransformChanged)` and fit test 0x35FE530; character → flag 0x1000000000000000 (0x35E32A0); notifies `esv::ProjectileManager` (DAT_1460D9310); hit/impact 0x2ADBF80 |
  | 0x35E37B0 | server character SetTranslate: queues {EntityRef, old pos, new pos, 3 bools} in `esv::CharacterManager` (DAT_146050800, list `+0x160`) and calls `TransformSystem` SetTranslate |

  On the client the same object rides `ecl` projectiles (`ProjectileCreate/Update` 0x4E/0x4F → 0x18937F0,
  moving object at client projectile `+0x1D0`).

### 9.3 What a physical VR throw can reuse (assessment)

* The game's throw is "a projectile carrying the object": the trajectory is the projectile's path
  (spell `Trajectories` rules → `eoc::projectile::TrajectoryRules`), the object is moved by
  `Projectile::SetPosition` every update, landing snaps to the AI-grid height and wakes item physics,
  and damage/impact come from the spell's `SpellSuccess` / `SpellProperties` functors. There is no
  separate "physics impact damage" path in the item code.
* Options for VR, in increasing integration:
  1. **Self-simulated flight + game placement**: fly the object client-side (§12), on landing report
     the position (client flag 0x40000 or `TeleportToPosition`), apply effects with `Osi.ApplyDamage`
     (0x36A8B40) / `Osi.ApplyStatus`. Cheapest, no game rules for hit chance.
  2. **Game throw spell**: `Osi.UseSpell(thrower, "Throw_Throw", …)` → CastRequestSystem / Osiris
     task 0x12 (§6). How to give it the thrown object and a free destination is **unconfirmed**
     (open item 4); the throw then uses the game trajectory, not the hand's velocity.
  3. **Native projectile with the object attached**: create an `esv::projectile::InitializationComponent`
     whose block has the moving object at `+0x398` and start/target at `+0x0C/+0x18` → the game flies
     and lands it (0x2E8DE30 path). Building that block correctly (path settings, spell id, caster) was
     not traced; high effort.
* Landing placement for a self-simulated throw: final position through the client report path (§5.2)
  or `TeleportToPosition` on the server (§6).

---------------------------------------------------------------------------------------------------

## 10. Corpses

* **A dead character stays an `esv::Character`** (confirmed): Osiris `IsDead` 0x364DEF0 resolves its
  argument with the character resolver 0x2F609A0 (→ 0x28198F0) and then tests death with
  **0x784340(serverWorld, &handle)**; the same 0x784340 test makes `Pickup` / `Use` / `MoveItemTo`
  refuse a dead *actor*. `IsFreshCorpse` 0x36AE430 reads a component through 0x0B20660 / 0x1193030.
* The game's throw rules address corpses as characters: `(IsMovable() or Character() or Dead())` in
  `ThrowableTargetConditions` (§9.1) — so a dead character can be thrown subject to `CanThrowWeight()`.
* Death ECS: `esv::death::{DeathSystem, CharacterSystem, DeathTypeSystem, CombatEntityAssignmentSystem}`,
  components `eoc::death::{DeathComponent, StateComponent, DownedComponent, DeathTypeComponent,
  DeadByDefaultComponent}`, `esv::death::{DeadEntitiesSingleton, DiedEventOneFrame,
  ResurrectionRequestOneFrame, ExecuteDieLogicRequestOneFrame, …}`, client `ecl::death::{StateComponent,
  DeathImpactComponent, ReloadDynamicPhysicsVisualRequestOneFrameComponent, …}` (ragdoll / dynamic
  physics visual → agent D).
* Moving a body server-side: `TeleportTo` / `TeleportToPosition` character branch → 0x35F09A0 (request
  struct {object, pos, rot, level, event, flags}); the character position replication uses the character
  messages (0x24 Steering, 0x2A/0x2B CharacterUpdate, 0x33 CharacterTransform, 0x34 CharacterTeleport) —
  their payloads were not decoded here.
* Osiris `IsFreshCorpse` 0x36AE430, `IsDead` 0x364DEF0, `Resurrect` 0x364DC70.

---------------------------------------------------------------------------------------------------

## 11. Picking (what is under the cursor / a ray)

* `ecl::PickingHelperManager` (system id DAT_146058F94; registration 0x7299A0, update wrapper 0x7588E0
  → 0x1CC2F40 which only flags/clears the per-player helpers). Per-player helpers are created through
  0x184CE60 (stores the client `ItemManager`, `GameActionManager`, `PickingHelperManager`,
  `phx::PhysXScene`, `SurfaceManager`, `ShroudManager`, `AtmosphereManager`).
* Per-player helper methods live around 0x1C3E000…0x1C4A000 (decompiled to `dec/pickrange.c`);
  0x1C422E0 builds the default target action for the hovered entity (+0xA0 = hovered entity,
  +0x70 = picked position).
* SE layout of `ecl::PlayerPickingHelper` (not verified): `Inner[2] { CharacterPosition, DeadCharacterPosition,
  ItemPosition, TooltipPosition, Character, DeadCharacter, Item, Tooltip, GameObject, … }`,
  `PlaceablePosition/Normal`, `Position/Normal`, `WorldPosition/Normal`, `PickingDirection`,
  `WindowCursorPos`, `TargetOverride`.
* For a VR hand, a direct physics raycast against the client physics scene and mapping the hit
  object back to an entity is the independent route. The game's own example (client item ground snap,
  disassembly 0x1C7C0F0…0x1C7C1E1, confirmed):
  ```
  scene = *(clientLevel + 0x30)            // clientLevel = *(DAT_146007A28 + 0x90)
  ok = scene->vtbl[0x40/8](scene, &from /*vec3*/, &to /*vec3*/, &res,
                           0xF, 0xFFFF, 0x4280, 1, -1, -1, &extra16)
  // res+0x50: array of hit objects, res+0x5C: hit count
  // res+0x00: per-hit vec3 array used as the surface normal, res+0x10: per-hit vec3 array (positions)
  // entity of hit i:  *(EntityHandle*) hitObj[i]->vtbl[0xA0/8](hitObj[i])
  // item from handle: FUN_140793890(clientWorld, handle)  (ecl::Item*), character: FUN_1407841A0
  ```
  The meaning of the masks 0xF / 0xFFFF / 0x4280 is **unconfirmed** here (physics groups → agent D).

---------------------------------------------------------------------------------------------------

## 12. Recommended implementation for the VR features (design notes)

| Feature | Client side (native, game thread) | Server side (Lua/Osiris) |
|---|---|---|
| Grab / hold item | each frame: `ecl::Item` 0x1C77050 + 0x1C77130 with the hand pose; avoid triggering 0x1C7BD70 settling (it only runs while `item+0x18&1` and stops after 5 s) | nothing while held; optionally `SetCanPickUp`/`SetCanInteract` off |
| Release (place) | final pose + `ecl::Item::SetFlags(0x40000)` → `ItemUpdate` 0x44 → server accepts | or `TeleportToPosition(item, …)` |
| Gravity-glove pull | animate client transform toward the hand | or server `ItemMoveToPosition` / `ItemDragToPosition` (game-driven movement, replicated with `ItemDestination` 0x43) |
| Throw | simulate flight client-side; report landing as above | damage via `ApplyDamage` / statuses, or the Throw spell (open) |
| Pick up into inventory | – | `Pickup(char, item, "", 1)` or `ToInventory(item, char, -1, 0, 0)` |
| Equip into hand | – | `Equip(char, item, …)` |
| Use / open | – | `Open/Close(item)`, `Use(char, item, …)`; native: `esv::item::UseSystem` path (§8) on the server thread |
| Body pick-up / throw | character transform (client) — agent C/D for the ragdoll | `TeleportToPosition(char, …)` for the final position |

### 12.1 Script Extender (Lua) routes and where they end up

| Lua (server context) | Game function reached (this build) |
|---|---|
| `Osi.ItemMoveToPosition(item, x, y, z, speed, accel, event)` | 0x3626A20 → 0x35FAF90 → `ItemMover::MoveTo` 0x36EEC00 or `MoveToWorld` 0x35FAA60; clients see `ItemDestination` 0x43 and interpolate (client mover `itemMgr+0x68`) |
| `Osi.ItemDragToPosition(item, x, y, z)` | 0x3626670 → 0x3625FE0 |
| `Osi.TeleportToPosition(obj, x, y, z, "", 0, 0, 0, 0, snap)` | 0x36740A0 → item 0x35F2740 / character 0x35F09A0 |
| `Osi.TeleportTo(obj, target, "", …)` | 0x3673DC0 |
| `Osi.ItemRotateY(item, deg, speed)` | 0x3626D60 (state machine 0xE) |
| `Osi.ToInventory(item, holder, amount, notify, clearOwner)` | 0x368A110 → 0x35F3380 |
| `Osi.Pickup(char, item, event, 1)` / `Osi.Drop(item)` / `Osi.DropTo(item, x, y, z)` | 0x364EFB0 (task 0xE) / 0x3627A90 / 0x3627AD0 → 0x35FF3D0 |
| `Osi.Equip(char, item, …)` / `Osi.Unequip(char, item)` | `EquipmentSystem` 0x34F73C0 / 0x34F74E0 |
| `Osi.Use(char, item, …)` | Osiris task 0xF (character acts/walks) |
| `Osi.Open(item)` / `Osi.Close(item)` | 0x362EC00 / 0x362ED20 |
| `Osi.SetCanPickUp(item, b)` / `Osi.SetGravity(item, m)` | flag 0x4 / `esv::GravitySystem` |
| `Osi.ApplyDamage(target, amount, type, source)` | 0x36A8B40 → 0x36E9F60 |
| `Osi.UseSpell(…)` / `Osi.UseSpellAtPosition(…)` | 0x3671750 (task 0x12 or CastRequestSystem) |

SE also exposes ECS components to Lua (`Ext.Entity.Get(guid).Transform`, `entity:Replicate(name)` —
SE docs `Docs/API.md`). Whether `ls::TransformComponent` is an ECS-replicated component in this build
was not checked; item positions reach clients through the legacy `ItemUpdate` / `ItemDestination`
messages (§5), which are driven by the `esv::Item` flags, so a Lua write to the component alone would
not set flag 0x400 (unconfirmed what clients would see).

Threading: the native setters above must run on the thread that owns that world (client world:
client game-state thread; server world: server thread). Which hook point is safe is agent C/B's area
(unconfirmed here).

---------------------------------------------------------------------------------------------------

## 13. Function index

| RVA | Name (mine unless from strings) | § |
|---|---|---|
| 0x119A320 | network message registration (all `eocnet::*Message`) | 4 |
| 0x40395F0 | net `GetFreeMessage(factory, id)` | 1 |
| 0x4031600 | net send to peer (client → server) | 1 |
| 0x40317A0 | net send to peer list (server) | 1 |
| 0x40396B0 | net release message | 1 |
| 0x4039250 | net `RegisterMessage(factory, id, msg, channel, name)` | 4 |
| 0x11DA4B0 | ItemUpdateMessage::Serialize | 4 |
| 0x11D9350 | ItemMoveToWorldMessage::Serialize | 4 |
| 0x11D8A40 | ItemDestinationMessage::Serialize | 4 |
| 0x11D8F60 | ItemMovedInformMessage::Serialize | 4 |
| 0x11D7830 | ItemActionMessage::Serialize | 4 |
| 0x11DA050 | ItemTransformMessage::Serialize (template transform) | 4 |
| 0x11ED250 | CharacterPickUpMessage::Serialize | 4 |
| 0x11ED140 | CharacterDropEntityMessage::Serialize | 4 |
| 0x11A4160 | UseItemActionMessage::Serialize | 4 |
| 0x426D480 | esv::ItemProtocol ctor | 4 |
| 0x426DC40 | esv::ItemProtocol::ProcessMsg | 5 |
| 0x426F5F0 | esv::ItemProtocol per-peer sync (S2C 0x43/0x44/0x47/0x48/0x49) | 5 |
| 0x1808570 | creates ecl::ItemProtocol | 4 |
| 0x1FF25E0 | ecl::ItemProtocol::ProcessMsg | 5 |
| 0x1FF37D0 | ecl::ItemProtocol post-update (C2S 0x46/0x44/0x47) | 5 |
| 0x1FF3C10 | ecl: build ItemUpdate entry for one item | 5 |
| 0x1FF4CE0 | ecl: get/create pending item record (0x108 bytes) | 5 |
| 0x306ADA0 / 0x306BBB0 | esv::CharacterProtocol ctor / ProcessMsg | 4,7 |
| 0x4276270 | esv::InventoryProtocol::ProcessMsg | 7 |
| 0x30A0760 | server protocol registration | 4 |
| 0x2F60BA0 / 0x2F609A0 / 0x2F60F50 | Osiris arg → esv::Item / esv::Character / game object | 6 |
| 0x281A1E0 / 0x28198F0 | esv::Item / esv::Character from EntityHandle | 1 |
| 0x3631830 / 0x3663710 / 0x36B0C00 | Osiris built-in registration (item / character / general) | 6 |
| 0x3626A20 | Osiris ItemMoveToPosition | 6 |
| 0x36266C0 | Osiris ItemMoveTo | 6 |
| 0x3626670 / 0x3626600 | Osiris ItemDragToPosition / ItemDragTo | 6 |
| 0x3626D60 / 0x3626E00 | Osiris ItemRotateY / ItemRotateYToAngle | 6 |
| 0x36740A0 / 0x3673DC0 | Osiris TeleportToPosition / TeleportTo | 6 |
| 0x368A110 | Osiris ToInventory | 6 |
| 0x3627A90 / 0x3627AD0 | Osiris Drop / DropTo | 6 |
| 0x364F240 | Osiris MoveItemTo | 6 |
| 0x364EFB0 | Osiris Pickup | 6 |
| 0x364F100 | Osiris Use | 6 |
| 0x3652D20 / 0x3652EA0 | Osiris Equip / Unequip | 6 |
| 0x362EC00 / 0x362ED20 | Osiris Open / Close | 6 |
| 0x362F490 | Osiris SetCanPickUp | 6 |
| 0x362E3C0 | Osiris SetGravity | 6 |
| 0x36A6F30 | Osiris Die | 6 |
| 0x36A8B40 | Osiris ApplyDamage | 6 |
| 0x36A82A0 / 0x36A82B0 → 0x3671750 | Osiris UseSpell / UseSpellAtPosition | 6 |
| 0x35FAF90 | esv item MoveToPosition core | 6 |
| 0x3625FE0 | esv item DragToPosition core | 6 |
| 0x35FAA60 | esv item MoveToWorld core (also client msg 0x46) | 5,6 |
| 0x36EEC00 | esv::ItemMover::MoveTo | 6 |
| 0x36EE170 | ItemMover: start move record | 6 |
| 0x36EFB90 | ItemMover: stop | 6 |
| 0x35F2740 | esv item teleport core | 6 |
| 0x35F09A0 | esv character teleport request | 6,10 |
| 0x35F3380 / 0x35F8760 | ToInventory item / object cores | 6 |
| 0x35FF3D0 | esv item drop core | 6 |
| 0x35F4DB0 | queue Osiris task on character (`char+0x120`) | 6 |
| 0x30E7AA0 | create Osiris task (`FUN_1430E7AA0(DAT_146041830, type, charHandle)`; type 0xE pick-up, 0xF use, 0x10 move item) | 6 |
| 0x30BCE40 / 0x30BD0C0 | esv::Item SetTranslate / SetRotate | 3 |
| 0x30BC760 / 0x30BCB40 | esv::Item SetFlags / ClearFlags | 3 |
| 0x30C64F0 / 0x30C6610 | esv::Item physics suspend / resume | 3 |
| 0x30C5D90 | esv::Item::SetUser (item+0x90 = character; flags the character `+0x1A5 bit 0x4`) | 2,5 |
| 0x1FF37B0 → 0x1FF3E60 | ecl::ItemProtocol pre-update: apply pending S2C item records | 5 |
| 0x1FF52D0 | ecl::ItemProtocol: ProjectileCreate / ProjectileUpdate handler | 5,9 |
| 0x18937F0 | ecl projectile: set transform + moving object (`+0x1D0`) | 5,9 |
| 0x1C7DD00 / 0x1C7DE60 | ecl::Item physics suspend / resume | 3 |
| 0x1BB7C40 | ecl::Item push transform into physics shapes | 2 |
| 0x3C77BD0 / 0x3C77D20 | ls::TransformSystem SetTranslate / SetRotate | 3 |
| 0x3C79010 | TransformComponent get-mutable | 3 |
| 0x1C77050 / 0x1C77130 | ecl::Item SetTranslate / SetRotate | 3 |
| 0x1C76D50 | ecl::Item SetFlags | 3 |
| 0x1C785D0 | (ecl/esv) Item → physics object | 2 |
| 0x1CCD960 | ecl::ItemManager per-frame item update | 5 |
| 0x1C7BD70 | ecl::Item settle / ground update | 5 |
| 0x1C7BFF0 | ecl::Item snap to ground (raycast + orient) | 5 |
| 0x1C7BFB0 / 0x1C7B3A0 | ecl::Item settle state helpers | 5 |
| 0x1FC46E0 | ecl::Item dynamic-physics test | 5 |
| 0x1BF74E0 | ecl pick-up task step (sends 0x116) | 7 |
| 0x206ACC0 / 0x206ABE0 | ecl drop task (sends 0x114 / 0x115) | 7 |
| 0x159DB00 | ecl UI "use game object" (tasks 5/0x14 or msg 0x45) | 8 |
| 0x14F9170 | ecl UI use-item action (msg 0x121) | 8 |
| 0x2E2F5D0 | esv::drop::DropEntityRequestSystem::AddRequest | 7 |
| 0x35B4800 / 0x35B4920 | esv::pickup::PickUpLegacyRequestProxySystem add / cancel | 7 |
| 0x34F73C0 / 0x34F74E0 | esv::inventory::EquipmentSystem equip / unequip | 7 |
| 0x3471500 / 0x3471830 / 0x3471670 / 0x3471480 | InteractionRequestSystem add / move-to-slot / transfer / sort | 7 |
| 0x33A47F0 | esv remote use handler | 8 |
| 0x35E6700 | esv use + broadcast CharacterItemUsed | 8 |
| 0x35E6820 | esv UseItem core | 8 |
| 0x33D8D30 / 0x33D8DB0 | esv::item::UseSystem add request / add result | 8 |
| 0x2371F40 / 0x239B5E0 / 0x2AF9AB0 | esv::thrown::ThrowAnimationSystem registration / wrapper / update | 9 |
| 0x23721E0 / 0x239B630 / 0x2E97280 | esv::thrown::AttachmentSystem registration / wrapper / update | 9 |
| 0x2AFC650 / 0x2AFBBB0 | AddComponent ThrownParameters / Landed | 9 |
| 0x2373EA0 | esv::improvised_weapon::PickUpSystem registration | 9 |
| 0x236F4E0 / 0x236F840 / 0x236FA40 / 0x236FC70 | esv::drop Request / Split / Execute / EndUp system registrations | 7 |
| 0x2E8DE30 | esv projectile init from InitializationData (attaches moving object `d+0x398`) | 9 |
| 0x2AE3AD0 | esv::Projectile::AttachMovingObject (`proj+0x4E8`) | 9 |
| 0x2AE2B20 | esv::Projectile::SetPosition (moves the moving object) | 9 |
| 0x2ADE7A0 | esv projectile landing / ground snap | 9 |
| 0x35E37B0 | esv character SetTranslate (CharacterManager queue + TransformSystem) | 9,10 |
| 0x784340 | server "is dead" test (world, &handle) | 10 |
| 0x364DEF0 / 0x36AE430 | Osiris IsDead / IsFreshCorpse | 10 |
| 0x4283DF0 | esv::spell_cast::Protocol::ProcessMsg (→ CastRequestSystem 0x336E950) | 6 |
| 0x3671750 | Osiris UseSpell core | 6 |
| 0x7299A0 / 0x7588E0 / 0x1CC2F40 | ecl::PickingHelperManager registration / wrapper / update | 11 |
| 0x184CE60 | per-player picking helper ctor | 11 |
| 0x1C422E0 | picking: default target action for hovered entity | 11 |

---------------------------------------------------------------------------------------------------

## 14. Open items (unconfirmed) and how to confirm

1. ~~Where client pending item records are applied~~ — resolved: 0x1FF3E60 (§5.1).
2. **Producer of the client MoveToWorld list (`itemMgr+0x90`)** — find writers of `+0x9C` in the
   client task code (ItemMove / ItemMoveDummy tasks); scans of 0x1BE0000–0x1C10000,
   0x1C60000–0x1C90000 and 0x2050000–0x2080000 found none.
3. ~~`esv::Item+0x90`~~ — resolved: user character, setter 0x30C5D90 (§2.1). Which game action sets it
   for thrown / improvised-weapon items is not traced.
4. **Throw spell with a chosen object** — decompile 0x3671750 (UseSpell core) and the spell-cast
   path that fills `ProjectileTargetData.ThrownObject` (SE `spell_cast::ProjectileTargetData`).
5. **Whether `TransformSystem` listeners move the PhysX actor** of an item (client and server) — agent D;
   or check the listener functors registered at `TransformSystem+0x18/+0x30`.
6. **Character transform messages** (0x24, 0x2A/0x2B, 0x33, 0x34) payloads, for corpse moving.
7. Runtime test to settle the authority finding: hold an item, call 0x1C77050 / 0x1C77130 each frame
   and 0x1C76D50(item, 0x40000) on release; log server `esv::Item` translate (vtbl+0xD0 +0x10) before
   and after the next frame, and log `ItemUpdateMessage` traffic by hooking 0x426DC40 case 0x44.
