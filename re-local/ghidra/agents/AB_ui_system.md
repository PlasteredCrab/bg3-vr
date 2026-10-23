# The whole UI system (Noesis GUI) for VR - agent AB, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_F, read-only, headless, request server
`agents\AB\ABServer.java`; new helper scripts `PatternNear.java` / `PatternAll.java` = instruction-pair search,
`py\findref.py` = rel32 / RIP-relative reference search, `py\sig.py` = byte signature search) plus the UI XAML that
agent M extracted (`agents\M\gamedata\game\...`). RVAs are relative to 0x140000000. Raw output is in
`re-local\ghidra\agents\AB\` (ab1..ab46; `ab_sm_keyboard.txt` / `ab_sm_controller.txt` = the two UI state machines
flattened by `py\smdump.py`; `ab_pages.txt` = root element, data context and size of every page, `py\pages.py`).
"SE" marks a name or layout taken from the Script Extender source (`downloads\bg3se-src`). The game links
**NoesisGUI 3.1.6** statically (source paths at 0x52F5D80..). Builds on F_ui.md (rendering, IView vtable, mouse path,
controller mode), M_ui_open_and_light.md (state events, input event dispatch, hotbar), B (player manager),
C (ECS systems, frame order), I (picking), O/X (UI size), V (PointToScreen, paperdolls).

**Confirmed** = shown by the cited code or XAML. **Unconfirmed** items say what would confirm them; all of them are
in the one-run logging plan (section 7).

## Hook points / calls for the VR mod

`GameUI = *(u8**)(*(u8**)(base+0x6047958) + 0x130)` (F). "game thread" = the thread that runs the client ECS update
(C 3.5); "render thread" as in A.

| need | RVA | how | thread | confidence |
|------|-----|-----|--------|------------|
| Per-frame UI hook (the one place where everything below is safe) | **0x1210210** GameUI::Update(ui, ctx), GameUI vtable slot 0xC0; called only through **0x7493E0** (stub: `rcx=[[0x6047958]+0x130]; jmp [rax+0xC0]`), which is the update callback of the ECS system **`ecl::UISystem`** (type id global 0x6059234, registered by 0x716F00) | Detour 0x1210210. Before calling the original: inject pointer/clicks (3.2), read view-models (4). After it: read the widget list (1.6). The update holds `GameUI+0x3038` (UpdateCS) and the InputManager lock `[0x604B1A8]` for its whole run | ECS update (game thread; worker vs main Unconfirmed) | Confirmed (call chain); thread id Unconfirmed |
| Enumerate open widgets | WindowManager = **GameUI+0x4C0**; widgets array **WM+0x108** (`UIWidget**`), count **u32 WM+0x114**; lock **WM+0x150** | Per widget: FileName string **+0x328** (char* = `*(u32*)(w+0x33C) >= 16 ? *(char**)(w+0x328) : (char*)(w+0x328)`), LayerIndex **+0x320**, IndexInLayer **+0x324**, PlayerId (short) **+0x394**, state GUID +0x308, UIStateWidget* +0x2F8. Array is kept sorted by (layer, index) | game/UI thread, under WM+0x150 | Confirmed (0x20EA050 writes, 0x20E86E0 sorts, getter 0x14792B0); FileName text form Unconfirmed |
| Know when a widget opens | **0x20EA050** WindowManager::AddWidget(wm, uri, UIStateWidget*, UIStateInstance*, flag, UIWidget*) and **0x20EA720** (finish: adds to WM+0x108) | Detour 0x20EA720: widget = rdx, state instance = r8. Removal goes through the WM+0x1C8 list in 0x20E86E0 | UI update | Confirmed |
| Open / close panels without the mouse | **0x20C8340** UIStateMachine::FireEvent(sm=`[GameUI+0x810]`, result, args, cb) with args from **0x20EEA70** (M route A) | Event names per panel: section 1.5. `uiPlayer` = **`*(u16*)([0x60487A0]+0xD0)`** (player id of input slot 0, B; ids start at 1) | game thread | Confirmed (code + XAML); runtime Unconfirmed |
| Watch every UI state change | same 0x20C8340 (args +4 kind: 1 game event "GE.*" by index, 2 input event "IE.*", 3 named; +8 event FixedString; +0x12 player) | Log/compare FixedStrings made with 0x4194CA0 for the names you care about (GE.OnDialogueShow, OpenTrade, ...) | game thread | Confirmed (layout, 0x20C8AE0) |
| Active state tree | UIStateMachine `+0x98` root UIStateInstance | Instance: +0x00 widgets (ptr, u32 count +0xC), +0x10 sub-state stacks (ptr, u32 count +0x1C; each is an array of instances, count +0xC), +0x20 UIState*, +0x28 PlayerId, +0x30 CS, +0x58 flags (0x20 = skip), +0x5B owner kind, +0x60 name FixedString (Unconfirmed), +0xC8 GUID | UI update, under inst+0x30 | Confirmed except name |
| Players / selected character VMs | **0x1300990**(VMProvider=GameUI+0x878, short* playerId) -> VMPlayer* | VMPlayer: +0x128 PlayerId, +0x2D8 UIData, +0x650 **IsCurrentTurn**, +0x668 CanEndTurn, +0x6B8 **SelectedCharacter** (VMCharacter*), +0x738 CurrentCombatTurnTarget. VMCharacter: +0x420 PlayerCharacterProperties, +0x628 **InTurnBasedMode** | UI update | Confirmed (getters) |
| Dialogue state | VMPlayerCharacterProperties **+0x9B0 ActiveDialogue** (VMDialogue*) | VMDialogue: +0x300 ShowAnswers (u8), +0xF8 Answers collection, +0x2E0 LocalHighlightedAnswer, +0x2C0 SelectedAnswer, +0x318 ContinueAnswer, +0x340 DialogInstanceID. VMDialogueAnswer: +0x98 AnswerIdx, +0xC8 BoundEvent (input event id), +0x48 Enabled, +0x60 BodyText | UI update | Confirmed (getters) |
| Pick a dialogue answer without the pointer | input event = **`VMDialogueAnswer+0xC8`** fired with M route B (0x40F1550), or DCDialogue command handler **0x1741EC0**(dcDialogue, 0, answerVM) | The answer buttons are `LSButton BoundEvent="{Binding BoundEvent}" Command=SelectAnswerCommand` (DialogueTemplates.xaml l.110); the handler ends in 0x1FC3130(dialogMgr, instanceId, AnswerIdx, ...) | game thread | Confirmed (XAML + decompile); runtime Unconfirmed |
| Pointer into the UI (game-side, no OS cursor) | cursor float2 at `*(float**)0x614FB70` + InjectRaw 0x40F1F00 (F 2.6) | Write UI-view pixels, inject motion/buttons on device 1. Keeps double-click, drag-and-drop, CursorText and "handled" bookkeeping | before InputManager::Update | Confirmed (F) |
| Pointer into Noesis directly | IView = `*(void**)(GameUI+0x130)`; slots +0xD0 MouseMove, +0xB8 Down, +0xC0 Up, +0xC8 DoubleClick, +0xD8 Wheel (F 2.2); lock **GameUI+0x258** | Call from the 0x1210210 detour (3.2). Nothing re-applies the OS cursor to Noesis each frame (only real SDL motion does) | UI update, NUIM CS held | Confirmed (code); behaviour Unconfirmed |
| Stop invisible UI from eating world input | GameUI vtable slot 0x50 = **0x120E0C0** returns `GameUI+0x240` (NUIM MouseOverUI); called by picking 0x1C45A90 and 0x1B40730, 0x1BCD000, 0x1BD5FF0, 0x1FB3420, 0x2066950.., 0x4261440, 0x4261630 | When the laser is not on a UI panel, return 0 (or write the byte after 0x20C3EF0). Also send world clicks as Action1 events (route B), not raw mouse buttons, so NUIM never sees them (3.3) | any | Confirmed (callers); effect Unconfirmed |
| UI size | GameUI slot 0x60 **0x120E0E0** SetSize (F, O, X) | unchanged | game thread | Confirmed (F) |
| Stop drawing the UI (UI hidden in VR) | GameUI slot 0x38 **0x120E0A0** SetActive(bool) = byte **GameUI+0x84** | Render job 0x3CC04F0 skips Render when slot 0x40 (reads +0x84) is false, and 0x120FD10 also checks +0x84. Update and hit testing keep running | any | Confirmed (code); side effects Unconfirmed |
| Separate textures for hotbar / portraits / combat HUD / dialogue | none in the game | Crop sub-rectangles of the single UI texture (2.3) with the anchors from XAML; refine with an alpha bounding box on the GPU, or exact element rects through Noesis **PointToScreen 0x45FC6B0** / SE Lua | render thread (crop) | Crop: Confirmed feasible (one texture, layout from XAML); exact rects Unconfirmed natively |

---------------------------------------------------------------------------------------------------

## 1. Architecture

### 1.1 Object map (Confirmed unless marked)

GameUI is allocated by the factory 0x11F1080 (`alloc(0x31C0, align 0x40)` + ctor 0x120BAE0), stored in
`DAT_145FDA238` by client setup 0x5777D0 and installed at `ResourceManager+0x130` by the game-state handler 0x1847660
(it replaces the previous object, passes what the old object's slot 0x10 returns to the new one's slot 8
`SetParams` 0x120DCE0, then calls slot 0x88 and slot 0x18) [ab21].
Its primary vtable 0x555EF78 has 30 slots (0x00..0xE8); 0x555F068 directly after it is the vtable of the input-listener
interface at GameUI+8 (so e.g. 0x12154A0 at "slot 0x138" is listener slot 9, `this = GameUI+8`) [ab9, ab13].

| GameUI + | what | evidence |
|----------|------|----------|
| 0x08 | input-listener interface (vtable 0x555F068; OnInputEvent 0x1214430, F) | ctor, F |
| 0x50 | GameUIParams; **+0x84 active flag** (slot 0x38 setter 0x120E0A0, slot 0x40 getter 0x120E0B0) | ab13, F |
| 0xC0 | NoesisUIManager (NUIM, F). Absolute: +0x100 RenderDevice, +0x120 MainCanvas, **+0x130 IView**, +0x138 second view, +0x140 reload view, +0x190 design height (2160), **+0x194/+0x198 view size**, **+0x240 MouseOverUI**, +0x250 view mouse pos, **+0x258 NUIM CRITICAL_SECTION**, +0x280 NUIM flags (0x10 controller, 0x80 text input) | ctor, F, 0x1215260, 0x120FD10 |
| 0x2C0 | BrushManager (vtable 0x557BF28) | ctor |
| 0x480 | passed to WindowManager as its +0xD8 (SE: ModManager) | ctor |
| **0x4C0** | **WindowManager** (ctor 0x20E74B0, update 0x20E86E0) | ctor, 0x1210210 |
| 0x7C0 | ui::StateMachine: +0x7D8 = &WindowManager, **+0x810 = UIStateMachine\*** (M) | ctor, M |
| 0x878 | ViewModelProvider (SE name); **+0x880 = `ls.GameData` root VM** | 0x1215260, 0x1300990 |
| 0x1B80 | ContextMenuManager (SE name; WM+0xF8 points here) | ctor |
| 0x1CC0 | DataContextProvider (SE BinaryMappings use `[r15+1CC0h]`) | ctor, SE |
| 0x1EC0 | DragAndDropManager (SE name; updated by 0x1391170 in 0x1210210) | ctor, ab2 |
| 0x2180 | TooltipManager (SE name; WM+0xF0 points here) | ctor |
| 0x2840 | MessageBoxManager (SE name) | ctor |
| 0x2D88 | layout mode 0 KB/M, 1 controller (F) | F |
| 0x2D94 | init state; Render needs 5 | 0x120FD10 |
| 0x2DA0 / 0x2DA8 | `[0x60089B0]` / **PlayerManager `[0x60487A0]`** (B: +0xD0 player id of input slot 0, +0x70..+0x78 slot->player map) | ctor disasm 0x120C5A9 |
| 0x2DB4 | XAML reload in progress | F, 0x1210210 |
| 0x2FC8 / +0x2FD4 | queued FireEventDesc (0x38 bytes) handed to 0x1214070 in the update (SE QueuedFireEvents); CS +0x2FE8 | 0x1210210 |
| 0x2FD8 / +0x2FE4 | queued input events (0x24 bytes, +0 event id) for 0x1215260 (deferred mouse-up) | 0x1215260 |
| **0x3038** | UpdateCS (held by Update, tried by Render) | 0x1210210, 0x120FD10 |
| 0x3060 | RenderCS | 0x120FD10 |
| 0x3180 / 0x3188 / 0x318C / 0x3190 | last time (double) / scaled dt / dt (clamped 0.2) / tick counter | 0x1210210 |

### 1.2 Threads and locks

* **Update**: `ecl::UISystem` (type id global 0x6059234; name from the type-id map `G\typeidmap.txt`) is
  registered by **0x716F00** with update callback **0x7493E0** (found only as a RIP-relative LEA at 0x716FB1;
  Ghidra had no function there). The stub tail-calls GameUI slot 0xC0 = **0x1210210** with the ECS context as the
  second argument, whose first field the update reads as the game time (double). So the whole UI update (state
  machine 0x20C86E0 / 0x20C6AB0, drag-and-drop 0x1391170, WindowManager 0x20E86E0, NUIM update 0x20C2FD0 =
  `IView::Update`, deferred properties 0x20F5950) runs inside the client ECS tick (C 3.5: App slot 0x288 on the
  game-loop thread). Confirmed. Whether the ECS scheduler can place UISystem on a worker thread is Unconfirmed.
  (SE calls the thread of its `PredicatesManager::UpdateFromUI` hook, which sits inside 0x20C2FD0, "the render
  thread"; that is SE's label only.)
* Locks held by the update: InputManager object lock (`TryEnterCriticalSection([0x604B1A8])` around everything,
  the same lock the input code uses), GameUI+0x3038 around each step, NUIM CS GameUI+0x258 inside NUIM calls,
  WM+0x150 for the widget array. Confirmed [ab2].
* **Render** 0x120FD10 (render thread, F): needs `+0x84` and init state 5; normal path takes RenderCS +0x3060 and calls
  renderer `vf[0x58]` (offscreen pass) + `vf[0x68](0,0)` for the main and second views; when it can grab UpdateCS it
  first runs 0x20C35E0 (render-tree update). During a XAML reload it renders the reload view (+0x140) or the main view
  under UpdateCS + NUIM CS [ab40].
* **Input**: the GameUI listener (GameUI+8) is called from InputManager::Update (game thread, M 1.1); mouse/navigation
  events are then also handled in the update from the queue at +0x2FC8 (0x1214070 -> NUIM 0x20C3660, F).

### 1.3 How a page (widget) is created, opened, closed and layered

1. **State machine.** `StateMachine.xaml` (root, Public/Game/GUI) plus `Mods/MainUI/GUI/StateMachines/Keyboard.xaml`
   or `Controller.xaml` (M) define `ls:State`s with `Layout` (Common/Player), `Owner` (All/Player/DefaultPlayer),
   `IsModal`, `DisableStatesBelow`, `HideStatesBelow`, `ForceSplitscreen`, a widget list
   (`ls:StateWidget Filename Layer SoundLayer IgnoreHitTest BlockedLoading`) and events with actions
   (`SetSubstate`, `AddSubstate`, `PushState`, `RemoveState`, `RemoveAllSubstates`, `RequestCloseWidget`,
   `PreloadState`, `ClearPreloadState`). Flattened listing: `ab_sm_keyboard.txt`. Confirmed (XAML).
2. **Events.** 0x20C8340 FireEvent (M) -> 0x20C8AE0 walks the instance tree depth-first from the root `sm+0x98`
   (children before parent, last sub-state first) and runs the first matching state event: kind 1 = indexed game
   event through `UIState+0x1E8[index]`, kind 2 = input event (the "IE.*" names), kind 3 = named event through the
   UIState hash map +0xA28/+0xA30. Owner filter: instance `+0x5B` = 2 always, 1 only if the event's player equals
   the instance player `+0x28`, 0 only if it equals `sm+0x470`. Instances with flag `+0x58 & 0x20` are skipped.
   Confirmed [ab4]. (Which XAML owner string maps to 0/1/2 is Unconfirmed; order All/Player/DefaultPlayer -> 2/1/0
   is the likely reading.)
3. **Widget creation.** For each `StateWidget` of a state being entered, the WindowManager loads the XAML
   (NoesisGUI LoadXaml 0x45C38A0, F; async completion 0x20E83B0 / 0x20E9150) and then calls
   **0x20EA050 AddWidget** [ab42]: checks that the instance's player is a local player (`[0x60487A0]+0x20` list),
   turns the `Layer` string (`UIStateWidget+0x38`) into a FixedString and looks it up in the **layer map WM+0x128**
   (buckets +0x130, bucket count +0x12C, node +8 name, +0xC index) -> **LayerIndex** (widget+0x320); IndexInLayer
   (+0x324) = highest in that layer + 1; appends the widget to the instance (inst+0x00); fills widget +0x2D0 context-
   menu manager, +0x2D8 sound, +0x2E0 tooltip manager, +0x2E8 WM, +0x2F0 StateMachine, +0x2F8 UIStateWidget,
   +0x300 InputManager, +0x308 state GUID, +0x328 FileName (from the URI), +0x378..+0x388 ids of UIPaste / UICopy /
   UICut / UISelectAllText / ContextMenu, sets the `PlayerId` dependency property and **+0x394 PlayerId**,
   hit-test visibility from `UIStateWidget+0x280` (IgnoreHitTest). Then **0x20EA720** pushes it into
   **WM.Widgets (WM+0x108)** under WM+0x150 and raises the finish-load signal (WM+0x18), or parks it in the pending
   list WM+0x260 (0x20-byte entries) when it is still loading [ab26].
4. **Layering.** Each update 0x20E86E0 sorts WM.Widgets by (LayerIndex, IndexInLayer) and gives every widget its
   position with 0x45BFAA0(widget, i) (Noesis z-order setter; name Unconfirmed); widgets queued in the WM+0x1C0
   collection are added to the MainCanvas panel (`NUIM+0x60`, child collection behind `canvas+0x2E8`) and get
   `widget+0x318 = canvas`; WM+0x1C8 holds widgets to remove [ab3]. Layers, in the order `WindowManager.xaml`
   declares them: Default, HUD, Panels, HUDTop, PopupPanels, Notifications, DragAndDrop, Pause, PauseOverlay,
   ScreenFade, MessageBox. That LayerIndex equals this position (0..10) is Unconfirmed (log the map, section 7).
5. **Closing.** State events `RemoveState` / `RequestCloseWidget` (the widget plays its close animation first, M's
   "CloseWidget") remove the instance; its widgets go through the WM removal list. The GameUI helper 0x121DE10 fires
   "CloseWidget" (M).
6. **Every page is a full-canvas element.** All HUD/panel pages are `ls:UIWidget` roots of 3840x2160 design units
   (`ab_pages.txt`) whose content is anchored inside (alignment + margins). MainCanvas.xaml = `ls:UICanvas` ->
   `Viewbox` -> `Grid ContentRoot 3840x2160` with LeftPlayer/RightPlayer columns (split screen). NUIM lays the canvas
   out at `(w*2160/h) x 2160` (F 1.4).

### 1.4 Data contexts

Each page names its data context with `ls:UIWidget.ContextName` (e.g. HotBar -> `gui::DCHotBar`, Dialogue ->
`gui::DCDialogue`, TurnModeInfo -> `gui::DCFTBParticipants`, CombatantsOverlay -> `gui::DCCombatants`, CombatLog ->
`gui::DCCombatLog`, Container -> `gui::DCContainerInventory`, Overlay -> `gui::DCOverlay`, OverheadInfo ->
`gui::DCOverheads`; full list in `ab_pages.txt`). Every DC derives from `ls.DCWidget` and exposes `CurrentPlayer`
(the VMPlayer of the widget's player); XAML bindings like `CurrentPlayer.PlayerId`, `CurrentPlayer.SelectedCharacter`
come from there. The view-model tree is rooted at `ls.GameData` (GameUI+0x880), whose players collection
(+0xC8/+0xE0) holds one VMPlayer per local player (0x1300990 searches it by `VMPlayer+0x128`) [ab36].

### 1.5 Top-level widgets (Keyboard.xaml; Confirmed from XAML)

States always present while playing (`Running` -> `PlayerUIs` -> `PlayerUIsHolder` -> `PlayerHUD`,
`PlayerWorldContext`, `PlayerPanelsHolder`; plus `PlayerNotifications`):

| page (XAML) | layer | state | opened by |
|-------------|-------|-------|-----------|
| HotBar.xaml (DCHotBar; root `HotbarRoot` bottom-centre) | HUDTop | PlayerHUD | always (collapsed when no selected character) |
| PlayerPortraits.xaml (root `Root` left) | PopupPanels | PlayerHUD | always |
| TurnModeInfo.xaml (DCFTBParticipants; `ParticipantsListContainer` top-centre, margin 0,20) | HUD | PlayerHUD | always (visible in turn-based mode) |
| CombatantsOverlay.xaml (DCCombatants; `RootPanel` top-centre) | HUD | PlayerHUD | always |
| TargetInfo.xaml (`TargetInfoRoot` top, margin 0,80; cast info bottom) | HUD | PlayerHUD | always (IgnoreHitTest) |
| Minimap.xaml (top-right, 15 px margin, 624 px map) | HUD | PlayerHUD | always |
| CombatLog.xaml (bottom-right) | HUD | PlayerHUD | always |
| Overlay.xaml, HudIndicator.xaml, OverheadInfo.xaml, CursorText.xaml, DragAndDropPreview.xaml, PassiveRoll.xaml, AlwaysOnTopOverlay.xaml | Notifications / HUD / PopupPanels / DragAndDrop | PlayerHUD | always |
| WorldContextMenu.xaml / WorldTooltips.xaml | Notifications / HUD | PlayerWorldContext | always |
| Notification.xaml (top-centre y 400, bottom y 510) | Notifications | PlayerNotifications | always |

Panels (all in `PlayerPanels` or `PlayerUIsHolder` unless stated; event names are the ones to fire with route A):

| page | layer | state | opened by (state events) |
|------|-------|-------|--------------------------|
| CharacterPanel.xaml (inventory, equipment, character sheet, spellbook, alchemy) | PopupPanels | CharacterPanel | ToggleInventory, ToggleCharacterPanel, ToggleSpells, ToggleAlchemy, ToggleIllithid, ToggleInterrupts, OpenInventory, OpenSpellbook, OpenAlchemyPanel; close CloseWidget |
| PartyPanel.xaml (IsModal) | Panels | PartyPanel | TogglePartyPanel; close ClosePartyPanel |
| JournalQuests / JournalMap / JournalInspiration / JournalDialogues / JournalInfo / JournalTutorials .xaml | PopupPanels | Journal* | ToggleJournalQuests, ToggleJournalMap / ShowJournalMap / ToggleWaypoints, ToggleJournalInspiration, ToggleJournalDialogues, ToggleJournalInfo, ToggleJournalTutorial |
| Trade.xaml / Donate.xaml | Panels | Trade / Donate | OpenTrade, OpenDonate (also from Dialogue) |
| Container.xaml (loot) | PopupPanels | ContainerInventory | OpenContainerInventory |
| Pickpocket, Combine, Insert, Examine, SplitItem .xaml | HUD / PopupPanels / MessageBox | same names | OpenPickpocket, OpenCombine, OpenInsert, OpenExamine, SplitItemToggle |
| Dialogue.xaml (+ Overlay, AlwaysOnTopOverlay; `HideStatesBelow`, `DisableStatesBelow`) | HUD | Dialogue | GE.OnDialogueShow (game), PreloadDialogue; close GE.OnDialogueHide / DialogueHide |
| DialogueHistory.xaml | HUD | DialogueHistory | OpenDialogueHistory |
| ActiveRoll.xaml (dice) | PopupPanels | ActiveRoll | ActiveRollOpen, DialogActiveRollOpen |
| ReactionDecisionPopup.xaml | PopupPanels | ReactionsPopup | OpenReactionsPopup |
| SelectionFlyOut.xaml | PopupPanels | SelectionFlyOut | OpenSelectionFlyOut (from PlayerHUD / ActiveRoll) |
| GameMenu.xaml (+ AlwaysOnTopOverlay, ScreenFade) | Pause | Paused | ToggleGameMenu, IE.ToggleInGameMenu |
| Options + Audio/Video/Game/Keybinding/Controller/Interface/AccessibilityOptions.xaml | Pause | Options (SystemUIs) | OpenOptions, Open*Options |
| LoadGame.xaml / SaveGame.xaml | Pause | Paused.LoadGame / Paused.SaveGame | LoadGameOpen, SaveGameOpen |
| MessageBox.xaml | MessageBox | MessageBox | OpenMessageBox |
| PinnedTooltips.xaml | Notifications | PinnedTooltips | OpenPinnedTooltips |
| TadpolePowersTree, LearnSpells, CompanionsPanel, RewardPanel, LongRest, MakeCamp, Tutorial, TutorialPopup, Book .xaml | various | same | OpenTadpoleTree, OpenLearnSpells, ToggleCompanionsPanel, OpenRewardPanel, OpenLongRest, OpenMakeCamp, OpenTutorial, OpenTutorialPopup, ShowBook |
| CharacterCreation / CharacterLevelUp / CharacterRespec / CharacterFullRespec / CharacterChangeAppearance / Henchmen .xaml | HUD | same | Open* (ForceSplitscreen, HideStatesBelow) |
| Loading (LoadingScreen.xaml), Movie (AlwaysOnTopOverlay), Busy, Saving, MainMenu, Lobby, ... | Default / MessageBox / Pause / Notifications | | GE.* game events |

Controller mode uses `Controller.xaml` and the `_c` pages (`ab_sm_controller.txt`); e.g. PlayerPortraits_c is a stub
that loads `PreloadedPlayerPortraits_c`, and radial menus live in PlayerRadials. Not needed while the mod stays in KB/M.

### 1.6 Enumerating open widgets and states at run time (Confirmed layout)

```c
u8* ui = *(u8**)(*(u8**)(base + 0x6047958) + 0x130);
u8* wm = ui + 0x4C0;
EnterCriticalSection((CRITICAL_SECTION*)(wm + 0x150));
UIWidget** w = *(UIWidget***)(wm + 0x108);  u32 n = *(u32*)(wm + 0x114);
for (u32 i = 0; i < n; ++i) {
    u8* e = (u8*)w[i];
    const char* file = *(u32*)(e + 0x33C) >= 16 ? *(const char**)(e + 0x328) : (const char*)(e + 0x328);
    int layer = *(int*)(e + 0x320), order = *(int*)(e + 0x324);  short player = *(short*)(e + 0x394);
}
LeaveCriticalSection((CRITICAL_SECTION*)(wm + 0x150));
```

Do it from the 0x1210210 detour. The state tree is the alternative: `inst = *(u8**)(*(u8**)(ui+0x810) + 0x98)`, recurse
through `inst+0x10` (count `+0x1C`, each entry an array with count `+0xC`), widgets at `inst+0x00`, lock `inst+0x30`.
Whether preloaded (PreloadState) or hidden-below (HideStatesBelow) widgets stay in WM.Widgets is Unconfirmed; the
instance flags `+0x58` very likely carry that (log them, section 7).

---------------------------------------------------------------------------------------------------

## 2. Rendering

### 2.1 Where Noesis draws (F 1.2, Confirmed; additions here)

One Noesis view tree (MainCanvas) for all players, rendered once per frame on the render thread by stage 0x19's job
0x3CC04F0 -> GameUI Render 0x120FD10 into the frame-sized colour key **0x0C419ABC** (+ depth 0xFF6F1F7D), then
composited by stage 0x07. The RenderDevice builds one render pass for colour key `+0x4C` / depth `+0x48`
(0x212FAF0, 0x212FF30) with no explicit viewport; its "Offscreen" targets (0x212EB70, string 0x56C2068) are Noesis'
own temporary targets for opacity groups and effects, not per widget [ab37, ab40]. The second view (`NUIM+0x78`) is
drawn into the same target. Nothing in the exe renders a single widget or layer into its own texture.

### 2.2 Ways to get separate textures (evaluated)

| option | verdict |
|--------|---------|
| Per-widget render target in the game | does not exist (2.1). Confirmed |
| A second Noesis view per panel | Noesis elements have one parent; a widget would have to be moved out of MainCanvas into the new view's tree, and the view needs a renderer initialised on the game's RenderDevice (0x2128E90) with its own key. Possible in principle (CreateView 0x45C3A10, F), but it breaks the WindowManager layering and the game's per-frame update of that view. Not recommended |
| Render the main view several times with different widgets visible | needs a layout + render-tree update per pass (IView::Update is the expensive part) and visibility changes are dependency-property writes. Too costly |
| VisualBrush copy of an element (Noesis 3.1 has VisualBrush, strings 0x5382208) | would still land in the same target; only useful to re-layout, not to separate |
| **Crop sub-rectangles of the one UI texture** | cheapest: zero extra rendering. Each VR quad samples its own UV rect of the existing UI texture. Requires the element bounds (2.3). Recommended |

### 2.3 Bounds for the crops

The canvas is `W x 2160` design units with `W = w*2160/h` (F). Anchors from the XAML (Confirmed):

| element | anchor in design units | notes |
|---------|------------------------|-------|
| Hotbar `HotbarRoot` (HotBar.xaml) | bottom-centre; main grid margin bottom 12; level-up button at x+292, concentration at x+104, "call allies" up to 320 above | width depends on hotbar columns (SaveColumns/SetColumns commands, M 2.2); item tooltips pop up above it |
| Portraits `Root` (PlayerPortraits.xaml) | left edge, vertical stack of party portraits; companion button bottom-left (margin 0,0,0,48) | height grows with party size |
| Turn order `ParticipantsListContainer` (TurnModeInfo.xaml) | top-centre, margin 0,20 | collapsed outside turn-based mode |
| Combatants `RootPanel` (CombatantsOverlay.xaml) | top-centre, list 600 high, margin top -180 | |
| Target info `TargetInfoRoot` (TargetInfo.xaml) | top-centre, margin top 80, name panel 1400 wide; cast info bottom-centre 600 above the bottom | |
| Minimap (Minimap.xaml) | top-right, margin 15; map 624x624 + frame | |
| Combat log (CombatLog.xaml) | bottom-right, 20 from the right, 284..540 above the bottom, feed up to 700 high | |
| Dialogue (Dialogue.xaml + DialogueTemplates.xaml) | bottom; body text and answers centred (answer column 2120 wide, list max 490 high, `AnswersContainer` margin top 32); buttons bottom-left (600x460 hitbox); skip prompt bottom-right | dialogue state hides the HUD (HideStatesBelow) |
| Reaction popup (ReactionDecisionPopup.xaml) | bottom-centre, 360 above the bottom | |
| Notifications | top-centre at y 400; bottom-centre 400..510 above the bottom | |
| Character panel | panel 2080 wide, left margin 1672, bottom margin 1460 (relative to a 3840 canvas) | |

Three ways to turn anchors into pixel rects:

1. **Static generous rects** (Confirmed inputs): e.g. hotbar = bottom 22 % x centre 60 %, portraits = left 12 %,
   turn order = top 18 % x centre 60 %, dialogue = bottom 45 % x centre 75 %. The UI texture is transparent outside the
   drawn content (CombineUI blends it over the scene, F 1.3), so oversizing costs nothing visually; overlaps are rare
   because the states make HUD, dialogue and full-screen panels mutually exclusive.
2. **GPU alpha bounding box** inside each static rect (a small compute reduction over the UI texture, mod-side):
   exact content bounds every frame without touching Noesis. Recommended refinement.
3. **Exact element rects from Noesis**: `Noesis::Visual::PointToScreen` = **0x45FC6B0**(visual, Point* out, const
   Point* in) (SE signature, single match; V used it) maps element-local points to view pixels. The element pointers
   and their sizes need Noesis 3.1 internals (FindName, ActualWidth) that this pass did not locate natively
   (Unconfirmed). With Script Extender installed, Lua does all of it today: `Ext.UI.GetRoot():Find("HotbarRoot")`,
   `ActualWidth`/`ActualHeight` (dependency properties read through Noesis reflection, SE NsHelpers.inl l.186) and
   `PointToScreen` (SE VisualHelpers). Note SE's own warning: only from `Ext.UI.Defer` (the NUIM update).

Resolution: set the UI size with SetSize (F/O/X) high enough that the hotbar crop is sharp on the wrist. Whether a
SetSize smaller than the frame fills the whole 0x0C419ABC target (stretched, since no viewport is set in 0x212FF30) or
only its top-left corner is still Unconfirmed (F lead 2): the crop UV maths depends on it, so log it (section 7).

---------------------------------------------------------------------------------------------------

## 3. Input

### 3.1 Paths (F, M; Confirmed)

* Raw SDL / injected raw input -> InputManager::Update 0x40F0450 -> bindings -> FireEventDesc -> dispatch 0x40F1550
  -> InputListenerGroup (`im+0x2E8`) -> GameUI listener 0x1214430 (GameUI+8):
  a) per-player widget handlers (map in GameUI's input interface keyed by input event id; filled by
     `ls:LSInputBinding BoundEvent=".." PlayerId="{Binding CurrentPlayer.PlayerId}" Command=".."` elements, so a
     binding only fires for its own player), b) otherwise a kind-2 "IE.<event>" state-machine event (M 1.1).
  The input player index is mapped to the player id through PlayerManager `+0x70..+0x78` (B; GameUI+0x2DA8 holds the
  manager).
* Mouse: events 0x4E/1/2/0x5E/0x5F -> queued at GameUI+0x2FC8 -> in the update 0x1214070 -> NUIM 0x20C3660 /
  0x20C3B70 -> IView MouseMove/Down/Up/DoubleClick/Wheel at the cursor global `*(float**)0x614FB70` (F 2.1).
  0x1215260 (in the update) sends a pending MouseButtonUp (IView +0xC0) at the cursor global for queued event 1/2 when
  NUIM flags `+0x280` bits 2/4 are set [ab13].
* Text: listener slot 9 0x12154A0 feeds UTF-16 characters to IView +0x130 (Char) under the NUIM CS [ab13].
* NUIM update 0x20C2FD0 does **not** move the Noesis pointer each frame: it only recomputes `PointFromScreen(cursor)`
  (+0x250) and the mouse-over flag (+0x240) from the cursor global [F d15 lines 47-60, 289-320].

### 3.2 Driving Noesis without the OS cursor

Two levels, both independent of the Windows cursor:

* **Game-side pointer (recommended for panels)**: write the laser point in UI-view pixels
  (`u*[GameUI+0x194]`, `v*[GameUI+0x198]`, after mapping the panel UV through its crop rect) into the cursor global and
  inject raw motion / buttons with 0x40F1F00 before InputManager::Update (F 2.6). This keeps the game's double-click
  detection, drag-and-drop (the DnD manager and DragAndDropPreview / CursorText widgets follow the cursor global), hover
  and the "handled" flags. Replace the mod's current OS-cursor move with this.
* **Direct IView calls** (from the 0x1210210 detour, before the original):
  ```c
  void** view = *(void***)(ui + 0x130);
  EnterCriticalSection((CRITICAL_SECTION*)(ui + 0x258));
  ((bool(*)(void*,int,int))(*(void***)view)[0xD0/8])(view, x, y);          // MouseMove
  ((bool(*)(void*,int,int,int))(*(void***)view)[0xB8/8])(view, x, y, 0);   // MouseButtonDown, 0 = left
  ((bool(*)(void*,int,int,int))(*(void***)view)[0xC0/8])(view, x, y, 0);   // MouseButtonUp (next frame)
  LeaveCriticalSection((CRITICAL_SECTION*)(ui + 0x258));
  ```
  Slots and lock Confirmed (F 2.2, every game call site takes +0x258). Caveats (Confirmed from the code paths):
  double-click must be sent explicitly (+0xC8); CursorText / drag preview / mouse-over flag still follow the cursor
  global; real mouse motion over the window re-sends MouseMove at the real cursor. Use it for a second pointer or when
  the game-side pointer is busy with the world.

### 3.3 Invisible UI and world input (Confirmed paths, effect Unconfirmed)

The UI is still laid out over the whole screen while the eyes do not show it. Two consequences:

* World code asks GameUI slot 0x50 (**0x120E0C0**, returns `GameUI+0x240`) "is the mouse over the UI" before picking
  (0x1C45A90) and in the other callers listed in the table. When the laser points at the world and the projected
  cursor lands where e.g. the hotbar is laid out, picking is suppressed. Fix: detour 0x120E0C0 to return 0 unless the
  laser is on a UI panel.
* A raw left-mouse press produces both UILeftMouse (1, UI) and Action1 (4, world, I). If the UI is under the cursor,
  Noesis takes the click. For world actions inject **Action1 / ActionCancel as events** (M route B) instead of raw
  mouse buttons; keep raw mouse buttons for UI panels only.

### 3.4 Keyboard vs controller mode, and synthetic gamepad input

* The game decides the mode globally (F 2.4): 0x5E3C60 switches on input from device 0/1 (KB/M) or 3..10 (pads) and
  calls 0x5EB2B0 / 0x5EB490 (byte 0x614FBC8, read by picking and camera), whose callbacks reach GameUI slot 0xB0
  **0x1211EB0** SetInputDevice(device): layout +0x2D88 (0 KB/M, 1 pad), controller type from
  `InputManager+0x43C+dev*0x28`, option `GlobalSwitches+0x1D6` (1 = force KB layout, 2 = force pad), then the reload
  0x12126D0 (`_c` themes, `StateMachine_c` / Controller.xaml) and NUIM controller flag 0x10 (mouse ignored) [F d22].
* **Verdict**: synthetic gamepad (device 3..10) events switch the whole game to controller mode, so KB/M camera control
  and the laser pointer stop working. Controller-mode UI is therefore not usable alongside KB/M control without
  patching 0x5E3C60 and the many readers of 0x614FBC8. Calling 0x1211EB0 with a pad device while the global flag stays
  0 would load the `_c` UI with KB/M world control; nothing confirms the `_c` widgets work in that mix (Unconfirmed,
  not recommended).
* What works in KB/M mode without a pointer (Confirmed bindings): fire input events through route B with device 0:
  UIUp/UIDown (Dialogue.xaml binds them to SelectorUp/DownCommand), UISelectSlot0..12 (hotbar slots and dialogue
  answers via `BoundEvent`), UIAccept/UIBack/UICancel, UIEndTurn 0xE3, ToggleInventory etc. That event dispatch with
  DeviceId 0 leaves the mode alone is Unconfirmed (0x5E3C60 keys on the device id).

---------------------------------------------------------------------------------------------------

## 4. Game state the UI exposes

### 4.1 Reading it natively (offsets Confirmed by the property getters; chain Confirmed by 0x1741EC0 / 0x1300990)

```
gameData   = *(u8**)(ui + 0x880)                               // ls.GameData
vmPlayer   = 0x1300990(ui + 0x878, &playerId)                   // playerId = *(u16*)([0x60487A0] + 0xD0)
  +0x128 PlayerId (u16)  +0x2D8 UIData*  +0x650 IsCurrentTurn (u8)  +0x668 CanEndTurn (u8)
  +0x6B8 SelectedCharacter (VMCharacter*)  +0x738 CurrentCombatTurnTarget
vmChar     = vmPlayer+0x6B8
  +0x420 PlayerCharacterProperties*  +0x628 InTurnBasedMode (u8)
pcProps    = vmChar+0x420
  +0x9B0 ActiveDialogue (VMDialogue*)  +0x9D0 IsListening (u8)
vmDialogue = pcProps+0x9B0   (also cached in DCDialogue+0x878)
  +0x68 Layout  +0xF8 Answers (collection)  +0x2C0 SelectedAnswer  +0x2E0 LocalHighlightedAnswer
  +0x300 ShowAnswers (u8)  +0x318 ContinueAnswer  +0x340 DialogInstanceID (u32)
answer (gui::VMDialogueAnswer)
  +0x48 Enabled (u8)  +0x60 BodyText (LocaString)  +0x98 AnswerIdx (u32)  +0xC8 BoundEvent (input event id)
```

Collections (players, answers): count = `0x45C28A0(coll + 0x10)`, item = `0x45C2690(coll + 0x10, &ptr, i)` (returns a
ref-counted pointer; release it the way 0x20E86E0 does: decrement `ptr+8`, call `vf[0x20]` at 1) [ab3]; GameData's
players are read with `0x1289500(gameData + 0xC8, i)` and counted on `gameData+0xE0` [ab36]. Getters: 0x138A500
(IsCurrentTurn), 0x140CE90 (SelectedCharacter), 0x1389F90 (InTurnBasedMode), 0x13D61D0 (ActiveDialogue), 0x14E0CC0 /
0x14E0CB0 / 0x126A730 / 0x138A4F0 / 0x14E0C10 (VMDialogue), 0x1389660 / 0x13FB7C0 (answer). Read them from the
0x1210210 detour; VM objects are written by the gui ECS systems, so read pointers once per frame and do not keep them.

### 4.2 Combat and turns

* In turn-based mode (combat, or manual TB): selected `VMCharacter+0x628`. Whose turn: `VMPlayer+0x650` (the local
  player has the turn), `+0x668` (can end turn). Turn order / current combatant: DCFTBParticipants (TurnModeInfo) and
  DCCombatants (`CurrentCombatant.IsCurrentTurn` in XAML) - not decoded natively here.
* Engine-side alternatives (not UI): `eoc::IsInTurnBasedModeComponent` (type id 0x605BAA4), `eoc::TurnBasedComponent`
  (0x6058DE4) (type-id map).

### 4.3 Dialogue

* Active: VMDialogue pointer non-null, or the `Dialogue` state / Dialogue.xaml widget open (1.6), or FireEvent
  GE.OnDialogueShow / GE.OnDialogueHide.
* Answers visible: `ShowAnswers` (+0x300); otherwise the player is listening ("press to continue":
  `LSInputBinding BoundEvent="UISelectSlot1"` while ShowAnswers is false, Dialogue.xaml l.87).
* Highlighted: `LocalHighlightedAnswer` (+0x2E0; the list box binds `SelectedItem` to it). Number of answers: the
  collection at +0xF8.
* Select: fire the answer's `BoundEvent` (+0xC8) as an input event (route B) - exactly what the answer button binds;
  or call the DCDialogue command handler **0x1741EC0**(DCDialogue*, 0, answerVM) (DCDialogue fields: commands at
  +0x880 ShowAnswers, +0x888 StopListening, +0x8B0 SelectorUp, +0x8B8 SelectorDown, +0x8C0 SelectorEnter
  (0x1741E90, selects the highlighted answer), +0x8C8 SelectorSkip, +0x8D0 HighlightAnswer (0x1741D80), **+0x8D8
  SelectAnswer (0x1741EC0)**; handler addresses from the ctor 0x17411B0's `lea rax, handler` before each
  `0x2119670` + store) [ab33, ab35]. 0x1741EC0 ends in **0x1FC3130**(`[[0x60089B0]+0x160]`, DialogInstanceID,
  (u8)AnswerIdx, VMPlayer+0x148) unless a multiplayer speaker switch applies.

### 4.4 Which windows are open

Walk WM.Widgets (1.6) and match the file names of section 1.5 (CharacterPanel = inventory / character sheet /
spellbook, PartyPanel, Journal*, Trade, Container (loot), Pickpocket, GameMenu, Options pages, MessageBox, ...). The
CharacterPanel tab is carried by the state metadata (`Metadata=ToggleInventory/ToggleSpells/...`), not the file. For
event-driven tracking, hook FireEvent 0x20C8340 (named events of 1.5) or 0x20EA720 (widget added).

### 4.5 Tooltips and the context menu

Item/spell tooltips are Noesis tooltips / popups inside the same view (ls tooltip commands on every UIWidget:
ToggleTooltipOnUIElementCommand, PinTooltipCommand, ... [ab29]; TooltipManager GameUI+0x2180), so they appear in the
UI texture next to the pointer, not as separate widgets. Pinned tooltips are a widget (PinnedTooltips.xaml). The world
context menu is WorldContextMenu.xaml (Notifications layer, input event ContextMenu 0xD6); world tooltips
WorldTooltips.xaml (F 3.1 projection). Tooltip rectangles natively: Unconfirmed (use the alpha bounding box around the
pointer).

### 4.6 Script Extender route

`Ext.UI.GetRoot()` (MainCanvas), `Ext.UI.GetStateMachine()` (UIStateMachine), DependencyObject property get/set by
name, `ICommand:Execute`, `PointToScreen`, `Find`; only inside `Ext.UI.Defer` (runs in the NUIM update). SE
`Ext.UI.SetState` is a stub (M). Useful for exact element rects (2.3) and for reading DC properties by name.

---------------------------------------------------------------------------------------------------

## 5. Recommended VR UI design

Everything comes from the one existing UI texture (crops), the panel visibility is mod-side, and the game keeps
running its normal KB/M UI.

| panel | content (crop of the UI texture) | placement | shown when | driven by |
|-------|----------------------------------|-----------|------------|-----------|
| Wrist HUD (left wrist, palm up) | hotbar crop + portraits crop (two quads) | wrist, tilted to face the head | wrist gesture / button; auto in turn-based mode | laser (game-side pointer, 3.2); buttons -> UISelectSlotN events; spell = UseSpell 0x1FE4150 (M) |
| Combat bar | turn order (TurnModeInfo) + combatants + target info crops | head-locked high or world-anchored above the party | `VMCharacter+0x628` InTurnBasedMode of the selected character; highlight when `VMPlayer+0x650` | read-only; end turn = UIEndTurn 0xE3 event (button) or laser on the hotbar's end-turn button |
| Dialogue | bottom ~45 % x centre ~75 % of the canvas (answers, speaker text) | in front, 1.2-1.5 m, slightly below eye level, facing the speaker | VMDialogue non-null (or Dialogue widget open) | laser click; stick up/down = UIUp/UIDown events (bound to SelectorUp/Down), trigger = click, quick select = answer `BoundEvent`; continue = UISelectSlot1 while `ShowAnswers` = 0 |
| Window panel | the whole UI texture | in front, large (1.6 m wide at 1.8 m) | any widget whose file is a panel / pause / message box (1.5) is in WM.Widgets | laser click + wheel (game-side pointer); open/close via route A events from a VR menu |
| Pointer tooltip | crop around the Noesis pointer, refined by alpha bbox | next to the panel the laser is on | while the laser is on any panel | none |
| Minimap (optional) | top-right crop | back of the hand | button | laser |

Per-frame flow (all in the 0x1210210 detour except the render-side crop):
1. Read state (4.1): turn-based flag, current turn, VMDialogue / ShowAnswers, open widget files (1.6).
2. Decide panels; compute crop rects (static + alpha bbox from the previous frame).
3. Laser hit test against visible panels; if on a panel, map UV -> crop -> UI-view pixels -> cursor global + raw
   motion/buttons (or direct IView); else force "mouse over UI" to 0 (3.3) and send world actions as events.
4. Optionally `SetActive(false)` (GameUI+0x84) while no panel is visible, to save the UI draw (X #5); confirm first
   that hidden-UI side effects are harmless (section 7).
5. Render thread: after stage 0x19 the UI texture is complete (F); draw each panel quad with its UV rect.

Rendering characters / items in 3D stays as in P and V (paperdolls, item icons); the panels above do not change it.

---------------------------------------------------------------------------------------------------

## 6. Function / data index (new in this report; F/M tables still apply)

| rva | what |
|-----|------|
| 0x716F00 / 0x7493E0 | ecl::UISystem registration / update callback -> GameUI slot 0xC0 |
| 0x6059234 | ecl::UISystem type id global |
| 0x11F1080 / 0x5FDA238 | GameUI factory (alloc 0x31C0) / factory pointer global (set by 0x5777D0) |
| 0x1847660 / 0x1847770 | game-state handlers that install / activate GameUI (`[gRM+0x130]`) |
| 0x1210210 | GameUI::Update |
| 0x120E0A0 / 0x120E0B0 | GameUI SetActive / IsActive (+0x84) |
| 0x120E0C0 | GameUI IsMouseOverUI (+0x240) |
| 0x120E0D0 | GameUI text-input flag (NUIM flags bit 7) |
| 0x12101F0 | GameUI ready test (+0x2D95 & 8, state 4/5) |
| 0x1215260 | GameUI deferred mouse-up from queued input events |
| 0x12154A0 | GameUI listener slot 9: text input -> IView Char |
| 0x20E74B0 / 0x20E86E0 | WindowManager ctor / update (sort, add to canvas, removals) |
| 0x20EA050 / 0x20EA720 | WindowManager AddWidget / finish (into WM.Widgets) |
| 0x20E83B0 / 0x20E9150 | AddWidget callers (load completion) |
| 0x210F0D0 / 0x210D590 | UIWidget ctor / `ls.UIWidget` reflection |
| 0x14792B0 | UIWidget Filename getter (+0x328) |
| 0x20C8AE0 / 0x20C89B0 | UIStateMachine event dispatch over the instance tree / per-widget broadcast |
| 0x20C86E0 / 0x20C6AB0 | UIStateMachine per-frame steps (called from the update) |
| 0x1300990 | VMProvider: VMPlayer by player id |
| 0x14DFFA0 / 0x14E2C00 / 0x141B590 / 0x13D0830 / 0x13D6290 / 0x17407D0 | reflection: VMDialogue / VMDialogueAnswer / VMPlayer / VMCharacter / VMPlayerCharacterProperties / DCDialogue |
| 0x17411B0 | DCDialogue ctor (command bindings) |
| 0x1741EC0 / 0x1741E90 / 0x1741D80 | DCDialogue SelectAnswer / SelectorEnter / HighlightAnswer handlers |
| 0x1FC3130 | dialogue answer request |
| 0x45FC6B0 / 0x45FC390 | Noesis Visual::PointToScreen / PointFromScreen (SE signatures) |
| 0x45C28A0 / 0x45C2690 | Noesis collection count / item (usage) |
| 0x3BB1DC0 | per-view render setup that copies view record +0x3B into GlobalSwitches+0x0D (F lead 4: +0x0D is a per-view "draw UI" flag set by the renderer, not a user option) |

---------------------------------------------------------------------------------------------------

## 7. One-run logging plan (everything Unconfirmed above)

All from one session: a detour on 0x1210210 (log before/after the original), detours on 0x20EA720, 0x20C8340,
0x120E0C0, 0x7493E0 and the render job 0x3CC04F0. Log on change, plus a full dump every 300th frame.

1. **Threads**: `GetCurrentThreadId()` in 0x7493E0 / 0x1210210, in the GameUI listener 0x1214430, in 0x3CC04F0, and in
   the game loop (the existing InputManager::Update hook). Settles 1.2.
2. **Widget list**: per widget: pointer, FileName text (+0x328), LayerIndex (+0x320), IndexInLayer (+0x324), PlayerId
   (+0x394), state GUID (+0x308). Also dump the layer map WM+0x128 (FixedString ids -> index) once, and the
   FixedString ids of the 11 layer names (created with 0x4194CA0) to map them. Settles FileName form and layer
   indices. Open the inventory, the map, a trade, a dialogue, the game menu during the run.
3. **State tree**: recursive dump from `[[ui+0x810]+0x98]`: depth, inst+0x28 player, +0x58/+0x59/+0x5B bytes,
   +0x60 id, widget count, UIState pointer. Compare +0x60 with FixedStrings made for "Dialogue", "CharacterPanel",
   "PlayerHUD" etc. Also note which instances change +0x58 when Dialogue opens (HideStatesBelow) and whether
   preloaded Dialogue widgets appear in WM.Widgets before GE.OnDialogueShow.
4. **FireEvent**: kind, EventIndex, event FixedString id, SubState id, player, `result+0x38`, `result+0x3B` - while
   pressing I, M, J, Esc, starting a dialogue and a fight. Confirms M's uiPlayer (= `[0x60487A0]+0xD0`?).
5. **View-models** (in the 0x1210210 detour): player id, VMPlayer, +0x650, +0x668, SelectedCharacter, +0x628,
   ActiveDialogue, ShowAnswers, answers count and for each answer AnswerIdx / BoundEvent / Enabled,
   LocalHighlightedAnswer. Enter and leave combat, end a turn, run one dialogue.
6. **Dialogue selection**: in one dialogue, fire the highlighted answer's BoundEvent through route B with DeviceId 0;
   log ActiveDialogue before/after and byte 0x614FBC8 (mode must stay 0).
7. **UI size**: after SetSize(1600, 1200) with a larger frame, copy 0x0C419ABC to a staging texture and log the
   bounding box of non-zero alpha; tells whether the UI is stretched to the target or drawn top-left.
8. **Direct IView injection**: while the game menu is open, call MouseMove + Down + Up (3.2) on the Resume button
   position (GameMenu.xaml: centred, buttons from y 900); log the return values, NUIM +0x240 and whether the menu
   closes (Paused state gone in the state dump).
9. **MouseOverUI override**: log 0x120E0C0's caller (return address) and the original value while the laser points at
   the world over the hotbar area, with and without the override; check that world picking works.
10. **SetActive(false)**: toggle GameUI+0x84 for 300 frames; log update still running (tick +0x3190), hit-test flag,
    and whether anything else (cursor, overheads, Bink videos in stage 0x19) changes.
11. **Alpha bounding boxes**: per static crop rect, log the GPU bbox once per second against the XAML anchors (2.3).

---------------------------------------------------------------------------------------------------

## Open leads

1. Native element rects (Noesis 3.1 FindName / ActualWidth / visibility) - needed only if the alpha bbox is not good
   enough; SE Lua already has them.
2. DCDialogue / other DC instances natively (DataContextProvider GameUI+0x1CC0 per-player map, SE layout) - not needed
   for the design above.
3. `TogglePresentation` (input event 0xA2) handler - possibly the game's own "hide HUD" switch.
4. DCFTBParticipants / DCCombatants fields (turn order, current combatant) for a native turn-order display.
5. `GameUI+0x2B90` (used by render prep 0x120FFE0 and the update's deferred step 0x20F5950) and `[0x60089B0]`
   (dialogue manager source at +0x150/+0x160).
