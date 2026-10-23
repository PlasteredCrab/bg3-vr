# G: camera system and dialogue camera (bg3_dx11.exe 4.1.1.7631656)

Agent G, static analysis only. All addresses are RVAs (image base 0x140000000). Raw decompiles and dumps are in
`re-local\ghidra\agents\G\` and are cited as `[rNN]` (file `G\rNN_*.c/.txt`). "SE" marks names/layouts taken from the
Script Extender source (`downloads\bg3se-src`); a SE name is only attached to an offset when the code's behaviour fits
it, and the report says so.

New tooling (in `G\`):
- `TypeIdMap.java` + `typeidmap.txt`: map of **10 483 type-id / query-id globals to names**, including every
  `ecs::query::spec::Spec<...>` query id (resolved through their `GetTypeName<>` helper functions, which the older
  TypeIdRefs could not follow). `ann.ps1 <file.c>` lists the named globals used in a decompile.
- `SysReg.java` (`r9_sysreg_named.txt`): registration function, update callback and body for 150 camera / timeline /
  sound / fade / splitscreen systems.

---------------------------------------------------------------------------------------------------

## Hook points for the VR mod

| Need | RVA | How to use | Confidence |
|---|---|---|---|
| **Keep the gameplay camera on the player's view during a timeline (dialogue)** | Timeline object **+0x4C5** (byte), tested at **0x2007AE9** in `Timeline::ActivateCamera` 0x2007570 | Set `timeline[+0x4C5] = 1` for timelines that must stay first person. 0x2007570 then skips its only view switch (`call 0x17F7E10` at 0x2007B2C) for every shot change, join and re-activation of that timeline; everything else in it (lighting camera, bookkeeping) still runs. No instruction in the exe writes `[reg+0x4C5]` (whole-program displacement search [r16]), so the flag stays as set. SE name of this field: `InGameTimelineFreeCamEnabled`. | Confirmed (disasm 0x2007AE9..0x2007B2C, [r24]); field init by the Timeline ctor unconfirmed |
| Where to decide per timeline (has TimelineType) | **0x1D5B890** `OnTimelinePlayerJoined(sys, ctx, Timeline*, TimelineData*, playerEntity*, …)` | Hook entry: `r8` = Timeline*, `r9` = TimelineData* (`+0x78` = TimelineType, `+0x60` = DialogInstanceId (SE)). Types {1,2,3,8,10,11} (mask 0xD0E) take the camera here; set +0x4C5 before calling the original. | Confirmed (decompile [r24] 0x1D5B890) |
| Alternative single choke point for the view switch | **0x2007570** entry (rcx = camera-actor Guid*, dx = player id or 0 = all, r8 = Timeline*, 5th arg flag) or the call at **0x2007B2C** | Hook and return without switching (or set +0x4C5 around the original). All 13 callers (shot components 0x20458E0 / 0x2046450 / 0x2046DA0 / 0x2047470 / 0x204E720, TimelineSystem 0x1D55400 / 0x1D5B890 / 0x1D5BE40 / 0x1D62890 / 0x1D63A90 / 0x1D64220, re-activation 0x1D79A50, 0x1F21810) go through it. | Confirmed ([r8], [r24]) |
| Lowest-level camera → view assignment (catches everything) | **0x3CC90B0** `SetCameraController(view, controller, keep)`; game wrapper **0x17F7CD0** `SetPlayerViewCamera(mgr, playerId, controller, keep, immediate, combatFlag)` | Reject controllers whose `ls::CameraComponent.MasterBehaviorType` (component +0x00) is 11 (scene/timeline camera) when the mod wants first person. Callers of 0x3CC90B0: 0x17F7CD0, 0x1D11AE0 (character creation), 0x17F2B30 (view vtbl slot 10), 0x1B76070 / 0x1B77520 (BasicGameObjectRenderer). | Confirmed ([r7], callers_setcam.txt) |
| Camera return after a dialogue | **0x1D5BE40** (actor leaves timeline) → `0x17F7AD0(playerMgr, cb 0x1D79890)` → **0x1D79890** → 0x17F7CD0(own `GameCamera_playerN` controller) | Nothing to do if the switch was suppressed; note it re-calls SetCameraController with the gameplay controller (Exit/Enter of the same controller). | Confirmed ([r24] lines 1901-1910, [r7]) |
| Gameplay camera re-target to the character after a type-1 timeline | **0x1F579A0** | Camera target request (0x1E1A1A0 → 0x1E1AE70) and `GameCameraBehavior+0x1E8 = character`. Harmless with the Transform override. | Confirmed ([r37]) |
| Render view "cinematic mode" flag | **0x3CCCC20**(view, flag) writes **view+0x2961**; called by **0x1F90B00** with 1 on join / 0 on leave for types {1,2,3,10,11} | Skip (or force 0) to keep gameplay render settings during first-person dialogues. 15 render functions read +0x2961 (list in 4.4). SE: `GameRenderView::IsCinematicMode`. | Confirmed write/callers; effect of readers unconfirmed |
| Combat-camera view flag | **view+0x2962** written by 0x17F7CD0 from 0x2007570's `IsSystemic/AuthoredCombat` flags (+0x48A / +0x48B) | Read only by 0x3EE6070. SE: `IsInCombatCameraMode`. | Confirmed write; reader unconfirmed |
| Sound listener position | **0x1E93FA0** (ecl::sound::CameraSystem 0x1E93410) | The listener is attached to the camera entity of the controller in each player view's ReadState (`view+0x21D8 → +0x318`). Keeping the gameplay controller on the view keeps the listener at the HMD camera. | Confirmed ([r35]) |
| Tactical camera | event **0xD9** in 0x1E0EE20 → **0x1E1A860**(gcb, 4, on) | Tactical = bit 0x04 of `GameCameraBehavior+0xA8` (SE `CameraMode`). Swallow event 0xD9 or clear the bit. | Confirmed ([r49], C's r44) |
| Obstruction fading on/off | **0x18CF450** (obstruction RaycastSystem body) | `results+0x80 = [[sys+0x20]+0x1CE] && gamestate::Running`; ecl::obstruction::FadeSystem 0x18D4AA0 does nothing when results+0x80 is 0. Force the byte at results+0x80 (or the source flag) to 0 to stop fading objects between camera and character. | Confirmed control flow; meaning of `[sys+0x20]` unconfirmed |
| Gameplay camera per-frame update | **0x1E0DDB0** (called from CameraSystem body 0x1E0A160 for MasterBehaviorType 8 when `CameraComponent+0x14` (Active) and 0x1E0D940 pass) | Already overridden through the Transform hook at 0x4085150 (camera.md); this is where the orbit logic lives if it ever needs to be stopped. | Confirmed ([r1]) |
| Close-up dialogue framing of the gameplay camera | **0x1FC2A70** (ecl::DialogSystem, via 0x1FC2330) | Issues gameplay-camera target/distance requests for dialogues of kind 5 (`+0x158 == 5`); distance halves when GlobalSwitches **+0x67D** (`GameCameraEnableCloseUpDialog`) is set. Harmless with the Transform override. | Confirmed ([r22]) |
| Timeline camera creation | **0x1F1C730** (TimelineCameraSystem) | Creates a 0x330-byte controller (vtable 0x58B6750), registers it in the camera manager under the camera's GUID string, adds `ls::CameraComponent` with MasterBehaviorType **11**. Log here to see every dialogue camera. | Confirmed ([r13], [r15]) |

---------------------------------------------------------------------------------------------------

## 1. Camera system structure

### 1.1 Objects

**Camera entity + controller.** A camera is an ECS entity with `ls::CameraComponent` (type id 0x6060A08, 0xB0 bytes)
plus behaviour components. The component holds a pointer to an `rf::CameraController`.

`ls::CameraComponent` (0xB0), from the writer 0x1F1FC40 [r15] (field names SE):

| Off | Field | Evidence |
|---|---|---|
| +0x00 | MasterBehaviorType (u32; low byte compared everywhere) | 0x1F1FC40 `*puVar9 = *param_5`; camera-system loops compare `*pc == 8 / 0xB / 0xC / 6` |
| +0x08 | Controller* | 0x1F1FC40; `*(comp+8)` used by 0x17F7E10, 0x2007570 |
| +0x10 | ExposureSettingIndex | 0x1F1FC40 |
| +0x14 | Active (u8) | 0x1F1FC40 writes 0; CameraSystem body skips behaviours with `[+0x14]==0` [r1] |
| +0x15 | AcceptsInput | 0x1F1FC40; C 3.4 (0x1E0B740 checks +0x15) |
| +0x16 / +0x17 | UseCameraPPSettings / UseSplitScreenFov | 0x1F1FC40 |
| +0x18.. | PostProcess settings (initialised by 0x3D59960) | 0x1F1FC40 |

**Controllers.** The ECS-backed controller class (vtable **0x58B6750**, 0x330 bytes, base ctor 0x4084780 sets base
vtable 0x58EDC60) is used for the gameplay camera, timeline cameras, photo-mode and character-creation cameras. It is
created by:

| Creator | Who | Evidence |
|---|---|---|
| 0x3B96300 (generic, via 0x3B96260 "create camera entity") | player cameras `GameCamera_playerN` (B 4.3 step 3), photo mode (0x1E0BEC0) | [r33] |
| 0x1F1C730 | timeline (scene) cameras | [r13] |
| 0x1C9ACE0 | character creation camera (unconfirmed purpose) | vtable xref [r32] |
| 0x5878A0 | app init (unconfirmed purpose) | vtable xref [r32] |

Controller fields used by the code: `+0x310` behaviour handler (the owning system object), `+0x318` camera entity
handle, `+0x320` entity world/storage, `+0x80` ID (FixedString; set by the camera manager 0x40873B0). Every
transform/fov call is forwarded to the handler: vtable dump [r31]/(this report):

| Slot (+off) | Fn | Does |
|---|---|---|
| 1 (+0x08) Enter | 0x3CF0080 | per-view cull entry (A 1.3), then `handler->vf[0x90](handler, &entity, &prevCamEntity)` |
| 2 (+0x10) Exit | 0x3CF0210 | removes cull entry, `handler->vf[0x98](handler, &entity)` |
| 3 / 4 GetFov / GetAspect | 0x3CF0870 / 0x3CF08A0 | `handler->vf[0x80] / [0x88]` |
| 5 (+0x28) | 0x3CF07C0 | `handler->vf[0x58]` (SE "Update"; camera.md found it never runs for the gameplay camera) |
| 9 / 11 / 13 | 0x3CF0740 / 0x3CF0760 | camera entity Transform (camera.md) |
| 10 / 12 / 14 setters | 0x3CF0470 / 0x3CF0560 / 0x3CF0650 | `TransformSystem::SetTranslate/SetRotate/SetTransform` 0x3C77BD0 / 0x3C77D20 / 0x3C78110 (E report) on the camera entity |
| **15 (+0x78)** | **0x3CF0430** | `handler->vf[0x38](handler, out, &entity, event)` → for the gameplay camera **0x1E0B740** = the input handler of C 3.4 |
| 17 (+0x88) | 0x3CF0780 | `handler->vf[0x70](handler, &entity, f1, f2)` → 0x3B96610 (moves the camera along its right/forward axes; a pan, not input) |

**Correction to C_animation_systems.md 3.4:** slot 17 (0x3CF0780) is a two-float call, not OnInputEvent; the
OnInputEvent-shaped slot is **15 (+0x78, 0x3CF0430)**, which reaches 0x1E0B740 through handler slot +0x38. The
endpoint C found (0x1E0B740 → 0x1E0EE20) is right. Confirmed: vtable 0x58B6750 dump, disasm of 0x3CF0780,
handler vtable 0x5681E98 (+0x38 = 0x1E0B740, +0x70 = 0x3B96610).

**Handler of the gameplay camera = `ecl::CameraSystem`** (ctor 0x1E09340, primary vtable **0x5681E98**,
secondary 0x5681F40 / 0x5681FB0) [r32]. Handler slots: +0x38 0x1E0B740 (input), +0x58 0x3B93540, +0x60 0x3B93660
(look-at: sets rotation, writes controller LookAt, calls the view update 0x4085150), +0x68 0x1E1A9D0, +0x78
0x1E1AA60 (zoom-to-fit), +0x80 0x3B967F0 (fov = controller+0x14), +0x88 0x3B96860 (aspect = controller+0x20),
**+0x90 Enter 0x1E0BB60**, **+0x98 Exit 0x1E0BCD0**, +0xB8 0x1E0C570 (clears CameraMode bits 0-1) [r33].
- Enter 0x1E0BB60: resets the `ecl::EocCameraBehavior`-sized component (0x702EB0 getter, fields +0x18..+0x3A),
  then for `GameCameraBehavior` snaps the camera to its target (0x1E1FAC0 → 0x1E0D5A0).
- Exit 0x1E0BCD0: clears GameCameraBehavior motion fields (+0x94, +0x9C, +0xA0, bit 0 of +0xA9, +0x1F0), toggles
  +0x188 (SE `ResetRotation`), and for **every render view whose ReadState controller (+0x21D8) has this camera's ID**
  resets per-view data at `[view+0x28D8]+0x1A8..+0x229` (writes 2.8f, 10.0f, 0) and calls 0x3CCA9A0(view
  controller, …). Values look like DOF/focus defaults (unconfirmed).

**Camera manager** `DAT_14604AB20`: name → controller map. Register 0x40873B0(mgr, controller, &FixedString name)
(also writes controller+0x80 = name); lookup **0x4087790(mgr, &name)** [r15]. Gameplay cameras are registered by
0x3B96300, timeline cameras by 0x1F1C730 under their **GUID string** (`"%08x-%04hx-…"`).

### 1.2 Components (resolved type ids)

Behaviour / camera components and their query use in the CameraSystem body (all names from `typeidmap.txt`):
`ls::CameraComponent`, `ecl::EocCameraBehavior` (0x40 bytes; +0x38 = player id, [r1] 0x1E0A160 / [r7] 0x2007570),
`ecl::GameCameraBehavior` (0x258 bytes = SE layout size; per-entity stride 600 in [r1]), `ecl::SceneCameraBehavior`,
`ecl::SpectatorCameraBehavior`, `ecl::FreeCameraBehavior`, `ls::DefaultCameraBehavior`, `ls::EditorCameraBehavior`,
`ls::EffectCameraBehavior`, `ls::OrthoCameraBehavior`, `ls::WorldMapCameraBehavior`, `ls::ArcBallCameraBehavior`,
`ecl::camera::PhotoModeCameraBehaviorComponent`, `ecl::camera::{Target,CombatTarget,PlatformTarget,SelectorMode,…}`,
`ecl::TimelineCameraShotComponent`, `ecl::TimelineCameraRequestComponent`, `ls::SoundCameraComponent`,
`ecl::splitscreen::LetterboxSingletonComponent`. Full list: `G\types_camera.txt`.

### 1.3 MasterBehaviorType values (from code)

| Value | Behaviour | Evidence | Confidence |
|---|---|---|---|
| 3 | sound camera (entity with only `ls::SoundCameraComponent`) | camera.md runtime probe | Confirmed (runtime) |
| 6 | `EocCameraBehavior` without `GameCameraBehavior`; update 0x1E0CBB0 (handles Atmosphere/Lighting trigger queries) | 0x1E0A160 query 0x60C4984 + `*pc == 6` | Confirmed type; name unconfirmed |
| 8 | gameplay camera (`EocCameraBehavior` + `GameCameraBehavior` + Transform); update **0x1E0DDB0** | query 0x60C498C + `*pc == 8` | Confirmed |
| 11 (0xB) | **scene / timeline camera** (`SceneCameraBehavior` + `EocCameraBehavior`); per-player update 0x1E1D4C0; created with 11 by 0x1F1C730 → 0x1F1FC40 | query 0x60C4980 + `*pc == 0xB`; 0x1F1C730 passes 0xB | Confirmed |
| 12 (0xC) | spectator camera (`SpectatorCameraBehavior`); update 0x1E10E70 | query 0x60C4988 + `*pc == 0xC` | Confirmed |

### 1.4 ecl::CameraSystem body 0x1E0A160 [r1], order of work

1. On `ecl::UnloadSessionEventComponent` added: per player 0x1E207B0 (clears the tactical bit, `&= 0xFB`, at 0x1E20855).
2. 0x1E12B90 (all `EocCameraBehavior+GameCameraBehavior`), 0x1E14130 (render frames).
3. If `sys+0x101`: zero GameCameraBehavior motion fields for all game cameras.
4. On `camera::PlatformTargetComponent` added: GameCameraBehavior +0x144/+0x148 = 0.
5. If a level is loaded (`[sys+0x118]+0x90`): 0x1E0C870 (party follower removed), 0x1E122D0 (jump started; reads
   GlobalSwitches +0x682 `GameCameraEnableFlyFollow`), 0x1E12800 (spell cast state added), **0x1E14CB0 (on
   `ecl::TimelineActorLeftEventOneFrameComponent`: per player 0x1E204B0)**, 0x1E14F60;
   then for cameras that are not type 8 but have pending GameCameraBehavior snaps (+0x244 / +0x254) → 0x1E0D8F0;
   then type 11 → 0x1E1D4C0, type 6 → 0x1E0CBB0, type 12 → 0x1E10E70, type 8 → 0x1E0D940 (screen-fade check) →
   **0x1E0DDB0**; then 0x1E11860 (screen fade visual data), 0x1E0BEC0 (photo mode sessions), 0x1E10BE0 (enter photo
   mode requests).

### 1.5 How the active camera reaches the render view

There is no per-frame "pick the active camera": each player's render view holds one controller
(WriteState `+0x2260`, ReadState `+0x21D8`, A/B reports), and it changes only through **0x3CC90B0
SetCameraController** (Exit old, Enter new, A 11). The game-side routes:

```
player join     0x17F5CB0 → 0x17F7E10(mgr, id, &GameCamera_playerN, 1,1,0) → 0x17F7CD0 → 0x3CC90B0
timeline camera 0x2007570 → 0x17F7E10(mgr, id, &sceneCamEntity, 0,0,combat) → 0x17F7CD0 → 0x3CC90B0
camera return   0x1D5BE40 → 0x17F7AD0(mgr, 0x1D79890) → 0x1D79890 → 0x17F7CD0(own controller)
level/scene     0x424E6A0 (data-ref'd lambda) → 0x17F7E10(…,0,1,0) + 0x17F8160 (scene)
lambda          0x1C57980 (from 0x1C573C0) → 0x17F7E10(…,1,1,0)
char creation   0x1D11CE0 → 0x1D11AE0 → 0x3CC90B0 directly, then 0x3CCCC20(view,0)
```
`0x17F7E10(mgr, id, &cameraEntity, keep, attach, combatFlag)`: controller = `CameraComponent(entity)+8`
(0x17FAB50), then 0x17F7CD0. `0x17F7CD0`: if the view exists and `view+0x2C98 == 0` → 0x3CC90B0; always
`view+0x2CA0 = controller`, `view+0x2962 = combatFlag`; if no view and `immediate == 0` it queues 0x17FA690 [r7].
`view+0x2C98` (camera override) is only written by the view ctor 0x17F1F49 (whole-program search [r39]), so in
practice the override is never set.

---------------------------------------------------------------------------------------------------

## 2. Dialogue vs cutscene

### 2.1 Systems (registration → callback → body)

| System | Reg | Callback | Body | Source |
|---|---|---|---|---|
| ecl::TimelineSystem (class system, 0x1080 bytes, ctor 0x1D3D140) | 0x731450 | 0x75A780 | **0x1D3FF10** | [r11], [r12] |
| ecl::TimelineCameraSystem | 0x748330 | 0x7618B0 | **0x1F1CBA0** (scene camera update), **0x1F1C730** (create) | [r11], [r13] |
| ecl::TimelinePostSystem | 0x7486D0 | 0x7619D0 | 0x1F208D0 | [r13] |
| ecl::TimelinePreAnimationUpdateSystem | 0x747DA0 | 0x761740 | 0x1F1A1E0 | C |
| ecl::TimelineTransformFlushSystem | 0x748A00 | 0x761A20 | 0x1F22770 | [r9], [r37] |
| ecl::TimelineFadeTransitionSystem | 0x731C30 | 0x75AEB0 | 0x1D92880 | [r9] |
| ecl::timeline::ActorParticipationSystem | 0x731250 | 0x75A730 | 0x1D3A4D0 | [r9] |
| ecl::DialogSystem (0x5D0 bytes) | 0x731E40 | 0x75B2C0 | 0x393F950 + **0x1FC2330** | [r11], [r12] |
| ecl::dialog::EventsSystem | 0x714E50 | 0x75B1A0 | 0x1D95AD0 | [r9] |
| ecl::CameraSplineSystem | 0x715010 | 0x75C0B0 / 0x7346D0 | 0x1C98BB0 / 0x4003B20 | [r9] |
| ecl::screen_fade::ScreenFadeSystem / Syncing / Movie | 0x7243D0 / 0x724700 / 0x7248E0 | | 0x1901BA0 / 0x1910910 / 0x1911EC0 | [r9] |

### 2.2 Timeline camera = MasterBehaviorType 11

TimelineCameraSystem 0x1F1C730 [r13] creates a timeline camera: controller (vtable 0x58B6750, handler =
TimelineCameraSystem, `+0x318` = camera entity), registered in the camera manager under the camera's GUID string;
components: `ls::CameraComponent` (MasterBehaviorType 11, AcceptsInput 1, UseCameraPPSettings 1, FOV 0.4712 rad,
0x1F1FC40), a 0x40-byte component type 0x6053D84 (name unresolved; also required by the sound listener collector
0x1E93FA0), and per-camera config from the shot data (`+0x10`, `+0x3C`). 0x1F1CBA0 runs each frame over
`SceneCameraBehavior` entities whose CameraComponent type is 11 and, if 0x1F1C500 accepts (timeline data
`PlaybackState(+0x79) == 2 && CreationState(+0x7A) == 1`), updates it in 0x1F1D180.

### 2.3 The switch: `Timeline::ActivateCamera` 0x2007570 [r7], [r24]

`0x2007570(Guid* camera, i16 player /*0 = all players of the timeline*/, Timeline* tl, ?, bool reactivate)`:
1. Resolves the camera's FixedString (0x20074F0, or formats the GUID); if `tl+0x500 != *camera` copies it to
   `tl+0x500` and sets `tl+0x510 = 1`.
2. Player list = 0x1F8FFB0(tl): `tl+0x6C0` if set, else players of the actors in `tl+0x218` (count `+0x224`).
3. Per player: player node (player manager `DAT_1460487A0` map +0x130) → player's camera entity; looks the scene camera
   controller up by name (0x4087790 on `DAT_14604AB20`); if found: scene camera `EocCameraBehavior+0x38 = player`,
   player camera `GameCameraBehavior+0x188 = 0`, a component `+0xCC = 0` (0x7047F0), then
   **`if (tl+0x4C5 == 0 || camera is null-guid) 0x17F7E10(playerMgr, player, &sceneCamEntity, 0, 0, tl+0x48A||tl+0x48B)`**
   (disasm 0x2007AE9..0x2007B2C). Returns 1 if a camera was found.

Callers and what triggers them: 0x1D5B890 (player joins; uses `tl+0x4F0` if non-null else `tl+0x4E0`), 0x1D79A50
(per-player re-activation, same rule), 0x1D5BE40 (actor leaves: null guid), 0x1D55400 / 0x1D62890 / 0x1D63A90 /
0x1D64220 (TimelineSystem; 0x1D63A90 reads GlobalSwitches +0x131C `TimelinesQCameras` and `tl+0x498/0x499`),
0x1F21810, and the timeline shot components 0x20458E0, 0x2046450, 0x2046DA0, 0x2047470, 0x204E720 (vtable-called; one
per camera cut).

### 2.4 Timeline object fields (ecl::Timeline, SE layout where it fits the code)

| Off | Meaning | Evidence | Confidence |
|---|---|---|---|
| +0x218 / +0x224 | joined player actors, 16-byte entries {character handle, player id} | 0x1D5B890 appends; 0x1F8FFB0 reads | Confirmed |
| +0x488 | IsPreviewTimeline (SE) | many `== 0` gates before player-facing work | Name: SE |
| +0x489 | set 1 for TimelineType 0, 2, 3, 7, 9 (and 2/3/10 combat creation), 0 for 1 and 6 | 0x1D58800, 0x1D53CB0 | Confirmed; SE name IsGameplayTimeline |
| +0x48A / +0x48B | systemic / authored combat timeline (type 2 / 3) | 0x1D53CB0 | Confirmed |
| +0x48C | set for type 10 in the same creator | 0x1D53CB0 | Confirmed; SE name IsMainMenuTimeline (unconfirmed for type 10) |
| +0x490 / +0x494 | SkippingMode (u32) / byte (SE DeferredRequestOngoing) | 0x1D5EEB0 | SE |
| +0x498 / +0x499 | question-hold automation / force camera behaviour update (SE) | 0x1D63A90, many `= 1` | SE |
| +0x4C4 | timeline paused (time step 0) — SE `PausedForInGameControls` | 0x1D4D2E0 writes, 0x1F208D0 / 0x1D4D2E0 use dt = 0 | Confirmed behaviour |
| **+0x4C5** | **skip the view camera switch** — SE `InGameTimelineFreeCamEnabled` | only reader 0x2007AE9; no writer | Confirmed behaviour |
| +0x4C6 | `PlaybackState == 1` (SE IsHoldingAtZero) | 0x1D4D2E0 | Confirmed |
| +0x4C7 | set 1 (SE LSDialogStarted) | 0x1D431C0, 0x1D69A90 | SE |
| +0x4E0 / +0x4F0 | current camera actor / override (override wins when non-null) | 0x1D79A50, 0x1D5B890 | Confirmed |
| +0x500 / +0x510 | activated (lighting) camera Guid / its dirty flag | 0x2007570 | Confirmed |
| +0x6C0 | owning player id (type 11 timelines) | 0x1D58800 | Confirmed |

`eoc::TimelineDataComponent` (0xA8 per entity): +0x08 InstanceId (key of the Timeline map at TimelineSystem+0xB20),
**+0x78 TimelineType**, +0x79 PlaybackState, +0x7A CreationState (SE offsets; +0x78/+0x79/+0x7A confirmed by use in
0x1D4D2E0, 0x1F1C500, 0x1F208D0). `ecl::ClientTimelineControlComponent` +0x34 = TimelineType (0x1D53CB0 writes 2/3/10).
`eoc::TimelineActorDataComponent` (0x28) +0x20 = TimelineType (0x192F940).

### 2.5 What each TimelineType does (code-derived)

| Type | Takes the view camera on join (mask 0xD0E, 0x1D5B890) | Other behaviour |
|---|---|---|
| 0 | no | +0x489 = 1; grouped with 9 in 0x1D3FF10, 0x1D4F0E0, 0x1D5BE40, 0x1D62600 |
| **1** | **yes** | +0x489 = 0; **screen fade on join and leave when GlobalSwitches +0x1320 (`TimelinesFade`)** (0x1D5B890, 0x1D5BE40); `TimelinePlayerJoinedOneFrame` tag (0x1D71640); **letterbox** in split screen (0x192F940); gameplay camera re-targeted to the character on leave (0x1F579A0); TimelinePost lighting path (0x822 mask → 0x2001D50) |
| 2 / 3 | yes | systemic / authored combat (+0x48A / +0x48B), created by 0x1D53CB0; view+0x2962 = 1 |
| 5 | no | time step 0 unless PlaybackState 3/4 (0x1D4D2E0, 0x1F208D0); 0x822 mask |
| 6 | no | +0x489 = 0 |
| 7 | no | +0x489 = 1; time step 0 while PlaybackState 1 |
| 8 | yes | time-step rule as 5 |
| 9 | no | as 0 |
| 10 | yes | +0x48C, created by 0x1D53CB0 |
| 11 | yes | single owning player (+0x6C0); 0x822 mask |

Every camera-taking type also gets `IsCinematicMode` on the player view (0x1F90B00 → 0x3CCCC20(view, 1) on join, 0
on leave, types {1,2,3,10,11}).

**Ordinary NPC conversation vs cinematic cutscene: unconfirmed.** The code shows type 1 is the faded, letterboxed,
non-"gameplay" camera timeline, which fits both dialogue scenes and scripted cinematics; nothing found so far
separates the two inside the timeline code (both are dialog timelines in BG3's data model). To confirm, the mod should
log at 0x1D5B890 entry: `TimelineData+0x78` (type), `+0x60` (DialogInstanceId), `+0x08` (instance), `Timeline+0x4E0`
(camera actor) and the timeline resource (`TimelineData+0x18` InitialTimeline, SE), for (a) a normal NPC conversation,
(b) a cutscene with no choices (e.g. the nautiloid crash), (c) a combat cinematic, (d) a long-rest scene, and also
whether the dialog UI shows answer choices. Candidate discriminators to check in that log: TimelineType, presence of
answer nodes in the current dialog node, and `TimelineData+0x7B..0x8F` bytes.

### 2.6 What a dialogue does around the camera (and what keeping first person changes)

- **Camera**: view switched to the scene camera per cut (2.3); gameplay camera keeps existing; on leave the view gets
  the gameplay controller back (0x1D79890) and the gameplay camera is re-targeted (0x1F579A0). Suppressing the switch
  (+0x4C5) leaves the gameplay controller on the view the whole time: no Exit/Enter, our Transform hook keeps running.
- **Gameplay-camera framing for non-timeline dialogues**: DialogSystem 0x1FC2A70 moves the gameplay camera to frame
  the speakers for dialogue kind 5, with a distance from the speaker bounds and FOV, halved when
  GlobalSwitches +0x67D (`GameCameraEnableCloseUpDialog`) is set [r22].
- **Fades**: type-1 join/leave screen fades gated by GlobalSwitches +0x1320 (`TimelinesFade`); other fade systems in
  2.1. Unaffected by keeping the gameplay camera; the mod may want to clear +0x1320 to avoid black fades in the HMD.
- **Letterbox**: only in split screen: RenderViewportSystem 0x192ED80 scales the view height to 0.85 when the view's
  camera entity has `TimelineActorData.TimelineType == 1` (0x192F940, B 7.3). With the gameplay camera kept, the view's
  camera entity is the gameplay camera, which presumably has no TimelineActorData, so no letterbox (unconfirmed:
  log `TimelineActorData` on the `GameCamera_playerN` entity during a dialogue).
- **Render "cinematic mode"**: view+0x2961 set by 0x3CCCC20 on join (independent of the camera switch).
- **Lighting**: `tl+0x500/+0x510` are set even when the view switch is skipped (lighting camera follows the shot).
- **Player character**: timeline actors are positioned by the timeline (TimelinePostSystem 0x1F208D0 →
  0x1F95E80, which uses the query {`ls::TransformComponent`, `ecl::TimelineTransformComponent`,
  `ecl::ClientTimelineActorControlComponent`} minus `SceneCameraBehavior` (query 0x60CE1C0, users 0x1F95660,
  0x1F95E80, 0x1F9FAB0); flush tags `TimelineSwitchStage/LocationEvent` via 0x1F22770). So the player's body is moved
  and animated by the scene; the first-person camera should follow the head bone (C 1.5), not the old pose.
  Confirmed [r51]/[r52]: 0x1F95E80 iterates that query and calls **0x1F96210** per actor, which writes the actor's
  `ls::TransformComponent` through TransformSystem SetTransform 0x3C78110, SetRotate 0x3C77D20, 0x3C77FA0 (scale,
  unconfirmed) and SetTranslate 0x3C77BD0 (call sites 0x1F96210 lines 392 / 1005-1011). So timeline actors,
  including joined player characters (they carry `ecl::ClientTimelineActorControlComponent`), are teleported/posed by
  the timeline every TimelinePost update. When the original transform is restored: unconfirmed.
  The letterbox test 0x192F940 queries `eoc::TimelineActorDataComponent` (0x192FA60 annotation [r52]).
- **Look-at / visibility**: components `ecl::TimelineAutomatedLookatComponent`, `TimelineEyeLookAtOverrideComponent`,
  `TimelineActorVisibilitySwapSingletonComponent`, system `ecl::lookat::TargetSystem` (reg 0x727300, body 0x19336E0)
  exist; not decompiled (lead).
- **Depth of field**: timeline cameras carry their own post-process (`UseCameraPPSettings = 1`, TLCameraDoF channels
  in the resources); gameplay Exit 0x1E0BCD0 resets per-view focus data. With the gameplay camera kept, the timeline
  DOF never applies to the view (unconfirmed: depends on whether DOF is read from the view's controller only).
  The DOF graphics option (`DOF` in graphicSettings.lsx, real-vr.md) turns it off globally.

---------------------------------------------------------------------------------------------------

## 3. Other users of the camera

### 3.1 Audio listener (Confirmed)

ecl::sound::CameraSystem (reg 0x73FFF0, cb 0x75E940, body **0x1E93410**) [r34]: walks every RenderFrame's views
(`frame+0x42E0`, count `+0x42EC`) through **0x1E93FA0** [r35], which skips views whose ReadState is disabled
(`view+0x2158`) or whose frame flag `+0x42F0` is 0, takes **`view+0x21D8` (ReadState controller) → `+0x318` camera
entity**, and maps player id (0x3BAACE0/0x3BAADF0) → that camera entity if it has component 0x6053D84 and the view
byte `+0x2960` is set (`param_2[0x52C]`). It then adds
`ls::SoundListenerComponent` to those camera entities (queries 0x60C8F64 / 0x60C8F70) and removes it from others.
So the Wwise listener sits on the camera entity of whatever controller the view is drawing. Split-screen listener
handling: `ecl::sound::ListenerSpatializationSystem` (reg 0x737BE0, body 0x1E04020) uses
`ecl::splitscreen::SoundCameraSingletonComponent` and GlobalSwitches `AudioListener_SplitScreen_CameraDistance`.

### 3.2 Obstruction fading

- `ecl::obstruction::RaycastSystem`: reg 0x723350 (object 0xA8, vtable 0x5601FE8; fields +0x10 player manager, +0x18,
  +0x20 from the r9/r13 args of 0x70EC90), cb 0x7566C0, body **0x18CF450** [r43]: computes the enable byte
  (results+0x80, see hook table), then per split-screen player (A/B) 0x18CFAF0 [r45], which needs the player's view
  (0x3BAAE20) and character (0x17F7230) and fills `RaycastResultsSingletonComponent` per view (View0 / View1).
- `ecl::obstruction::FadeSystem`: reg 0x7236B0, cb 0x756810, body **0x18D4AA0** [r40]: returns without work unless
  results+0x80; consumes the per-view result lists (+0x60, two views).
- `ecl::FadeResolveSystem`: reg 0x723CC0, cb 0x756A00, body 0x18DC350 (combines all `*FadeView0/1` request types:
  Obstruction, RoomTrigger, Crowd, CharacterOutOfSight, Timeline, ItemPreview, … `types_camera.txt`).
- Ray origin (camera position vs. character): unconfirmed (decompile 0x18D3750 / 0x1418529F0 inside 0x18CFAF0).

### 3.3 Culling and occlusion

- `ls::CullSystem`: reg 0x3AE5E20, cb 0x3AE8880, body 0x3EB9370 — light cull flags (`ls::CullComponent` +
  `ls::LightComponent`, Set(Character)LightCullFlags requests) [r40]; no camera read in the body.
- `ls::OcclusionSystem`: reg 0x3AE58D0, cb 0x3AE87E0, body 0x3D656B0 — iterates `ls::CameraComponent` (query
  0x613DED4) and calls the controller's vtable +0x48 (GetWorldTranslate) [r40]: it follows the camera entity
  Transform, i.e. our hook pose.
- Per-view camera cull entries come from controller Enter (A 1.3).

### 3.4 Render-view flags set by timelines

`view+0x2961` (IsCinematicMode, SE) readers: 0x17F2910, 0x17F2B80 (view vtable slots 7 / begin-record focus CB),
0x17FCF10, 0x180FA77, 0x1816520, 0x20458E0, 0x3CC8760, 0x3CC9DB0, 0x3CCB020, 0x3CCCCC0, 0x3E1F520 [r39].
`view+0x2962` (IsInCombatCameraMode, SE) reader: 0x3EE6070. Effects not decompiled (lead).

### 3.5 Tactical camera, camera modes, collision, shake

- `GameCameraBehavior+0xA8` (SE `CameraMode`) bits: 0x04 tactical (0x1E1A860 sets/clears; on set it loads the
  tactical default distance into +0x5C from GlobalSwitches camera blocks +0x79C / +0x930 / +0xC58 (+0xC4) and clears
  GlobalSwitches +0x792; 0x1E090F0 tests it; session unload clears it 0x1E20855), 0x01 and 0x02 tested throughout the
  game-camera update 0x1E0DDB0 and helpers 0x1E155E0..0x1E1C0F0 (meaning unconfirmed; 0x1E0C570 clears both) [r48].
- Event 0xD9 (ToggleTacticalCamera) in 0x1E0EE20 is ignored when GameCameraBehavior+0x13C > 1, when a flag in the
  event context is set, or when `DAT_14614FBC8` is set and the camera type is not 8 (C's r44 lines 378-411).
- GlobalSwitches (load 0x5D0360): +0x127 `CameraShakeEnabled`, +0x67D `GameCameraEnableCloseUpDialog`,
  +0x67E `GameCameraEnableDynamicCombatCamera`, +0x682 `GameCameraEnableFlyFollow`, +0x131B `TimelinesShowDisabled`,
  +0x131C `TimelinesQCameras`, +0x1320 `TimelinesFade` (disasm of 0x5D0360 around each FixedString load).
  Readers of +0x127 in the camera code: none found in 0x1E0xxxx-0x1E2xxxx [r48] (shake reader unconfirmed).
- Camera collision / arm avoidance: in the game-camera update (GameCameraBehavior `ArmCollisionDistance*`, SE); not
  decompiled further because the Transform override bypasses it.

### 3.6 Photo mode, free camera

- Photo mode session processing 0x1E0BEC0 (CameraSystem body) creates its own camera entity through 0x3B96300
  (registered in the camera manager), adds components (0x3B7CFB0, 0x3B7D020, 0x6D0930), copies the player's camera
  settings (FOV from GlobalSwitches +0x7A4) and clears the player camera's GameCameraBehavior+0x188 [r36].
  Enter requests: 0x1E10BE0 (`EnterPhotoModeRequestSingletonComponent`). How its controller reaches the view is
  unconfirmed (no direct call to 0x17F7E10 / 0x3CC90B0 from these functions).
- Photo-mode systems: `ecl::camera::PhotoModeInputSystem` 0x7398E0 → 0x1E2E470, `ecl::photo_mode::*` 0x750F10..0x751D60
  (bodies 0x1AEFA30 / 0x1AF0C80 / 0x1AF2050 / 0x1AF2B90), `ls::photo_mode::PhotoModeSystem` 0x7533B0 → 0x3C49730.
- Free camera: input events 0x6F-0x86 (C 3.3) and `ecl::FreeCameraBehavior`; not traced.

---------------------------------------------------------------------------------------------------

## 4. Camera input (beyond C 3.4)

- The gameplay controller's OnInputEvent is vtable slot **15 (+0x78) 0x3CF0430** → handler +0x38 **0x1E0B740**
  (see 1.1 correction). The other two-float slot 17 (0x3CF0780) is a pan (0x3B96610), not input.
- For VR the camera pose comes from the Transform hook; camera input only matters for (a) the tactical toggle 0xD9
  (3.5) and (b) camera-relative movement (C section 4).

---------------------------------------------------------------------------------------------------

## Open leads

1. **TimelineType of real scenes** (2.5): the runtime log at 0x1D5B890 described there decides whether "NPC
   conversation" and "cutscene" can be told apart by type; if both are type 1, look for answer nodes in the dialog
   (DialogSystem 0x1FC2330 / dialog manager) as the discriminator.
2. Timeline ctor: confirm `+0x4C5` starts at 0 (log it at 0x1D5B890 on a fresh timeline).
3. Effects of `view+0x2961` readers (3.4), especially 0x17F2B80 (focus CB) and 0x3CC9DB0 / 0x3CCB020.
4. Player-character transforms during timelines: the write is confirmed (0x1F96210); still open: the restore on
   leave (0x1F95660 / 0x1F9FAB0, server-side `esv` timeline end) and whether the player character is hidden.
5. Look-at during dialogues: `ecl::lookat::TargetSystem` 0x19336E0, TimelineAutomatedLookat / EyeLookAtOverride.
6. Obstruction ray origin (3.2) and the object at `RaycastSystem+0x20` (+0x1CE enable byte).
7. Photo mode's view attach path (3.6); component type 0x6053D84 name (resolve its GetTypeName initialiser).
8. MasterBehaviorType 6 meaning (0x1E0CBB0); character creation camera 0x1C9ACE0.
9. Camera shake: readers of GlobalSwitches +0x127 and the controller/handler `SetLookAtShake` path.
10. `GameCameraBehavior+0xA8` bits 0x01 / 0x02 meaning (controller vs mouse mode, unconfirmed).
