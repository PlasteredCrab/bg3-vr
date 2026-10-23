# Agent M: opening UI screens from code, hotbar spell selection, light sources (bg3_dx11.exe 4.1.1.7631656)

Static analysis only (Ghidra project `BG3_H`, headless, read-only, request server `agents\M\MServer.java`; game data
read from the paks with LSLib Divine, read-only, extracted to `agents\M\gamedata\`). Image base 0x140000000; every
address is an RVA. Raw decompiles / xref dumps: `re-local\ghidra\agents\M\` (cited [file]). "SE" = name or layout
from the Script Extender source (`downloads\bg3se-src`). Builds on F_ui.md, K_weapons.md, E_items_interaction.md,
I_picking_actions.md, C_animation_systems.md section 3.

Status: complete for this pass (2026-10-20); request server stopped. See Open leads.

## Hook points for the VR mod

| Need | RVA | How to use | Confidence |
|---|---|---|---|
| Open/close any UI panel (inventory, character sheet, spellbook, party, map, journal) | **0x20C8340** `UIStateMachine::FireEvent(sm, result[0x40], args, callback[0x40]={0})`, args from **0x20EEA70** `MakeArgs(out[0x20], FixedString* event, short uiPlayer, FixedString* subState(-1), bool remove)`; `sm = *(GameUI+0x810)`, `GameUI = [[0x6047958]+0x130]`; FixedString from **0x4194CA0**([0x604B2E0]+0xC600, &out, {char*,len}) | Fire the named state event the widgets fire: "ToggleInventory" (inventory and character sheet), "ToggleSpells", "TogglePartyPanel", "ToggleJournalMap", "ToggleJournalQuests", "OpenInventory"/"OpenSpellbook" (open-only), "CloseAll", "CloseWidget". Game thread. Table 1.2 | Code path Confirmed (game helpers e.g. 0x121DE10 do exactly this); CustomEvent -> same event Unconfirmed in code, Confirmed in XAML; uiPlayer value Unconfirmed (log `result+0x38`) |
| Input event ids / default keys | ids from 0x1B8E510 (C); defaults `PlayerProfiles/Eoc/keyboard_1.json` (Engine.pak) loaded by **0x584F90**, user overrides `inputconfig_p1..4.json` | ToggleInventory 0xC3 `I`, ToggleCharacterOverview 0xC2 `N`, ToggleSpells 0xC7 `K`, ToggleMap 0xC6 `M`, ToggleJournal 0xC5 `J`, TogglePartyCharacterView 0xD2 `Tab`, ToggleInGameMenu 0xBE `Esc`, UISelectSlot0..12 0xFF..0x10A `0..9,-,=`. Not compiled into the exe | Confirmed |
| Inject an input *event* (not a key) | dispatch **0x40F1550**(im, out[0x48], {FireEventDesc* begin, end}*, 0); desc from InputDefinitions map im+0xE8/+0xEC/+0xF0 | Build a 0x38-byte FireEventDesc (1.4) with Type 1 (press) then 2 (release), call on the game thread | Layout and dispatch Confirmed; out-of-band call Unconfirmed at runtime |
| Prepare (select) a spell for targeting, as a hotbar click | **0x1FE4150** `UseSpell(ecl::Character*, SpellId*)` (SpellId = SE layout, Prototype FixedString at +0x30) | Same call the hotbar's UseSlotCommand handler 0x1718060 -> 0x1721D00 makes. KB/M mode: arms the Spell task (`ic=[char+0xA0]`, `ic->vf[0x68](ic,4)`, 0x1FB2F40, SetSpell 0x1FB2BF0(task, spell, null, 1)) -> normal targeting preview; spells flagged for immediate cast are sent as Cast 0x120 (0x1FFA6E0). Returns 1 when armed. ecl::Character from 0x7841A0 (I) | Confirmed (decompile) |
| Hotbar from Lua instead | `gui::DCHotBar.UseSlotCommand` (+0x958, handler 0x1718060); slot buttons have `Command`/`CommandParameter = VMHotBarSlot` | SE: `btn.Command:Execute(btn.CommandParameter)` on a hotbar slot button | Confirmed (XAML + SE API); runtime Unconfirmed |
| Confirm / cancel targeting | Action1 (4) and ActionCancel (0xAE) into InputController OnInputEvent 0x1B3F680 (I 2.3); CastSystem request iface Confirm 0x11E / Cancel 0x11F (I 2.4) | Trigger -> inject Action1 (raw left mouse or route B event) with the laser ray driving picking (I 1.7); cancel -> ActionCancel event | Confirmed (I) |
| Torch light (game's own) | status **BURNING** on a torch item -> MultiEffectInfo 2156dd48 -> effect **b6314fa1 `VFX_LightSources_WPN_Torch_A_01`** (has a "Light" component) on item bone `Dummy_FX_01`; gameplay light via constellation `EquippableTorch` (radius 7.5) | Give the player a torch item and draw it; it follows `Dummy_R_Hand` (K) | Confirmed (data) |
| Light without a torch | status **LIGHT** (`GameplayLight(7.5,false,0.1)` + effect db2affb3); Osiris `PlayLoopEffect` **0x367C490**(target, effect, bone, scale) -> handle; `StopLoopEffect` 0x367C680 | Server Lua: `Osi.ApplyStatus(char,"LIGHT",-1,1)` or `Osi.PlayLoopEffect(char, "VFX_LightSources_WPN_Torch_A_01_b6314fa1-8ac0-7a52-5865-5a4e9f98ddad", "Dummy_R_HandFX", 1.0)` | Data Confirmed; PlayLoopEffect args Confirmed (disasm); in-game look Unconfirmed |
| Move / tune an engine light each frame | `ls::LightComponent` (proxy, vtable 0x58B9840): WorldTransform +0x08 quat, **+0x18 position**, +0x24 scale; Color +0x88, Radius **+0xB8**, Intensity **+0xBC**, IntensityOffset +0xC0, EdgeSharpening +0xCC, Flags +0xD1, LightType +0xD3, PositionOffset +0xE0 | Write position then call **0x3C6F860**(light) (as ls::LightSystem 0x3D3AFD0 does); after Radius call **0x3D425F0**(light); Intensity/IntensityOffset plain writes (fade callbacks do that). Get the component: 0x189B300(entityWorld, handle) | Confirmed (decompile of LightSystem, ctor 0x3D409F0, fade callbacks) |
| Create a light from a descriptor | **0x3D444E0**(EntityRef*, LightDesc*, scene, u16* cullFlags, ..) -> 0x3D45DC0 -> ctor 0x3D409F0 (LightDesc = SE layout) ; from template **0x3D44130** | Needs an existing ECS entity (EntityRef {handle, world}); used by timeline/character lights (0x1D65EE0) and level lights (0x3C75390) | Code Confirmed; entity creation for a mod-owned light not traced (Open lead) |

---------------------------------------------------------------------------------------------------

## 1. Opening UI screens from code

### 1.1 How a key press becomes an open panel (Confirmed: game data + code)

```
raw key (SDL) --InjectRaw 0x40F1F00--> InputManager raw queue
  InputManager::Update 0x40F0450 -> per raw entry 0x40F1FA0 (bindings) -> FireEventDesc (0x38 bytes)
  -> dispatch 0x40F1550 -> per-event-desc listener, then InputListenerGroup (im+0x2E8)
  -> GameUI input listener 0x1214430 (interface GameUI+8)
       a) per-player widget input handlers (map GameUI+8+0x2EA0 + uiPlayer*0x40, keyed by input event id):
          a widget's  <ls:LSInputBinding BoundEvent="ToggleInventory" Command="{Binding CustomEvent}"
                       CommandParameter="ToggleInventory"/>  executes the DCWidget command CustomEvent
          -> UI state machine event "ToggleInventory" (named, kind 3)
       b) if no widget handled it: state machine event of kind 2 carrying the input event
          (matched in StateMachine XAML as "IE.<InputEventName>", e.g. "IE.ToggleJournal")
  -> UIStateMachine 0x20C8340 -> XAML <ls:StateEvent Name="..."> actions (SetSubstate / PushState /
     RemoveState / RequestCloseWidget) -> widget XAML loaded into MainCanvas
```

Evidence:
* Listener fallback, 0x1214430 [F/d19_gameui_input.c lines 529-531]: builds args `{+0 0, +4 kind=2, +8/+0xC
  FixedString -1, +0x10 0, +0x12 uiPlayer, +0x18 inputEvent}` and calls `0x20C8340(*(GameUI+0x810), &result,
  &args, &callback)`. The UI player id is looked up from the event's InputPlayerIndex (FireEventDesc+0x12) in the
  map `[GameUI+8+0x2DA0]` (+0x74 bucket count, +0x78 buckets, node key byte +8, value short +0xA); 0 = no UI
  player [F/d19 lines 165-187].
* State machine XAML: `Mods/MainUI/GUI/StateMachines/Keyboard.xaml` (KB/M) and `Controller.xaml` (pad), in
  `Data\Game.pak` (not overridden by Patch8_HotFix9/10.pak, checked with the pak listings
  [Patch8_HotFix*_pak_list.txt]). Extracted to `M\gamedata\Keyboard.xaml`, `Controller.xaml`, all GUI XAML in
  `M\gamedata\game\`.
* Widget bindings (KB/M XAML): `PlayerPortraits.xaml` line 239-244, `Minimap.xaml` lines 360-363 and 423-426,
  `HotBar.xaml` line 1106 [gamedata/game/Mods/MainUI/GUI/Pages/].

### 1.2 Input event ids, default keys, and the state event to fire (Confirmed from data)

Input event ids from 0x1B8E510 (C's `input_event_ids.txt`), default keyboard keys from
`PlayerProfiles/Eoc/keyboard_1.json` (Engine.pak), widget/state machine wiring from the KB/M XAML.

| Screen | Input event (id) | Default key | Widget binding -> named state event | State machine result (Keyboard.xaml) |
|---|---|---|---|---|
| Inventory | ToggleInventory (0xC3) | `I` | PlayerPortraits LSInputBinding -> **"ToggleInventory"** | PlayerPanels: SetSubstate `CharacterPanel` Metadata=ToggleInventory; in CharacterPanel "ToggleCharacterPanel" closes |
| Character sheet | ToggleCharacterOverview (0xC2) | `N` | PlayerPortraits LSInputBinding -> **"ToggleInventory"** (same panel) | same CharacterPanel |
| Spellbook | ToggleSpells (0xC7) | `K` | PlayerPortraits LSInputBinding -> **"ToggleSpells"** | SetSubstate `CharacterPanel` Metadata=ToggleSpells |
| Open (never close) | - | - | **"OpenInventory"**, **"OpenSpellbook"**, "OpenAlchemyPanel" (NotificationLib.xaml) | PlayerPanels: SetSubstate CharacterPanel (comment in XAML: "Open if not already, but don't close if it is") |
| Party overview | TogglePartyCharacterView (0xD2) | `Tab` | PlayerPortraits `TogglePartyViewButton` (BoundEvent TogglePartyCharacterView) -> **"TogglePartyPanel"** | SetSubstate `PartyPanel` (IsModal); close: "ClosePartyPanel" / "TogglePartyPanel" / IE.TogglePartyCharacterView |
| Map | ToggleMap (0xC6) | `M` | Minimap `MapBtn` (BoundEvent ToggleMap) -> **"ToggleJournalMap"** | SetSubstate `JournalMap`; in JournalMap "ToggleJournalMap" / IE.ToggleMap -> RemoveState |
| Journal | ToggleJournal (0xC5) | `J` | Minimap `JournalBtn` -> **"ToggleJournalQuests"**; also **"IE.ToggleJournal"** handled directly by PlayerPanels | SetSubstate `JournalQuests`; IE.ToggleJournal in JournalQuests -> RemoveState |
| Alchemy | UIToggleAlchemy (0xF1) | `H` | PlayerPortraits `AlchemyShortcut` -> "ToggleAlchemy" | CharacterPanel Metadata=ToggleAlchemy |
| Game menu | ToggleInGameMenu (0xBE) | `Esc` | (IE.ToggleInGameMenu in many states) | Paused / closes the top panel |
| Close all panels | - | - | **"CloseAll"** | PlayerPanels: RemoveAllSubstates |
| Close one panel | - | - | **"CloseWidget"** (GameUI helper 0x121DE10 fires exactly this) | CharacterPanel/PartyPanel: RequestCloseWidget; Journal*: RemoveState |

Notes (Confirmed from the files):
* keyboard_1.json also has `ToggleInspiration: p`, `ToggleReactions: l`, `ToggleCombatMode: u`,
  `ToggleWeaponSet: f`, `UIToggleTadpolePowers: b`, `UISelectSlot0..9: 0..9`, `UISelectSlot11: -`,
  `UISelectSlot12: =`, `UICancel: escape`, `ActionCancel: escape, mouse:right`, `UIAccept: return`
  [gamedata/engine/PlayerProfiles/Eoc/keyboard_1.json].
* The default binding table is **not compiled into the exe**: 0x584F90 loads `PlayerProfiles/Eoc/keyboard_1.json`
  (string 0x549DC58, `0x411F480(scheme, path, 0, 1)`), then the user's overrides `inputconfig_p1..p4.json`
  (0x549DC80.., `0x411F480(scheme, path, player, 0)`) and `inputremap_p1..4.json` [d1_input.c 0x584F90 lines
  49-222]. Engine-level defaults (UI mouse, widget keys) come from `PlayerProfiles/Default/inputconfig.json`
  (string 0x5899820, loaded by 0x3AC84D0) and the pad defaults from `inputconfig_controller.json`, both in
  Engine.pak. A user can rebind every one of these, so injecting raw keys is not robust.
* In controller mode the game uses `Controller.xaml` and the `_c` widgets, where the wiring differs (radial
  menus); everything here assumes KB/M mode, which F_ui.md recommends keeping for the laser.

### 1.3 Route A (recommended, native): fire the UI state event directly

The game itself fires named state events through two functions; e.g. GameUI helper **0x121DE10(GameUI, short
uiPlayer)** = "CloseWidget" for that player [d5_fire_helpers.c]:

```c
// all on the game thread (the UI listener that normally does this runs inside InputManager::Update)
GameUI* ui  = *(GameUI**)(*(uintptr_t*)(base + 0x6047958) + 0x130);   // F 1.1
void*   sm  = *(void**)((char*)ui + 0x810);                           // UIStateMachine (SE StateMachineComponent)
uint32_t fs = 0xFFFFFFFF;  struct { const char* p; uint32_t n; } sv = { "ToggleInventory", 15 };
FS_Create(*(void**)(base + 0x604B2E0) + 0xC600, &fs, &sv);           // 0x4194CA0, SE MainTable::CreateFromString
uint32_t none = 0xFFFFFFFF;
uint8_t args[0x20];  MakeArgs(args, &fs, uiPlayer, &none, 0);        // 0x20EEA70
uint8_t result[0x40], callback[0x40] = {0};                          // callback: {vtbl* = 0, storage}
FireEvent(sm, result, args, callback);                               // 0x20C8340
// then DecRef the FixedStrings at args+8, args+0xC and fs: 0x41950A0([0x604B2E0]+0xC600, &fs)
```

* **0x20EEA70 MakeArgs(out, FixedString* stateEvent, short uiPlayer, FixedString* subState, bool remove)**
  [d4_smargs.c]: writes `+0 = 0` (EventIndex), `+4 = 3` (kind: named event), `+8 = stateEvent` (IncRef
  0x4170950), `+0xC = subState`, `+0x10 = remove`, `+0x12 = uiPlayer`, `+0x18 = 0`. Matches SE
  `UIStateMachine::EventArgs`. Confirmed.
* **0x20C8340 FireEvent(UIStateMachine*, EventResult* out, EventArgs*, Callback*)** [d3_sm.c]: takes the CS at
  `sm+0x4B0`; if the machine is busy (`sm+0x8F`/`+0x8A`) a kind 1/3 event is queued in `sm+0x700` (processed later
  by 0x20C7A30), otherwise processed now through 0x20C8AE0; `result+0x38` = uiPlayer, `result+0x3B` bit 1 = "handled".
  Destroys the callback via its vtable if non-null. Confirmed.
* FixedString creation **0x4194CA0(MainTable = [0x604B2E0]+0xC600, uint32* out, {char*, u32 len}*)**: same call
  shape at 0x4262BA9 and 0x20F7366 (disassembly), SE `GlobalStringTable::MainTable::CreateFromString`. Confirmed.
* `uiPlayer`: the value the listener derives from the InputPlayerIndex map (1.1). **Unconfirmed** which number
  the first local player has (non-zero; very likely 1): confirm by logging `result+0x38` from a hook on
  0x20C8340 while pressing `I`, or by reading the map at `[GameUI+0x2DA8]`.
* Effect identical to the key press for every row of 1.2 whose path goes through a widget LSInputBinding
  (Inventory, Character sheet, Spellbook, Party, Map, Journal buttons). For rows handled as `IE.<name>`
  (e.g. IE.ToggleJournal) fire the named event that the XAML lists in the same state instead (e.g.
  "ToggleJournalQuests"), or use route B.
* **Unconfirmed in code**: that DCWidget's `CustomEvent` command turns its string parameter into exactly a kind-3
  event with `StateEvent = parameter` (the command object was not decompiled; `CustomEvent` is a property of
  `ls.ViewModel` at field +0x1D0 per the reflection builder 0x20F8A10/0x20F8E30, and `ls.DCWidget` derives from
  it, 0x20EACC0). The state machine XAML only ever names these strings as `StateEvent Name`, so the match is
  strong; a runtime test of route A with "ToggleInventory" settles it.

### 1.4 Route B (native): inject the input *event* rather than a raw key

* Event descriptors: InputManager `InputDefinitions` hash map at **im+0xE8** (count), **+0xEC** (bucket count),
  **+0xF0** (buckets); node = `{next, u32 id at +8, InputEventDesc at +0x10}`; desc `+0 EventID, +4 Flags,
  +8 Type mask (1 Press, 2 Release, 4 ValueChange, 8 Hold, 0x10 Repeat, 0x20 AcceleratedRepeat), +0xC category,
  +0x10..+0x2F per-player listener[4], +0x30 name` [d2_fire.c 0x40F36F0 = register event]. Confirmed.
* FireEventDesc (0x38 bytes, SE layout matches the writes in 0x40F3250/0x40F3420/0x40F3500): `+0 InputEventDesc*`,
  `+8 u8 player`, `+0xC int EventId`, `+0x10 u16 DeviceId`, `+0x12 u8 InputPlayerIndex`, `+0x13 u8 Type`,
  `+0x14 OldValue {f,f,u8}`, `+0x20 NewValue {f,f,u8}`, `+0x2C WasPreferred`, `+0x30 u16 DeviceId`,
  `+0x32 u16 raw input id`. Confirmed (writes).
* Dispatch **0x40F1550(im, out[0x48], {FireEventDesc* begin, FireEventDesc* end}*, 0)**: per event, the
  descriptor's per-player listener (`desc+0x10+player*8`, `vf[0x10]` filter, `vf[0x38]` OnInputEvent), then every
  listener in the InputListenerGroup at im+0x2E8 via 0x40F1970 [d2_fire.c]. This is how Update 0x40F0450 delivers
  the queued press/release arrays [d1_input.c lines 382-412]. Calling it from the game thread with a one-element
  array `{desc, 0, desc->EventID, dev 0, idx 0, Type 1 (press), old {0,0,0}, new {1,1,1}}` then a release (Type 2)
  should be equivalent to a bound key. **Unconfirmed** (never observed at runtime; the hold/repeat bookkeeping in
  im+0x68 is bypassed, harmless for toggles). Route A is simpler for UI panels; route B is the general way to fire
  gameplay events (e.g. UISelectSlot0..12, ToggleWeaponSet) without knowing the user's binding.
* Route C (robust raw fallback, uses only the confirmed InjectRaw): read the *current* binding of the event from
  the InputScheme (SE `InputScheme.InputBindings[player]`: map event id -> array of `InputBinding {u16 InputId,
  u16 DeviceId, u8 Modifiers, ...}`) and inject that raw input with 0x40F1F00. The scheme offset inside the
  InputManager is SE-only here (Unconfirmed); `0x411F480(scheme, ...)` in 0x584F90 is the loader that fills it.

### 1.5 Route D (Script Extender Lua, client)

* SE `Ext.UI.GetRoot()` returns `GameUI->NoesisUIManager.MainCanvas` and SE can read Noesis properties and call
  `ICommand:Execute(arg)` (`GameDefinitions/PropertyMaps/UI.inl` lines 96-106; `NsHelpers.inl` 660-678). SE has no
  Lua string -> Noesis BoxedValue conversion (`Builtins.inl` line 469 "TODO boxed types"), so pass an existing
  element's `CommandParameter`:

```lua
Ext.UI.Defer(function()
  local root = Ext.UI.GetRoot()
  local btn = root:Find("MapBtn")                -- Minimap.xaml, CommandParameter "ToggleJournalMap"
  btn.Command:Execute(btn.CommandParameter)      -- same as clicking the map button
end)
```

  Elements with usable Command/CommandParameter pairs (all `CustomEvent`): `MapBtn` ("ToggleJournalMap"),
  `JournalBtn` ("ToggleJournalQuests"), `TogglePartyViewButton` ("TogglePartyPanel"), `AlchemyShortcut`
  ("ToggleAlchemy"); unnamed LSInputBindings in PlayerPortraits ("ToggleInventory", "ToggleSpells") must be found
  by walking the visual tree. `Find` / tree walking: SE NsHelpers (VisualHelpers). Confirmed from SE source and
  XAML; runtime Unconfirmed. `Ext.UI.SetState` is a stub in this SE (`ERR("Deprecated")`, Module.inl line 80).
* SE also offers `Ext.Input.InjectKeyPress(scancode)` (raw SDL key, ClientInput.inl), i.e. route C with the key
  hard-coded.

---------------------------------------------------------------------------------------------------

## 2. Hotbar spell selection from code

### 2.1 What a hotbar click does in the data (Confirmed from XAML)

* Every hotbar slot button uses style `HotBarSlotStyle` (`Public/Game/GUI/Library/DataTemplates.xaml` lines
  1546-1548): `Command = DataContext.UseSlotCommand` of the owning widget (`gui::DCHotBar`),
  `CommandParameter = {Binding}` = the slot's `ls.VMHotBarSlot`, `BoundEvent = {Binding BoundEvent}` = the slot's
  key event (UISelectSlot0..12, ids 0xFF..0x10A). So pressing `1`..`=` and clicking the icon run the same command
  with the same slot view-model.
* Fixed buttons in HotBar.xaml: `MainAttack` (DataContext `PlayerCharacterProperties.MainMeleeSpell`,
  line 1357), `LaunchDefaultActionCommand` with "Jump" / "Shove" / "Throw" / "Hide" (LSInputBindings lines
  1079-1085, BoundEvents Jump / Shove / Throw / ToggleSneak), `CancelBinding` BoundEvent ActionCancel (line 4537).

### 2.2 gui::DCHotBar commands (Confirmed: reflection + constructor)

Type registration 0x1712C90 (`"gui::DCHotBar"`, builder 0x1723B50 -> property list 0x1712CD0 [d9_useslot.c]);
constructor 0x1713B50 binds each command to a member handler (`lea rax, handler` ... `call 0x2119670(cmd,
this+0x40, delegate)` ... `mov [rdi+off], rax`, disassembly `dis_1713B50.txt`):

| Field | Command (reflection 0x1712CD0) | Handler |
|---|---|---|
| +0x958 | UseSlotCommand | **0x1718060** |
| +0x960 | UseWeaponSpell | 0x171CD10 |
| +0x968 | LockHotBarCommand | 0x171D2E0 |
| +0x970 | DropOnHotBarCommand | 0x171BFF0 |
| +0x978 | DragSlotCommand | 0x171B780 |
| +0x980 | FilterActionResourceCommand | 0x171D460 |
| +0x988 | FilterCantripsCommand | 0x171DEA0 |
| +0x990 | ClearSingleHotbarCommand | 0x171E460 |
| +0x998 | UseMeleeWeaponCommand | 0x171E7A0 |
| +0x9A0 | UseRangedWeaponCommand | 0x171F300 |
| +0x9A8..+0x9C8 | SaveColumns / SetColumns / SetIsShowingCustomDeck / SetIsShowingPassivesDeck / SetCurrentShownDeck | 0x171FE90, 0x171FEB0, 0x171FED0, 0x17200B0, 0x1720290 |

(`gui::DCSelectionFlyOut` has its own UseSlotCommand at +0x830, reflection 0x179B590.)

### 2.3 UseSlotCommand handler 0x1718060 -> 0x1721D00 -> 0x1FE4150 (Confirmed) [d10_useslot.c, d11_usespell.c, d12_usespell2.c]

* 0x1718060(DCHotBar*, Ptr<arg>*, BaseComponent* slotVM): normal path (not in swap/drag mode `+0x8F0`): resolves the
  player's character (`0x7841A0(world, handle)` -> `ecl::Character*`, I 2.1), checks it can act, then for a spell slot
  calls **`0x1721D00(dcHotBar, character, &VMCharacterAction+0x408 /*SpellId*/, 1)`** followed by `0x1722C00`.
  Items go through 0x1719BE0 / 0x1721640, passives/upcast variants through 0x169EE20 / `ls.VMUpcast`.
* 0x1721D00: if the character is currently casting (spell-cast state 4, or 7 with a check) it queues a request on
  `ecl::spell_cast::CastSystem` (type id 0x6059194; object = `[[0x5FDA240]+0x1A8]+0x30` table entry `id*0xF8`) at
  **CastSystem+0x208** (0x28-byte entries, same shape as helper **0x1EB1FF0**) and remembers the spell in
  `DCHotBar+0xCB0..+0xCC8` to use afterwards; otherwise, without a pending item target (`+0xC10` invalid), it calls
  **`0x1FE4150(character, spellId)`**.
* 0x1FE4150 `UseSpell(ecl::Character* ch, SpellId* id)`: looks up the prototype by `id+0x30`
  (`0xEF4560(DAT_145FFE830+8, ...)`); in KB/M mode (`byte 0x614FBC8 == 0`): `ic = ch[0x14]` (= `ch+0xA0`,
  InputController, I 2.1); returns 0 if the running task is already the Spell task with this spell (`0xF1ACB0`);
  `task = ic->vf[0x68](ic, 4)`; `0x1FB5B40(task,0)` must succeed; `0x1FB2F40(task)`; then
  **SetSpell 0x1FB2BF0(task, id, nullEntity DAT_145EEB6E0, 1)** and returns 1: the Spell task becomes the preview
  task (I 2.3/2.4: StartPreview 0x11C ... Confirm 0x11E). For prototypes flagged for immediate casting
  (`proto+0x10` bit 35) it builds a target list for the caster and sends **Cast 0x120** through `0x1FFA6E0` (I 2.4).
  Controller mode adds a branch for some spell types (case 6).
* SpellId (SE `SpellMetaId` + Prototype): `+0 FixedString OriginatorPrototype, +8 u8 SourceType, +0x10 Guid Source,
  +0x20 Guid ProgressionSource, +0x30 FixedString Prototype`. The mod can copy it from the character's spell book
  (SE `eoc::spell::BookComponent`, Lua `entity.SpellBook.Spells[i].Id`) or from a hotbar slot VM
  (`VMCharacterAction+0x408`).

### 2.4 Recommended wrist-menu spell selection

1. Native, game thread: `ch = 0x7841A0(world, playerHandle)`; `0x1FE4150(ch, &spellId)` -> the targeting preview
   starts exactly as after a hotbar click. If a cast is in progress, cancel first (ActionCancel event), or call
   0x1721D00 which does the cancel + queue itself (needs a DCHotBar pointer).
2. Or fire input event UISelectSlotN (route B, 1.4) for the visible hotbar row (only 12 slots are bound).
3. Or SE Lua: walk `Ext.UI.GetRoot()` to the HotBar widget's slot `LSButton`s and `btn.Command:Execute(btn.CommandParameter)`.
4. Confirm: inject Action1 (id 4; default raw = mouse left, I 2.2) with the controller ray substituted for the mouse
   ray (I 1.7). Cancel: ActionCancel (0xAE; default keys Esc / right mouse). Both via route B or raw keys.

---------------------------------------------------------------------------------------------------

## 3. Light sources for a VR torch

### 3.1 How a held torch is lit (Confirmed from game data)

Chain (all files extracted under `M\gamedata\`):
1. Stats `WPN_Torch` (`Shared Weapon.txt` line 1202): `Flags "Torch"`, `Weapon Properties "...;Torch"` (K: WeaponFlags
   Torch 0x4000), **no light boost**. Lit variant `WPN_Torch_StartLit`. A torch is lit by the status **BURNING**
   (`DIPPED_FIRE_SWITCH`: `IF(Tagged('TORCH')):ApplyStatus(BURNING,100,-1)`); BURNING's own damage functors are
   skipped for `Tagged('TORCH')` (Status_BOOST.txt lines 691-711). BURNING has **no GameplayLight boost**.
2. Root template `50c43f27-...` "WPN_HUM_Torch_A" (Shared RootTemplates, `rt/shared_rt.lsx` line 233411):
   `ConstellationConfigName = "EquippableTorch"` with parameters `GameplayLight = true`, `GameplayLightRadius = 7.5`,
   `GameplayLightSharpness = 0.1`, `PreviewEffect = VFX_LightSources_WPN_Torch_A_01_b6314fa1-...`.
3. Constellation (Larian "anubis" scripts, `Mods/Shared/Scripts/constellations/`): `EquippableTorch.cln` ->
   `BurningItem.cln` -> `StatusDrivenLight.cln` (Status = "BURNING") -> `Light.cln`. The torch state calls
   `EnableStatusVisual(me, "BURNING")` with the comment "equipped items cannot normally have a VFX - one exception
   being status VFXs", turns on when `Equipped` / `Unsheathed`, and `Light.cln` calls
   `SwitchGameplayLight(me, on, radius, checkLOS=false, sharpness, isSunlight=false, offset, verticalLimit)`.
   The exe registers these as `ls.anubis.game.SwitchGameplayLight`, `...EnableStatusVisual`,
   `...SetupScriptedLight`, `...SwitchOnScriptedLight` (strings 0x5756400, 0x57537C8, 0x5756B80, 0x5759A00).
4. So the **visible light** of a held torch is the BURNING status VFX: MultiEffectInfo `2156dd48-...`
   ("BURNING_StatusEffect", `fx/..._2156dd48...lsx`) has an entry for torches (TargetTag a8139e1a-...) playing
   effect **b6314fa1-8ac0-7a52-5865-5a4e9f98ddad `VFX_LightSources_WPN_Torch_A_01`** on the item's bone
   **`Dummy_FX_01`**. That effect (LightSources effect bank, `fx/..._LightSources__merged.lsf.lsx` line ~20183,
   looping, CullingDistance 60) contains an effect component named **"Light"** (position offset z -0.25) plus the
   flame particles. The **gameplay light** (stealth/visibility, not rendering) is separate:
   `esv::light::GameplayLightSystem` / `eoc::GameplayLightComponent` (SE: Active, Radius, EdgeSharpening,
   SpotlightAngle, DirectionalDimensions, LightType).
5. Why it follows the hand: the effect is bound to the torch item's visual bone, and the torch visual is attached
   to `Dummy_R_Hand` / `Dummy_L_Hand` (K_weapons.md). With the mod's IK in skeleton slot 30 the whole chain follows
   the controller.

### 3.2 Light on the player without a torch (Confirmed from game data)

| Option | Data | Effect |
|---|---|---|
| Status **LIGHT** (the Light cantrip) | Status_BOOST.txt 3399: `Boosts "GameplayLight(7.5,false,0.1)"`, `StatusEffect 9e863d8f-...` = MultiEffectInfo "LIGHT_StatusEffect" -> effect `db2affb3-57a7-76f9-9315-d1783cdfc576`, Pivot Target, no bone (entity root) | Server: `Osi.ApplyStatus(char, "LIGHT", -1, 1)` (the game's own goals do exactly `ApplyStatus(_Weapon, "LIGHT", -1.0, 0, NULL...)`; Osiris handler 0x36967F0, E). The spell normally targets the main-hand weapon (`ApplyEquipmentStatus(MainHand,LIGHT,100,-1)` in Target_Light). Applied to the weapon it follows the hand; applied to the character it sits at the character root. |
| Other light statuses | DANCING_LIGHTS / PRODUCE_FLAME / BRANDING_SMITE `GameplayLight(9,..)`, DAYLIGHT `GameplayLight(15,false,0.1,true)` (sunlight flag), FLAME_BLADE / LIGHT_FLAME_BLADE `GameplayLight(3,..)`, MOONBEAM_AURA 2, FAERIE_FIRE 3 | Same mechanism: GameplayLight boost + status VFX. |
| **ActiveCharacterLight(guid)** boost | Darkvision passives/statuses: `ActiveCharacterLight(233033a1-...)`, `(e278f6a0-...)`, `(051648e6-...)`, `(c46e7ba8-...)` (Passive.txt 88/109/507/1013..., Status_BOOST 2321, 9290) | Client `ecl::CharacterLightSystem` / `ecl::CharacterLightComponent` (character light rig). The GUIDs are not in `CharacterLight_Shared/Gustav.lsx` (those hold the per-region "PL_*" key/fill/rim rigs); where they are defined: Open lead. |
| Custom status (SE stats override) | e.g. a status with `Boosts "GameplayLight(r,false,0.1)"` and `StatusEffect` = a MultiEffectInfo that plays `VFX_LightSources_WPN_Torch_A_01` with `TargetBone = "Dummy_R_HandFX"` | Hand-following torch light on the character without an item. MultiEffectInfo TargetBone is the documented mechanism (BURNING uses it). Unconfirmed: whether a status VFX on a *character* honours TargetBone the same way (runtime test). |
| Osiris effects | `PlayLoopEffect` 0x367C490 / `StopLoopEffect` 0x367C680 / `PlayEffect` 0x367C310 (E's Osiris table); goal usage `PlayEffect(_Character, VFX_..., "Dummy_OverheadFX", 1.0)` | `Osi.PlayLoopEffect(player, "VFX_LightSources_WPN_Torch_A_01_b6314fa1-8ac0-7a52-5865-5a4e9f98ddad", "Dummy_R_HandFX", ...)` attaches the torch flame + light to a hand bone. Argument list of PlayLoopEffect: see 3.4. |


### 3.3 Engine light component (Confirmed) [d13_lightsys.c, d14_lightfns.c, d15_lightctor.c, d16_lightfade.c]

* Type ids: `ls::LightComponent` 0x605BC20 (query) / storage index word **0x60A6728**; `ls::LightSystem` 0x6058D28
  (registration 0x3AE3D30 -> callback 0x3AE7F00 -> body **0x3D3AFD0**); `ecl::CombinedLightSystem` 0x6059178
  (0x741C50 -> 0x75F430 -> 0x1A328B0); `ls::LightFlickerComponent` 0x60CEAD0, LightMovement 0x60CEAD4 (written
  by 0x3D44130); `eoc::GameplayLightComponent` 0x605161C; `esv::light::GameplayLightSystem` 0x605167C
  ([s1_lightsys.txt], [t1_lightcomp.txt]).
* Layout of `ls::LightComponent` (0x110 bytes, ctor from LightDesc **0x3D409F0**, vtable 0x58B9840 over MoveableObject
  0x58B2F58); every offset below is written by that ctor and matches SE `ls::LightComponent` / `LightDesc`:
  `+0x08` quat rotation, `+0x18` position, `+0x24` scale (MoveableObject WorldTransform), `+0x30` LocalTransform*,
  `+0x38` scene, `+0x58..+0x6F` local bound, `+0x80` owner entity, `+0x88` Color (vec3), `+0x94/+0x98` spot inner /
  outer angle, `+0x9C` DirectionLightDimensions, `+0xA8/+0xAC/+0xB0` attenuation side/end/side2, `+0xB4` Kelvin,
  **`+0xB8` Radius**, **`+0xBC` Intensity**, `+0xC0` IntensityOffset (flicker), `+0xC4` ScatteringIntensityScale,
  `+0xC8` Gain, `+0xCC` EdgeSharpening, `+0xD0` attenuation function, `+0xD1` flags (1 UseTemperature, 2 Flickering,
  4 Moving, 8 CastShadow, 0x10 VolumetricShadow, 0x20 Enabled, 0x40 PreExpose, 0x80 from desc FlatFalloff),
  `+0xD2` light channel, `+0xD3` LightType (0 point, 1 spot, 2 directional), `+0xD4` blackbody colour from Kelvin
  (0x4129000), `+0xE0` PositionOffset, `+0xF0` cookie texture, `+0x100` scene/handle, `+0x108` cull flags (0x601),
  `+0x10C` UUID.
* Per-frame `ls::LightSystem` body 0x3D3AFD0: (a) for each entity in the system's explicit-update list (`+0x130`,
  count `+0x13C`; SE `ExplicitUpdateTransform`) copies its `ls::TransformComponent` (0xE59570) into the light
  (`+0x08` quat, `+0x18 = pos + PositionOffset(+0xE0)`, `+0x24` scale) and calls **0x3C6F860** (MoveableObject
  bound + scene update under the scene SRW lock); (b) fade maps: EdgeSharpening / Radius / Intensity /
  IntensityOffset (callbacks 0x3D3C410 -> `+0xCC`; 0x3D3C3A0 -> `+0xB8` then **0x3D425F0** = recompute local bound by
  light type, then 0x3C6F860; 0x3D3C340 -> `+0xBC`; 0x3D3C2E0 -> `+0xC0`); (c) flicker: `+0xC0 =
  (sin(a*t+s) + sin(1.1*a*t))*amount`; (d) movement: `+0xE0..+0xE8` sine offsets, position = base + offset, then
  0x3C6F860.
* Moving a light each frame from the mod: on the game thread write `+0x18` (and `+0x08`) then call
  `0x3C6F860(light)`; radius: write `+0xB8` and call `0x3D425F0(light)`; Intensity / IntensityOffset / EdgeSharpening
  are plain writes (that is all the fade callbacks do). Colour `+0x88`: plain field; whether the renderer needs any
  further refresh is Unconfirmed. Lookup: `0x189B300(entityWorld, entityHandle)` returns the component (used by
  0x3D3AFD0, 0x3D3C3A0, 0x2004170, 0x19E21D0).
* Creation: **0x3D444E0(EntityRef* {handle, EntityWorld*}, LightDesc*, scene, u16* cullFlags, ..)** (component via
  0x3D45DC0 -> ctor; flicker/movement from LightDesc +0x6D/+0x6E; Transform through `ls::TransformSystem` 0x3C78110;
  attached to the scene with 0x3CD2F20); template variant **0x3D44130(EntityRef*, LightTemplate*, scene, ...)**.
  Callers: 0x1D65EE0 (timeline lights), 0x3C75390 (from 0x19E21D0 / 0x2003B10 / 0x1B77810 / 0x3F4BD70
  "Appearance.Light UUID"), 0x4234E70 / 0x4235720, 0x1A32C50 (CombinedLight) [x9_lightcreate.txt,
  x10_lightcallers.txt].
* Effects: the torch effect's "Light" component becomes a light at runtime (SE `aspk::LightComponent :
  FxBaseComponent`); the code path from the AllSpark component to a `ls::LightComponent` was not traced (0x2001D50 /
  0x2002690 call 0x189B300 and are reached from 0x1F20BA7 / 0x1D638B1; Unconfirmed which belongs to effects).

### 3.4 Recommendation for the VR torch

1. Simplest, all game-driven: equip a real torch (`WPN_Torch_StartLit`, template 8d175e5e-...) in a hand; the BURNING
   status VFX light and the gameplay light follow `Dummy_R_Hand` / `Dummy_L_Hand`, which the IK already drives (K).
2. Torch-less light: server Lua `Osi.PlayLoopEffect(char, "VFX_LightSources_WPN_Torch_A_01_b6314fa1-8ac0-7a52-5865-5a4e9f98ddad", "Dummy_R_HandFX", 1.0)`.
   Arguments from the handler 0x367C490 (disassembly): arg0 target (0x2F60F50), arg1 effect resource (0x2F62000,
   must be loaded: `res+0xA0`), arg2 bone string -> FixedString, arg3 float scale (written to effect `+0x24`), arg4 out
   = effect handle (`[rsi+0x48]`); creates through 0x2D66620. Add `Osi.ApplyStatus(char, "LIGHT", -1, 1)` when the
   stealth/visibility gameplay light matters. Stop with `Osi.StopLoopEffect(handle)` (0x367C680).
3. Fully native headlamp: take the effect's (or any) `ls::LightComponent`, each frame write `+0x18` + call 0x3C6F860,
   tune `+0xB8` (then 0x3D425F0) / `+0xBC` / `+0x88`. A brand-new light needs an ECS entity first (Open lead 4).

---------------------------------------------------------------------------------------------------

## 4. Function / data index

| rva | name |
|---|---|
| 0x6047958 -> +0x130 | GameUI (F); `GameUI+0x810` UIStateMachine*, `+8` input-listener interface |
| 0x1214430 | GameUI input listener (fires kind-2 "IE." state events when no widget handled the input event) |
| 0x20C8340 / 0x20EEA70 / 0x20C8AE0 / 0x20C7A30 | UIStateMachine FireEvent / MakeArgs / process now / process queued |
| 0x121DE10 | GameUI helper: fire "CloseWidget" for a UI player |
| 0x13F4050 | fires "SplitItemToggle" (FixedString 0x5EF0BC4) |
| 0x20F7320 | FixedString globals CloseWidget 0x5EF0BE0, SplitItemToggle 0x5EF0BC4, MainMenu, LoadingScreen, OpenMessageBox, ... |
| 0x20F8A10 / 0x20F8E30 | `ls.ViewModel` reflection (CustomEvent field +0x1D0, OpenMessageBox +0x1D8); `ls.DCWidget` derives from it (0x20EACC0) |
| 0x4194CA0 / 0x4170950 / 0x41950A0 | FixedString create / IncRef / DecRef (MainTable = [0x604B2E0]+0xC600) |
| 0x40F0450 / 0x40F1FA0 / 0x40F1550 | InputManager Update / raw entry -> events / dispatch events to listeners |
| 0x40F3250 / 0x40F3420 / 0x40F3500 | queue Release / Repeat / AcceleratedRepeat FireEventDesc |
| 0x40F36F0 | register input event (InputDefinitions map im+0xE8..+0xF0) |
| 0x584F90 | load keyboard_1.json + inputconfig_pN.json + inputremap_pN.json |
| 0x3AC84D0 | load PlayerProfiles/Default/inputconfig.json |
| 0x1712C90 / 0x1712CD0 / 0x1713B50 | gui::DCHotBar type / reflection / ctor |
| 0x1718060 | DCHotBar UseSlotCommand handler |
| 0x1721D00 | DCHotBar use spell (cancel + queue if casting, else 0x1FE4150) |
| 0x1FE4150 | UseSpell(ecl::Character*, SpellId*) |
| 0x1EB1FF0 / 0x1EB1EB0 | CastSystem request queues (+0x208, 0x28-byte entries; +0x228, 0x48-byte entries) |
| 0x367C490 / 0x367C680 / 0x367C310 | Osiris PlayLoopEffect / StopLoopEffect / PlayEffect |
| 0x3D3AFD0 | ls::LightSystem update |
| 0x3D444E0 / 0x3D44130 | create LightComponent from LightDesc / LightTemplate |
| 0x3D45DC0 / 0x3D409F0 | emplace / construct LightComponent |
| 0x3D425F0 | light: recompute bound after radius / angle change |
| 0x3C6F860 | MoveableObject: update world bound + scene after transform change |
| 0x189B300 | get ls::LightComponent of an entity |

---------------------------------------------------------------------------------------------------

## Open leads

1. **UI player id** for the first local player (route A argument): log `result+0x38` in a hook on 0x20C8340 while
   pressing `I`, or dump the map at `[GameUI+0x2DA8]` (+0x74 / +0x78).
2. **CustomEvent command object**: decompile the code that fills ViewModel+0x1D0 for `ls.DCWidget` (DelegateCommand
   made by 0x2119670, as in DCHotBar's ctor) to prove CustomEvent(param) == FireEvent(kind 3, param). One runtime
   call of route A with "ToggleInventory" settles it too.
3. **Route B** (0x40F1550 out of band): one runtime test with ToggleInventory press/release; check that the hold /
   repeat bookkeeping (im+0x68) does not leave the event latched.
4. **Mod-owned light entity**: locate ecs EntityWorld::CreateEntity and the scene pointer the callers of 0x3D444E0
   pass, or reuse the effect light from PlayLoopEffect; trace the AllSpark "Light" component to a LightComponent
   (start at 0x1F20BA7 -> 0x2001D50 and 0x1D638B1 -> 0x2002690).
5. **ActiveCharacterLight GUIDs** (233033a1-..., e278f6a0-..., 051648e6-..., c46e7ba8-...): find the bank defining
   them (not in `Timeline/CharacterLight/*.lsx`); then judge a custom `ActiveCharacterLight(<guid>)` boost as a
   "player glow".
6. **Status VFX with TargetBone on a character** (custom MultiEffectInfo bound to `Dummy_R_HandFX`): runtime check
   that the binding follows the IK-edited hand bone like it does on items.
7. **Colour refresh** (+0x88): confirm at runtime that a colour write alone is picked up next frame.
8. **CastSystem queues** 0x1EB1FF0 / 0x1EB1EB0: name them from the CastSystem update 0x75F670 body (I's lead 6); only
   needed to cancel a cast without injecting ActionCancel.
9. **Controller-mode wiring** (`Controller.xaml`, `_c` widgets): only if the mod ever leaves KB/M mode.
