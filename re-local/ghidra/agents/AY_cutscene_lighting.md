# Cutscene / dialogue lighting with the VR camera (agent AY, 2026-10-26)

Static analysis of bg3_dx11.exe 4.1.1.7631656. Ghidra project BG3_G (read-only, -noanalysis, request server
`agents\AY\AYServer.java`, stopped at the end), bg3vr_disasm, the game's own data (Shared.pak / Gustav.pak, read with
AX's `pak.py` / `pakget.py`, sampler `agents\AY\ay_scan_scenes.py`). RVAs relative to 0x140000000.
"SE" = Script Extender source (`downloads\bg3se-src`). **Confirmed** = shown by the cited code or data.
**Unconfirmed** = not shown; each one has a log item in section 5. Raw output: `re-local\ghidra\agents\AY\` (y1..y30,
s1/s2, `data\`). Builds on AV_cutscenes.md, AO_lighting_system.md, AL_eye_lighting.md, AN_shadow_system.md,
T_shared_work.md, AT_perf_sharing.md.

**Naming note for the mod authors.** AO/AL call view+0x2880 "atmosphere" and view+0x28B8 "lighting". By SE they are
the other way round: Timeline+0x4A0 (SE `AutomaticLightingIdFromAtmosphere`) is compared with `[view+0x28B8]+0x2C8`
(SE `Atmosphere.TimelineAutomaticLightingDefaultSetup`, 0x2001D50), and the 0x378-byte objects of bank
`[0x6049010]` (set into view+0x2880 by 0x1F80030 -> 0x3CC9700) hold the sun (`RotationAsVec3` at +0x3C, SE
`Lighting.Sun`). So **view+0x28B8 = Atmosphere resource** (post-process, timeline automatic-lighting setup +0x2C8,
DisableFlip +0x2D8, character-light setup id +0x2E0), **view+0x2880 = Lighting resource** (sun, SSAO, fog). The
offsets in AO/AL/views.cpp are right; only the words are swapped. This report uses the SE words.

---------------------------------------------------------------------------------------------------

## 1. Fix for the VR mod

Root cause (Confirmed, section 2.2): a scene's own lights ("TL lights", authored per stage) are **linked to cameras**
(`TLCameras ... LinkedLights` in the scene data). On every shot, 0x2007570 writes the shot camera into
`Timeline+0x500` (ActivatedLightingCamera) and sets `+0x510` **before** it tests `+0x4C5`, so this still happens with
the mod's camera override. Next, 0x2002690 switches **on** only the lights linked to that camera for the current
stage and switches **off** every other TL light; and 0x2001D50 removes the scene's automatic Key/Fill/Rim rigs from
all actors whenever that camera has linked lights, or rebuilds them when it has none. With the cinematic camera the
cuts hide this; in VR every cut is a visible relight of every actor. The engine does this per camera, and the user's
"lights only for whoever is on camera" guess matches it.

| # | Mechanism | RVA | Exact hook / patch / data write | What it changes | Thread | Risk | Confidence |
|---|---|---|---|---|---|---|---|
| **F1 (recommended)** | Camera-linked TL lights and auto-lighting mode follow `Timeline+0x500` | 0x2007570 (store at 0x2007792), 0x2002690, 0x2001D50 | In the mod's 0x2007570 detour (AV F6; prologue `48 89 5C 24 10 55 56 57 41 54 41 55`; rcx = camera GUID*, dx = player, r8 = Timeline*): **after** calling the original, if the mod set `[r8+0x4C5]` for this timeline and `[dialogue] scene_lighting = uniform`: save `*(GUID*)rcx` as "game lighting camera" for that timeline; if the 16 bytes at `r8+0x500` are not zero, write zero to `r8+0x500..+0x50F`, then `byte [r8+0x510] = 1`, `byte [r8+0x513] = 1`. | Null lighting camera = the state the game itself sets when a scene ends (0x2007570 with a null GUID, AV). 0x2002690: every camera-linked TL light off (Type-4 rigs stay on), the same every shot. 0x2001D50 (TimelinePost): no linked lights, so the scene's automatic Key/Fill/Rim setup goes on **every character actor**, oriented from that actor's own transform (with a sun-side flip), not from a camera. The look stays the same through every shot. | ECS timeline update: the same call that wrote +0x500, so no frame sees the shot camera | (a) Authored per-camera mood lights (e.g. coloured rims in cinematics) are gone; actors get the scene's default rig instead. (b) Types 2/3/8/10 never get automatic rigs (TimelinePost runs 0x2001D50 only for types 1, 5, 11: mask 0x822, 0x1F20B8x); in those, actors keep only world lights + CharacterLight rigs. (c) Automatic rigs also need `[GlobalSwitches 0x604B1B8]+0x131A` != 0 (only reader 0x1F20BA0; name unknown), log item 3. | Code Confirmed; that every scene has a usable setup: data Confirmed for 500/500 sampled scene files (section 3.3); visual result Unconfirmed (log 1-3) |
| F1b (with F1) | Automatic rigs are placed only when `+0x513` is set (shot start, stage switch, camera override); they don't follow walking actors | 0x2001D50 -> 0x2002EE0 / 0x2003480 | Each game frame while F1 is active for a timeline: `byte [T+0x513] = 1` before TimelinePost (any per-frame point on the game thread; TimelinePost clears it at 0x1F20BAF). | Rigs follow actors every frame instead of jumping at (now invisible) cuts. | game thread, before TimelinePost 0x1F208D0 | Small CPU (about 3 transform writes per actor per frame). The flip (0x2002EE0, `[[view+0x28B8]+0x2D8] == 0`) can now change mid-shot when an actor turns across the sun direction | Code Confirmed |
| F2 (alternative to F1) | Same switch, other direction: **every** TL light on | 0x2002C26 | Byte patch `74 41` -> `EB 41` at 0x2002C26 (the `cmp byte [r13+0x50],4 ; jz` in 0x2002690) while a VR-camera timeline is active; restore `74` afterwards. After each apply/restore: `byte [T+0x510] = 1`. | Every TL light of the scene takes the "linked" path: it is on or off by its own per-stage value (`[rig+0x48]+0x540` map keyed by stage `T+0x478`, 0x2002C69..0x2002C9B) and placed by its per-stage transform (0x2002D00), whichever camera is active. The automatic rigs stay off in scenes that have linked lights (same as vanilla). | ECS timeline update | Several key/rim lights from different directions at once (brighter, flatter); more shadowed local lights competing for the per-view shadow atlas (AN 3.4), whose budget is camera-dependent (section 2.4). Global patch: all timelines while applied. | Code Confirmed; look Unconfirmed |
| F3 (not recommended) | Keep the first shot's lighting camera | 0x2007570 | In the post-call: if `T+0x500` was non-zero before the call, write the old value back (keep `+0x510`). | One camera's linked lights for the whole scene. | ECS timeline update | Actors not covered by that camera stay without TL lights; later stages may have no links for it, so the lighting mode still changes per stage | Code Confirmed |
| F4 (only if log 4 shows it) | Timeline atmosphere/lighting keys (`TLAtmosphereAndLighting`) change the whole environment over time | 0x1F7FA10 -> 0x1F7FC10 / 0x1F80030 | Log first. To freeze: detour 0x1F7FC10 and 0x1F80030 (prologue `48 8B C4 48 89 58 20 55 56`), call through for the first call per timeline and return without calling afterwards. | Sun, fog, post, the character-light setup id and the automatic setup id stay as the first key set them. | ECS timeline update | Loses intended mood changes (fire, night). These keys are timed, not per camera, so they are a less likely cause | Code Confirmed |
| none | CharacterLight rigs (per character Key/Fill/Rim, `Timeline/CharacterLight/`) | 0x19E0F90 | No change. They already orient to the view's own camera, i.e. the VR head **position**, and do not change when the head only turns. | - | - | - | Code + data Confirmed |

Restore / scene end:
* F1: nothing to undo when the scene ends (the leave path calls 0x2007570 with a null GUID, AV 3.2 step 5; the
  Timeline is then destroyed). If VR camera use is switched off **during** a scene: write the saved "game lighting
  camera" back to `T+0x500`, set `T+0x510 = T+0x511 = T+0x513 = 1`; the next 0x2002690 / 0x2001D50 run restores
  vanilla lighting.
* F1b: stop writing +0x513.
* F2: restore byte `74` at 0x2002C26 and set `+0x510 = 1` on every live timeline.
* Apply F1 to the same timelines for which the mod sets `+0x4C5` (AV F6: camera-taking types 1, 2, 3, 8, 10, 11).

Can the dialogue's own lighting-camera logic be pointed at the player camera? Yes, but it gives the **same result**
as F1, at more cost. 0x2001880 resolves a lighting-camera GUID that is not a scene actor through the camera
manager (`[0x604AB20]`, FixedString lookup, controller +0x318 camera entity, component 0x1BBF8F0 -> TL camera id +0x48).
The gameplay camera has no TL camera data, so it gets an empty light list: all linked lights off, automatic rigs on.
No TL light in the data is placed relative to the active camera. Linked lights use authored per-stage transforms
(0x2002D00). Automatic rigs follow the actor and the sun (0x2002EE0). So no camera-relative relighting exists to aim
at the VR camera. The one camera-relative system, CharacterLight, already uses the view's camera, which the mod
drives.

---------------------------------------------------------------------------------------------------

## 2. Evidence

### 2.1 Timeline fields used (SE order matches every offset checked)

| Off | SE name | Evidence |
|---|---|---|
| +0x178 / +0x180 | Content / ContentOverride | `content = [T+0x180] ?: [T+0x178]` in 0x2002690, 0x20024A0, 0x2001D50, 0x3CBB560 |
| content+0x250 | the timeline's TLScene (lights, setups, cameras) | +0x88 light-rig map, +0x38/+0x44 LightingSetups list (0x2003B10), camera objects via 0x3C0CB70 (0x2001880) |
| +0x2B8 / +0x2C4 / +0x2C8 | actor GUIDs / count / entities | loops in 0x2002690, 0x2001D50, 0x2003A70, 0x2004580 |
| +0x3A8 (+0x3AC, +0x3B0) | per-actor automatic-light list map (SE ActorLighting) | 0x2001D50 lookup / insert 0x3CBE030 |
| +0x458 / +0x468 | Atmosphere / Lighting override GUID | written by 0x1F7FC10 / 0x1F80030 (y16) |
| +0x478 | SceneStageId (current stage) | written by TLSwitchStageEvent slot 0x204DF30; key of LinkedLights / per-stage maps |
| +0x488 / +0x490 | IsPreviewTimeline / SkippingMode | 0x2002690 top; 0x1F7FC10 (blend 0 while skipping) |
| +0x4A0 | AutomaticLightingIdFromAtmosphere | 0x2001D50 compares with `[view+0x28B8]+0x2C8`, reset 0x2003A70 on change |
| +0x4D0 / +0x4E0 / +0x4F0 | PreviousCamera / CurrentCameraActor / Override | 0x204E720, 0x204E7C0 (AV) |
| **+0x500** | **ActivatedLightingCamera** | written only by 0x2007570 (0x2007792) in the timeline code range (scan y9: all `[reg+0x500]` writes in 0x1D00000..0x2100000); read by 0x2002690 (0x2002B7F) and 0x2001D50 (0x2001DDA) |
| +0x510 | ShouldUpdateActiveCustomLighting | set 0x2007570 (0x2007764), 0x204DF30, 0x1D46F50 (word 0x101 with +0x511); consumed+cleared 0x2002690 (0x2002B14/0x2002B22) |
| +0x511 | ShouldUpdateCustomLighting | consumed 0x20024A0 (called from 0x1D4D2E0, 0x1D63260, 0x1D685E0) |
| +0x513 | ShouldUpdateAutomatedLighting | set by TLShot slot 0x2045C10, TLShotHoldPrevious slots 0x204E720 / 0x204E7C0, TLSwitchStageEvent 0x204DF30; consumed + cleared in TimelinePost 0x1F208D0 (0x1F20B98 / 0x1F20BAF) |
| +0x5C0..+0x5F0 | player -> render view map | 0x1F8FCC0 (view for automatic lighting), 0x1F7FC10/0x1F80030 (views that get the override) |

Component classes: identified by the type-name getter in the same vtable (DumpQwords y27; getters 0x3D9BB90 ->
FixedString `TLAtmosphereAndLighting`, 0x3DA9B90 -> `TLShot`, 0x3DAB650 -> `TLSwitchStageEvent`, 0x3DAB9B0 ->
`TLShotHoldPrevious`; names from the global-string init 0x3D7F6C0). Confirmed.

### 2.2 Camera-linked scene lights ("custom lighting") - Confirmed

Data (`data\SCL_Drider_CINE_CaravanIntro_Scene.lsx`, `_Add_EXT_NOON_Px1_Goblin.lsx`): the TLScene has `Lights`
(each: Id, Name like `Init_Key1_Stage_0`, `Addn_Rim1_Stage_1_Far`, TemplateId, `Type` byte, per-stage `Transforms`
keyed by stage GUID). Each `TLCameras` entry has `LinkedLights`: a map stage GUID (null = default) -> list of light
Ids. Inherited environment setups (`Public/Shared/Timeline/Scenes/Default/LightingSetup/<ENV>/Additional/*`) carry
camera-linked lights too (`Add1_Key1` linked to `cam_player_CU` etc.).

Code:
1. **0x2007570** (y8, disasm): `if ([T+0x500] != *rcx) { [T+0x510] = 1; if (*rcx != null || [T+0x224] < 2)
   [T+0x500] = *rcx }` (0x200774B..0x2007792). The +0x4C5 test that skips the view switch comes later
   (decompile line 266, AV disasm 0x2007AE9). Shot callers per AV: 0x20458E0, 0x2046450, 0x2046DA0, 0x2047470,
   0x204E720 (TLShotHoldPrevious).
2. **0x2001880**(out, T, &GUID) (y13): the camera's actor slot (0x3CBB560 -> content descriptor), `+0x48` = its TL
   camera id -> camera object (0x3C0CB70) -> map +0x1A8 keyed by the stage `T+0x478` (or an index variant for
   question-hold cameras) -> the list of linked light Ids. Non-actor GUIDs go through the camera manager. Unknown or
   null GUID -> empty list (0x2001D21).
3. **0x2002690**(T) (y10, disasm 0x2002B14..0x2002CB2), called every frame per timeline from **0x1D637E0**
   (TimelineSystem, called from 0x1D4D2E0 / 0x1D632D0): if `+0x510`: clear it; list = 0x2001880(T+0x500); for each
   actor whose descriptor has a light-rig id `+0x38` present in content+0x88: rig = map value; if `rig+0x50 == 4` or
   the id is in the list -> 0x3D3BA70(enable = per-stage byte from `[rig+0x48]+0x540` keyed by T+0x478), then
   0x2002D00 (place the light entity from the rig's per-stage transform `rig+0x58`) -> else 0x3D3BA70(…, 0).
   0x3D3BAE0 toggles bit 5 of light `+0xD1` (SE LightComponent.Flags) and adds/removes the light's render
   component. So **lights not linked to the active lighting camera are switched off**.
4. Types in data: in the 500-scene sample, light `Type` values 0, 1, 2, 3, 4 occur (counts 13/16/31/52/2); type 4 is
   the "always" kind in step 3. 51 of 320 regular dialogue scene files have `LinkedLights` themselves; ambient
   dialogues (`_AD_`) 0 of 180; inherited setup scenes add more (data Confirmed for the files read; the exact share of
   scenes that end up with links after inheritance is Unconfirmed, log 1).

### 2.3 Automatic Key/Fill/Rim per actor ("automated lighting") - Confirmed

1. **TimelinePost 0x1F208D0** (y12; callback 0x7619D0): after writing actor transforms (0x1F95E80), for each
   timeline of type 1, 5 or 11 (`0x822 >> type`): `0x2001D50(T, [GlobalSwitches+0x131A], [T+0x513])`, then
   `[T+0x513] = 0`.
2. **0x2001D50** (y10): if the switch is 0, or 0x1F8FCC0(T) (the timeline's player view from `T+0x5C0..+0x5F0`, else
   the primary view `[0x6046F98]+0xA0`) has no atmosphere `[view+0x28B8]` -> 0x2004580 (remove all automatic lights:
   0x3CBCF20 per character actor). If any light linked to the lighting camera (0x2001880(T+0x500)) is in content+0x88
   -> 0x2004580 too (custom lighting wins). Else: atmosphere `+0x2C8` GUID vs `T+0x4A0` (change -> 0x2003A70 resets);
   for each actor that is a character (0x20046A0: in set T+0x318 or descriptor type FixedString `"character"`): if
   it has lights in map T+0x3A8 -> 0x2002EE0 **only when +0x513**; else 0x2003B10 creates them, then 0x2002EE0.
3. **0x2003B10** (y13): setup = per-actor override by stage (0x2003E80 -> +0x50 map keyed by T+0x478); if none and
   atmosphere+0x2C8 set -> that setup; else the setup with that (null) id. The setup comes from the scene's
   `LightingSetups` list (content+0x250 +0x38/+0x44, `setup+8` = Id). One light entity per setup light (0x3C75390),
   LightChannelFlag |= 0x1E (`+0xD2`), the actor visual's light channel set (0x3C672E0).
4. **0x2002EE0 / 0x2003480** (y13/y14): rig rotation = the actor's transform rotation (yaw optionally replaced by
   component 0x1BBD7A0 +0xC when +0x14). The flip test runs when the atmosphere's DisableFlip `[atm+0x2D8] == 0`. It
   compares the actor's facing with the **sun** of the Lighting resource at the scene position. That resource is
   the containing-trigger lookup 0x42376D0 or the level default `[[[0x6007A28]+0x90]+0x18]+8`, bank `[0x6049010]`,
   and it reads `RotationAsVec3` at +0x3C..+0x47 (SE Lighting.Sun layout). Each light = attach bone (TLAutomatedLight
   +0x30, via skeleton) or rig origin + rotate(LightDesc.Position +4..+0xC, x negated when flipped), aimed at the
   actor, written through 0x3C785C0. **No camera input.**
5. Data: every one of the 500 sampled `_Scene.lsx` files has `LightingSetups`. Default scenes carry setup Id
   `00000000-…` "Default" = Key (spot, shadow, 10), Fill (2.5), Rim (spot, shadow, 15)
   (`data\_Base_EXT_NOON_Px1_ratfinal.lsx`).

### 2.4 View-dependent effects on lighting (question 2)

| Effect | Code | Depends on | Visible in VR as | Confidence |
|---|---|---|---|---|
| CharacterLight rig yaw | 0x19E25F0: yaw = direction from camera position (controller vtable +0x68 result, float3 at +0x10) to the character; pitch/roll from the character | camera **position** of the view in the player-view map (main) | key light stays on the viewer side as the head moves around a character; no change from head rotation alone | Code Confirmed; that vtable +0x68 is the camera world pose: Unconfirmed (log 6) |
| CharacterLight update gating | 0x19E48A0: skip unless entity % N == frame % N, N = 1/2/4 from `[[0x604AA10]+0x3E]` (0/1/2); 0x19E1830: update only if the character's bounds (visual+0x40) pass 0x4126300 against the view camera's planes `controller+0x210` (10 planes, 6 if ortho `+0x12`) and its per-view fade (FadingView0/1) > 0 | view frustum | a character that moved while out of view keeps a stale rig until it is seen again, then snaps (within 1-4 frames) | Code Confirmed |
| CharacterLight per-view mask | 0x19E0CE0: view mask 0x200 (`[view+0x2268]==0`) / 0x400 (==1); 0x19E2FB0 pushes cull-flag changes `{light+0x80, flags, enabled}` when `(light+0x108 & mask) != enabled` | view split index | none for the mod: the eye is not in the player-view map 0x19E0AE0 walks (B 0x3BAB1C0), and it renders with index 0, so it sees main's rigs | Code Confirmed; eye not iterated: Unconfirmed (log 6) |
| CharacterLight setup | 0x19E0CE0: target = `[view+0x28B8]+0x2E0` (SE `CharacterLightViewData.TargetAtmosphere`); on change, crossfade over the view's blend time `+0x28E4`; setups from `Timeline/CharacterLight/` (0x19E06C0): Shared "Default" (null id), Gustav PL_EXT_NOON, PL_DGN_DARK, … **PL_NOLIGHTS** (15 setups, `data\CharacterLight_Gustav.lsx`) | atmosphere changes, i.e. timeline start/end override (0x3CBCB80 / 0x3CBCA10) and TLAtmosphereAndLighting keys | rig swap with crossfade at those moments, not per shot | Code + data Confirmed |
| Local-light shadow budget | stage 0x35 (AN 3.4, T 3.1; y17): priority 0x3DFC0D0 = angular size of the light seen from the camera position x screen config; distance cut in 0x3DFBF80; smoothed fade +0x21A8 | camera position and FOV | shadows of shadowed local lights (CharacterLight Key/Rim, TL lights) can fade when budget is exceeded; changes with position more than with rotation | Code Confirmed (AN/T); budget pressure in dialogues Unconfirmed (log 7) |
| Ordinary light culling | CullStage buckets (frustum) | frustum | none (culled lights light nothing on screen) | Confirmed (AT/AN) |

No camera-distance fade of light intensity was found in the code read here. SE `ls::LightSystem` has fade request
maps (FadeIntensity etc.) driven by requests, not by the camera. Unconfirmed; not searched further.

### 2.5 Per-view environment and timelines (question 3) - Confirmed

* Start / end: 0x1D59830 -> 0x3CBCB80 (lock + force the timeline's Atmosphere/Lighting) / 0x3CBCA10 (unlock +
  restore) on the views of the timeline map (AO #2).
* During the scene: `TLAtmosphereAndLighting` slot **0x1F7FA10** (vtable entry 0x56A3BF8; y14) evaluates its two
  channels (`+0x68`, `+0x70`, vtable +0xD0); when a channel key changed (`+0xAC`): channel 0 -> **0x1F7FC10**
  (`T+0x458` = atmosphere GUID; for every view in `T+0x5F0`: clear lock +0x2AEB, 0x3CCAAC0(view, res, &req, blend,
  force 1), lock +0x2AEB = 1; blend = key's +0xA8, 0 while SkippingMode), channel 1 -> **0x1F80030** (`T+0x468`,
  0x3CC9700, lock +0x2AEC). Keys are on the timeline's time axis, not tied to cameras.
* Shots do not touch the view environment. Through the atmosphere they do change: the CharacterLight setup (+0x2E0,
  2.4) and the automatic-lighting setup id (+0x2C8 -> 0x2003A70 rebuild).
* The eye: the timeline map is filled only from GetPlayerView (AO #2), so the mod's `copy_environment` (with AO's
  lock bypass) must carry these to the right eye, as now.

---------------------------------------------------------------------------------------------------

## 3. System map

### 3.1 Three light systems on actors during a timeline

| System | Who gets lights | Placement | Switches | Runs in |
|---|---|---|---|---|
| CharacterLight (ecl, per view) | every on-stage character and dummy (queries 561E1A0 / 561E4D0 / 561EB60 / 561EFE0 / 561F290; excludes Invisibility / OffStage) | Key/Fill/Rim from the setup chosen by the view's atmosphere +0x2E0; camera-relative yaw | atmosphere change (crossfade), frustum/throttle updates | 0x19E0F90 (callback 0x75DB80): 0x19E0AE0 views -> 0x19E38C0/0x19E3D70 (new chars) / 0x19E4220 (new dummies) / 0x19E48A0 (chars) / 0x19E5110 (dummies) -> 0x19E1C40 (rebuild per view) / 0x19E25F0 -> 0x19E2FB0 (place, enable) |
| Timeline automatic lights | timeline actors of type "character", timeline types 1/5/11 | scene LightingSetup (Default or per-actor/per-atmosphere), actor-relative, sun-side flip | rebuilt/re-placed on +0x513 (shot, stage, override); removed while the lighting camera has linked lights | TimelinePost 0x1F208D0 -> 0x2001D50 |
| Timeline custom (camera-linked) lights | lights authored in the scene / inherited setup scenes | per-stage authored transforms | **on only while linked to Timeline+0x500** (or type 4) | 0x1D637E0 -> 0x2002690; per-stage moves 0x20024A0 |

### 3.2 CharacterLight internals (Confirmed, y3-y7)

* Singleton (query 0x60ADF6C, SE `CharacterLightSingletonComponent`): two 0x30-byte view slots {SwapTime, ratio,
  Time, -, Target id, Previous id}, Scene at +0x60 (loaded from `Timeline/CharacterLight/` by 0x19E06C0 on the
  TLSceneReload one-frame tag, query 0x60ADFA4).
* 0x19E0AE0 walks the player-view map (`[sys+0x40]+0x30`), skipping views without ReadState +0x2158 / frame +0x42F0
  and without controller `+0x21D8` or atmosphere `+0x28B8`; builds one 0x60 record per view: {changed, fade times,
  ids, mask 0x200/0x400, controller, split index, setup target, setup previous}.
* Per character: component `Lights` = array per view of map setup-id -> [{light entity, TLAutomatedLight*}]
  (SE `CharacterLightComponent`). 0x19E21D0 creates the lights (0x3C75390) and sets intensities with the crossfade
  (0x3D3BDA0). 0x19E5970 drops setups that are neither target nor previous.
* 0x19E2FB0: LightChannelFlag `+0xD2` = 0x20 (0x80 for darkvision lights); enabled = component.Enabled && not
  hidden by InvisibilityVisual for that view && fade(view) > 0 && (not darkvision-only or darkvision on). The cull
  flags are changed by request.

### 3.3 Game data read (agents\AY\data)

* `CharacterLight_Shared.lsx` (Shared.pak): one setup "Default" (null id): Key (-0.8,0.3,-0.5) shadow, Fill
  (0.6,-0.55,-0.75), Rim (0.7,0.8,1.0) shadow; LightChannelFlags 2.
* `CharacterLight_Gustav.lsx` (Gustav.pak): 15 setups (Default, PL_TESTLEVEL, PL_DGN_NTRL, PL_EXT_NOON, …,
  PL_NOLIGHTS, PL_EXT_CITY, PL_DGN_SHAR, PL_EXT_ASTRAL, PL_DGN_BHAAL), 38 lights.
* Sample of 500 of 9789 `Timeline/Generated/*_Scene.lsx` (seed 7): LightingSetups 500/500; inherits 481/500;
  LinkedLights 51/320 dialogue, 0/180 ambient-dialogue; scene-own Lights 15/320. (No `_CINE_` file fell in the
  sample; the cinematic read by hand, SCL_Drider_CINE_CaravanIntro, has 48 lights and 9 LinkedLights blocks.)

---------------------------------------------------------------------------------------------------

## 4. What else was checked

* 0x2002690 runs only for timelines with players (0x1F90550) or preview. 0x2001D50 only for types 1/5/11.
* 0x20024A0 (`+0x511`) re-places linked lights per stage; it does not switch them on or off. Under F1 it only moves
  lights that 0x2002690 has switched off.
* The 0x2007570 null-GUID store rule (`[T+0x224] < 2`) only matters for the game's own write; F1 writes directly.
* Other `+0x500` writers found by the displacement scan outside the timeline range are stack frames or other classes
  (0x20CF270, 0x20E4FC0: `puVar[0x1C0]` objects; 0x20C57F0: critical section; 0x1D4063A/0x1D44312: TimelineSystem
  SRW lock at +0x500). Confirmed for those listed.
* GlobalSwitches neighbour names: `TimelinesAD`, `TimelinesFade`, `TimelinesShowDisabled` (+0x131B per G),
  `TimelinesQCameras`. The name of +0x131A is not resolved (only reader 0x1F20BA0).

---------------------------------------------------------------------------------------------------

## 5. One-run logging plan

One run, about 6 minutes: a normal NPC dialogue with several cuts, an ambient one-liner, a companion dialogue at
camp (night), one cinematic (`_CINE_`) if reachable. The user presses a **mark** hotkey whenever they see an actor's
lighting change (log line `MARK`). Every line has the frame `[0x614FB88]`, sequence number and thread id. All reads
are under `__try`. Per-frame items log on change plus once a second.

Live A/B hotkey: cycle `scene_lighting` = vanilla -> uniform (F1+F1b) -> all (F2). On each switch set `+0x510`,
`+0x511`, `+0x513` = 1 on every live timeline (and apply/restore the F2 byte).

1. **0x2007570** (the mod's detour, entry and exit): rcx GUID (16 bytes), dx, r8, return address, `[r8+0x4C5]`,
   `[r8+0x500]` before / after / after the mod's F1 write, `[r8+0x510]`, `[r8+0x478]` stage, `[r8+0x224]`, timeline
   type (TimelineData+0x78 from AV's join log).
2. **0x2002690** entry/exit detour (prologue `48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55`): T, `+0x510`,
   `+0x500`, `+0x478`. **0x3D3BA70** detour (prologue `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20`): only when the
   return address is 0x2002C5D (off) or 0x2002CA0 (on): light entity `*rdx`, r8b, `[light+0xD1]` before/after (light
   = 0x189B300 result, or log the handle only), plus the rig's type `[r13+0x50]` (read in a mid-hook at 0x2002C21) and
   the light Name if cheap. Summary per call: on-count / off-count.
3. **0x2001D50** detour (same prologue form as 2): T, dl (switch +0x131A), r8b (+0x513), `[view+0x28B8]+0x2C8`
   GUID, `T+0x4A0`, path taken. Detours on **0x2004580** (prologue `40 53 55 57 48 83 EC 30`: "automatic removed"),
   **0x2003B10** (`40 55 53 56 57`: actor GUID `*r8`, return, light count `[r9+0xC]` after) and **0x2002EE0**
   (`40 53 56 41 54 41 56`: entity `*r8`, `[rcx+0x2D8]` DisableFlip, light count `[r9+0xC]`).
4. **0x1F7FC10 / 0x1F80030** (`48 8B C4 48 89 58 20 55 56`): T = `[rcx+0x18]`, GUID `*rdx`, blend r8d, views in
   `T+0x5F0`. Also 0x3CBCB80 / 0x3CBCA10 entry (AO plan B).
5. **Correlation line** per frame while a timeline is active: for each actor entity of `T+0x2C8` that is the
   controlled character or within 6 m: number of TL lights on (from item 2 state), automatic lights (map T+0x3A8 count),
   CharacterLight setup id `[view+0x28B8]+0x2E0`. Log only on change.
6. **CharacterLight**: detour 0x19E0CE0 (`48 81 EC 98 00 00 00 …`: the prologue has a `jz rel32` at +0xD, so the
   hooking library must relocate it): view rdx, `[view+0x2268]`, `[[view+0x28B8]+0x2E0]`, xmm (dt), return value;
   **count views per frame** (expect 1 = main only). Detour 0x19E1830 (prologue
   `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20`), return addresses 0x19E3C56 / 0x19E4106 /
   0x19E4784 / 0x19E4C8D / 0x19E54EF (calls at 0x19E3C51, 0x19E4101, 0x19E477F, 0x19E4C88, 0x19E54EA): result
   (al), and the visual `*(u64*)[rdx]`. Count true/false per frame. Once: controller vtable +0x68
   target and the float3 it returns vs the mod's head position.
7. **Shadow budget** (AN plan): every 30 frames per view, stage 0x35 list `+0x2178`/`+0x2184`: per entry light
   pointer, `+0xC`, `+0x324`, `[light+0xD2]` channel, `[light+0x100]` scene. Flag entries whose fade changes while the
   head only rotates.

What the log decides: whether the relights at MARK lines are the item-2 on/off switches at 0x2007570 lighting
camera changes (the expected answer), or the item-3 automatic/custom toggling, or item 4 keys, or item 6 setup swaps,
or item 7 shadow fades. It also shows whether F1 leaves any actor without lights (items 3/5) and whether +0x131A is
set.

---------------------------------------------------------------------------------------------------

## 6. Open leads

* Name and writer of `[0x604B1B8]+0x131A` (the automatic-lighting enable). If it is ever 0 in the user's game, F1
  gives no timeline lights. Log 3 shows the value.
* Exact semantics of light `Type` 0-3 (only 4 = "on regardless of camera" is shown by code).
* Whether the TL scene merges inherited scenes' `LightingSetups`/`Lights` into content+0x250. The sample shows every
  scene file has its own setups, so F1 does not depend on it.
* 0x3EB9370 (render side) applies the CharacterLight cull-flag requests; not decompiled.
* Light-intensity fades by camera distance (SE LightSystem fade maps): not found and not searched beyond SE.

## Raw files (agents\AY)

s1/s2 type and query-spec strings; y1/y2 type ids and query users; y3-y7 CharacterLight (0x19E02D0, 0x19E0F90,
0x19E15D0, 0x19E1920, 0x19E06C0, 0x19E0AE0, 0x19E1780, 0x19E38C0, 0x19E3D70, 0x19E4220, 0x19E48A0, 0x19E5110,
0x19E6460, 0x19E6CC0, 0x19E7480, 0x19E7C30, 0x19E0CE0, 0x19E1830, 0x19E1C40, 0x19E25F0, 0x19E00D0, 0x19DFE80,
0x19E2FB0, 0x19E21D0, 0x19E5970, 0x4126300, 0x19E0020, 0x19E6170); y8 0x2007570; y9 displacement scan
0x500-0x513; y10 0x2002690 / 0x20024A0 / 0x2001D50; y11 0x2045C10 / 0x204E720 / 0x204E7C0 / 0x204DF30; y12
TimelinePost 0x1F208D0; y13 0x2002EE0 / 0x2003B10 / 0x20046A0 / 0x2004580 / 0x2001880 / 0x1F8FCC0 / 0x2002D00 /
0x2003A70; y14 0x2003480 / 0x3D3BA70 / 0x1F7FA10; y15/y24 callers; y16 0x3D3BAE0 / 0x3CB68C0 / 0x42376D0 /
0x1F7FC10 / 0x1F80030; y17 0x3DFBF80 / 0x3DFC0D0 / 0x3DFD980; y18/y23 other +0x500 users; y19-y22/y25/y26
string and FixedString refs; y27 vtable dumps; y28 +0x131A scan; y29 switch names; y30 0x3CBB560 / 0x2003E80.
data\: CharacterLight_Shared.lsx, CharacterLight_Gustav.lsx, _Base_EXT_NOON_Px1_ratfinal.lsx,
_Add_EXT_NOON_Px1_Goblin.lsx, TLS_2SP_DEFAULT.lsx, SCL_Drider_CINE_CaravanIntro_Scene.lsx,
MOO_Docks_AD_JumperGreeting_Scene.lsx, gustav_timeline_files.txt. Scripts: AYServer.java, req.ps1, run.ps1,
ay_scan_scenes.py.
