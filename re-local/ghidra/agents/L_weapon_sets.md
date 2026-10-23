# L - Weapon sets, draw/sheathe, per-hand attacks, equipping (bg3_dx11.exe 4.1.1.7631656)

Status: complete for this pass (see Open leads). Agent L, static analysis only (Ghidra project BG3_G, headless,
read-only, persistent request server `agents/L/LServer.java`; disassembly with bg3vr_disasm; game data read-only with
LSLib Divine from Engine.pak / Game.pak / Models.pak into `agents/L/gamedata/`). All addresses are RVAs (image base
0x140000000). Raw decompiles and dumps: `re-local/ghidra/agents/L/` (cited as [lNN] = `L/lNN_*`). "SE" = name or
layout from the Script Extender source (`downloads/bg3se-src`). Builds on K_weapons.md (K), E_items_interaction.md (E),
I_picking_actions.md (I), C_animation_systems.md (C), G's type-id map (`agents/G/typeidmap.txt`).

ECS access pattern used in every recipe below (Confirmed, it is how the game's own message handlers do it, e.g.
0x306BBB0 case 0xEC [l02], 0x35F19A0 [K r56]):
- server system: `world = *(*(void**)0x145FF03C0 + 0x2B0); sys = *(void**)(*(world + 0x30) + typeIndex * 0xF8)` with
  `typeIndex = *(u32*)(image + typeIdGlobal)`.
- client system: `world = *(*(void**)0x145FDA240 + 0x1A8)`, same indexing (0x15A6710 [l07] end).
- client net object: `net = *(client + 0x98)` (B_splitscreen; 0x1A6BF00 reads it as `*(*(sys+0x60)+0x98)` [l01]).

## Hook points for the VR mod

| Need | RVA | How to use | Confidence |
| --- | --- | --- | --- |
| Switch to a SPECIFIC weapon set (server) | esv::WeaponSetSystem (type 0x60D93A0), request array `sys+0x10`, append 0x5BC780 | Server thread: `struct {u64 entity; u8 set /*0 Melee,1 Ranged*/; u8 toggle /*0*/; u8 pad[6];} r; 0x5BC780(sys+0x10, &r)`. Processed by the system update 0x2B89930 the same frame/next frame; Ranged is refused if slots 5 and 6 are both empty | Confirmed (code) |
| Same, toggle (what the client message does) | same, `{entity, 2, 1}` | 0xEC handler pushes exactly `{entity, 0x02, 0x01}` | Confirmed |
| Switch set from the client | message `SwitchCharacterWeaponSetMessage` 0xEC (toggle only) | `msg = 0x40395F0(*(net+0x238), 0xEC); 0x4014CB0(*(*(void**)0x146007F68 + 8), &netId, entityHandle); *(u64*)(msg+0x28) = netId; 0x4031600(net, *(u32*)(net+0x418), msg)`. For "set X": read `eoc::WeaponSetComponent` (getter 0xAFA070, byte 0 Melee / 1 Ranged) and send only if different | Confirmed |
| Client toggle through the game's own queue | ecl::WeaponSetSystem (type 0x60591F4) HashSet `sys+0xA8` | `0x720010(sys+0xA8, &tmp16, &entityHandle)`; next client update 0x1A6BF00 sends 0xEC unless the entity is casting (IsCasting check 0x110F630) | Confirmed |
| Know when the set changed | eoc::WeaponSetChangedEventOneFrameComponent (0x605BC94) `{u8 old, u8 new}`; server write in 0x2B89930 via 0x1A6C300 | hook 0x2B89930 tail or read the component | Confirmed |
| Draw / sheathe the CURRENT set, animated | Osiris `SetWeaponUnsheathed(char, int, int)` 0x36512C0 -> 0x35F19A0 | Lua `Osi.SetWeaponUnsheathed(char, 1, 0)`; only arg 1 is read (3rd arg ignored in this build). Drawn state = Melee(1) if WeaponSet 0, Ranged(2) if 1 | Confirmed |
| Force a specific drawn state, NO draw animation | Osiris `PushUnsheathedState(char, state)` 0x3651150 / `PopUnsheathedState(char)` 0x3651250 | `Osi.PushUnsheathedState(char, 2)` = ranged weapons in hand immediately (state 0 Instrument, 1 Melee, 2 Ranged, 3 Sheathed); priority 0xC beats default/user/combat; pop to release. StateComponent+0x18 = 0 -> client sets the visual state directly, no text-key wait | Code Confirmed; visual result Unconfirmed |
| Native equivalent of the above | esv::unsheath::ScriptSystem (0x60D9518) push 0x32EC8B0(sys, &entity, state) / pop 0x32EC970(sys, &entity) | server thread | Confirmed |
| Read drawn state + which items are "in hand" | `eoc::unsheath::StateComponent` +0x00 MainHandWeapon, +0x08 OffHandWeapon, +0x10 winning priority, +0x14 State, +0x18 animate flag | written by esv::unsheath StateSystem 0x32F1C40 | Confirmed |
| Main-hand melee hit with dice | Osiris `Attack(attacker, target, 0)` 0x3654360 (I) | picks main-hand attack of the active set via 0xFD1E00 | Confirmed (I + this report §3) |
| OFF-hand melee hit with dice | Osiris `UseSpell(char, "Target_OffhandAttack", target, ...)` 0x36A82A0 -> 0x3671750 | the selector 0xFD1E00 never returns the off-hand spell when a main-hand attack exists; name it explicitly. Costs BonusActionPoint:1; UseSpell sets IgnoreSpellRolls-type option 0x4 (I lead 1) | Code Confirmed; roll behaviour Unconfirmed |
| Bow shot at a target / ground point | `Osi.UseSpell(char, "Projectile_MainHandAttack", target, ...)` / `Osi.UseSpellAtPosition(char, "Projectile_MainHandAttack", x, y, z, ...)` 0x36A82B0 | spell has GROUND: properties and TargetFloor -1, so a position target is valid data-wise; overwrite the projectile start with the hand (E 0x2E8DE30) | Data Confirmed; position target at runtime Unconfirmed |
| Equip a specific item into a specific slot (client) | `eocnet::inventory::EquipMessage` 0x128, sender 0x1F491C0 | `{+0x28 character NetId, +0x30 item NetId, +0x38 u8 ItemSlot}`; server builds an EquipRequest with `Slot = msg byte, HasSlot = 1` | Message layout Confirmed; slot honoured Unconfirmed (runtime) |
| Equip (server, native) | `EquipmentSystem::Equip` 0x34F73C0(sys, &character, &item, u8 checkCosts, u8 opts[11]) (type 0x60516D8) | `opts = {slot, hasSlot=1, evaluateConditions, discardOnFail, IgnoreCombat, IgnoreLocked, OnlyMainHand, AllowShapeshifted, ?, BlockUnequip, CanSwapHands}` | Call Confirmed; field names SE |
| Equip (server, Lua) | Osiris `Equip(char, item, a, b, c)` 0x3652D20 | no slot argument (HasSlot = 0, auto slot) | Confirmed |
| Input event ids | ToggleCombatMode 0xBA, **ToggleWeaponSet 0xBB**, ToggleDualWielding 0xBC, ToggleCharacterOverview 0xC2, ToggleInventory 0xC3, UIToggleCharacterOverview 0x13C | registration 0x1B8E510 | Confirmed |
| Default bindings | `Engine.pak: PlayerProfiles/Eoc/keyboard_1.json` (data, not exe) | ToggleWeaponSet key `f`, ToggleCombatMode `u`, ToggleDualWielding `r`, ToggleInventory `i`, ToggleCharacterOverview `n`; controller: only ToggleDualWielding `c:x` | Confirmed (data) |
| Fire an input EVENT (not a raw key) | InputManager FireEvents 0x40F1550(im, out72, span{FireEventDesc*begin,end}, bool) | builds listener dispatch for 0x38-byte FireEventDesc records; see §1.6. For ToggleWeaponSet this is pointless: the only consumer is a Noesis hotbar button | Confirmed (function); manual use Unconfirmed |

## 1. Weapon-set switching end to end

### 1.1 Data

- `eoc::WeaponSetComponent` (type 0x6050330): one byte, SE `WeaponSetType`: Melee 0, Ranged 1, Unknown 2. Getter used
  by server and client code: **0xAFA070(world, entity) -> u8*** (0x2B89930, 0x15A6710, 0x32F1C40, 0x36559E0).
- `eoc::WeaponSetChangedEventOneFrameComponent` (0x605BC94) `{u8 old, u8 new}` - written by 0x1A6C300 (`*p = old;
  p[1] = new`, [l04]), created by both the server system (0x2B89930) and the client system (0x1A6BF00 from its map
  `sys+0x88/+0x98`).
- Request one-frame components (server, consumed by esv::WeaponSetSystem):
  `esv::spell_cast::WeaponSetChangeRequestOneFrameComponent` (0x60D16BC, SE `{bool IsRanged}`) and
  `esv::inventory::WeaponSetChangeRequestOneFrameComponent` (0x60D2244, one byte; created by 0x34FEFE0 with value 2,
  overwritten by its caller 0x34F9AD0 with the set chosen by an equipment set [l07, l23]).

### 1.2 Client: input -> UI -> system -> message (Confirmed)

1. Key `f` (keyboard_1.json) -> input event **0xBB ToggleWeaponSet** (id written at 0x1B91F26 right after the
   "ToggleWeaponSet" string 0x564AEB8 in 0x1B8E510).
2. No C++ code consumes 0xBB: the string is referenced only by the registration, and the binding lives in XAML:
   `Game.pak Mods/MainUI/GUI/Pages/HotBar.xaml` style `WeaponSetSwitchStyle` (`ls:LSRadioButton`,
   `BoundEvent="ToggleWeaponSet"`, line 482); the inactive set's button gets
   `Command="{Binding DataContext.SwitchWeaponSetCommand}"` (lines 4724-4729). Controller UI: `Overlay_c.xaml:358`
   button bound to `UISelectionLeft` (hold). So **the F key only works while the hotbar is loaded and that radio
   button is enabled.**
3. `SwitchWeaponSetCommand` (string 0x55C2EA0) = VM command slot `+0x3F8` set to **0x15A6710** in the VM ctor 0x15991B0
   [l09]. 0x15A6710 [l07]: finds the selected player character, and if its InputController's running/preview task is
   the Spell task (type 4) with a weapon attack, re-selects the attack for the other set (0xFD1E00 + SetSpell
   0x1FB2BF0); finally **`0x720010(clientWeaponSetSystem + 0xA8, &tmp, &character.entity)`** (HashSet insert).
4. **ecl::WeaponSetSystem** (type 0x60591F4, registration 0x716B10 -> callback 0x761350 -> update **0x1A6BF00** [l01,
   l05]): for each entity in the set's key array (`sys+0xC8`, count `+0xD4`): if 0x12AFC30 (presence of the component
   with type index 0x6052E3C, i.e. a locally controlled character - name Unconfirmed) and **not**
   0x110F630 (eoc::spell_cast::IsCastingComponent 0x6066718), it sends **0xEC**:
   `msg = 0x40395F0(*(net+0x238), 0xEC)`, `msg+0x28 = NetId` (0x4014CB0 on `*(DAT_146007F68+8)`), `0x4031600(net,
   *(net+0x418), msg)`. Then clears the set (+0xA8..+0xD4) and the event map (+0x68..+0x94).
5. Message class (ctor 0x11A7F30, vtable 0x5549F50): id 0xEC, payload one NetId at +0x28 (ctor sets +0x28 = -1).
   **No set value: it is a toggle.**

### 1.3 Server: handler and the system that performs the switch (Confirmed)

- `esv::CharacterProtocol` ProcessMsg 0x306BBB0 **case 0xEC** [l02]: character by NetId (0x2A761F0 on DAT_14603B360),
  then `0x5BC780(esvWeaponSetSystem + 0x10, &{entity, u16 0x0102})` = request `{set = 2, toggle = 1}`.
  (Case 0xED, SwitchCharacterArmorSetMessage, goes to the system with index DAT_1460D939C via 0x720010(+0x90).)
- **esv::WeaponSetSystem** (type 0x60D93A0; registration 0x23443E0 -> callback 0x238D570 -> update **0x2B89930** [l04,
  l05]). Request array at `sys+0x10` (16-byte entries `{u64 entity; u8 set; u8 toggle}`, count `+0x1C`, capacity
  `+0x18`). Each update it appends requests from:
  1. query 0x60F9B84 (added `esv::spell_cast::WeaponSetChangeRequestOneFrameComponent`): `{entity, IsRanged, 0}`;
  2. query 0x60F9B88 (added `esv::inventory::WeaponSetChangeRequestOneFrameComponent`): `{entity, byte, 0}`;
  3. unless a savegame is loading (query 0x609BDEC, `+0x1EC != 0`): `esv::item::UnequippedEventOneFrameComponent`
     (query 0x60F9B8C): `{character, (unequipped weapon WeaponGroup & 0xFD) == 0, 0}` and
     `esv::item::EquippedEventOneFrameComponent` (0x60F9B90): `{character, (WeaponGroup & 0xFD) != 0, 0}`
     (WeaponComponent getter 0xB08E10, byte +0x34 = SE `WeaponGroup`). I.e. equipping a ranged weapon switches to
     ranged, unequipping one switches to the other set. The WeaponGroup value order (bit 1 = ranged) is
     Unconfirmed (data ValueLists; data values SimpleMelee/SimpleRanged/MartialMelee/MartialRanged).
  Then it pops requests (last first), removes other requests for the same entity, and for each entity that has a
  WeaponSetComponent (0xA87520):
  - toggle == 0: `want = set`; toggle != 0: `want = (current == 0) ? 1 : (current == 1 ? 0 : current)`;
  - **want == 1 (Ranged) requires an item in slot 5 or 6** (`0x1104E20(.., entity, 5)` / `(.., 6)`), else the
    request is dropped silently;
  - if `want != current`: marks the component dirty for replication, writes it, and creates
    WeaponSetChangedEvent `{old, new}` (0x1A6C300 on the server world).
- Consumers of WeaponSetChangedEvent on the server (registrations reading 0x605BC94 [l16]): esv::unsheath::StateSystem
  (0x235A600 -> callback 0x2393A80 -> 0x32F0EB0 / 0x32F1C40), esv::combat::ThreatRangeSystem (0x234CBF0),
  esv::light::GameplayLightSystem (0x23535C0), esv::VisualSystem (0x23653B0) and two more (0x23941D0, 0x2399C40).
  Client: ecl::EquipmentVisualsSystem (0x72BE80), ecl::CharacterManager (0x73B440), sound (0x73A850) and others -
  K's bulk visual re-request 0x1FAC650 picks the slots by weapon set.
- So a set change while drawn re-runs the unsheath state resolution (§2.2): Melee(1) <-> Ranged(2), with the draw
  animation flag set (Default candidate, flag 1).

### 1.4 Requesting a specific set (recipes)

- **Server native (best, exact):** `0x5BC780(sys + 0x10, &req)` with `req = {entity, set (0/1), 0}` on the server
  thread (same context as the Osiris handlers, which write into these system containers directly). Confirmed by the
  0xEC handler using the same call with `{entity, 2, 1}`.
- **Server, Lua:** there is no Osiris built-in for weapon sets (full name list in `agents/E/osiris_reg_all.txt`: no
  name contains "WeaponSet"; related queries only: `GetEquippedWeapon` 0x36559E0 (main hand of the active set:
  slot 3 if WeaponSet 0 else 5), `HasMeleeWeaponEquipped` 0x3689550, `HasRangedWeaponEquipped` 0x3689A10,
  `GetEquippedItem(char, slotName)` 0x3655E60). From SE Lua the options are: write
  `entity.WeaponSet.WeaponSet` + `Replicate` (bypasses the system: no event, no unsheath re-resolve - not
  recommended), or create one of the two request one-frame components (whether SE can create one-frame components
  is Unconfirmed), or call into the DLL.
- **Client:** read WeaponSetComponent (0xAFA070 on the client world) and send 0xEC only if it differs (toggle).
- **Inject the key:** raw `f` = SE InputRawType 41 on device 0 via InjectRaw 0x40F1F00 (press + release). Works only
  while the hotbar's WeaponSetSwitch radio button is live (1.2). Raw id from SE (C confirmed SE raw ids match for
  the ids it checked; 41 itself Unconfirmed in the exe).

### 1.5 Default key/controller bindings (data, Confirmed)

Not compiled into the exe: the exe only holds the file names (strings 0x549DC30 `PlayerProfiles/Eoc/controller_1.json`
referenced by 0x5B7AB0, 0x549DC58 `PlayerProfiles/Eoc/keyboard_1.json` referenced by 0x584F90,
`PlayerProfiles/Default/inputconfig.json` 0x5899820 by 0x3AC84D0, `inputconfig_controller.json` 0x5899848 by
0x3AC8190, per-player overrides `inputconfig_p1..p4.json`, `inputremap_p1..p4.json`). The files are in **Engine.pak**
(extracted to `L/gamedata/PlayerProfiles/`). Format: JSON `"EventName": [ "key:f", "c:x", "shift+key:c",
"mouse:left" ]`. Relevant defaults (keyboard_1.json): ToggleWeaponSet `key:f`, ToggleCombatMode `key:u`,
ToggleDualWielding `key:r`, ToggleInventory `key:i`, ToggleCharacterOverview `key:n`, ToggleEquipment none,
TogglePartyManagement `key:y`, Interact none, ActionCancel `key:escape, mouse:right`, UISelectSlot0..12 `0-9,-,=`.
There is **no separate party-inventory event**: party inventory is a tab of the CharacterPanel opened by
ToggleInventory (`CharacterPanel.xaml:712-717`, `PartyPanel.xaml:129-179` bind ToggleInventory /
ToggleCharacterOverview). controller_1.json: ToggleInventory `[]`, ToggleCombatMode `[]`, ToggleDualWielding `c:x`,
UIToggleCharacterOverview `c:y`; inputconfig.json: Action1 `mouse:left`, ContextMenu `mouse:right`.

### 1.6 Feeding the InputManager an event id directly (Confirmed function, Unconfirmed use)

- Raw path (C): InjectRaw 0x40F1F00 -> Update 0x40F0450 -> 0x40F1FA0 (bindings) -> **0x40F1550**.
- **0x40F1550 FireEvents(im, out[9 qwords], span{begin,end} of FireEventDesc, bool)** [l13]: for each 0x38-byte record
  (SE `FireEventDesc`: +0x00 `InputEventDesc*`, +0x08 player index, +0x0C `InputEvent{EventId, u16 DeviceId, u8
  PlayerIndex, u8 Type, InputValue Old (+0x14), New (+0x20), bool WasPreferred}`, +0x30 u16 DeviceId, +0x32 u16
  InputId): updates the preferred device (`im+0x398`), gives the event first to a captured listener
  (`desc+0x10+player*8`), else walks `InputListeners` (`im+0x2F0`, count `+0x2FC`) through 0x40F1970, which filters
  by `listener->vf[0x10]()` against desc flags (`desc+4`) and calls `listener->vf[0x38](listener, &ret, &event)`
  (SE `OnInputEvent`). Callers: only 0x40F1FA0 and 0x40F0450.
- To fire 0xBB yourself you need the `InputEventDesc*` for 0xBB from `InputManager.InputDefinitions` (SE
  `LegacyRefMap<int, InputEventDesc>`; offset in this build Unconfirmed). For the weapon set this gives nothing over
  §1.4 (the listener is the Noesis hotbar).

## 2. Drawing and sheathing

### 2.1 Requests (all Confirmed)

| Source | Code | Effect |
| --- | --- | --- |
| Client toggle (0x2D, K) | server UserRequestSystem 0x32F8CD0 -> UserRequestOneFrame; esv::unsheath::DefaultSystem 0x32E8980 | `DefaultComponent = {cause 3, wantDrawn = (State == 3)}` - toggle |
| Leaving combat | DefaultSystem, query 0x6098FD4 (IsInCombat removed) | `{cause 1, wantDrawn = false}` |
| Osiris `SetWeaponUnsheathed(char, drawn, x)` 0x36512C0 -> 0x35F19A0 | writes `DefaultSystem` map (`sys+0x10` keys, `+0x40` values); DefaultSystem turns it into `{cause 2, wantDrawn = drawn}` | 3rd argument (used as 0/1 in game scripts, e.g. `SetWeaponUnsheathed(_Guard,1,1)`) is **not read** (handler reads only `+0x18`) - no "immediate" in this build |
| Osiris `PushUnsheathedState(char, state)` 0x3651150 | validates `state` against table 0x5EF01F8 (values 0,1,2,3; else default 0x5EF01E8 = 0) and pushes it onto `esv::unsheath::ScriptSystem` (0x60D9518) per-entity array, 0x32EC8B0 | ScriptOverrideComponent (SE `Array<int>`), top of stack used |
| Osiris `PopUnsheathedState(char)` 0x3651250 | 0x32EC970 | pops |

`DefaultComponent` (SE `esv::unsheath::DefaultComponent {int field_0; bool field_4}`) = `{cause, wantDrawn}`, written by
0x32E8980 through getter/creator 0x32E9530 [l17].

### 2.2 State resolution: esv::unsheath::StateSystem (Confirmed, 0x32F1C40 [l18])

Per dirty character it builds candidates `{int priority, int state, u8 flags[3]}`, sorts by priority, takes the
highest, and writes StateComponent. Candidates found (priority -> source):

| Prio | Source (getter) | State |
| --- | --- | --- |
| 0 | always | 3 Sheathed (flag 1) |
| 1 | DefaultComponent (0x26D7FD0) | wantDrawn ? D : 3, flag 1, where **D = 2 if WeaponSet == 1 (Ranged) else 1** (WeaponSet getter 0xAFA070; no component or 2 -> 1) |
| 2, 0xB | weapon in slot 3/4 or 5/6 with WeaponProperties & 0x80000 (0x32F0DC0) | data-driven |
| 4 | SpellAnimationConcludedComponent (0x32F4900) | |
| 6-9 | 0x32E43F0 checks | |
| **0xC** | **ScriptOverrideComponent (0x26D7E60): top of the PushUnsheathedState stack, flag 0** | pushed state |
| 0xD-0x10 | CoatingComponent (0x32F4A70) | |
| 0x11 | SpellAnimationLifetimeComponent (0x32F4BE0) | spell's state |
| 0x12-0x14 | 0xAF9270 component / two tag checks (state 3) | |
| 0x15-0x1C | 0xEF6870 lookups, 0x783B30, 0xACE850, IsDead 0x784340 (0x1A/0x1B), slot 5/6 & 0x40000 (0x1C) | |

Writes: `+0x00/+0x08` MainHandWeapon/OffHandWeapon = items in slots **0x10** (state 0), **3/4** (state 1),
**5/6** (state 2), none (3) via 0x1104E20; `+0x10` = winning priority; `+0x14` = state; `+0x18` = animate flag;
`+0x19/+0x1A` = candidate flag bytes; StateChangedEvent `{old, new}` via 0x32F4070. Animate flag = `(state changed &&
candidate flag0) || prio 6 || prio 7 || (prio in {0,1,4,5} && state != 3 && membership checks)`, masked by the
character flag `+0x48 bit 11`.

Consequences:
- **There is no "draw main hand only" state**: State 1 puts both melee slots in the hands, State 2 both ranged
  slots. Per-hand visuals are separate only in the animation text keys (K §2.3: `Unsheathed_R` -> main-hand slot,
  `Unsheathed_L` -> off-hand slot). A VR "draw only the right weapon" must be done visually by the mod (K §3
  attachment tools) while the logical state is Melee.
- **Draw ranged without changing the set:** `PushUnsheathedState(char, 2)`. Recommended to also switch the set to
  Ranged (§1.4) so attacks (selector 0xFD1E00 uses the set) and visuals agree.
- **Instant vs animated:** server esv::unsheath::AnimationSystem 0x32E02A0 [l20] queues the draw animation only for
  entities whose StateComponent `+0x18 != 0` (0x32E0670). Client ecl::unsheath VisualState update 0x1A1BC80 [l20]:
  if `+0x18 == 0` it writes the new State straight into `ecl::unsheath::VisualStateComponent` (both ints) - the
  weapons are re-requested in their unsheathed bones without waiting for text keys; if `+0x18 != 0` and State != 3
  it first sets the visual state to 3 and waits for the animation's text keys. SetWeaponUnsheathed / 0x2D toggle /
  weapon-set change use flag 1 (animated); PushUnsheathedState uses flag 0 (instant). Code Confirmed; that the
  instant path shows the weapon in the hand in the same frame is Unconfirmed (log `StateComponent+0x14/+0x18` and
  K's 0x198B680 attach call with timestamps).
- **Timing of the animated draw:** the hand-over happens at the `Unsheathed_R/L` text keys (K 0x1FACE90); their
  time inside the unsheath animation is data (animation resources of the unsheath animation set) - Unconfirmed.
  Runtime: timestamp the StateComponent change (server) vs the 0x1FACE90 calls (client).

## 3. Attacks per hand

### 3.1 The weapon-attack selector 0xFD1E00 (Confirmed [l27, l28])

`0xFD1E00(SpellId* out, spellbook, AttackSpellOverride map, ?, const u8* weaponSet, ?, u8 forceSet /*2 = use
*weaponSet*/, bool anyFallback, bool unarmedFallback)`. Spell names come from 0xFD0DC0(isMainHand, isRanged,
overrides), keyed like SE `SpellAttackTypeOverride` with defaults: (1,0) key 0 **Target_MainHandAttack** (FixedString
0x5EEB8AC), (0,0) key 1 **Target_OffhandAttack** (0x5EEB8B0), (1,1) key 2 **Projectile_MainHandAttack** (0x5EEB8B4),
(0,1) key 3 **Projectile_OffhandAttack** (0x5EEB8B8); 0xFD27C0 = Target_UnarmedAttack. It scans the spellbook for
attack spells (`proto+0x10 & 8`), classifies each as melee (`proto+0x29A == 3` or flags 0x1000) / ranged (`== 4` or
flags 2), keeps only the requested set, and returns **main-hand if present, else off-hand** (then unarmed/any if the
fallbacks are set). Callers include Osiris `Attack` 0x3654360, the client Spell task priority 0x1FB3420 (I) and
the weapon-set UI command 0x15A6710.

### 3.2 Recipes

- **Main-hand melee swing:** `Osi.Attack(player, target, 0)` (I §3.1; normal roll and action cost; uses the active set).
- **Off-hand swing:** `Osi.UseSpell(player, "Target_OffhandAttack", target, NULL_00000000-0000-0000-0000-000000000000)`.
  Data (Shared Spell_Target.txt): SpellRoll `Attack(AttackType.MeleeOffHandWeaponAttack)`, damage
  `OffhandMeleeWeapon`, UseCosts `BonusActionPoint:1`, RequirementConditions `not Tagged('AI_BLOCKOFFHANDATTACK')`.
  Osiris UseSpell on a character always sets options 0x1|0x2|0x4|0x200 (I) - may skip roll and the IsAttack checks;
  for a rolled off-hand attack the client route (Spell task SetSpell 0x1FB2BF0 with this SpellId, I §3.2) or a
  native CastRequestSystem request without 0x4 is needed. Unconfirmed which is cleaner; see Open leads.
- Dual-wield main-hand spells carry `CastOffhand[...]` in SpellRoll/SpellSuccess (Target_MainHandAttack,
  Projectile_MainHandAttack): the off-hand part is the "AlternativeCastTextEvents CastOffhand" text event of the
  dual-wield animation, not a second spell.
- **Bow:** `Projectile_MainHandAttack` (SpellType Projectile, TargetRadius RangedMainWeaponRange, TargetFloor -1,
  SpellProperties `GROUND:DealDamage(...)`, `Sheathing "Ranged"`, UseCosts ActionPoint:1). At a creature:
  `Osi.UseSpell(char, "Projectile_MainHandAttack", target, NULL)`; at a point: `Osi.UseSpellAtPosition(char,
  "Projectile_MainHandAttack", x, y, z, ...)` (0x36A82B0 -> 0x3671750 with bool 0). For a VR draw-and-release, aim the
  point along the arrow direction (e.g. raycast hit or far point) and replace the projectile start with the hand
  (E 0x2E8DE30). Position casting at runtime Unconfirmed.
- **Casting a ranged spell switches the set:** spell-cast event 0x15 (SE `Event_WeaponSetRequest {bool IsRanged}`) is
  handled in 0x3357380 by writing `esv::spell_cast::WeaponSetChangeRequestOneFrameComponent.IsRanged` (0x335D940
  creates it [l29, l30]), which esv::WeaponSetSystem consumes (§1.3). So `Projectile_MainHandAttack` while in the
  melee set switches to ranged (if ranged weapons exist) and the unsheath state follows.

## 4. Equipping from code

- Client message **0x128 EquipMessage** (sender 0x1F491C0, a slot of the client inventory request interface,
  vtable entry 0x54E3930) [l21]: `+0x28` character NetId, `+0x30` item NetId, `+0x38` u8 (the sender's 5th argument).
  Server `esv::InventoryProtocol` 0x4276270 case 0x128 [l22]: `EquipmentSystem::Equip(sys, &character, &item,
  checkCosts = 1, opts)` with `opts = {msg+0x38, 1, 1, 0,0,0,0,0,0,0, 1}`.
- **0x34F73C0 Equip(sys, EntityHandle* character, EntityHandle* item, u8 checkCosts, u8* opts)** packs
  `{item, checkCosts, opts[0..10]}` into the request variant 0 (0x34F5FE0) and queues it on `sys+0x10` (0x34FE880);
  executed by the system update 0x34F6E10 (0x34F7B00 / 0x34F88A0 / 0x34FA7A0). With SE's `EquipRequest` layout
  `{Item, CheckActionResourceCosts, ItemSlot Slot, field_A, EvaluateConditions, DiscardOnEquipFail, EquipSettings
  {IgnoreCombat, IgnoreLocked, OnlyMainHand, AllowShapeshifted, field_4, BlockUnequip, CanSwapHands}}`: opts[0] =
  Slot, opts[1] = "slot given" (client 1, Osiris 0 - Osiris leaves opts[0] uninitialised, which only makes sense if
  opts[1] is the engaged flag of an optional slot), opts[2] EvaluateConditions, opts[3] DiscardOnEquipFail,
  opts[4..10] settings. **So the client message carries the target slot (ItemSlot: MeleeMainHand 3, MeleeOffHand 4,
  RangedMainHand 5, RangedOffHand 6, MusicalInstrument 0x10).** Layout of the call Confirmed; the meaning of opts[0]/[1]
  rests on SE + the Osiris/client contrast (Unconfirmed until the executing code 0x34F7B00 path is read or a runtime
  test equips a dagger with slot 4).
- **Osiris `Equip(char, item, a, b, c)`** 0x3652D20 [l21]: `c != 0` -> 0x3600AC0(item) first; Equip(sys, char, item, 0,
  `{?, 0, 0, a != 0, 1, 1, 0, 0, 0, 0, 1}`); `b != 0` -> esv::inventory::EntityReceivalNotificationSystem 0x350B730(item,
  char). No slot. Game scripts use `Equip(char, item, 0, 0, 1)` / `(char, item, 1, 0, 1)`.
- Osiris `Unequip(char, item)` 0x3652EA0 -> 0x34F74E0 (E). `GetEquipmentSlotForItem` 0x368A0B0,
  `GetEquippedItem(char, "MeleeOffHand")` 0x3655E60 (slot string -> enum 0x11040B0, 0x13 = invalid), `LockUnequip`
  0x362E220, `IsEquipped` 0x362E850.
- Equipment-set path (0x34F9AD0, CharacterGiveEquipmentSet / CC): when the chosen slot is already taken in the same
  set it moves 3<->4 and 5<->6 (code at 0x34F9AD0 lines 222-241 [l23]) and requests the weapon set with 0x34FEFE0.
- A grabbed loose weapon: server Lua `Osi.Equip(char, item, 0, 0, 1)` (auto slot; ranged weapons go to the ranged
  slots and trigger the ranged set switch via EquippedEvent §1.3), or native client 0x128 with the slot byte for
  off-hand placement.

## 5. In-hand orientation beyond K

- Grip: weapons are attached with offset 0 at the parent bone, source bone only for skinned weapons (K §1.2, §2.2).
  No per-weapon grip offset exists in code; the grip is the mesh origin (K bounds table).
- Two-handed grip of versatile weapons: `BoneVersatileUnsheathed` (`Dummy_R_Hand`) with source bone
  `Dummy_Attachment_Versatile` on the weapon (K equipment_types.txt), chosen in 0x1FA9490 when the weapon has
  Versatile (0x800) and the off-hand slot is empty. The "alternative" source bone `Dummy_Attachment_Rage` is used
  for flags 0x1800/0x1400 (K).
- Bows: `WPN_HUM_Longbow_A_1` has a skeleton (resource 24ae5bfb-a277-f206-c17c-92c57995a632, GR2
  `Generated/Public/Shared/Assets/Weapons/Resources/WPN_HUM_Longbow_A_1.GR2`, extracted to `L/gamedata/models/`) and an
  animation blueprint (1092f012-d737-b9d2-a91c-d1640cfdfd7c; K weapon_visual_bounds.csv). Its bone names (string
  bone) could not be read: the GR2 is compressed and Divine's convert-model failed (exit 102). The string pull is
  therefore driven by the bow's own blueprint (K §1.3: VisualFlags 0x10000 attachments go through
  AnimationBlueprintSystem+0x240). Bone names Unconfirmed.

## Open leads

1. Slot semantics of EquipRequest opts[0]/[1]: read the executing path (0x34F7B00, the per-request equip
   function it calls) or runtime: send 0x128 with slot 4 for a dagger while a sword is in slot 3, log
   `EquipableComponent`/inventory slot afterwards.
2. Whether SE Lua can create `esv::inventory::WeaponSetChangeRequestOneFrameComponent` (would give a pure-Lua
   "set Melee/Ranged"). Otherwise use the native 0x5BC780 recipe.
3. Name of the component checked by 0x12AFC30 (type index global 0x6052E3C, not in G's map) - expected
   ClientControl/controlled-character.
4. Instant draw via PushUnsheathedState: confirm visually and log the delay between server State change and K's
   0x198B680 attach with `Dummy_R_Hand`.
5. Unsheath animation text-key times (Unsheathed_R/L) - from the animation resources of the unsheath animations.
6. Rolled off-hand attack without Osiris UseSpell's 0x4 option: native CastRequestSystem 0x336E820 request with
   `Target_OffhandAttack` (layout in I/E), or the client Spell task with SetSpell 0x1FB2BF0.
7. WeaponGroup enum values (byte +0x34) to confirm the equip-triggered set switch rule.
8. Bow skeleton bone names (string bone) - decompress the GR2 (LSLib export options) or log the bow visual's skeleton
   bones at runtime.
9. `InputManager.InputDefinitions` offset, if the mod ever wants to fire engine input events directly via 0x40F1550.
