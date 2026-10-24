# Cutscenes, timelines and the VR camera - agent AV, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656. Ghidra project BG3_G (read-only, -noanalysis, request server
`agents\AV\AVServer.java`), `bg3vr_disasm.exe`, and two read-only PE helpers (`agents\AV\pe_rtti.py`, `pe_ptr.py`).
RVAs are relative to 0x140000000. Raw output: `re-local\ghidra\agents\AV\` (v1..v22). Builds on G_camera.md,
U_cutscene_actors.md, C_animation_systems.md, AF_acting_character.md, W_cpu_and_pipelining.md.
"SE" = Script Extender source (`downloads\bg3se-src`). **Confirmed** = shown by cited code (or by a mod log, said so).
**Unconfirmed** = not shown; each one is in the logging plan (section 5).

One run log was already on disk: `scratch\bg3vr-crash-2026-10-23-1234.log` (lines 11060-11065). It shows the swap hook
working, player ids 1 and 2 (the mod's eye view added player 2), and dummies whose +0x88 is null.

---------------------------------------------------------------------------------------------------

## 1. Anchor table: what the VR camera should sit in, per scene case

"Controlled" = `skeleton.cpp controlled_raw()` (hotbar character via gameui, else the player manager; AF).
"Eyes" = `Visual+0x08 world x bone model matrix` of Dummy_EyeFX_01/02 (Head_M fallback), as skeleton.cpp does now.

| # | Scene case | Body whose eyes are "the player" on screen | How to find it natively, every frame | Confidence |
|---|---|---|---|---|
| A | Dialogue or cutscene where the player character is a **registered actor** (the server gave the timeline the real entity) | The controlled character's own entity and visual. The timeline moves that entity (TimelinePost 0x1F96210 -> TransformSystem with notify -> listener 0x3BF9900 -> Visual SetWorldTransform 0x3C65100, same call stack). | `visual = *(0x1830210(world, controlled))`, world = `controller+0x320`. No swap call happens for it. | Mechanism Confirmed. Which scenes register the player: data (Unconfirmed per scene). |
| B | The scene plays the player character with a **TLPreviewDummy copy** (actor GUID not registered: 0x1D65EE0 slot kind 0 -> 0x1D64EA0 creates a copy) | The dummy entity's visual. The real body is hidden per player view and left where it was. | Hook 0x1D5A350: `show && 0x13FA710(world, *r8) && dummy+0x88 == controlled` -> follow `*r8`. The dummy entity **is** the entity that carries the proxy (`dummy+0x30` = that entity, written by 0x6D5D50), so `0x1830210(world, *r8)` gives its visual. Validate every frame: `0x13FA710(world, followed)+0x88 == controlled`. | Confirmed (0x1D5B890, 0x1D59830, 0x1D5BE40, 0x4234D20, 0x6D5D50). |
| C | Player character **moved / teleported** by the scene (real entity) | Same as A. The written transform is world space, also when parented to a chair, cart or bone (TimelineTransform+0x68). | Same as A. camera.cpp's teleport branch follows the jump. | Confirmed (U 1.1, G r52). |
| D | Player character **not in the scene** (no actor, no dummy for it) | The controlled character's real body, which the scene does not touch (it is not in any swap, not hidden by it). | Same as A. The view camera is **not** switched for that player either: `0x1F8FFB0` takes players only from `Timeline+0x218` or the owner `Timeline+0x6C0`. | Code Confirmed. Whether real cutscenes ever leave the player out: Unconfirmed (data). |
| E | **Several party members** in the scene | Only the controlled character (or the dummy whose +0x88 is it). Other party members get their own actors/dummies; `Timeline+0x218` lists `{character, player}` for each joined player character. | As A/B. Ignore swaps whose dummy+0x88 is another party member (the hook already does). If the controlled character changes mid-scene (AF 0x17F5100), re-pick from the set of currently shown dummies. | Confirmed (0x1D5B890 appends +0x218; 0x1D59830 walks all actors). |
| F1 | Player shown as a **different model by the scene** (timeline shapeshift) | A dummy (TimelineShapeShiftComponent is only queried together with TLPreviewDummy: query 0x60BE4A8, U). Its skeleton may lack Head_M / Dummy_EyeFX. | As B. If no eye/head bone: use the dummy visual's world position + the last good eye offset. | Mechanism Confirmed; bone names data (Unconfirmed). |
| F2 | Model changed **on the real entity** (polymorph, wild shape, Dark Urge forms) | The same entity's new ls::Visual. | `0x1830210` re-read every frame (skeleton.cpp already re-attaches when the pointer changes; check `visual+0x108 == entity`). | Confirmed (U 2.3; runtime log line 11247 shows +0x108 = the character). |
| F3 | Stand-in made from a **visual resource only** (slot kind 4 / 5, 0x4234D20) | Not linked to any character: its +0x88 stays null (log lines 11060-11065 show such dummies). It cannot be identified as the player by code. | Keep the controlled character. | Confirmed (0x1D65EE0, 0x1897CF0 inits +0x88 null). |
| G | **Sitting, lying, kneeling, riding** | Same entity/dummy as above. Only the eye bones give the right height; the visual root stays on the floor/seat. | Eyes as now. The default `camera_anchor = body` puts the camera at root + calibrated standing height, which is wrong in these poses (policy, section 2 F10). | Confirmed (mod code camera.cpp 210-219). |
| H | **Camera-only scene** for the player (type 11 owned by the player via +0x6C0, no body of the player in it) | No body. Data that exists: the scene camera's pose. | `Timeline+0x4F0` (override) else `+0x4E0` = camera actor GUID -> camera manager `0x4087790(*(exe+0x604AB20), &FixedString(guid))` -> controller -> `+0x318` camera entity -> `0xE59570(world, e)` Transform; or `Timeline+0x500` (lighting camera GUID, set even when the view switch is skipped). | Confirmed (G 2.3, 0x1D658D0). That such scenes reach a VR player: Unconfirmed. |

Notes that apply to all rows:
- The hidden real body in case B keeps its old place. Anything that falls back to it puts the camera "behind". In
  the current mod three paths do that (section 2: F2, F3, F7).
- Dummies start hidden: 0x1D64EA0 hides a new dummy for the players right after creating it (calls to 0x1D5A350
  with show 0). They become visible on join (0x1D59830 from 0x1D5B890).

---------------------------------------------------------------------------------------------------

## 2. Fixes to the mod

Ordered by impact on "the camera does not follow the bodies".

**F1. The per-frame work runs on every camera view update, on several threads.** `camera.cpp on_view_update`
(lines 305-308) calls the frame callback (`bootstrap.cpp on_game_frame`: skeleton, cull, views, follow, grab...)
before the `ctx.rcx == g_controller` test. 0x4085150 has 39 call sites (U u20): the gameplay camera path, the cull
frustum 0x4086010, TimelinePreAnimationUpdate 0x1F1A6CB, picking 0x4085960, and render-side stage code 0x3E1B0C0 /
0x3E1BB10 / 0x3E1C940 (render thread and a worker, W 4.1). So `skeleton::on_frame` calls ECS getters (0x1830210,
gameui VMs) off the game thread, and `g_main_thread` records whatever thread came last. Fix: call the frame callback
only when `ctx.rcx == g_controller` and only from one thread (learn it from the first call whose return address is in
the ECS camera path, e.g. 0x1E13A3C / 0x1E13D1C / 0x1F1A6D0; log first, section 5 item 1). Once per game frame
(`[exe+0x614FB88]` changed). Confirmed (mod code; caller list).

**F2. Stale pose -> camera jumps to the real (hidden) body.** `camera.cpp update_follow` uses the skeleton only when
`head->fresh` (`pose stamp +0x124 + 1 >= frame`); otherwise it reads `g_follow->transform`, the TransformComponent
pointer Lua sent for the **controlled character**. While following a dummy that means the hidden real body. Dummy
animation is gated: TimelineDummyAnimationThrottleSystem 0x1F1B2C0 sets `TLPreviewDummy+0xC0` (ActiveDummy) per
dummy when GlobalSwitches+0x132E is set, and TimelinePreAnimationUpdate 0x1F1A1E0 animates type 1/8 dummies only
when flagged or when 0x4126300 passes for a player view. Fix: when the pose is stale, keep using the followed
visual's world transform (`Visual+0x08`, which TimelinePost keeps current) plus the last good eye offset; never use the
Lua pointer while a stand-in is followed. Confirmed (mod code, 0x1F1B2C0, u1 0x1F1A1E0); how often dummies go stale:
Unconfirmed (item 7).

**F3. No head bone -> same fallback.** `skeleton.cpp on_frame` returns at `g_head_bone < 0` (line 606) after
`attach_now` reset `g_head`, so `head()` is empty and camera.cpp uses the Lua transform (real body). A shapeshifted
dummy (row F1) can lack Head_M. Fix: publish a Head with `has_eyes = false`, `fresh = true` and the visual's world
transform, so update_follow keeps the followed body. Confirmed (mod code).

**F4. Validate the followed dummy every frame.** `g_dummy` is only changed inside the hook. If a timeline is unloaded
without a hide call, or the controlled character changes, the mod keeps a dead handle; `visual_of` then returns 0 and
`g_visual` keeps the old pointer. Fix: each frame `d = 0x13FA710(world, g_dummy)`; drop it unless
`d && *(u64*)(d+0x88) == controlled`. Keep a small set `{dummy, represented}` of dummies currently shown (add on
show=1, remove on show=0) and pick the one whose +0x88 is the controlled character, so a mid-scene character switch
lands on the right body. Confirmed need (mod code); leave paths 0x1D5BE40 / 0x1D5A960 Confirmed.

**F5. Filter the swap hook by player.** The run log shows calls for players 1 and 2 (player 2 = the mod's eye view
added through CreatePlayerView). `0x1D5A350` with player 0 recurses over all players itself. Act only when
`player == primary` (`*(u16*)(mgr+0xD0)` / 0x3BAADF0; it was 1 in the log). Otherwise a show of the real body for
player 2 (`!dummy && show && handle == controlled`) can clear the dummy. Confirmed (log + 0x1D5A350 recursion, U u6).

**F6. Keep the timeline camera off the view for every camera-taking type.** dialogue.cpp sets `Timeline+0x4C5` only
for `[dialogue] first_person_types` (default "1") and only at the join (0x1D5B890). Types 2, 3, 8, 10, 11 (mask 0xD0E)
still switch the view to the scene controller (MasterBehaviorType 11). camera.cpp then writes nothing (its test is
`rcx == g_controller`), so the headset shows the rigid cinematic camera. Also 0x2007570 is called outside the join
(shots 0x20458E0 / 0x2046450 / 0x2046DA0 / 0x2047470 / 0x204E720, re-activation 0x1D79A50, 0x1D55400, 0x1D62890,
0x1D63A90, 0x1D64220, 0x1F21810), and a type-11 timeline has a player (`+0x6C0`) before any join. Fix: hook
**0x2007570** entry (prologue `48 89 5C 24 10 55 56 57 41 54 41 55`; rcx = camera GUID*, dx = player, r8 =
Timeline*) and set `[r8+0x4C5] = 1` while VR drives the camera. Keep the 0x1D5B890 hook for logging. Add a guard:
if the primary view's controller (`view+0x2260` WriteState / `+0x21D8` ReadState) is not `g_controller`, log it and
put it back with `0x17F7CD0(mgr, player, g_controller, 0, 0, 0)` (what the game's own return path 0x1D79890 does).
Confirmed (G 2.3, disasm 0x2007AE9..0x2007B2C, 0x1D79890 in G r7).

**F7. Lua lines fight the native follow.** `CameraProbe.lua NativeFollowTick` re-sends `skeleton=<character visual>`
on change and `follow=<TransformComponent*>` every 15 ticks. `skeleton=` runs `attach_now(lua visual)` and resets
`g_followed_entity`, so during a dummy scene the camera spends a frame on the real body and re-reads all bones. Fix:
when native follow is ready, treat `skeleton=` only as "follow on/off"; replace the `follow=` pointer by
`0xE59570(world, followed entity)` each frame (the followed entity, not the controlled one). Confirmed (mod code).

**F8. Head hiding on the stand-in.** FirstPerson.lua hides head parts of `BG3VR.Controlled().Visual` only; the dummy's
own head/hair/helmet attachments stay drawn and the camera sits inside them. The dummy's attachments are copied from
the character (0x1898C40 -> 0x1FAD880, or template of +0x88 via 0x1899170), so the same name patterns apply. Fix:
send the followed dummy handle to Lua (new to_lua key) and build `hide=` from that entity's visual; on leave give the
character's head parts back. Confirmed (mod code, 0x1898C40).

**F9. Scene start/end detection.** `update_conversation` uses view+0x2961 (cinematic mode). 0x1F90B00 sets it for
types 1, 2, 3, 10, 11, so combat cinematics also count as "dialogue". Add an entry hook on **0x1D5BE40** (leave;
prologue `40 55 53 56 57 41 54 41 55`) next to the join hook, keep `{timeline, actor, type}` for the controlled
character, and decide "dialogue with answers" from VMDialogue (AB 4.3). Confirmed (G r24, AB).

**F10. Eye height in scenes (policy, data only).** In body-anchor mode the camera is `visual root + g_follow->height`.
Sitting, lying, kneeling and short/tall stand-ins are then wrong by up to ~1.5 m. The only data that gives the right
height is the eye bones (already read). Options for the user: eye height only (no horizontal sway) while a timeline
holds the controlled character, low-pass filtered; or re-calibrate the height when the followed body changes.
Confirmed data path; choice is the user's (camera-head-only rule).

What is right today: the getters and offsets (0x3BAADF0, 0x17F4FF0, 0x1830210, 0x13FA710, +0x88, Visual+0x108,
controller+0x320) are correct; the swap hook semantics are correct; re-resolving the visual every frame is correct;
the teleport branch in camera.cpp is correct for case C.

---------------------------------------------------------------------------------------------------

## 3. System map

### 3.1 Objects and layouts

**ecl::TLPreviewDummy** (proxy object, 200 bytes; ctor 0x1897CF0; vtable 0x55FC8C8, second vtable 0x55FC8B0 at +0x08;
type-id getter slot 8 = 0x1897CC0 -> `[0x60A6708]`; component type id `[0x60531DC]`).

| Off | Meaning | Evidence |
|---|---|---|
| +0x10 / +0x18 | EntityRef {dummy entity, world} | 0x4234D20 writes |
| +0x30 | the dummy entity handle (the entity that carries this proxy) | 0x6D5D50 `*(dummy+0x30) = entity`; 0x1898580 gets the visual from it |
| +0x88 | the character this dummy stands in for; null for prop/empty dummies | ctor sets null (0x1897CF0); read by join 0x1D5B890 (Character of +0x88 -> player id), 0x1D3C870, 0x1D3CC70, 0x1D59830, 0x1898C40 (copies the look from Character(+0x88)), 0x1899170 (template of +0x88). Store happens in the deferred creation queued by 0x1D462C0 (source entity passed by 0x1D64EA0); the exact store instruction was not located. |
| +0xA8 | flags; bit 1 used before re-applying effects (SE VisibilityFlags) | 0x1D49B30 |
| +0xB9 | TimelineType | 0x4234D20 writes |
| +0xC0 | ActiveDummy (animate or not) | 0x1F1B2C0 writes 0/1; 0x1F1A1E0 reads |
| +0xC2 | set 1 when the visual is set up | 0x189A5B0 |

Visual+0xD8 = the TLPreviewDummy object for a dummy's visual (set 0x1898C40, cleared 0x1898100). Confirmed.

**ecl::Timeline** (fields used here; G 2.4 has more): +0xE4 scene transform; +0x1B8 dialog manager; +0x208/+0x204 and
+0x2C8/+0x2C4 actor entity arrays (walked by 0x1D59830 and 0x1D3CC70); +0x218/+0x224 JoinedPlayerActors
`{EntityHandle, i16 player}` 16 bytes; +0x228..+0x258 RegisteredActors map GUID -> EntityHandle (lookup 0x3CBABF0);
+0x488 preview; +0x489 gameplay; +0x48A/+0x48B combat; +0x4C4 paused; +0x4C5 skip view switch; +0x4E0 / +0x4F0
camera actor GUID / override; +0x500 lighting camera GUID; +0x528 DialogInstanceId; +0x6C0 owning player.

**ecl::TimelineSystem** (fields used): +0x310 (passed to 0x1FF80F0 on visibility), +0x318 PlayerManager, +0x328
camera manager, +0x480 ActorLeaveRequests (0x38 each), +0x630 CharacterAssignedRequests (0x18), +0x640
TimelineVisibilityRequests (0x18: GUID, i16 player, bool visible), +0x6B0 control create/destroy (0x48), +0x68C
camera re-activation count, +0xB20 Timelines map (lookup by GUID 0x1D428E0, values +0xB50), +0xC50 + 0x40*viewIndex
per-view sets, +0xDA0 deferred dummy creation (0x1D462C0). Confirmed by the cited functions.

### 3.2 Start, run and end of a scene (client)

1. **Content and actors.** 0x1D65EE0 walks the timeline content's actor slots (GUIDs at content+0x198, 0xF8-byte
   descriptors at +0x1A8, kind byte at +0x2C). For a slot whose GUID has **no registered entity** (0x3CBABF0 returns
   null): kind 0 (character) -> 0x1D64EA0 makes a TLPreviewDummy copy of the GUID's character; kind 1 (camera) ->
   0x1D658D0 creates/looks up the scene camera (camera manager by GUID string); kind 4 -> visual-resource dummy;
   kind 5 -> empty dummy (camera-taking types). Each gets ClientTimelineActorControl (0x1D77E00). Registered GUIDs
   play with the real entity. Confirmed control flow.
2. **Dummy creation.** 0x4234D20 creates the entity (TimelineTransform, TLPreviewDummy via 0x6D5D50, +0xB9 type,
   +0x10 ref); 0x1D462C0 queues the deferred visual/state copy from the source character; 0x1D64EA0 then hides the new
   dummy for the players (0x1D5A350 show 0). The dummy visual is built by 0x1898C40 / 0x1899170 from the source
   character (equipment copy 0x1FAD880) or its template. Confirmed.
3. **Join.** TimelineSystem body 0x1D3FF10 iterates `TimelineActorJoinedEventOneFrameComponent + TimelineData`
   (query 0x60BE474) and calls **0x1D5B890**(sys, ctx, Timeline*, TimelineData*, actor*) then 0x1D596D0 per actor.
   0x1D5B890: Character = 0x7841A0(world, actor) or, for a dummy, of `dummy+0x88`; player = `Timeline+0x6C0` or
   Character+0x12C; appends `{Character+0x10, player}` to +0x218 unless IsAdditional (ClientTimelineActorControl+0x21).
   If the actor is that player's current character (0x17F5050 EntityRef == Character+0x28/+0x30), or the timeline's
   dialog entry (Timeline+0x1B8 keyed by +0x528) has +0x189 set or +0x1D4 valid: for types 0xD0E it calls 0x1D59830 (show the timeline to that player: every dummy shown, every represented
   entity hidden, via 0x1D5A350), then **0x2007570** (view switch unless +0x4C5); types 1,2,3,10,11 -> cinematic flag
   0x1F90B00; type 1 + GlobalSwitches+0x1320 -> screen fade; type 1 -> TimelinePlayerJoinedOneFrame (0x1D71640).
   Other actors: for each timeline player, hide `+0x88` (if any) and show the actor. Confirmed (G r24, disasm
   0x1D5B8E0..0x1D5B97A).
4. **Run, each frame.** TimelineSystem 0x1D3FF10 (requests, time 0x1D4D2E0, visibility apply 0x1D6A6C0);
   TimelineCameraSystem 0x1F1CBA0 updates scene cameras (0x1F1D180 calls TransformSystem SetRotate 0x3C77D20)
   whether or not they hold a view; TimelinePreAnimationUpdate 0x1F1A1E0 (dummy animation gating);
   TimelineDummyAnimationThrottle 0x1F1B2C0 (ActiveDummy); AnimationUpdate 0x1F14C80 -> jobs (C 1.3);
   **TimelinePost 0x1F208D0 -> 0x1F95E80 -> 0x1F96210** writes actor TransformComponents (notify -> visual moves at
   once); TimelineTransformFlush 0x1F22770 adds SwitchStage / SwitchLocation / ShowVisual one-frame components (the
   ShowVisual consumer is 0x1DCB370, not decoded). Shots call 0x2007570 per cut. Confirmed (G, U, here).
5. **Leave.** Query 0x60BE478 (`TimelineActorLeftEventOneFrameComponent + TimelineData`) -> 0x1D596D0 and
   **0x1D5BE40**(sys, ctx, Timeline*, &instance GUID, actor*, ...): swaps back (dummy hidden, +0x88 shown), cinematic
   flag off, 0x2007570 with a null GUID, type 1 -> gameplay camera re-target 0x1F579A0, camera return 0x17F7AD0 ->
   0x1D79890 -> 0x17F7CD0(own controller), restore of the saved transform in 0x1F96210 (U 1.1 step 5).
   ActorLeaveRequests (sys+0x480) -> 0x1D5A960 / 0x1D3CAA0. Confirmed.

TimelinePost's declared dependencies (registration 0x7486D0): ecl::TimelineSystem and ls::TransformSystem; access
sets include ls::TransformComponent and ls::VisualComponent. ecl::CameraSystem (0x738A70) depends on
ls::TransformSystem, character_creation, TurnOrder, FadeLegacyRequests and also accesses Transform and Visual. No
declared order between TimelinePost, AnimationUpdate and CameraSystem was found. Order: Unconfirmed (item 1).

### 3.3 Scene type

| Data | Where | Meaning |
|---|---|---|
| TimelineType | TimelineData+0x78 (join r9), dummy+0xB9, TimelineActorData+0x20 | G 2.5 table; camera-taking = mask 0xD0E (1,2,3,8,10,11) |
| Dialog | TimelineData+0x60 / Timeline+0x528 DialogInstanceId; VMDialogue (AB 4.1) ShowAnswers +0x300 | dialog-driven; answers on screen |
| Combat cinematic | Timeline+0x48A / +0x48B | types 2 / 3 |
| Owned by one player | Timeline+0x6C0 != 0xFFFF... (`DAT_145EF3A2C`) | type 11; camera switch without a body |
| In a scene at all | +0x218 contains the controlled character, or a shown dummy has +0x88 == it | join/leave hooks keep this |

"Cinematic dialogue vs full cutscene": both are timelines; the code does not separate them beyond the type and the
dialog data. Which type each real scene uses: Unconfirmed (item 2; G 2.5).

### 3.4 Camera

- Scene cameras: controller 0x330 bytes (vtable 0x58B6750) created by 0x1F1C730 / looked up by GUID (0x1D658D0);
  MasterBehaviorType 11; camera entity at controller+0x318, world at +0x320 (G).
- Only route onto the view: 0x2007570 -> 0x17F7E10 -> 0x17F7CD0 -> 0x3CC90B0 (G 1.5). With +0x4C5 the view keeps the
  gameplay controller for that timeline; the scene camera entity keeps being updated and the lighting camera
  (+0x500) still follows the shots.
- Return: 0x1D79890 only when the leaving actor is the player's current character; it re-sets the player's own
  controller (G: Exit/Enter of the same controller).
- What the mod's override must do: F6. With the gameplay controller on the view, camera.cpp's transform write at
  0x4085150 stays the only pose source. Screen fades (type 1 + GlobalSwitches+0x1320) and the cinematic flag still
  happen; letterbox needs a camera entity with TimelineActorData type 1 (G), which the gameplay camera does not have.

### 3.5 Visual and skeleton of a stand-in

The dummy entity has its own VisualComponent, TransformComponent, TimelineTransform, AnimationBlueprint (query
0x60CC61C, U) and its own ls::Visual and Skeleton. Its look is copied from the represented character, so the rig is
expected to be the same (Head_M, Dummy_EyeFX_01/02). The bones' presence is data: skeleton.cpp `attach_now` logs them
on every switch (Unconfirmed until that log line exists for a dummy).

---------------------------------------------------------------------------------------------------

## 4. What the mod reads today, checked

| Item | Verdict |
|---|---|
| `controlled_raw` (gameui hotbar, then 0x3BAADF0 + 0x17F4FF0) | Right (AF). Called inside the swap hook too, i.e. inside the ECS TimelineSystem update; reading UI VMs there is a thread question (F1 log). |
| `by_handle_raw` / `visual_of` (0x1830210 with controller+0x320) | Right; must run on the game thread only (F1). |
| `camera_world` (controller+0x320) | Right (0x3CF0740 uses it). |
| Swap hook 0x1D5A350, `dummy+0x88 == controlled` | Right semantics; needs player filter (F5), per-frame validation (F4). Callers: 0x1D59830, 0x1D5B890, 0x1D5BE40, 0x1D46F50, 0x1D64EA0, 0x1D653B0, 0x1D65EE0, 0x1D67430, 0x1D6C1E0. |
| `fresh` test and Lua transform fallback (camera.cpp) | Wrong for stand-ins (F2, F3). |
| `skeleton=` / `follow=` from CameraProbe.lua | Fights native follow (F7). |
| dialogue.cpp +0x4C5 at join for type 1 | Too narrow (F6). |
| update_conversation via view+0x2961 | Also true for combat cinematics (F9). |
| Head hiding (FirstPerson.lua on `BG3VR.Controlled()`) | Misses the stand-in (F8). |
| on_game_frame from every 0x4085150 call | Wrong threads (F1). |

---------------------------------------------------------------------------------------------------

## 5. One-run logging plan

One run: walk; an ordinary NPC conversation; the cutscene where the camera stayed behind; a long rest / camp scene; a
combat cinematic if one appears. Every line carries the frame `[exe+0x614FB88]`, a global sequence number and the
thread id. All reads under `__try`. Rate-limit per-frame lines to changes plus one line per second.

1. **0x4085150 calls** (camera.cpp hook, before any filter): return address, thread id, rcx, `rcx == g_controller`,
   frame. Pick the game-thread call for F1. Also in the same frame: sequence numbers of items 3 and 6 below. This
   settles the order TimelinePost / animation / camera.
2. **0x1D5B890 entry** (have): add actor `*[rsp+0x28]`, `d = 0x13FA710(world, actor)`, `d+0x30`, `d+0x88`, `d+0xB9`,
   `d+0xC0`, `d+0xC4`; `Timeline+0x6C0`, `+0x218` entries; `TimelineData+0x60/+0x78/+0x79/+0x7A`; `+0x4E0/+0x4F0`
   GUIDs; controlled handle; VMDialogue non-null and ShowAnswers.
3. **0x1F96210 entry**: `[r8+0x50]` when it is the controlled entity or the followed dummy; sequence number.
4. **0x1D5BE40 entry** (new): r8, r9 GUID, actor, dummy fields as in 2.
5. **0x1D5A350 entry** (have): add the return address (which caller), player, `d+0x30` vs the handle, `d+0x88`,
   `d+0xC0`, the handle's visual (0x1830210) and `visual+0x108`.
6. **0x2007570 entry**: GUID bytes at rcx, player dx, Timeline r8, `+0x4C5` before/after, return address. After the
   call: the primary view's `+0x2260` and `+0x21D8` controller vs `g_controller`.
7. **Per frame while following** (the F1 thread): followed entity, controlled, the visual and `+0x108`, Visual+0x18
   position, skeleton `+0x124` vs frame, `fresh`, Head_M index, which source update_follow used (eyes / visual / Lua
   transform), distance between the Lua transform and the followed visual, `d+0xC0` of a followed dummy, slot-30 calls
   for its skeleton, GlobalSwitches+0x132E once.
8. **Real body while a dummy is followed**, once a second: its Visual+0xF4 cull flags, Visual+0x18, skeleton +0x124.
9. **Swap set changes**: every add/remove of the shown-dummy set (F4) and why the followed body changed.
10. **0x3BF9900 post-hook** for the followed entity (U plan item 4): frame, thread, Visual+0x18 after.

What the log decides: thread and order of the per-frame work (1, 3, 10); which scenes use a dummy for the player (2,
5); whether the scene camera ever gets the view (6); whether stale dummy poses trigger the real-body fallback (7);
whether the hidden real body still animates (8).

---------------------------------------------------------------------------------------------------

## Raw files (agents\AV)

v1 refs (type ids, getters), v2/v9 displacement 0x88/0x80 scans, v3 callers of 0x13FA710, v4/v5 dummy vtable refs and
ctor callers, v6-v7 dummy class (0x1897CF0, 0x1898C40, 0x1899170, 0x189A5B0, 0x1898100, 0x1898580), v8 (0x1D59830,
0x1D59170, 0x1D5A960, 0x1D428E0), v10 bundle builders, v11/v12 camera call chain, v13 0x1E13270 / 0x1E0D940,
v14 proxy factory 0x6D5D50, v15/v16 creator callers, v17 dummy creation (0x4234D20, 0x1D64EA0, 0x1D653B0, 0x1D65EE0,
0x1D67430), v18 deferred creation 0x1D462C0 / camera actor 0x1D658D0, v19/v20 throttle and dummy update systems,
v21 ShowVisual consumers, v22 0x3CBABF0 / 0x1F90550. Scripts: AVServer.java, req.ps1, run.ps1, pe_rtti.py, pe_ptr.py.
