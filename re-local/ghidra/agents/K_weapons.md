# K - Weapons and held equipment in the hands (bg3_dx11.exe 4.1.1.7631656)

Status: complete for this pass (see Open leads). Agent K, static analysis only (Ghidra project BG3_G, headless; disassembly with bg3vr_disasm; game data read
from Shared.pak with LSLib Divine, read-only). All addresses are RVAs (image base 0x140000000). Raw decompiles and
xref dumps: `re-local/ghidra/agents/K/` (cited as [rNN] = `rNN_*.c|txt`); extracted data: `K/gamedata/`,
`K/equipment_types.txt`, `K/weapon_visual_bounds.csv`. "SE" = name or layout taken from the Script Extender source.
Builds on C_animation_systems.md (skeleton vtable, slot 30 hook, finish job 0x3BD6670), E_items_interaction.md
(equip, messages, projectile init) and G/C type-id tables.

## Hook points for the VR mod

| Need | RVA | How to use | Confidence |
| --- | --- | --- | --- |
| Weapon/shield/torch follows the IK hand | skeleton slot 30 0x3A20590 (C) + bones `Dummy_R_Hand` / `Dummy_L_Hand` | Weapons are attached to the **Granny bones** `Dummy_R_Hand` / `Dummy_L_Hand` (not sockets). The attachment update reads that bone's world matrix from `skel+0xE0` *after* slot 30, so editing the pose in slot 30 moves the weapon - but world poses are not hierarchical: when you overwrite `Wrist_R`/hand you must also rewrite `Dummy_R_Hand` (and `Dummy_R_HandFX`, `Dummy_R_Hand_IK`, `Dummy_CastFX` if used) as `newParent * (oldParent^-1 * oldDummy)` | Confirmed (code 0x3BD6670 -> 0x3C63900 -> 0x3C68F40 -> skel vf[0x58]; bone status from HUM_M_Base skeleton data) |
| Per-attachment transform override | 0x3C68F40 `UpdateAttachment(Visual* parent, Attachment* a)` | Detour; for the weapon attachment write your own world transform (or let original run and post-correct). Called per attachment each animation finish, also from SetWorldTransform 0x3C65100 and physics paths | Confirmed |
| Freeze game-driven placement | `Attachment+0x18` flag 0x800 (DoNotUpdate) | 0x3C68F40 returns immediately when set; the mod then places the child itself (child `Visual` vtable +0x28 = SetWorldTransform 0x3C65100, or `ls::TransformSystem` 0x3C78110 for entity-backed visuals) | Confirmed (code); side effects on ECS transform sync Unconfirmed |
| Find the weapon visual | character `Visual+0xB8` Attachment[] (stride 0x30, count `+0xC4`) | Attachment with `Flags & 0x8000` (Weapon); `+0x00 Visual*`, `+0x0C` parent bone index, `+0x10` parent bone FixedString, `+0x1D` target type (1 bone, 2 socket) | Confirmed |
| Re-parent (other hand, two-handed grip) | AddAttachment 0x3C630D0, RemoveAttachment 0x3C63610 / 0x3C635C0 | Either patch `Attachment+0x0C` (bone index, from skeleton vf[0x88] FindBone) in place, or remove + re-add. The equipment system re-attaches on the next sheathe/unsheathe or visual refresh, overwriting your change | Confirmed (code), persistence Unconfirmed |
| Request draw / sheathe (client) | SheathUnsheath task (type 14) vtable 0x591A5D0, Enter 0x4265D90 | Enter sends `CharacterUnsheathingMessage` id 0x2D (array of net ids) to the server: a toggle request. Replicate: GetMessage 0x40395F0(net+0x238, 0x2D), append net id at msg+0x28, send 0x4031600 | Confirmed (client side) |
| Force draw / sheathe (server) | Osiris `SetWeaponUnsheathed` handler 0x36512C0 -> 0x35F19A0 | Writes the desired state into `esv::unsheath::DefaultSystem`'s map; `IsWeaponUnsheathed` 0x3651300 -> 0x35EAED0 | Confirmed (call chain); effect timing Unconfirmed |
| Read drawn state | `eoc::unsheath::StateComponent +0x14` State | 0 Instrument, 1 Melee, 2 Ranged, 3 Sheathed (unsheathed = State-1 < 2, 0x35EAED0) | Confirmed |
| Visual hand-over point when drawing | text-key handler 0x1FACE90 | Animation events `Sheathed_L/R`, `Unsheathed_L/R` re-request the slot visuals with sheathed/unsheathed bones (0x1FACB20 -> 0x1FABC40). Hook to know when the weapon really is in the hand | Confirmed |
| Which bone a weapon uses | 0x1B53800 | `(EquipmentType guid, unsheathed, offhand, versatile, alternative, slot, &bone, &sourceBone)`; data in `Public/Shared/EquipmentTypes/EquipmentTypes.lsx` (table §2.2) | Confirmed |
| Weapon properties | `eoc::WeaponComponent` (getter 0xB08E10): `+0x20` WeaponRange, `+0x30` WeaponProperties | flags (SE WeaponFlags) Twohanded 0x400, Versatile 0x800, Melee 0x1000, Torch 0x4000, Range 0x20, Reach 0x40 ... used by 0x1FA9490 | Confirmed (+0x30 in code; other offsets SE + FieldMeta) |
| Weapon geometry for a blade capsule | VisualResource BoundsMin/BoundsMax (data) | Weapon mesh local AABB, grip at origin, blade along -Z (longsword z -1.156..+0.363). Runtime: `MoveableObject` WorldBound +0x40 / BaseBound +0x58 (SE) | Data Confirmed; runtime offsets Unconfirmed |
| Held/thrown object at the hand | 0x1A2A860 (ecl::thrown::AttachmentSystem) | Attaches the object to be thrown to the caster's `Dummy_CastFX` bone | Confirmed |
| Spell/projectile start point | 0x2E8DE30 (E): start position `InitializationData+0x0C` | Overwrite the start with the real hand position before the server projectile is initialised | Confirmed hook (E); how the game computes the default start (ProjectileTemplate CastBone +0x108) Unconfirmed |
| Weapon-set switch | ecl::WeaponSetSystem 0x1A6BF00 | sends `SwitchCharacterWeaponSetMessage` 0xEC | Confirmed |

## 1. Equipment visuals: creation, attachment, per-frame transform

### 1.1 Attachment struct (Visual+0xB8, stride 0x30) - Confirmed

Built field by field by AddAttachment 0x3C630D0 [r14] and read by 0x3C68F40 [r14] (names SE `Visual::Attachment`):

| Off | Type | Meaning | Evidence |
| --- | --- | --- | --- |
| +0x00 | `Visual*` | child (weapon) visual | 0x3C630D0 `local_b8 = param_2` |
| +0x08 | int | SourceIndex: bone/socket on the **child** (-1 = child root) | init -1, resolved by 0x3C624A0 |
| +0x0C | int | AttachIndex: bone/socket on the **parent** (<0 = parent root) | param_4; 0x3C68F40 `if (*(int*)(a+0xC) < 0)` |
| +0x10 | FixedString | parent bone name (Attach) | `*param_3` |
| +0x14 | FixedString | child source bone name | `*param_7` |
| +0x18 | uint | VisualAttachmentFlags (SE: KeepRot 0x4, KeepScale 0x8, UseLocalTransform 0x10, DoNotUpdate 0x800, Owned 0x1000, HasSkeleton 0x2000, Armor 0x4000, **Weapon 0x8000**) | param_6 |
| +0x1C | byte | SourceType (0 root, 1 bone, 2 socket) | 0x3C624A0 writes 1/2 |
| +0x1D | byte | TargetType (0 root, 1 bone, 2 socket) | param_5 |
| +0x20 | FixedString | skeleton slot name (RemapperSlot), empty = default slot | matched against `SkeletonSlot+0` |
| +0x24 | vec3 | offset | `*param_9` |

The count is `Visual+0xC4`, capacity `+0xC0`. AddAttachment also: sets child `+0xC8` (Parent) = parent, copies the
parent's per-view cull bits 0x200 / 0x400 to the child (0x3C6B810 calls at 0x3C630D0 end) - so hiding the body for
one eye/view (C §2.2) also hides held weapons unless the mod clears them on the weapon -, attaches the child to the
parent's scene, and computes the first transform with 0x3C68F40.

### 1.2 Who attaches an equipment visual - Confirmed

- **0x198B680** (ecl::EquipmentVisualsSystem region; reads `ecl::EquipmentVisualsComponent` type 0x6052F4C users nearby)
  [r23]: called with an `EquipmentVisualRequest` whose layout matches SE (`+0x10 Bone`, `+0x14 SourceBone`,
  `+0x18 Level`, `+0x1C AttachFlags`, `+0x20 Parent`, `+0x48 Item`, `+0x50 VisualFlags`, `+0x53 RequestFlags`,
  `+0x54 Slot`). It looks the bone up on the parent: first as a **socket** (0x3C68460, socket array `skel+0x88`,
  name at socket+0x28) -> TargetType 2, else as a **bone** (skeleton vf[0x88] FindBone) -> TargetType 1. Weapons
  (slots 3-6) with `Bone != -1` are attached with `AddAttachment(parent, weapon, &Bone, boneIdx, type,
  AttachFlags|0x1000, &SourceBone, emptySlot, offset 0)`; armour (AttachFlags 0x4000) is attached at the root with
  flag 0x2000 (skeleton sharing). If the bone is not found the weapon is not attached (`return 0`).
- The request is built by **0x1FA9490** (slot request for an item) [r27]: AttachFlags from 0x1B537C0, bones from
  **0x1B53800** [r25], Versatile = `WeaponComponent+0x30 & 0x800` and the off-hand slot empty, "alternative" grip
  when `(flags & 0x1800)==0x1800` or `(flags & 0x1400)==0x1400`. 0x1FAA070 [r9] is an older variant that only
  produces sheathed bones. Requests go through 0x1FABC40 -> 0x1FABD70 -> 0x1FA93D0 -> 0x1989330.
- Default bone FixedStrings (resolved from the static FixedString tables `{FS*, char*, len}` at 0x54F47D0 and the
  initialiser 0x3BB3860): `Dummy_R_Hand` 0x5EEC238, `Dummy_Weapon_R_1H` 0x5EEC23C, `Dummy_WingFX` 0x5EEC240,
  `Dummy_HornFX` 0x5EEC244, `Dummy_NeckFX` 0x5EEC248, `Dummy_Attachment` 0x5EF27BC (default child source bone),
  `Dummy_Sheath` 0x5EF2738, `Dummy_CastFX` 0x60B0058 (ptr 0x5FC54F8), `Dummy_L_HandFX` 0x5EF2628,
  `Dummy_R_HandFX` 0x5EF262C, `Sheathed_L` 0x5EECED4, `Sheathed_R` 0x5EECED8, `Unsheathed_L` 0x5EECEDC,
  `Unsheathed_R` 0x5EECEE0. (Table direction confirmed three ways: wings/horns/neck slots map to WingFX/HornFX/NeckFX,
  the unsheathed fallback is Dummy_R_Hand, and the L/R text keys map to off-hand / main-hand slots.)

### 1.3 Per-frame placement - Confirmed

Order inside the animation finish job 0x3BD6670 [r2] for each character visual: skeleton vf[0xE8] (swap) ->
vf[0xF0] (slot 30, skinning; the VR IK hook) -> 0x3F12D60 (renderable/cloth pre-update) -> 0x3F12F40 recursive ->
0x3C6B900 (mesh bindings, wrinkle bones 0x3C6BC10, blend shapes) -> **either** queue the visual on
`AnimationBlueprintSystem+0x240` when an attachment has VisualFlags 0x10000 (HasBlueprint, i.e. an attachment with
its own animation, e.g. skinned bows) - drained later by 0x3BD22C0, which calls 0x3C68F40 for every attachment - **or**
0x3C63900 directly, which calls **0x3C68F40(parent, attachment)** for every attachment [r12, r29].

0x3C68F40 [r14, disassembly `d_3C68F40.txt`]:
1. `if (flags & 0x800) return` (DoNotUpdate).
2. Parent frame: parent `+0x08` world transform (quat +0x08, translate +0x18, scale +0x24; SE MoveableObject), composed
   with `parent+0x30` (SE LocalTransform*) when set.
3. Child side (SourceIndex >= 0): child skeleton (slot matching `+0x20`) vf[0x58] GetBoneWorldTransform (type 1) or
   socket transform 0x3D498B0 (type 2), inverted; plus `+0x24` offset. SourceIndex < 0: offset only.
4. Parent side (AttachIndex >= 0): parent skeleton vf[0x58] (slot 11, reads the **current** pose `+0xE0`) for type 1,
   or 0x3D498B0(skel, socketIdx) for type 2 (socket = its bone `+0x2E` world transform via vf[0x58] times the socket
   local transform, plus an optional constraint bone `+0x2C` via vf[0x50]); edx = the attachment index (the
   decompiler shows a constant 0, the disassembly at 0x3C69700 / 0x3C69D2A passes r11d / r9d).
5. KeepRot (0x4) keeps the child's rotation, KeepScale (0x8) the child's scale, UseLocalTransform (0x10) uses
   `parent+0x30`.
6. Write: if the child visual has a valid entity (`+0x108/+0x110`) -> `ls::TransformSystem` 0x3C78110(system, &entity,
   &transform, 1, 0) (writes the entity's transform component and calls the listeners at `system+0x60`); else child
   vtable +0x28 SetWorldTransform 0x3C65100. Then 0x3C63900(child) for the child's own attachments (effects, trails).

**Consequence for the slot-30 IK hook.** The weapon reads exactly one bone of the edited pose: the parent bone of its
attachment. `Dummy_R_Hand` / `Dummy_L_Hand` are Granny bones of `HUM_M_Base` (SkeletonBank in
`[PAK]_Male_Body/_merged.lsf`: 122 bones including `Wrist_R`, `Dummy_R_Hand`, `Dummy_R_HandFX`, `Dummy_R_Hand_IK`,
`Dummy_CastFX`, `Dummy_Sheath_*`; its 52 sockets are all `Socket_*` hair/camera/light sockets, none for hands).
So the mod must write the dummy bones too (or do IK in local space and rebuild the world pose so children follow).
The parent bone index of `Dummy_R_Hand` (skeleton vf[0x98]) is not known statically; log it once.

## 2. Sheathe / unsheathe

### 2.1 State and requests

- State: `eoc::unsheath::StateComponent` (SE: MainHandWeapon +0x00, OffHandWeapon +0x08, State +0x14). 0x35EAED0
  reads `State` at +0x14 and treats 1/2 as unsheathed; 0x1FACE90 treats `State == 0` as instrument. Confirmed.
- Client request: the `SheathUnsheath` character task (type 14; vtable 0x591A5D0, debug name `SHEATH_UNSHEATH` just
  before it; slot 25 Enter = 0x4265D90 [r37, r39-r41]) sends `eocnet::CharacterUnsheathingMessage` id 0x2D
  (ctor 0x11AED60, vtable 0x55491B8; E's netmsg table) holding an array of character net ids - no target state, i.e.
  a toggle. Which input event creates the task is not traced (candidates `ToggleCombatMode` 0xBA, C §3.3).
- Server: `esv::unsheath::UserRequestSystem` update 0x32F8CD0 [r42, r43] adds `UserRequestComponent` (type 0x60D19F0,
  value 1), or `UserRequestFailedOneFrameComponent` (0x60D1A00) - details of the state change not decompiled.
  Osiris `SetWeaponUnsheathed(char, bool)` handler 0x36512C0 -> 0x35F19A0 sets a bool per entity in
  `esv::unsheath::DefaultSystem` (type 0x60D950C) at `system+0x10` map / `+0x40` values [r55, r56].
- Weapon set (melee/ranged): `ToggleWeaponSet` 0xBB -> ecl::WeaponSetSystem 0x1A6BF00 sends
  `SwitchCharacterWeaponSetMessage` 0xEC (E msg_senders). `eoc::WeaponSetComponent.WeaponSet` (SE).
- Other ECS players (registrations via SysReg [r8]): ecl::unsheath::AnimationSubSetSystem 0x1A18240,
  ecl::unsheath::VisualStateSystem 0x1A1B0D0 / 0x1A1BC80, ecl::unsheath::AnimationSystem 0x1A19CA0, esv::unsheath::
  AnimationSystem 0x32E02A0, esv::unsheath::CombatSystem 0x32E2F50 (auto draw on combat), esv::WeaponSetSystem
  0x2B89930, esv::inventory::WieldingSystem 0x349A020 / WieldedSystem 0x349B710, esv::character::EquipmentVisualSystem
  0x2EC4B50, ecl::dummy::EquipmentVisualsSystem 0x19F7D70.

### 2.2 Where weapons sit - bones (Confirmed: code 0x1B53800 + data EquipmentTypes.lsx)

0x1B53800 field selection (EquipmentType offsets +0x24.. match the SE field order with a 0x18-byte GuidResource
header): main hand sheathed `BoneMainSheathed`/`SourceBoneSheathed`, unsheathed `BoneMainUnsheathed`/default
source; off hand `BoneOffHandSheathed|Unsheathed`; versatile (two-handed grip of a versatile weapon)
`BoneVersatile*` + `SourceBoneVersatile*`; alternative unsheathed `SourceBoneAlternativeUnsheathed`; no
EquipmentType -> sheathed `Dummy_Weapon_R_1H` + source `Dummy_Sheath`, unsheathed `Dummy_R_Hand`. Data
(`K/equipment_types.txt`, excerpt):

| Type | Sheathed main / off | Unsheathed main / off | Versatile S / U |
| --- | --- | --- | --- |
| Longsword, Scimitar, Rapier, Shortsword, Mace, Handaxe, ... | Dummy_Sheath_Upper_R / Dummy_Sheath_Upper_L | **Dummy_R_Hand** / **Dummy_L_Hand** | Longsword: Upper_R / Dummy_R_Hand |
| Dagger | Dummy_Sheath_Hip_L / Hip_R | Dummy_R_Hand / Dummy_L_Hand | - |
| Greatsword, Greatclub, Glaive, Halberd, Pike | Upper_R or Upper_L | **Dummy_R_Hand** | - |
| Greataxe, Maul, Light/Heavy crossbow, Quarterstaff, Warhammer, Battleaxe | Upper_L | **Dummy_L_Hand** (main hand!) | Upper_R / Dummy_R_Hand |
| Longbow, Shortbow | Dummy_Sheath_Ranged | **Dummy_L_Hand** | - |
| Hand crossbow | Lower_R / Lower_L | Dummy_R_Hand / Dummy_L_Hand | - |
| Shield | (empty) | Dummy_L_Hand | - |
| Instruments | Dummy_Sheath_Music | Dummy_L_Hand | - |

Source bones on the weapon (`Dummy_Sheath`, `Dummy_Attachment_Versatile`, `Dummy_Attachment_Rage`) only matter for
weapon visuals that have a skeleton: 410 of 463 human weapon visuals have `SkeletonResource` empty (bows, crossbows,
flails have one; `K/weapon_visual_bounds.csv`), so for them the source lookup in 0x3C624A0 fails and the weapon's
mesh origin sits exactly on the parent bone (SourceIndex -1).

### 2.3 The hand-over moment - Confirmed

Drawing plays an animation whose events (text keys) `Unsheathed_R/L` / `Sheathed_R/L` are handled by 0x1FACE90
(called from 0x1D49B30 <- 0x1D3FF10): Sheathed_L -> slot 6/4 (or 0x10 instrument) sheathed, Sheathed_R -> slot 5/3,
Unsheathed_* the same with unsheathed = 1; each calls 0x1FACB20 -> 0x1FABC40 (new visual request with the new
bone). ecl::unsheath::AnimationSubSetSystem 0x1A18240 matches the same four FixedStrings on
`eoc::animation::TextKeyEventsOneFrameComponent` (query 0x60AF998). Bulk re-request for all weapon slots:
0x1FAC650 (melee/ranged by weapon set) and 0x1FAC970 (instrument).

## 3. Detaching, re-parenting, custom transforms

- Mechanisms (Confirmed in code): DoNotUpdate flag 0x800; detour 0x3C68F40; edit `Attachment+0x0C` / `+0x1D`;
  RemoveAttachment 0x3C63610(parent, index) (also clears child Parent `+0xC8`, recomputes parent flags) or
  0x3C635C0(parent, child); AddAttachment 0x3C630D0 (signature in §1.2). Detach of a whole visual 0x3C63D20.
- What follows automatically: anything attached *to the weapon visual* (its own Attachment[] - effect visuals,
  trails) is updated by 0x3C63900(child) at the end of 0x3C68F40 and by SetWorldTransform 0x3C65100, so it moves with
  the weapon whatever drives it (Confirmed for the update call; that trails/torch flames are child attachments rather
  than world-space effects is Unconfirmed - log `weaponVisual+0xB8` entries and their `Flags & 0x200` (Effect)).
- What would break / must be handled:
  - Two-handed weapons are parented to one hand only (often `Dummy_L_Hand`); the second hand is pure animation. For VR
    two-handed grips, parent to the dominant controller's dummy bone and IK the other hand onto the weapon.
  - Re-requests (sheathe text keys, weapon-set change, equipment change, `RefreshWeaponVisualsRequestOneFrame`)
    re-create the attachment with the data bone; persist the mod's override by re-applying after 0x198B680 or keeping
    the 0x3C68F40 detour.
  - Per-view culling: AddAttachment copies the parent's 0x200/0x400 cull bits to the weapon (0x3C630D0); a mod that
    hides the body in one eye must clear them on weapon visuals.
  - Ranged: arrows/bolts are server projectiles (E §9); the bow visual position does not drive the spawn point
    (Unconfirmed what does - see §5).
  - Torch light: `esv::GameplayLightEquipmentComponent` exists (server gameplay light for stealth checks; position
    source Unconfirmed). Rendered light/flame: Unconfirmed (likely effect attachments of the torch visual).

## 4. Weapon geometry and properties

- Mesh bounds (data, Confirmed): VisualBank `BoundsMin/BoundsMax` per weapon visual, weapon-local space, grip at the
  origin, blade/head along **-Z**: Longsword (-0.119,-0.065,-1.156)..(0.119,0.082,0.363), Greatsword z -1.580..0.350,
  Dagger z -0.408..0.176, Spear z -1.892..1.044, Warhammer z -0.794..0.382, Quarterstaff z -1.363..0.951, Torch
  z -0.698..0.253, Shield (-0.351,-0.081,-0.337)..(0.434,0.154,0.441), Longbow z -0.918..0.918. Full table:
  `K/weapon_visual_bounds.csv` (463 human weapon visuals). A blade capsule = segment from (0,0,zMin*0.95) to (0,0,~0)
  with radius ~max(|x|,|y|) is a reasonable first cut.
- Runtime: weapon `Visual+0xD0` = VisualResource (SE, BoundsMin/BoundsMax); `MoveableObject` WorldBound `+0x40`,
  BaseBound `+0x58` (SE AABound {Min, Max}) - offsets Unconfirmed in the exe (0x3C66090 recomputes bounds, not fully
  read). Confirm by logging both for a longsword against the table above.
- Physics: the weapons bank has a PhysicsBank with 160 collision resources (`Public/Shared/Assets/Weapons/WPN_*.bin`,
  e.g. `WPN_HUM_Scimitar_A_0`), used for the item when it lies in the world (D_physics). VisualBank entries carry no
  physics reference, and whether any collision body exists while the item is equipped is Unconfirmed (log the
  item's physics object, D §item physics, with the weapon equipped). The visual bounds are the practical source for a
  blade capsule.
- Item stats: `eoc::WeaponComponent` (getter 0xB08E10; FieldMeta strings give Rolls +0x00, VersatileRolls +0x10,
  AttackRollBonus +0x38; SE: WeaponRange +0x20, DamageRange +0x24, Functors +0x28, **WeaponProperties +0x30**,
  WeaponGroup +0x34, DamageType +0x35). `+0x30` flags used in 0x1FA9490 / 0x1C71460 (0x800 Versatile, 0x1000 Melee,
  0x400 Twohanded); full flag list SE `WeaponFlags`. `eoc::EquipableComponent` (getter 0xB08900): EquipmentType GUID
  +0x00 (-> 0xECFAB0 resource lookup), Slot +0x10. ItemSlot (SE): MeleeMainHand 3, MeleeOffHand 4, RangedMainHand 5,
  RangedOffHand 6, MusicalInstrument 0x10.

## 5. Shields, off-hand items, spellcasting origin

- Shields: EquipmentType `Shield` unsheathed main/off = `Dummy_L_Hand`, sheathed bone empty. With an empty bone
  0x198B680 skips the attach (Confirmed path); the skeleton has a `Dummy_Sheath_Shield` bone that this table does not
  reference - where a sheathed shield goes is Unconfirmed (log the shield's Attachment entry with weapons sheathed).
- Off-hand weapons use `BoneOffHand*` (`Dummy_L_Hand` unsheathed); dual wielding is `eoc::DualWieldingComponent`
  (SE), toggled by message 0x101 (sender 0x15A6DF0).
- Held objects for throwing: ecl::thrown::AttachmentSystem (type 0x605916C, update 0x1A287B0) -> 0x1A2A860 attaches
  the object's visual to the caster's `Dummy_CastFX` bone (flags 0x1008), source bone from 0x5FC5500 or the object's
  centre. Follows the edited pose if `Dummy_CastFX` is updated by the mod.
- Spell effects: data addresses bones as `Effect:Bone` strings (e.g. `...:Dummy_BodyFX`); hand bones
  `Dummy_L_HandFX` / `Dummy_R_HandFX` / `Dummy_CastFX` exist in HUM_M_Base. No gameplay code reads the HandFX
  FixedStrings (only the initialiser/destructor, [r45]), so they are data-driven via effects.
- Projectiles: `ProjectileTemplate.CastBone` (SE) is template `+0x108` (serialiser 0xF732B0 visits FixedString
  0x5EEBC6C "CastBone" at `rdi+0x108`). The code that turns it into the start position was not found (not in the esv
  projectile range 0x2E7FFF0-0x2EA0000 [r49]). The server initialises projectiles from `InitializationData+0x0C`
  (E: 0x2E8DE30), which is the robust VR hook: overwrite the start with the controller/hand world position.

## Open leads

1. Parent of `Dummy_R_Hand` / `Dummy_L_Hand` in HUM_M_Base and their rest offsets: log skeleton vf[0x98] (parent
   index) and the model-space matrices once; needed to re-derive dummy bones after IK.
2. Runtime check of the weapon Attachment: `Flags`, `TargetType` (+0x1D, expect 1), `AttachIndex`, `SourceIndex`
   (expect -1 for skeleton-less weapons) for longsword, greataxe (expect parent Dummy_L_Hand), longbow (has skeleton),
   shield, torch.
3. Whether writing the weapon transform directly (DoNotUpdate + SetWorldTransform 0x3C65100) is overwritten by the ECS
   transform path (0x3C78110 listeners at `TransformSystem+0x60`): log the weapon world transform one frame after a
   manual write.
4. Input event that creates the SheathUnsheath task; full server effect of message 0x2D (UserRequestSystem
   0x32F8CD0 -> state change).
5. Where `CastBone` / spell cast position is evaluated (client or server); search users of ProjectileTemplate+0x108
   or the spell-cast "cast" text key.
6. Effects on weapons (trails, enchant glow, torch flame): log `weaponVisual+0xB8` attachments and their flags.
7. Runtime offsets of WorldBound / BaseBound / VisualResource bounds (SE) in this build.
