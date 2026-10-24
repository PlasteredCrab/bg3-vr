# Physical dice: the ActiveRoll flow, triggering it, reading the result, hiding the UI dice - agent AQ, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_F, read-only, headless, request server
`agents\AQ\AQServer.java`, now stopped) plus the UI XAML that agent M extracted (`agents\M\gamedata\game\...`:
`Mods/MainUI/GUI/Pages/ActiveRoll.xaml`, `Public/Game/GUI/Library/DiceAnimation.xaml`,
`Mods/MainUI/GUI/StateMachines/Keyboard.xaml`). RVAs are relative to 0x140000000. Raw output is in
`re-local\ghidra\agents\AQ\` (aq1..aq34; `py\strx.py` = string + RIP-reference finder, `py\dispw.py` = byte scan for
`[reg+disp32]`). "SE" = name or layout from `downloads\bg3se-src`. Type names come from G's `typeidmap.txt`.
Builds on AB_ui_system.md (GameUI, FireEvent, WindowManager), M_ui_open_and_light.md (route A/B), I_picking_actions.md.

**Confirmed** = shown by the cited code or XAML. **Unconfirmed** items name the log that settles them (section 6).

## Summary

| what | where | how | confidence |
|------|-------|-----|------------|
| A roll is about to be shown | **0x1213B30** `GameUI::OpenActiveRoll(ui, short player, EntityHandle* rollEntity, bool inDialog, callback*)` | Creates `gui::DCActiveRoll` (alloc 0xE40, ctor **0x1544DD0**), registers it as custom DC "ActiveRoll", fires state event **"ActiveRollOpen"** (FixedString global 0x5EF032C) or, when `inDialog == 1`, **"DialogActiveRollOpen"** (FS at 0x6094C08) through FireEvent 0x20C8340. Called by the GameUI roll listener **0x121E680** (subscribed to `ecl::RollSystem` by 0x12199B0) and the deferred queue 0x121C130 | Confirmed |
| The roll's data model | `gui::DCActiveRoll` (type 0x1543CA0, reflection 0x1543CE0, vtable 0x55B3CD8) | Fields below (2.2). Roll entity **DC+0xCC8**; per-frame update **0x1548FC0** (vtable +0x58); init **0x1546360** (vtable +0x28) | Confirmed |
| Phase of the roll | **DC+0x7A8** `RollState` (u32) | 0 IntroductionAnimation, 1 **WaitForStart**, 2 WaitForReRoll, 3 **StartRoll** (UI dice animating), 4 StopRoll, 5 **ResultReady** (enum builder 0x154ECB0) | Confirmed |
| DC / modifiers / advantage | DC+0x898 TargetNumber (u8, `clamp(DC - bonus, 1, 20)`), DC+0x8B0 / +0x8C8 Min/MaxBonusValue, DC+0xD14 InspirationPoints, VMRoll `*(DC+0x940)`: +0xD0 DifficultyCheck, +0x170 RollAdvantageType, +0xB8 ShowDC, +0x1D8 ProficiencyBonus, +0x1F0 RollBonus, +0x208 AbilityValue | Read in the DC update detour | Confirmed (getters) |
| The player's "Roll" press | **0x15475D0** `UserTriggeredNextPhaseCommand` handler `(dc, 0, BaseComponent* param)` | In state 1 it sends **net message 0xEA (RequestedRollMessage)** with action **0** and the roll GUID; in state 2 action 1; in state 3 action **5** (skip); in state 5 it continues (0x1547B20, action 3). Needs `CanRespondToCommands` DC+0x928 = 1 | Confirmed |
| Trigger it natively | call **0x15475D0(dc, 0, nullptr)** | From a detour on the DC update 0x1548FC0 (`rcx` = dc), when `RollState == 1` and `+0x928 == 1` | Code Confirmed; runtime Unconfirmed |
| Trigger it by input | input event **UIAccept 0xDF** (route B, M 1.4) | ActiveRoll.xaml `LSInputBinding x:Name="UserTrigger" BoundEvent="UIAccept" Command=UserTriggeredNextPhaseCommand` | Confirmed (XAML); runtime Unconfirmed |
| Trigger it from SE Lua | `dc.UserTriggeredNextPhaseCommand:Execute()` on the ActiveRoll widget's DataContext, inside `Ext.UI.Defer` | SE `Execute` passes nullptr when no argument (NsHelpers.inl l.665); the handler ignores the parameter in state 1 | Confirmed (source); runtime Unconfirmed |
| Server reply "rolled" | client handler **0x1FF8320** for message 0xEA | Server action 2 -> event kind 0 -> DC state 3 StartRoll (0x1548840 case 0). Mapping server action -> kind: 2->0, 3->1, 4->3, 5->2, 6->4, 7->5 | Confirmed |
| Natural d20 + total + success on the client | replicated **`eoc::RequestedRollComponent`** of the roll entity (stride 0x1F0): **+0x42 natural (kept die)**, **+0x43 other die** (adv/disadv), **+0x44 bonus (signed char)**, **+0x41 DC**, **+0x40 advantage type**, **+0x4C success** (byte), +0x4D finished, +0x4E canceled, +0x08 RollUuid, +0x20 roller | Copied by **0x1547FC0** into DC: RolledNumber1 +0x838 / RolledNumber2 +0x850 (both dice, random order), **FinalResult +0x880 (= +0x42)**, **Success +0x8E0 (= +0x4C)**, total **+0xD58** (= natural + bonus, or natural on 1/20), ResultNumber +0x868 (animated count) | Confirmed (code); same-frame arrival with kind 0 Unconfirmed |
| Cleanest read point | DC update detour 0x1548FC0, on the transition of DC+0x7A8 to **3** | Read +0x880, +0x838, +0x850, +0xD58, +0x8E0 (that is what the UI animation itself binds to) | Confirmed (bindings); timing Unconfirmed |
| How long before the UI dice shows the face | DiceAnimation.xaml | Single die: storyboard `RollDieAnimation` 3.38 s, Tag `AnimDone` at **1.9 s** (end sound 1.6 s). Advantage: `JoinAnimPlaying` 1.4 s, `AnimDone` **3.22 s**. Then result count 1.0 s, then the modifier count-up, then `TriggerNextPhaseCommand` -> ResultReady | Confirmed (XAML) |
| Hide the UI dice | 3 options (section 4) | Recommended: VR mod does not show the ActiveRoll panel while `RollState == 3`; when the physical die rests, call 0x15475D0 again (game's own skip, action 5) -> `SkippedRoll` = 1 and ResultReady at once | Option 1 Confirmed (UI is one texture, AB); skip round-trip Unconfirmed |
| Result screen / continue | ContinueBtn -> `ContinueCommand` 0x1547B00 -> 0x1547B20 (action 3); or UIAccept in state 5 | Server action 3 -> kind 1 -> 0x1548840 fires "CloseWidget" (FS 0x5EF0BE0) -> ActiveRoll state removed | Confirmed (client side) |
| Attack rolls / saves (non-interactive) | `ecl::spell_cast::RollsComponent` (SE `ClientSpellCastRolls`): `Rolls[].Casts[].Hit.ConditionRolls[].Roll` (StatsRoll `Result.NaturalRoll`) | SE Lua component read; native layout not decoded | Type Confirmed (string 0x54BBA90); layout SE only |
| Passive rolls | RequestedRollComponent with +0x18 == 1; `ecl::RollSystem` 0x19D7170 collects {roller, +0x54} when +0x4D finished; `ecl::roll::PassiveRollSuccessOneFrameEventComponent` | Read the same component fields | Confirmed (code); values Unconfirmed |

---------------------------------------------------------------------------------------------------

## 1. How a roll opens (client)

1. The server creates the roll entity with `eoc::RequestedRollComponent` (type 0x605BEF0) and replicates it.
2. `ecl::RollSystem` (type 0x6058D10, size 0x2F8, ctor 0x19D3C50, registered by 0x734C50, ECS update stub 0x75C3F0
   -> **0x19D53C0**) watches that component. In 0x19D53C0, for each new roll entity (set S+0x278, filled by 0x19D5CF0)
   with `+0x18 == 0` (active roll), it calls the signal listeners at S+0x18 with `(player, component)`; for dialogue
   rolls (`+0x54 == 1`) it waits until `+0x4F != 0`. [aq18]
3. GameUI subscribed **0x121E680** to that signal (0x12199B0 at 0x121A366, `lea rcx,[r13+0x10]` with r13 = RollSystem).
   0x121E680 checks the roller character (`+0x20`, 0x7841A0) and calls **0x1213B30**(ui, player, &comp->RollEntity,
   `comp+0x54 == 1`). [aq29, aq30, aq31]
4. **0x1213B30** [aq28]: `dc = new DCActiveRoll(0xE40)` via ctor 0x1544DD0(dc, ui+0x878 VMProvider, ui+0x7C0, &rollEntity)
   (stores the entity at **DC+0xCC8**, write at 0x1545C9A); `0x20EF6A0(ui+0x1CC0, "ActiveRoll", &player, "ActiveRollOpen")`
   registers it in the DataContextProvider; remembers `{rollEntity, inDialog, callback}` in `ui+0x30A8` (0x50-byte
   entries keyed by player); then `FireEvent(ui+0x810, ..., args("ActiveRollOpen" or "DialogActiveRollOpen", player))`.
   0x12138E0 handles the event result. Returns 2 when the player already has an ActiveRoll DC.
5. State machine (Keyboard.xaml): `ActiveRollOpen` -> `PushState ActiveRoll` (from Running, l.648) or `AddSubstate
   ActiveRoll` (from Dialogue, l.1670-1678). State `ActiveRoll` (l.2160): `DisableStatesBelow`, `HideStatesBelow`,
   `IsModal`, `Owner=Player`, widget **ActiveRoll.xaml** on layer **PopupPanels**, sound layer DiceRoll. Its events:
   `CloseWidget` -> RemoveState, `OpenSelectionFlyOut`, `OpenTutorial`, `OpenMessageBox`, `ToggleGameMenu`. [Keyboard.xaml]
6. ActiveRoll.xaml root: `ls:UIWidget.ContextName="ActiveRoll"` -> the DC above. Elements: `DieControlHolder`
   (clickable die), `DieRollAnimation` (the dice animation, `DieRollStyle`), `ResultHolder` / `ResultAnimation`,
   `buttonGroup` (FailButtons: RollAgainButton, TryAgainButton; ContinueBtn; SuccessButtons), `UserTrigger`
   (UIAccept) and `UserTriggerEsc` (ToggleInGameMenu, disabled by default).

Detect a roll at run time, cheapest first: a detour on 0x1213B30 (player, entity, inDialog); a detour on the DC
update 0x1548FC0 (`this` = DC each frame); FireEvent 0x20C8340 with event FS == 0x5EF032C or 0x6094C08 (AB 4.4);
or the widget file "ActiveRoll.xaml" in WM.Widgets (AB 1.6).

## 2. The view-model

### 2.1 DCActiveRoll property map (getters disassembled, [aq4, aq5]; names from reflection 0x1543CE0)

| DC+ | property | type | source |
|-----|----------|------|--------|
| 0x790 | RollContext | u8 | comp+0x54 (0x1546360) |
| **0x7A8** | **RollState** | u32 enum EActiveRollPhase | written by 0x1547500, 0x1548840, 0x1546360 |
| 0x7C0 | SkillOrAbility | LocaString | |
| 0x810 | SkillOrAbilityResultText | LocaString | 0x1547FC0 |
| **0x838 / 0x850** | **RolledNumber1 / RolledNumber2** | u8 | comp+0x42 / +0x43 in random order (0x1547FC0) |
| 0x868 | ResultNumber | u8 | animated by AddToResultNumberCommand 0x1549A20 |
| **0x880** | **FinalResult** | u8 | comp+0x42 |
| **0x898** | **TargetNumber** | u8 | clamp(comp+0x41 - comp+0x44, 1, 20) (0x1546360) |
| 0x8B0 / 0x8C8 | MinBonusValue / MaxBonusValue | int | |
| **0x8E0** | **Success** | u8 | comp+0x4C |
| 0x8F8 | SkippedRoll | u8 | set by kind 2 (0x1548840) |
| 0x910 | HasBoostsToAdd | u8 | |
| **0x928** | **CanRespondToCommands** | u8 | 0x1548FC0; every command handler returns early when 0 |
| 0x940 | Roll | `ls.VMRoll*` | |
| 0xAE0 / 0xB00 | SourceObject / TargetObject | VM* | |
| 0xB40 | ContextObjectCount | int | |
| 0xB58 | IsTriggeredByScript | u8 | comp+0x55 |
| 0xB70 | SelectedDialogueLine | | |
| **0xCC8** | roll entity (EntityHandle) | | ctor |
| 0xD10 | phase flags (1 request sent, 2/4 reroll kind, 8 skip sent, 0x10 close sent, 0x20 init pending, 0x40 refresh) | u8 | |
| 0xD14 | InspirationPoints | int | 0x1546360 |
| 0xD50 | `ecl::RollSystem*` | | ctor reads type 0x6058D10 at 0x1545D87 |
| **0xD58** | total (natural + bonus; natural alone on 1 or 20) | u8 | 0x1547FC0 |

Commands (wrapper at DC+X, `ICommand*` at X+0x10 = the getter's field; handler from the ctor's `lea rax, handler` before
0x2119670) [aq6]:

| command | DC+ (getter) | handler | what it does |
|---------|--------------|---------|--------------|
| TriggerNextPhaseCommand | 0x960 | **0x1547500** | state 0 -> 1 (or 2 if flags & 6); state 3 -> **5** + result sound |
| AddToResultNumberCommand | 0x980 | 0x1549A20 | adds a modifier to ResultNumber during the count-up |
| ResetResultNumberCommand | 0x9A0 | 0x1549AF0 | ResultNumber = RolledNumber1 |
| **UserTriggeredNextPhaseCommand** | 0x9C0 | **0x15475D0** | see 3.1 |
| TryAgainCommand | 0x9E0 | 0x1547A30 | 0x19D5B40(sys, &e, 0) -> msg action 6 |
| RollAgainCommand | 0xA00 | 0x1547A90 | needs InspirationPoints > 0; 0x19D5B40(sys, &e, 1) -> action 7 |
| **ContinueCommand** | 0xA20 | **0x1547B00** | `if (+0x928) goto 0x1547B20` -> action 3 |
| CancelCommand | 0xA40 | 0x1547B10 -> 0x1547D70 | action 4 |
| AddModifierCommand / RemoveModifierCommand | 0xA60 / 0xA80 | 0x154BAB0 / 0x154BDC0 | -> 0x19D5980 (ActiveRollModifier message) |
| SelectDiceTypeCommand | 0xAA0 | 0x154BE40 | dice skin |
| SelectedCharacterChangedCommand | 0xAC0 | 0x1548ED0 | |

`ls.VMRoll` (`*(DC+0x940)`, reflection in 0x1450A00, getters [aq34]): +0x80 SkillOrAbility, +0xB8 ShowDC (u8),
**+0xD0 DifficultyCheck** (int, = comp+0x41), +0xE8 RolledNumber (int), +0x100 Chance (float), +0x118 Success,
+0x130 Finished, **+0x170 RollAdvantageType** (u8, = comp+0x40), +0x1D8 ProficiencyBonus, +0x1F0 RollBonus,
+0x208 AbilityValue, +0x220 RerollType; collections Modifiers (getter 0x1451450), Advantages (0x14514A0).
XAML compares RollAdvantageType with "None"/"Advantage"/"Disadvantage"; the numeric values are Unconfirmed (SE
AdvantageBoostType says 0/1/2; log it, 6.4).

### 2.2 eoc::RequestedRollComponent on the client (offsets from code use; names SE)

Component stride 0x1F0 (queries in 0x1550280/0x15507A0/0x1B305F0). Used offsets: +0x00 RollEntity, **+0x08 RollUuid**
(GUID; the network key, 0x1B305F0 matches +8/+0x10), +0x18 RollComponentType (0 active, 1 passive; 0x19D72C0,
0x1FF8320), +0x19 RollType, +0x20 Roller, +0x28 StatsExpression (string), **+0x40 AdvantageType**, **+0x41 DC**,
**+0x42 NaturalRoll** (kept die), **+0x43 second die** (SE DiscardedDiceTotal; the UI shows it as the other d20),
**+0x44 DiceAdditionalValue** (bonus, read as signed char), **+0x4C success** (the UI uses it as Success; SE calls this
byte SpellCheck), +0x4D Finished, +0x4E Canceled (server 0x3296CCB sets it before sending action 4), +0x4F (dialog
gate), +0x54 RollContext (1 = dialogue: selects DialogActiveRollOpen), +0x55 FromScript, +0x60 Subject, +0x88 (0 =
already rolled: 0x1546360 jumps straight to StartRoll and 0x1547FC0 copies results only when 0; meaning Unconfirmed),
+0x89/+0x8A flags, +0x90 ExcludedBonusesTags. SE's `Result` (StatsRollResult: Total, NaturalRoll, Critical) further
down the component is not read by the UI; whether it is filled on the client is Unconfirmed (6.3).

## 3. Sending the roll, and the result coming back

### 3.1 UserTriggeredNextPhaseCommand 0x15475D0 [aq7]

`void handler(DCActiveRoll* dc, void* unused, BaseComponent* param)`. Returns at once if `dc+0x928 == 0`.

| RollState | action |
|-----------|--------|
| 1 WaitForStart | if `!(flags & 1)`: flags |= 1; build message **0xEA** (`0x40395F0(factory, 0xEA)`), payload +0x28..+0x37 = RollUuid (comp+8), **+0x38 = 0**; send `0x4031600(client, [client+0x418], msg)` |
| 2 WaitForReRoll | flags |= 1; `0x19D57D0(rollSys, &entity, sub)` -> message 0xEA action **1**, +0x39 = sub (1 for an inspiration reroll, 2/3 for "try again") |
| 3 StartRoll | if `!(flags & 8)`: flags |= 8; message 0xEA action **5** (skip); plays the result sound (0x0E/0x0F) |
| 5 ResultReady | `cont = param ? param.bool : 1`; if `!Success && InspirationPoints > 0` return; if `cont` -> **0x1547B20** (continue: message 0xEA action **3**, flags |= 0x10) |

0xEA is `eocnet::RequestedRollMessage` (registration 0x119FABD, `mov edx, 0xEA`). The server handler **0x373A8E0**
[aq32]: action 0 -> `esv::active_roll::ModifierSystem`(0x60D94B4)+0x108 map[RollUuid] = 0 (start roll); action 1 ->
same map = +0x39; action 3 -> `esv::RollSystem`(0x605052C)+0x1E0 (finish); 4 -> +0x150 (cancel); 5 -> +0x210 (stop
= skip); 6 -> +0x180; 7 -> +0x1B0 (restart). The server rolls with its own rules; the client only asks.

The UI fires this command three ways (all Confirmed in XAML): left click on the die (`DieControlHolderTemplate`
`MouseLeftButtonDown` -> CommandParameter True, DiceAnimation.xaml l.3499); left click anywhere on the panel while
`UserTrigger` is enabled (CommandParameter False, ActiveRoll.xaml l.598); input event **UIAccept (0xDF)** through the
`UserTrigger` LSInputBinding (CommandParameter True, l.747). The binding is disabled for 0.2 s after every RollState
change (storyboard on `UserTrigger.IsEnabled`).

### 3.2 Triggering it from the mod

* **Native (recommended)**: detour **0x1548FC0** (DCActiveRoll update, vtable 0x55B3CD8 + 0x58, `void(dc, EcsCtx*)`).
  After the original: if `*(u32*)(dc+0x7A8) == 1 && *(u8*)(dc+0x928) && throwReleased` call
  `((void(*)(void*, void*, void*))(base+0x15475D0))(dc, nullptr, nullptr)`. This is exactly the handler the click
  runs; the server then rolls normally. Same call in state 2 starts a reroll. Runtime Unconfirmed (6.5).
* **Input event**: fire UIAccept 0xDF press/release with route B (M 1.4, dispatch 0x40F1550). It reaches the widget
  binding only for the ActiveRoll's player. Runtime Unconfirmed.
* **SE Lua**: inside `Ext.UI.Defer`, find the ActiveRoll widget (`Ext.UI.GetRoot()`, FileName/ContextName
  "ActiveRoll"), take its `DataContext` and call `dc.UserTriggeredNextPhaseCommand:Execute()` (nullptr parameter, SE
  NsHelpers.inl l.665-678). Property reads like `dc.RollState`, `dc.FinalResult` go through SE's Noesis reflection.
  Runtime Unconfirmed.

### 3.3 Result arrival [aq11, aq16, aq18, aq19, aq24]

* Server -> client message 0xEA is handled by **0x1FF8320**: it finds the RequestedRollComponent by RollUuid
  (0x1B305F0), and for active rolls maps the action to an event kind and appends `{EntityHandle, kind}` to
  `ecl::RollSystem+0x2C8`, then calls listeners at RollSystem+0xD8. Mapping: **2 -> 0 (rolled)**, 3 -> 1 (closed),
  4 -> 3 (canceled), 5 -> 2 (skipped), 6 -> 4, 7 -> 5 (reroll offers).
* Component signals feed RollSystem+0x2B8 (0x19D72C0: RequestedRoll signal bit 7 of the storage hooked by 0x19D6460;
  which ECS signal that is - create or replication change - is Unconfirmed) and +0x2D8/+0x2E8 (modifiers).
* At the start of the RollSystem update, **0x19D5DA0** moves those lists into the singleton
  **`ecl::roll::ActiveRollEventsSingletonComponent`** (type 0x60535CC, created by 0x19D6980): +0x00 roll entities whose
  component changed, **+0x10 `{EntityHandle, int kind}` (0x10 bytes)**, +0x20 / +0x30 modifier lists.
* The DC update 0x1548FC0 -> **0x1549B20** reads the singleton (query 0x609E490 = Modified): +0x20 -> 0x154C7F0,
  +0x30 -> 0x154BFA0, **+0x00 -> 0x1547FC0** (copies natural/second die/success/total into the DC), then **+0x10 ->
  0x1548840(dc, &ev, kind)**. 0x1547FC0 also runs once after init (flags & 0x20).
* **0x1548840** kinds: **0** -> RollState = 3 (StartRoll) if this player pressed Roll (flags & 1) or is a listener
  in a multiplayer roll (`PlayerCharacterProperties+0x9D0`, 0x1548F70), else 5; 1 and 3 -> 0x1547B20 + FireEvent
  "CloseWidget" for the player; **2 -> SkippedRoll = 1, RollState = 5, ResultNumber = total**; 4 / 5 -> RollState 0 with
  reroll flags 4 / 2.

So the natural d20 is on the client at the latest in the frame where RollState becomes 3: within one call of 0x1549B20
the +0x00 list (0x1547FC0) is processed before the +0x10 list (0x1548840). Whether the replicated component update and
the action-2 message always land in the same frame is Unconfirmed (6.2). The DC's own `FinalResult` drives the UI
animation's final face (`DiceEndAnim.Tag = FinalResult`, DiceAnimation.xaml l.1670), so reading DC+0x880 at the
transition to 3 matches what the UI shows.

Advantage / disadvantage: comp+0x42 is the kept die, comp+0x43 the other one; the DC puts them in random order into
RolledNumber1/2 (`rand()` in 0x1547FC0); the UI's double-die animation then highlights the die whose value equals
FinalResult (DiceAnimation.xaml l.1924-1960). For VR: throw two dice; make one land on +0x42 and the other on +0x43.

### 3.4 Time from Roll press to the UI showing the face

Network round trip (single player, local server): Unconfirmed; log it (6.2). Then the UI (Confirmed from XAML):
`RollDieAnimation` starts when RollState becomes StartRoll; single die: Tag `PlaySoundRoll_End` at 1.6 s, `AnimDone`
at **1.9 s** (storyboard 3.38 s); double die: `JoinAnimPlaying` 1.4 s, `AnimDone` **3.22 s**. `AnimDone` sets the
widget Tag `RevealResultAnimation`; `ResultHolder` counts 1.0 s; on a natural 1 or 20 `TriggerNextPhaseCommand` runs
right away, otherwise the modifier overlay sequence runs first (0.8 s steps, 1 s / 1.8 s completion delays) and then
`TriggerNextPhaseCommand` -> 0x1547500 -> **ResultReady (5)**.

## 4. Hiding the UI dice and reaching the result screen

1. **VR-side hiding (recommended, no game change)**. The UI is one texture that the VR mod crops (AB 2). While
   `RollState` is 1 (WaitForStart) show the panel (DC, modifiers, advantage) but not over the hand; while it is **3**
   (StartRoll) do not draw the ActiveRoll panel at all - the game animates invisibly. Show it again at 5.
2. **Game's own skip**. When the physical die comes to rest, call 0x15475D0(dc, 0, 0) again while RollState == 3:
   it sends action 5; the server stores it in its StopRoll set; the reply action 5 becomes kind 2 -> `SkippedRoll = 1`,
   RollState 5 at once, ResultNumber = total, and the result sounds switch to the `_Skipped` variants (ActiveRoll.xaml
   l.674-725). This cuts the 1.9-3.2 s animation and the count-up. That the server answers action 5 with action 5 is
   Unconfirmed (client side Confirmed); 6.6 settles it.
3. **SE Lua cosmetic**: set `Opacity = 0` on the elements `DieRollAnimation`, `ResultAnimation`, `DieControlHolder`
   (names from ActiveRoll.xaml). Keep them laid out: the flow depends on their `Tag` storyboards (`AnimDone`). Do not
   collapse them (that the storyboards still run when collapsed is Unconfirmed in Noesis).
4. **Result screen**: in state 5 `buttonGroup` is visible (FailButtons with RollAgain/TryAgain when the roll failed;
   ContinueBtn / SuccessButtons). Continue = `ContinueCommand` 0x1547B00(dc) (same as the button), or
   0x15475D0(dc, 0, 0) in state 5 (does nothing on a failure while InspirationPoints > 0 - the player must choose), or
   UIAccept. All end in 0x1547B20 -> action 3 -> server -> kind 1 -> FireEvent "CloseWidget" -> state removed.
   Reroll with inspiration: RollAgainCommand 0x1547A90 -> kind 5 -> state 0 -> 2 (WaitForReRoll) -> throw again ->
   0x15475D0 in state 2.

## 5. Non-interactive rolls (for later)

* **Attack rolls and saving throws**: resolved on the server (I 3.3). The client receives them in
  **`ecl::spell_cast::RollsComponent`** (type string 0x54BBA90; SE `ClientSpellCastRolls`) and the shared
  `eoc::spell_cast::RollsComponent` (0x54C9D30), changed-tag `ecl::spell_cast::SpellRollsChangedOneFrameComponent`
  (0x54BBBF0). SE layout: `Rolls[]` -> `Casts[]` -> `Hit` (HitDesc) -> `ConditionRolls[]` {RollType (attack / saving
  throw / ability check), `Roll` = StatsRoll {Result.Total, Result.NaturalRoll, Result.Critical}, Difficulty}. Native
  offsets not decoded here (Unconfirmed); read them with SE Lua first (6.8).
* The combat log has `ls.VMRollFinishedEntry` (0x14CDFC0) next to VMSpellEntry/VMHitResistedEntry (0x16BF230): a
  UI-side source, fields not decoded.
* **Passive rolls**: same RequestedRollComponent with +0x18 == 1. 0x19D7170 (RollSystem) collects `{roller, +0x54}`
  into RollSystem+0x2A8 when +0x4D (finished) is set and calls listeners at +0x78; the client also has
  `ecl::roll::PassiveRollSuccessOneFrameEventComponent` (0x60535D4) and the HUD widget PassiveRoll.xaml
  (`ls.OverheadPassiveRoll`). The natural value is presumably +0x42 as for active rolls (Unconfirmed, 6.8).
* Server side, for an SE server script: `esv::roll::RollFinishedEventOneFrameComponent` (string 0x56E8530; SE fields
  NaturalRoll, DC, Advantage, Disadvantage, Canceled) and the Osiris event "RollResult" (string 0x574DC68, refs
  0x28683BC, 0x287194B, 0x28E05C6; argument list not checked).

## 6. One-run logging plan

Hooks: 0x1213B30 (entry), 0x1548FC0 (DC update, after the original), 0x1547FC0 (entry), 0x1548840 (entry),
0x15475D0 (entry), 0x1FF8320 (entry; `r9` = message), 0x19D7170 (entry), 0x20C8340 (AB's FireEvent log). Every line
gets `QueryPerformanceCounter` and `GetCurrentThreadId()`. Log on change. In the run: one dialogue skill check, one
world check (e.g. a lock or perception), one check with advantage, one failed check with inspiration (reroll), one
skip by clicking during the animation, one passive roll, one melee attack.

1. **Open**: 0x1213B30 player, `*rollEntity`, inDialog, return value; FireEvent FS id (ActiveRollOpen 0x5EF032C /
   DialogActiveRollOpen 0x6094C08).
2. **Timing of the result**: in 0x1548FC0 log `dc`, RollState +0x7A8, +0x928, +0xD10, +0x838, +0x850, +0x868, +0x880,
   +0x898, +0x8E0, +0x8F8, +0xD14, +0xD58; in 0x1547FC0 the component bytes +0x18, +0x40..+0x4F, +0x54, +0x55, +0x88;
   in 0x1FF8320 message +0x38, +0x39 and the GUID at +0x28; in 0x1548840 the kind. Settles: frames between the Roll
   press (0x15475D0 state 1) and action 2; whether 0x1547FC0 with valid +0x42 always precedes kind 0; thread ids of the
   DC update vs GameUI::Update 0x1210210 (AB).
3. **Component dump**: once per roll after kind 0, hex-dump the whole 0x1F0-byte component; compare against SE's
   `Roll` / `Result` (Total, NaturalRoll, Critical) to see whether those are filled on the client, and find the meaning
   of +0x88 and +0x4F.
4. **Advantage**: log VMRoll+0x170 numeric value with the XAML text the user sees; check +0x43 is the dropped die.
5. **Native trigger**: with a debug key, call 0x15475D0(dc, 0, 0) from the 0x1548FC0 detour when RollState == 1;
   expect the 0x15475D0 log, then 0x1FF8320 action 2, kind 0, RollState 3. Also fire UIAccept 0xDF by route B in a
   second roll and compare.
6. **Skip**: 300 ms after RollState 3, call 0x15475D0 again; expect 0x1FF8320 action 5, kind 2, SkippedRoll 1,
   RollState 5 in the same or next frame.
7. **Continue**: call 0x1547B00(dc) in state 5; expect action 3, kind 1, FireEvent "CloseWidget", widget gone (AB 1.6).
8. **Non-interactive**: SE Lua (client) prints, on change, `ClientSpellCastRolls` of the caster:
   `Rolls[i].Casts[j].Hit.ConditionRolls[k].{RollType, Roll.Result.NaturalRoll, Roll.Result.Total, Difficulty}` and
   compares with the combat log; native: 0x19D7170 logs passive components +0x18, +0x42, +0x4C, +0x4D, +0x54.

## 7. Function / data index

| rva | what |
|-----|------|
| 0x1213B30 / 0x12138E0 | GameUI OpenActiveRoll / its FireEvent result handling |
| 0x121E680 / 0x121C130 / 0x12199B0 | GameUI roll listener / deferred open queue / subscription to ecl::RollSystem |
| 0x1544DD0 / 0x1543CA0 / 0x1543CE0 | DCActiveRoll ctor / type / reflection; vtable 0x55B3CD8 (+0x28 0x1546360 init, +0x58 0x1548FC0 update) |
| 0x154ECB0 | EActiveRollPhase enum names |
| 0x1546360 | DC init from RequestedRollComponent (TargetNumber, VMRoll, inspiration; state 3 when +0x88 == 0) |
| 0x1547FC0 | copy result (natural, second die, success, total) into the DC |
| 0x1548840 | apply roll event kind 0..5 |
| 0x1548FC0 / 0x1549B20 | DC update / read ActiveRollEventsSingleton |
| 0x1547500 / 0x15475D0 / 0x1547B00 / 0x1547B20 / 0x1547D70 | TriggerNextPhase / UserTriggeredNextPhase / Continue / continue-send (action 3) / cancel-send (action 4) |
| 0x1547A30 / 0x1547A90 | TryAgain / RollAgain |
| 0x19D3C50 / 0x19D3FA0 / 0x19D53C0 / 0x19D5DA0 | ecl::RollSystem ctor / init / update / move lists into the singleton |
| 0x19D57D0 / 0x19D5B40 / 0x19D6D90 | send 0xEA action 1 / actions 6-7 / component lookup |
| 0x19D72C0 / 0x19D7170 | RollSystem component callbacks (active list +0x2B8 / passive +0x2A8) |
| 0x1FF8320 | client handler of message 0xEA |
| 0x373A8E0 | server handler of message 0xEA (and 0x113 ActiveRollModifier) |
| 0x119FABD | RequestedRollMessage registration (id 0xEA) |
| 0x5EF032C / 0x5EF0330 / 0x6094C08 / 0x5EF0BE0 | FixedStrings ActiveRollOpen / ActiveRoll / DialogActiveRollOpen / CloseWidget |
| 0x605BEF0 / 0x60535CC / 0x6058D10 / 0x605052C / 0x60D94B4 | type ids RequestedRollComponent / ActiveRollEventsSingletonComponent / ecl::RollSystem / esv::RollSystem / esv::active_roll::ModifierSystem |
| 0xDF | input event UIAccept |
