# Cutscene / timeline actors and the first-person camera - agent U, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra headless, read-only, request server `agents\U\UServer.java`;
project BG3_G for u1, BG3_A for u2..u26). RVAs are relative to 0x140000000. Raw output: `re-local\ghidra\agents\U\`
(`u1_*.c` .. `u26_*.c/.txt`). "SE" marks a name or layout taken from the Script Extender source
(`downloads\bg3se-src`); a SE name is only attached to an offset when the code's behaviour fits it. Type names of
component / query ids come from agent G's `typeidmap.txt`. Builds on G_camera.md (timeline objects, join 0x1D5B890,
leave 0x1D5BE40), C_animation_systems.md (skeleton, Visual layout), K_weapons.md (attachments 0x3C68F40),
B_splitscreen.md (player manager).

Short answer to "why did the camera stay behind": a timeline can play a scene with a **separate dummy entity**
(`ecl::TLPreviewDummy`) that stands in for a real character. When the dummy joins, the game **hides the real
character for that player's view and shows the dummy** (and swaps back when it leaves). The dummy has its own entity,
TransformComponent, ls::Visual and skeleton. The real character's entity and visual are not touched by that scene, so
a camera that follows the real character's visual stays where the real character was. In scenes where the real
character itself is an actor (the TimelineType 7 log), the timeline writes the real entity's transform and the visual
follows, which is what the mod saw. The mechanism is Confirmed in code; that the user's failing cutscene used a dummy
for the player character is Unconfirmed (data-driven, see the logging plan).

---------------------------------------------------------------------------------------------------

## Hook points for the VR mod

| need | RVA | how to use | confidence |
|------|-----|-----------|------------|
| **Know when the player's body is swapped for a timeline dummy (and back)** | **0x1D5A350** `TimelineSetActorVisibleForPlayer(TimelineSystem* rcx, ctx* rdx, EntityHandle* r8, Timeline* r9, bool show [rsp+0x28], i16 playerId [rsp+0x30])` | Hook entry. `world = *(EntityWorld**)rdx`. `dummy = 0x13FA710(world, *r8)` (TLPreviewDummy or null). If `dummy && show` and `*(u64*)(dummy+0x88) == controlled character handle` -> follow the entity `*r8` (the dummy) from now on. If `dummy && !show` for the followed dummy, or `!dummy && show` for the controlled character -> follow the controlled character again. playerId 0 means "all players" (the function recurses once per player itself, so the hook sees each player call). | Confirmed (code: 0x1D5A350, call pairs in join 0x1D5B890 / leave 0x1D5BE40) |
| Read a TLPreviewDummy (read-only) | **0x13FA710** `void* GetTLPreviewDummy(EntityWorld*, EntityHandle)` | Returns the proxy component or 0. `+0x88` = EntityHandle of the entity the dummy stands in for (the one hidden while the dummy is shown). | Confirmed (getter; +0x88 use in 0x1D5B890, 0x1D5BE40, 0x1D5A350, 0x1D3A4D0). Meaning "real character": Unconfirmed |
| Player-controlled character handle | **0x3BAADF0** `u16 GetPrimaryPlayer(PlayerManager*)`, **0x17F4FF0** `EntityHandle* GetPlayerCharacter(PlayerManager*, EntityHandle* out, i16 player)` with `PlayerManager* = *(void**)(exe+0x60487A0)` | `id = 0x3BAADF0(mgr); 0x17F4FF0(mgr, &h, id)`; `h == *(u64*)(exe+0x5EEB6E0)` means none. 0x17F5050 is the same lookup returning an EntityRef `{handle, world}` where world = `*(void**)(exe+0x6007F68)`. Pure map reads, no side effects. | Confirmed (decompiles u7, u26) |
| Client EntityWorld | `*(void**)(exe+0x6007F68)`, or `*(void**)(cameraController+0x320)` inside the 0x4085150 hook | Either works; 0x17F5050 pairs player handles with the global, and the gameplay camera controller's Transform getter 0x3CF0740 uses `ctrl+0x320` / `ctrl+0x318`. | Confirmed (u7, u19, u26) |
| Entity -> its current ls::Visual | **0x1830210** `ls::VisualComponent* GetVisualComponent(EntityWorld*, EntityHandle)`; `Visual* = *(void**)comp` | Re-resolve every frame instead of caching a Visual pointer. The game calls it from ~35 places (animation pre-update 0x3BD3C60, render view 0x3CCB020, the transform->visual listener 0x3BF9900). It flags the component as accessed (an atomic bit set after the lookup, like other write-capable getters); what that bit drives is Unconfirmed. Call it on the game thread only. | Confirmed (u18) |
| Entity -> TransformComponent (read-only) | **0xE59570** `Transform* GetTransform(EntityWorld*, EntityHandle)` (0x28 bytes: quat +0x00, translate +0x10, scale +0x1C) | Cross-check the visual's world transform; fall back to it when an entity has no visual. No bit set (read path). | Confirmed (u7) |
| Visual world transform is always world space | Visual +0x08 Transform (quat +0x08, translate +0x18, scale +0x24) | Keep reading it as world. It is written by SetWorldTransform 0x3C65100 / SetWorldTranslate 0x3C64C30 directly, and attachment children get world values computed by 0x3C68F40. Visual+0x30 (`LocalTransform*`, SE) is an object-space pre-transform used for the bounds, not a parent-relative transform. | Confirmed (u13, u7) |
| Visual -> owning entity (stale-pointer check) | Visual +0x108 EntityHandle, +0x110 EntityWorld* (SE `VisualEntity`) | Each frame check `*(u64*)(visual+0x108)` equals the entity being followed; together with the vtable check (0x588CB30) this rejects a recycled Visual. | Offsets Confirmed (C, K, 0x3C68F40); that the root visual's +0x108 is the character entity: Unconfirmed (log) |
| Moment the game moves a visual from its entity transform | **0x3BF9900** (listener registered by 0x3BF2A20 on the TransformSystem full-transform list) | `(void* functor, {EntityHandle, EntityWorld*}* rdx)`. It does `visual->SetWorldTransform(GetTransform(world, h))` synchronously inside every TransformSystem SetTransform with notify. Post-hook and filter on the followed entity to know when (and on which thread) this frame's placement happened. | Confirmed (u23, u25) |
| Timeline writes an actor's transform | **0x1F96210** (per actor, from 0x1F95E80 <- TimelinePostSystem 0x1F208D0) | r8 = per-actor component bundle, `[r8+0x50]` = actor EntityHandle. Logging only. | Confirmed (G r52, u1) |

---------------------------------------------------------------------------------------------------

## 1. How timelines place their actors

### 1.1 The writer: TimelinePostSystem -> 0x1F95E80 -> 0x1F96210 (Confirmed)

`ecl::TimelinePostSystem` body 0x1F208D0 (u1) copies each running timeline's scene transform (`Timeline+0xE4`, 0x28
bytes; SE `CurrentTransform`) into its TimelineDataComponent (+0x38..+0x5F), then calls 0x1F95E80 over query
0x60CE1C0 = {required `ls::TransformComponent`, `ecl::TimelineTransformComponent`,
`ecl::ClientTimelineActorControlComponent`; optional `ecl::SceneCameraBehavior`, `ls::VisualComponent`}. (G listed the
last two as excluded; the bundle builder 0x1D79B10 shows they are optional slots: stride 0x10 = VisualComponent,
stride 0x108 = SceneCameraBehavior.) For each actor, 0x1F96210 does:

1. If the actor's clip slot (`TimelineTransform+0x108`) is valid, 0x1F98AB0 evaluates the timeline's transform
   channels into the component: local rotation +0x40, translation +0x50, scale +0x5C, dirty mask +0x100
   (1 translate, 2 rotate, 4 scale), active +0x110.
2. Parent frame: by default the timeline scene transform (`Timeline+0xE4`). If `TimelineTransform+0x68` holds an
   EntityRef, that entity's TransformComponent is used instead (0xE59570), optionally a bone/attach point of it
   (0x1F92C10, cached at +0x80..+0xA8, parent cached at +0xAC..+0xF8).
3. `world = parent x local` (quaternion/scale maths in the function), then through `ls::TransformSystem`:
   SetRotate 0x3C77D20, SetScale 0x3C77FA0, SetTranslate 0x3C77BD0, each with notify = 1.
4. If the actor has a VisualComponent, 0x3C64B20 writes the visual's 0x18-byte block at Visual+0x80 (SE
   `RenderPrecisionHack`): scene origin and actor offset, kept apart for rendering precision.
5. When the actor is no longer driven (+0x110 == 0 and the state bytes +0x111..+0x116 say so), it writes the
   Transform stored at `TimelineTransform+0x18` back with SetTransform 0x3C78110 (notify) and marks it done. That is
   the restore on leave G listed as open. That +0x18 is captured at join is Unconfirmed (writer not traced).

So the timeline moves the **entity**: it writes `ls::TransformComponent` of the client entity. It does not write
the Visual directly, and the parent/attach case still produces a world transform.

### 1.2 Entity transform -> visual (Confirmed)

`ls::TransformSystem::SetTransform` 0x3C78110 writes the component (0x3C79010) and, with notify set, calls every
functor in the list at TransformSystem+0x60 (count +0x6C). ls::VisualSystem's init 0x3BF2A20 registers 0x3BF9900 on
that list (and 0x3BF9860 / 0x3BF98B0 on two sibling lists, translate/rotate style; not decompiled). 0x3BF9900 looks
up the entity's VisualComponent and TransformComponent and calls the Visual's SetWorldTransform (vtable +0x28 =
0x3C65100). 0x3C65100 copies the transform to Visual+0x08, pushes it to every renderable (`+0x98` ObjectDescs, their
vtable +0x28), refreshes the precision block, re-places all attachments (0x3C68F40) and, if moved more than 1 cm,
calls 0x3C6AAA0. So the visual follows its entity in the same call stack as the timeline write. This matches the
TimelineType 7 run, where the visual jumped 34.5 m and the head followed.

### 1.3 Root motion inside the pose

Bones are model space (C 1.4). Whatever a timeline animation does to the root bone ends up inside the model-space
matrices the mod already reads with slot 10, and the entity-level placement comes through 1.1/1.2. Root-motion
events from ordinary animation go to the entity through 0x3BD1AB0 -> 0x3C78110 (C 1.7), which also notifies the
visual. Reading `eye = VisualWorld x boneModel` therefore covers all three cases (entity moved, visual moved, root
bone animated). Confirmed by the code paths; whether timeline clips carry root motion: not needed.

### 1.4 The other timeline systems asked about

| system | body | what it writes | confidence |
|--------|------|---------------|------------|
| ecl::TimelineTransformFlushSystem | 0x1F22770 | Reads `TimelineTransformFlushSingletonComponent` (query 0x60BE408) and adds one-frame tags to the listed entities: `TimelineSwitchStageEventOneFrameComponent` (0x1F22B00), `TimelineSwitchLocationEventOneFrameComponent` (0x1F22C60), plus a per-entity byte (0x1F22DC0). No transforms. The singleton is filled from 0x1F9DC70 inside 0x1F96210 (TimelineTransform +0x117 bits). | Confirmed |
| ecl::TimelinePreAnimationUpdateSystem | 0x1F1A1E0 | Only dummies: query 0x60CC61C = {AnimationBlueprint, TimelineTransform, TimelineActorData, **TLPreviewDummy**, VisualComponent} minus Invisibility. For TimelineType 1 or 8 dummies with `TLPreviewDummy+0xC0` set (SE `ActiveDummy`) it runs the dummy's animation update (0x1F19DB0) only if the dummy is flagged (bit 0x40, set from `TimelineTransform+0x117 & 7` or from two entity lists in the system at +0x60 / +0x70), or if 0x4126300(visual WorldBound +0x40, `controller+0x210`, 10 or 6) passes for some player view's controller (`view+0x2260`, refreshed with 0x4085150 first). That 0x4126300 is a frustum test: Unconfirmed (name from use). | Confirmed control flow (u1) |
| ecl::timeline::ActorParticipationSystem | 0x1D3A4D0 | Keeps `ActorParticipationSingletonComponent` (actor -> timeline map) from the Joined/Left one-frame events. For each actor it also maps `TLPreviewDummy(actor)+0x88`, i.e. the represented entity counts as participating too. | Confirmed (u1) |
| ecl::TimelineDummyUpdateSystem | 0x1D8FB90 / 0x1D8FF10 | Queries on TLPreviewDummy entities (0x60BF51C, 0x60BF520: look-at override, steering, visual, transform, blueprint). Not decompiled. | Located |
| ecl::TimelineSystem body | 0x1D3FF10 | Calls 0x1D6A6C0, which applies queued visibility requests per view through `TimelineFadeActiveView0/1Component` (queries 0x60BE4FC / 0x60BE500). | Located (u6) |

---------------------------------------------------------------------------------------------------

## 2. Can the player be represented by another visual or entity?

### 2.1 Timeline dummies (`ecl::TLPreviewDummy`) - the swap (Confirmed)

- `ecl::TLPreviewDummy` is a proxy component on a **separate entity** with its own VisualComponent,
  TransformComponent, TimelineTransform, TimelineActorData and AnimationBlueprint (all the dummy queries above
  require them together). SE fields include `OriginalCharacterTemplate`, `ShapeShiftSourceTemplateOverride`,
  `ArmorSetState`, `ActiveDummy`, `IsPlayer`, `TimelineType` (SE; only +0xC0 = ActiveDummy is used in code read
  here).
- **Join** (0x1D5B890, OnTimelinePlayerJoined, args: rcx sys, rdx ctx, r8 Timeline*, r9 TimelineData*, [rsp+0x28]
  actor EntityHandle*, ...): `d = 0x13FA710(world, actor)`; if d exists, `real = d+0x88`. For each player of the
  scene it calls `0x1D5A350(..., &real, tl, show=0, player)` then `0x1D5A350(..., actor, tl, show=1, player)`:
  hide the represented entity, show the dummy (G r24 lines 905-918, 1110-1118).
- **Leave** (0x1D5BE40): `real = TLPreviewDummy(actor)+0x88`; pairs `0x1D5A350(actor, show=0)` and
  `0x1D5A350(real, show=1)`, i.e. the reverse (G r24 lines 1314-1321, 1556-1610).
- **0x1D5A350** itself (u6): with playerId 0 it recurses for every player of the player manager (sys+0x318). It
  keeps four entity sets in `TimelineActorVisibilitySwapSingletonComponent` (query 0x60BE41C): dummies shown in view
  0 / 1 (offsets +0x00 / +0x40), real actors hidden in view 0 / 1 (+0x80 / +0xB0), returns early when the state is
  already right, toggles an effect attachment on component type 0x6052E3C (0x1C66780 detach / 0x1C66390 attach,
  through EffectsManager), and queues `{entity, player, show}` for 0x1D6A6C0. Other callers (scene transitions):
  0x1D59830, 0x1D46F50, 0x1D64EA0, 0x1D653B0, 0x1D65EE0, 0x1D67430, 0x1D6C1E0.
- The hiding is per player view (fade/visibility), not a teleport: nothing in these paths moves the represented
  entity. Its TransformComponent is only written by 0x1F96210 if it is itself a timeline actor
  (ClientTimelineActorControl).

Consequence: during such a scene `_C().Visual.Visual` (the controlled entity's visual) is the hidden real body, still
at its old place, while the visible body is the dummy elsewhere. The Lua change made by the main session (re-send
`skeleton=` when `_C().Visual.Visual` changes) does not catch this, because the controlled entity's visual does not
change (Confirmed by the code above: the dummy is a different entity).

Unconfirmed: which scenes create a dummy for a **player** character (data-driven; SE `IsPlayer` suggests they
exist), and whether the hidden real body keeps animating.

### 2.2 Shapeshift and look changes inside timelines

- `ecl::TimelineShapeShiftComponent` is only queried together with TLPreviewDummy and TimelineActorData (query
  0x60BE4A8, user 0x1D4D7A0): a timeline shapeshift is applied to a dummy, not to the real character. Confirmed
  (query spec); what 0x1D4D7A0 does: not read.
- `ecl::TimelinePlaceholderDummyComponent` (id 0x6053324) is added by 0x1D70A90 and read in the leave path 0x1D5BE40.
  Meaning not traced (lead).
- `ecl::dummy::*` (DummyComponent, HasDummy, OriginalTransform...) belong to **photo mode** doubles: their system
  0x1A04530 works with `eoc::photo_mode::DummyComponent` / `ecl::dummy::DummiesCreatedSingletonComponent`
  (u9). Not a timeline mechanism. Confirmed (query names).

### 2.3 Visual replaced on the same entity

The engine has a visual-change pipeline (`ls::VisualChangeRequestOneFrameComponent`, `ls::VisualChangedEventOneFrameComponent`,
ls::VisualChangeRequestSystem body 0x3D75A30, which swaps visual-resource load handles). A visual change gives the
entity a new ls::Visual in its VisualComponent; dozens of systems react to `VisualChangedEventOneFrame`. Whether the
old ls::Visual is destroyed immediately or deferred: **Unconfirmed** (destructor is virtual, writer of
VisualComponent.Visual not found). The robust answer is not to keep a Visual pointer across frames (section 6).

---------------------------------------------------------------------------------------------------

## 3. Attachments, parenting, local vs world

- `Visual+0x08` is the world transform for every visual. Writers: SetWorldTransform 0x3C65100 (full copy to
  +0x08..+0x2F), SetWorldTranslate 0x3C64C30 (+0x18..+0x23). Confirmed (u7, u13).
- `Visual+0x30` (SE `LocalTransform*`): allocated on first SetLocalTransform 0x3C6E550 (0x40 bytes: Transform + local
  bound). 0x3C6F860 uses it only to turn the local bound into the base/world bounds (+0x40 / +0x58). It does not make
  +0x08 relative to anything. Confirmed (u13).
- Attachment children (weapons, armour, effects; K 1.1) get a **world** transform computed by 0x3C68F40 from the
  parent's +0x08 (and the parent's +0x30 when the attachment flag 0x10 is set) times the parent bone; written either to
  the child's entity transform (0x3C78110) or the child's SetWorldTransform. Confirmed (K, u14 caller list).
- Timeline parenting (vehicle, chair, other actor, bone of another actor): done in 0x1F96210 through
  `TimelineTransform+0x68` (parent EntityRef) and the bone-attach path, but the result is written as the actor's world
  transform (1.1). Confirmed.
- So no case found where the character's root visual +0x08 is local. Keep reading it as world. Unconfirmed only for
  the root visual being itself an attachment child of another visual (no timeline code found that does that; if it
  happens, 0x3C68F40 still writes world values).

---------------------------------------------------------------------------------------------------

## 4. Resolving the controlled character's visual natively, every frame

### 4.1 Chain (all Confirmed, all plain Microsoft x64 calls)

```
mgr    = *(void**)(exe + 0x60487A0)                     // player manager (B)
player = 0x3BAADF0(mgr)                                 // u16, player of input slot 0 (0 if none)
0x17F4FF0(mgr, &h, player)                              // node->+0x10 character handle (map +0x134 count, +0x138 buckets)
if (h == *(u64*)(exe + 0x5EEB6E0)) -> no character     // the game's null-handle constant
world  = *(void**)(exe + 0x6007F68)                     // or ctrl+0x320 in the camera hook
target = followDummy ? dummyHandle : h                  // section 6
vc     = 0x1830210(world, target)                       // ls::VisualComponent* or 0
visual = vc ? *(void**)vc : 0                           // ls::Visual*
tf     = 0xE59570(world, target)                        // ls::TransformComponent* (read-only)
```

Handles: 64-bit; the game treats `(h >> 54) > 0x40` or `h == 0xFFC0000000000000` as invalid (same test in every
getter). All getters return 0 for an invalid handle or missing component.

Threading: everything above is game-thread data. The getters walk ECS storage without locks; call them only on the
game thread (the 0x4085150 calls from the gameplay CameraSystem, 0x1E11480 / 0x1E13270, are). 0x4085150 is also
called from render-side code for the sun-shadow cascade controllers (0x3E01E00, 0x3E02440, 0x3E1B0C0, 0x3E1C940;
A/T reports) and other places (u20); filter on the gameplay controller as the mod already does.

### 4.2 Is Visual+0x08 current at camera time?

- The visual is updated synchronously with its entity (1.2), so it is as current as the last timeline/movement write.
- Whether TimelinePostSystem runs before or after ecl::CameraSystem in the same frame: **Unconfirmed** (ECS
  scheduler order not traced). Same for the animation finish job (C 1.3, worker threads) relative to the camera
  update; the mod's existing slot-30 counters already measure that for the pose.
- The latest guaranteed-final point is the next frame's update (one frame of latency at most) or a post-hook on
  0x3BF9900 for the followed entity, which fires exactly when the world placement is written. Logging below decides.

---------------------------------------------------------------------------------------------------

## 5. Other reasons the camera can detach in cutscenes

1. **Body swap to a dummy** (section 2.1): the main finding; fits "bodies moved, camera stayed".
2. **Stale Visual pointer**: if the controlled entity's visual is replaced (2.3) and the mod keeps the old pointer,
   the vtable check alone may still pass if the memory was reused by another Visual. Check `visual+0x108` against the
   followed entity every frame. Confirmed offsets; reuse behaviour Unconfirmed.
3. **Dummy animation gating**: unflagged active dummies of TimelineType 1/8 only animate while 0x4126300 accepts their
   world bound for some player view's camera (0x1F1A1E0; frustum meaning Unconfirmed). Following a dummy from its eyes should keep it in
   view; if the mod's camera frustum test failed (camera inside the bound) the dummy's pose would freeze. Unconfirmed;
   the slot-30 counters for the dummy's skeleton show it.
4. **Hidden real body**: after a swap the real body is hidden per view; its animation may continue or stop (not
   traced). A frozen head on the real body is another sign of a swap.
5. **Skeleton slot change** (Visual+0x88): already handled by the per-frame re-resolve in skeleton.cpp. Dummies have
   their own skeletons; switching to a dummy means re-reading bone indices (Head_M, Dummy_EyeFX_01/02) for that
   skeleton.
6. **First-person head hiding**: the mod's head/cull flags are on the real visual. When following a dummy, the dummy's
   head must be hidden instead (C 2.2 slot 14 0x3C6B6F0), or the camera sits inside the dummy's head mesh.

---------------------------------------------------------------------------------------------------

## 6. Recommended design

Goal: every frame, eyes = the bones of the body the player actually sees for the controlled character.

1. **Followed entity, not a cached Visual.** Keep `g_followEntity` (EntityHandle). Default = the controlled
   character from 4.1 (re-read every frame; it changes when the player switches character).
2. **Swap tracking** - hook 0x1D5A350 (entry, game thread):
   - read `world = *(void**)rdx`, `h = *(u64*)r8`, `show = byte [rsp+0x28]`, `player = word [rsp+0x30]`;
   - only handle `player == primary player` (or 0, the recursion will call again per player);
   - `d = 0x13FA710(world, h)`;
   - `d && show && *(u64*)(d+0x88) == controlled` -> `g_followEntity = h` (dummy), remember `g_dummyOf = controlled`;
   - `d && !show && h == g_followEntity` -> back to controlled;
   - `!d && show && h == controlled` -> back to controlled.
   Then call the original.
3. **Every camera frame** (the gameplay-controller call of the 0x4085150 hook):
   - if following a dummy, verify `0x13FA710(world, g_followEntity)` still non-null and its +0x88 still the
     controlled handle, else fall back to controlled;
   - `visual = *(0x1830210(world, g_followEntity))`; check vtable 0x588CB30 and `visual+0x108 == g_followEntity`;
   - if `visual` differs from last frame -> re-run `find_skeleton` and the bone lookup (Head_M, Dummy_EyeFX_01/02),
     move the first-person head hiding from the old visual to the new one, and re-attach the slot-30 IK watch;
   - eyes as today: `VisualWorld(+0x08) x boneModel`.
   - If there is no visual (loading, invisible), use `0xE59570(world, g_followEntity)` + the last known eye offset.
4. **Lua** stays as a fallback and for diagnostics; the native path no longer depends on `skeleton=` / `follow=`
   pointers (the TransformComponent pointer from Lua is the one that goes stale when components move; 0xE59570 returns
   the current one).
5. **Timeline camera**: keep G's +0x4C5 approach so the view keeps the gameplay controller; this design only changes
   which body the gameplay camera sits in.

Confidence: chain and hook semantics Confirmed; the +0x88 = "the character this dummy plays" reading and the
frame-order questions are Unconfirmed until the run below.

---------------------------------------------------------------------------------------------------

## 7. One-run logging plan (everything Unconfirmed above)

Run: start VR, walk, then trigger (a) an ordinary NPC dialogue, (b) the cutscene where the camera stayed behind,
(c) a camp/long-rest scene, (d) one more cinematic. Log to the mod's log, one line per event, frame number
`[exe+0x614FB88]` on every line.

1. **0x1D5A350 entry** (every call): r8 handle, show, player, Timeline* r9, `Timeline+0x488/+0x489/+0x4C5`,
   `d = 0x13FA710(world,h)`, `d+0x88`, `d+0xC0`, controlled handle, and whether `d+0x88 == controlled`. Decides if
   the failing scene swaps the player.
2. **0x1D5B890 / 0x1D5BE40 entry**: actor handle, TimelineData +0x78 type, whether the actor is a TLPreviewDummy.
3. **0x1F96210 entry**: actor handle `[r8+0x50]`; mark when it is the controlled entity or the followed dummy (shows
   which bodies the timeline moves).
4. **0x3BF9900 post-hook**: when rdx->entity is the followed entity: frame, thread id, Visual+0x18 after the call.
   Compare with the 0x4085150 read in the same frame (before/after) -> answers 4.2.
5. **Per camera frame** (rate-limited to changes + once per second): controlled handle, followed handle, VisualComponent
   visual, Lua-provided visual, `visual+0x108`, `visual+0x110`, Visual+0x18 vs TransformComponent+0x10 (difference in
   metres), skeleton pose stamp `+0x124` vs frame, eyes world.
6. **Visual change**: when the visual pointer for the same entity changes, log the old pointer's vtable and +0x108 for
   the next 10 frames (freed / reused / still alive) -> answers 2.3.
7. **Dummy pose freshness**: slot-30 call count for the dummy's skeleton per second while following it -> answers 5.3.
8. **Real body while hidden**: once per second while following a dummy, the real body's Head_M model position and its
   skeleton `+0x124` -> answers 5.4.

---------------------------------------------------------------------------------------------------

## Open leads

1. `TLPreviewDummy+0x88` writer (dummy creation; SE `DeferredDummyCreation` in TimelineSystem) - confirms the
   meaning statically.
2. `ecl::TimelinePlaceholderDummyComponent` (0x1D70A90 adds, 0x1D5BE40 reads).
3. ECS order of TimelinePostSystem vs CameraSystem vs AnimationUpdate.
4. Writer of `VisualComponent.Visual` and the old Visual's lifetime.
5. 0x1D6A6C0: how the per-view hide is rendered (fade components), and whether a hidden body still animates.

## Raw files

u1_tlsys.c (0x1F22770, 0x1F1A1E0, 0x1D3A4D0, 0x1F208D0, 0x1D79B10, 0x1F9E9F0, 0x1F9DC70, 0x3C64B20), u2/u3/u5/u17
(type-id and query references), u4_dummy.c (0x13FA710 ...), u6_swap.c (0x1D5A350, 0x1D6A6C0, ...), u7_xform.c
(0x3C78110, 0x3C77BD0, 0x3C65100, 0x17F4FF0, 0x17FAB50, 0x1C66780, 0x1C66390, 0xE59570), u8/u10 (system bodies),
u9_vissys.c, u11_vischange.c, u12_visvt.txt (Visual vtable), u13_visset.c (Visual setters), u14 (setter callers),
u16 (VisualComponent type-id inits), u18_visget.c (0x1830210), u19_ecs.c (0x400C8A0, 0x4006700, 0x3CF0740, 0x4085150),
u20 (callers of 0x4085150 / 0x1830210), u21 (callers of 0x1D5A350), u22/u23/u24/u25 (transform->visual listener),
u26_player.c (0x3BAADF0, 0x3BAACE0, 0x17F5050).
