# Agent Q: ragdolls, corpse grabbing and active ragdoll (bg3_dx11.exe 4.1.1.7631656)

Static RE only (Ghidra project `BG3_G`, headless, read-only; request server `agents/Q/QServer.java`).
Image base 0x140000000; every address is an RVA. Raw decompiles and dumps are in `re-local/ghidra/agents/Q/`
(`rNN_*.c|txt|asm`, cited as [rNN]). Agent D's decompile `agents/D/dec/phx_region.c` is cited as [D:phx],
`agents/D/dec/ragnodes.c` as [D:ragnodes], `agents/D/dec/dd4fc0.c` as [D:dd4fc0].
Game data was read with LSLib (Divine + PhysicsTool, already in `downloads/lslib-v1.20.4`, read-only):
`Q/gamedata/*.xml` are PhysicsTool conversions of the shipped PhysX binaries, `Q/hum_m_ragdoll_summary.txt`
is the per-body/per-joint table produced by `Q/ragsum.py`.
Builds on D_physics.md (§3-§6), C_animation_systems.md (§1.3-§1.6), E_items_interaction.md (§10), K_weapons.md.
Names marked SE come from the Script Extender source (`downloads/bg3se-src`).

Status: complete for the ragdoll data, lifecycle, body↔bone mapping, per-bone actor access, D6/shape APIs, the
spring (physical animation) path and the write-back point. Open: corpse pose across save/load, who sets the
blueprint's ragdoll variables, living-character collision bodies, triangle-mesh getters (see Open leads).

## Hook points for the VR mod

| Need | RVA | How to use | Confidence |
|---|---|---|---|
| Ragdoll manager (all live ragdolls) | main scene `+0x90`, vtable 0x5894878; entries `M+0x40` (0x70 each), count `M+0x4C`, lock `M+0x18` | walk entries, match `entry+0x10` (EntityHandle) or `entry+0` (container); layout §2.3 | Confirmed |
| Per-bone PhysX bodies of a corpse | container `+0x28` wrappers (count `+0x34`), `wrapper+0x40` = PxRigidDynamic; template `entry+8`: `vf[8](cat)` count, `vf[0x10](cat,i)` = bone name | body i ↔ bone by name (Root_M … Head_M); wrapper `nBodies` = Dummy_Root (tether) | Confirmed |
| Ragdoll joints | container `+0x80` (count `+0x8C`), PxD6Joint vtable 0x5463670 | names `<Parent>_To_<Child>_Constraint`; setMotion +0xE8, setDrive +0x160, setDrivePosition +0x170 | Confirmed |
| Wake a frozen corpse | wrapper +0x100 MakeDynamic, +0x68 WakeUp; `entry+0x58` hard limit, `+0x60` elapsed, `+0x6C` bit0 frozen | on grab; keep elapsed < limit while held | Confirmed (code); effect to verify in game |
| Drive one bone to the hand | `PxD6JointCreate` 0x4CFF340 (hand kinematic actor ↔ bone), or wrapper +0x120/+0x130 velocities, or +0xF8 + +0xB8 kinematic | §4 options A/B/C | Confirmed (API); tuning by test |
| Tether (ragdoll stays ≤0.2 m / 1 m from entity) | joint `Dummy_Root_To_Root_M_Constraint`; update 0x3A8CAA0 re-anchors Dummy_Root and shifts sleeping bodies | free linear axes for long drags; re-anchor before committing a new entity transform (§6.2 step 5) | Confirmed |
| Raycast that can hit corpses | RaycastClosest (D §8.2), prefilter 0x3A8A890 | ragdoll group 0x4004: exclude mask must not contain 0x4 (the 0xC used now drops corpses) | Confirmed |
| Commit a moved corpse | server `TeleportToPosition` character branch 0x35F09A0 (E §10) | after the bodies sleep, at the Root_M body position | Confirmed (path, E); not tested |
| Ragdoll creation (to watch / veto / call) | AddRagdoll 0x3A8AF90 (mgr vf+0x08), descriptor §2.1, builder 0x3DD4FC0, node 0x3DCD200 | hook to log/disable per entity (desc+0x78); call with own descriptor for an active ragdoll | Confirmed |
| Ragdoll pose → skeleton | GetRagdollPose 0x3A8B880 → RagdollPose node 0x3A4E0B0 → graph → `[skel+0xE8]` → slot 30 | nothing to do for corpses; for own ragdolls write bones in slot 30 | Confirmed |
| Active-ragdoll write-back | Skeleton slot 30 0x3A20590 (vtable 0x588D4A8+0xF0), already hooked by bodyik.cpp | read targets from `[skel+0xE0]`, write simulated bones, re-derive children, call original | Confirmed |
| Pre-physics hook (set drive targets / kinematic hands) | `phx::PhysXSceneUpdateSystem` update 0x75BA10 (game thread; then ragdoll update 0x3A8CAA0 and job 0x3A74AC0) | set `setDrivePosition` / kinematic targets here, once per game frame | Confirmed (D §3.2) |
| Game's physical animation (springs) | `skeleton+0x30` PhysicsSkinnedObject (vtable 0x58944E8); write-back 0x3DD9AC0/0x3DD6310 (slot 26) then slot 30 | pattern to copy; slot 30 still runs last | Confirmed |
| Body shapes for an external sim | NpShape vtable 0x5430F08 (+0x38 type, +0x60 capsule, +0x98 local pose), `*_Ragdoll.bin` data | read at runtime from the template or a live ragdoll | Confirmed |

---

## 1. What a BG3 ragdoll is (data)

### 1.1 Files and resource chain (Confirmed)
* Skeleton resource fields (visitor 0x3F35F20 [r15]): `+0x78 RagdollResourceID`, `+0x7C SpringResourceID`,
  `+0x80 DynamicPhysicsResourceID`, `+0x84 SoftbodyResourceID`(SE name), more FixedStrings to +0x94. FixedString
  globals: RagdollResourceID 0x5FD1CE8, SpringResourceID 0x5FD1CF0, DynamicPhysicsResourceID 0x5FCF778
  (initialisers 0x550540 / 0x550610 / 0x53C410, read only by 0x3F35F20 [r13]).
* The runtime Skeleton copies them (SE layout `Skeleton`: +0x08 RagdollResourceID, +0x10 SpringResourceID,
  +0x14 DynamicPhysicsResourceID, +0x18 SoftbodyResourceID, **+0x30 `phx::PhysicsSkinnedObject* Physics`**).
  Confirmed uses: the AddRagdoll node passes `skeleton+8` as the ragdoll resource id to the scene
  (`plVar15->vf[0x118](plVar15, param_1[2] + 8, scale, entity)` in 0x3DCD200 [D:ragnodes]); the spring
  creator reads `skeleton+0x10` (0x3A62D00 `piVar11 = lVar12 + 0x10` [r16]); 0x3A848B0 writes
  `*(skeleton + 0x30) = this` [r20].
* The physics resources are **PhysX 4.1 binary serialized collections** (magic `SEBD`, platform `W_64`; e.g.
  `Public/Shared/Assets/Characters/_Anims/Humans/_Male/HUM_M_Ragdoll.bin`, 35 536 bytes, Models.pak).
  116 `*_Ragdoll.bin` files ship (one per body type and per creature, plus hair/beard "Spring_Ragdoll" files),
  139 `*_Spring.bin`, `*_Softbody.bin` per body type, `*_Rig_Death.bin` etc. (`K/models_pak_list.txt`).

### 1.2 HUM_M_Ragdoll content (Confirmed from the shipped file, `Q/gamedata/HUM_M_Ragdoll.xml`)
21 `PxRigidDynamic`, one capsule (or box) shape each, 20 `PxD6Joint`s, no articulations:
| body | kinematic | mass | shape (radius, halfHeight) |
|---|---|---|---|
| Dummy_Root | **yes** | 5 | box 1 cm |
| Root_M | no | 10 | capsule 0.123, 0.044 |
| Hip_L/R | no | 8 | 0.080, 0.157 |
| Knee_L/R | no | 5 | 0.063, 0.181 |
| Ankle_L/R | no | 2 | 0.050, 0.060 |
| Spine1_M / Spine2_M / Chest_M | no | 5 / 5 / 10 | 0.084,0.08 / 0.108,0.05 / 0.121,0.05 |
| Scapula_L/R, Shoulder_L/R, Elbow_L/R, Wrist_L/R | no | 5, 6, 5, 2 | 0.064,0.016 / 0.066,0.089 / 0.045,0.117 / 0.05,0.06 |
| Neck_M / Head_M | no | 2 / 8 | 0.047,0.011 / 0.093,0.035 |
All bodies: linear/angular damping 0.5, max linear velocity 10, max angular velocity 5, sleep threshold 0.5,
solver iterations 20/20 (Dummy_Root 20/10), material friction 0.5/0.5 restitution 0, contact offset 0.02,
inertia (1,1,1) (mass-space). Global poses are the bind pose in model space (Y up, metres).

Joints are named `<Parent>_To_<Child>_Constraint`; actor0 = parent, actor1 = child. Motions (X/Y/Z/Twist/Swing1/Swing2):
* `Dummy_Root_To_Root_M`: linear **Limited** (X −1..0.5 m, Y/Z ±0.2 m), all angular **Free** — the ragdoll's
  tether to the entity (see 2.4).
* Knees and elbows: everything Locked except Swing2 (pyramid Z 0..160°): hinges.
* Hips: twist ±45°, pyramid Y −45..90°, Z −90..60°. Shoulders (Scapula→Shoulder): twist ±90°, pyramid ±80°.
  Spine: twist ±10°, pyramid Y ±20°, Z −5..45°. Wrists: twist ±45°, Y −10..90°, Z ±45°. Ankles/neck/head: ±10..45°.
* Every joint carries D6 drives X/Y/Z/Swing/Twist/Slerp with stiffness 0.5, damping 0.5, acceleration mode,
  unbounded force (drive position = identity, i.e. the bind pose). That is a negligible pull toward the bind pose,
  but it shows the drive machinery is present and copied at runtime (2.3).
Full table: `Q/hum_m_ragdoll_summary.txt`.

### 1.3 Other physics templates on a character (Confirmed from files)
* `HUM_M_Softbody.bin`: 12 **kinematic** capsules on Root_M/Hips/Spine/Chest/Scapula/Shoulder/Neck/Head (the
  SoftbodyResourceID). Body collision proxies for cloth/soft bodies.
* `*_Spring.bin` (SpringResourceID): partial ragdolls on living characters. Example `LVT_Leg_Spring.bin`: upper body
  kinematic, legs dynamic, D6 twist limits with stiffness 500-1250 (soft limits = springs). Hair/beard springs the same.
* `HUM_M_Rig_Death.bin`: one kinematic box `SKEL_HUM_M_Rig_Death_Phys` (purpose not traced).

---

## 2. Ragdoll lifecycle in code (deeper than D §6)

### 2.1 Creation on death (Confirmed)
Chain (all client, animation-graph driven): death blueprint contains `PhysicsNode_AddRagdoll` → node runtime
0x3DCD200 [D:ragnodes] (runs on the animation job thread) → scene slot +0x118 0x3A62960 creates the 0x80-byte
handle `{+0 mgr, +8 ragdollResourceFS (from skeleton+8), +0xC scale, +0x10 short id, +0x18 entity handle,
+0x28 body pose array, +0x38 bone→body byte map, +0x40 count}` → 0x3DD4FC0 builds the descriptor → AddRagdoll
0x3A8AF90 → graph event `PhysicsEvent_RagdollAdded`.

**Node inputs** (sockets, read from graph variables in 0x3DCD200 at node+0x18..+0x32; socket names registered
by 0x3DCC420 in this order [r5]; the same order is used by the serializer 0x3F3BE90 [r5]):
`AngularVelocityModifier`, `LinearVelocityModifier`, `StiffnessMultiplier`, `DampingMultiplier`,
`DurationHardLimit`, `DurationSoftLimit`, `HitImpulseThreshold`, `HoldLastPose` (asks the node, vf+0x38, for a pose
array: pose-lock mode), `AnimationResourceID`, `DIE_ImmediateTriggerRagdoll`, …, `VerticalRaycastLength` and the
`LockLimits` bool (socket names confirmed from the shipped blueprint, §5.1). They become a float[7]+byte block passed to 0x3DD3440.

**Descriptor** (0x3DD4FC0 defaults [D:dd4fc0], overwritten by 0x3DD3440 [r1]):
| desc off | meaning | default | from |
|---|---|---|---|
| +0x00 | physics template (resMgr FindTemplate 0x3B24690 of the ragdoll resource) | | |
| +0x08 | entity world Transform (quat, pos, scale 1) | | node |
| +0x30 | root transform | | node |
| +0x58 | uniform scale (handle+0xC) | | |
| +0x5C | DurationHardLimit → entry+0x58 (freeze time) | 8.0 s | params[4] |
| +0x60 | DurationSoftLimit → entry+0x5C | 5.0 s | params[5] |
| +0x68 / +0x6C | StiffnessMultiplier / DampingMultiplier (pose-lock limits) | 1 / 1 | params[2]/[3] |
| +0x70 | bool `LockLimits` (rigid mode): lock Twist/Swing1/Swing2 of every non-tether D6 | 0 | params[7] |
| +0x78 | entity handle (→ container+0x20 and entry+0x10) | handle+0x18 | |
| +0x88 / +0x98 | per-body linear / angular velocity arrays (vec3) | | animation |
| +0xA8 | per-body pose array (0x28 Transform: quat, pos, scale) | | animation |
| +0xB8 | optional pose array for pose-lock mode (`HoldLastPose`) | null | node vf+0x38 |
The node's HitImpulseThreshold (params[6]) is written to a local of 0x3DD4FC0 (`ctx+0x6C`) and never read
afterwards: unused on this path (Confirmed by the decompile of 0x3DD4FC0/0x3DD3440).

**Initial velocities** (0x3DD3440 [r1]): per body, linear = (current − previous animated bone position) ×
LinearVelocityModifier / dt; angular = Euler delta × AngularVelocityModifier / dt, from the skeleton's current
(+0xE0, slot 11) and previous (+0xE8, slot 16) world poses. The ragdoll inherits the death animation's motion.

### 2.2 Body ↔ bone mapping (Confirmed)
0x3DD4F00 → 0x3DD47E0 [r1, r2]: for category 2 (rigid dynamic) then category 8 (articulation link), for each
template actor `i`: `name = template->vf[0x10](template, category, i)` (actor name), `skeleton->vf[0x88](FindBone, name)`;
on success `boneToBody[bone] = i` (byte map, handle+0x38), `bodyToBone[i] = bone`, and the bone's rest
transform (slot 14, from +0xF8) is inverted and stored as the body→bone offset. Bones without a body inherit
their parent's body (0x3DD4F00 loop over `skeleton->vf[0x98]` parent).
**So every ragdoll body is named exactly like the skeleton bone it drives** (HUM_M: Root_M, Hip_L, ... Head_M).
Body index order = category-2 actors in template order, then category-8 links. `Dummy_Root` (kinematic, category 4)
has no bone; it is the tether (2.4).

### 2.3 AddRagdoll internals 0x3A8AF90 [D:phx 41073]
1. Allocates the ragdoll container (0x98 bytes, vtable 0x58948A8): +0x08/+0x14 template list, +0x20 entity,
   +0x28/+0x34 wrapper array, +0x70/+0x7C articulations, **+0x80/+0x8C joint array**, +0x90 "picker" wrapper.
2. Body builder 0x3A8E320 [D:phx 42707]: per category-2 actor: instantiate (`template->vf[0x40](ctx, 2, i)`),
   wrapper (0x48 bytes, vtable **0x5893F48**, FixedString name "DummyPicker"), `actor->userData = wrapper`,
   contact-report threshold 0.01, `setGlobalPose(entityXf * scale * pose[i])`, `setLinearVelocity(linVel[i])`,
   `setAngularVelocity(angVel[i])`, eDISABLE_SIMULATION if the global pause byte `[0x604B1B8]+0x124` is set; then
   articulations (`template->vf[0x30]` count, `vf[0x50]` instantiate, links via getLinks); finally
   `container->SetPhysicsGroup(0x4004, 0xE14, 0)` (vf+0x50) and `vf+0x40(entity)`.
3. Kinematic builder 0x3A8FD70 [D:phx 43363] (D called it the joint builder; it is not): category-4 actors
   (Dummy_Root) are created at the entity transform and appended after the bodies. **The first category-4 wrapper
   is the "reference body" the per-frame update keeps on the entity transform.**
4. Joints: `n = template->vf[0x28]()`, `joint_i = template->vf[0x48](ctx, i)` → container+0x80. The template's
   joint instantiation is 0x3A78D10 [r11]: `PxD6JointCreate` (0x4CFF340, allocates `D6Joint` 0x78 bytes with
   ExtD6Joint.cpp) and copies from the serialized joint: constraint flags, distance limit, all 6 motions,
   projection tolerances, linear limits X/Y/Z, **pyramid** swing limit (+0x150; the cone limit +0x140 is not copied),
   twist limit, **all 6 drives (setDrive +0x160)**, drive velocity (+0x180), name. Drive position is not copied
   (identity).
5. Rigid mode (desc+0x70): for every D6 joint (concrete type 0x105) that is not a kinematic↔dynamic joint
   (0x3A8AC20 returns 1 when exactly one actor has eKINEMATIC [r11]), `setMotion(eSWING1/eSWING2/eTWIST, eLOCKED)`
   (disassembly 0x3A8B475..0x3A8B4A5: `xor r8d,r8d` = motion 0 = eLOCKED [r10]). The body becomes one rigid piece.
6. Pose-lock mode (desc+0xB8 non-null) 0x3A8D610 [D:phx 42392]: for every non-tether joint, sets the joint's
   actor1 local frame so that the current animated pose is the joint's rest pose, then twist limit ±5° and pyramid
   swing ±5° (0x3DB2B8C2 = 0.0872665 rad) with the template's limit stiffness/damping multiplied by
   desc+0x68/+0x6C (Stiffness/DampingMultiplier). A "stiff" ragdoll that holds the death pose softly.
7. 0x3A90780: sums body velocities, calls the scene's ragdoll listeners (scene+0x28 list), sets entry flag bit2
   (bodies are added to the PhysX scene by the next manager update, D §6.3), stores the entry (0x70 bytes) under
   the manager lock (mgr+0x18), id from counter 0x5FC3054.

**Manager entry (0x70 bytes, array mgr+0x40, count mgr+0x4C)** — refined from D:
| off | meaning | evidence |
|---|---|---|
| +0x00 | ragdoll container* | all manager functions |
| +0x08 | physics template (resource) | AddRagdoll `local_190 = *param_2`; update counts `entry[1]->vf[8](8)+vf[8](2)` |
| +0x10 | owning entity handle | update `FUN_140e59570(world, entry+0x10)` (entity transform) |
| +0x20 / +0x2C | per-body offset array (0x28 Transform each) / count | body builder appends to `param_2+4`; GetRagdollPose reads `entry+0x20` |
| +0x30..+0x57 | last entity transform (quat, pos, scale) | update writes `plVar16[6..10]` |
| +0x58 | DurationHardLimit (freeze time) | `desc+0x5C`; update `entry+0x58 <= entry+0x60` |
| +0x5C | DurationSoftLimit | `desc+0x60`; no reader found in the manager |
| +0x60 | elapsed time | update `+= dt` unless paused |
| +0x64 | uniform scale | GetRagdollPose divides positions by it |
| +0x68 | short id | |
| +0x6C | flags: bit0 frozen, bit1 remove, bit2 add to scene | |

### 2.4 Per-frame update 0x3A8CAA0 and the tether (Confirmed, extends D §6.3)
* The reference body (index = count(cat 8) + count(cat 2), i.e. Dummy_Root) is teleported to the entity's world
  transform whenever that changes (setGlobalPose under lockWrite), and every *sleeping* body is moved by the same
  delta. Dummy_Root is kinematic; Root_M hangs on it through a D6 joint whose linear axes are Limited to X −1..+0.5 m,
  Y/Z ±0.2 m and angular Free.
* Consequence for dragging: **a ragdoll can never get more than ~1 m from its entity's transform.** To drag a corpse
  around, either move the entity (client transform 0x3C78110, server TeleportToPosition, see §5), or free the tether
  (`joint->setMotion(eX/eY/eZ, eFREE)` on `Dummy_Root_To_Root_M_Constraint`) or move Dummy_Root yourself
  (kinematic target) and keep the entity in sync.
* Freeze: when every body sleeps or `elapsed >= entry+0x58` (DurationHardLimit, default 8 s), every wrapper gets
  `vf[0xF8]` MakeKinematic and bit0 is set. Otherwise the scene listeners get the average linear velocity.

### 2.5 Pose back to the skeleton (Confirmed)
* GetRagdollPose 0x3A8B880 [D:phx 41500]: under the manager lock, for each body `i < count(8)+count(2)`:
  `actor = container->wrappers[i]->actor (+0x40)`, `getGlobalPose` (vf+0x90) under lockRead, converted to entity
  space (inverse of entry+0x30 transform, scale entry+0x64) and multiplied by the per-body offset entry+0x20[i];
  output array of 0x28-byte Transforms in body order.
* Node `GrannyNode_RagdollPose` 0x3A4E0B0 [r1]: runs only if `(animInstance+0x24 & 2)` or ragdoll flag bit0
  (disassembly 0x3A4E109..0x3A4E116), calls GetRagdollFlags + GetRagdollPose into handle+0x28, then for every skeleton
  bone `b`: `body = handle+0x38[b]` (−1 → identity), composes the body transform with the bone's slot-14 transform,
  writes a 4x4 into a temporary Granny world pose, converts it with BuildLocalPoseFromWorldPose (0x4A0EA40) into a
  local pose and outputs it to the graph. The graph then blends it and FinalPose → plugin 0x3A051F0 → `[skel+0xE8]`
  → swap → slot 30 (C §1.3). **So the ragdoll pose reaches the skeleton through the normal animation pipeline; the
  mod's slot-30 hook sees it in `[skel+0xE0]` and can still override bones afterwards.**
* Unconfirmed: what `animInstance+0x24 & 2` means (instance from 0x188E770(world, entity)). Runtime log to confirm:
  in the slot-30 hook for a dying NPC, log `GetRagdollFlags` and whether the bones move while bit0 is clear.

### 2.6 Removal
Graph event `PhysicsEvent_RagdollRemovedInternal` → 0x3DCDD40 → mgr vf+0x18 MarkForRemoval (bit1) → next update
calls vf+0x10 RemoveRagdoll → 0x3A90940 (listeners, container RemoveFromScene, delete) (D §6.1, [D:phx 43703]).

---

## 3. Getting a corpse's per-bone PhysX actors (recipe, Confirmed parts cited above)

1. Main scene `S` (D §8.1 path A: `[[[base+0x6007A28]+0x90]+0x30]`, vtable 0x5891B30). Manager `M = *(S+0x90)`
   (vtable 0x5894878).
2. `EnterCriticalSection(M+0x18)`; walk `entry = *(M+0x40) + k*0x70`, `k < *(uint*)(M+0x4C)`; match
   `*(uint64*)(entry+0x10)` against the corpse's client EntityHandle. (Alternative from a raycast hit: hit wrapper
   +0x38 = container; match `*(entry+0x00) == container`.)
3. `container = *(entry+0)`; `wrappers = *(container+0x28)`, `count = *(uint*)(container+0x34)`;
   `nBodies = T->vf[8](T,2) + T->vf[8](T,8)` with `T = *(entry+8)`; wrapper `i < nBodies` is the body named
   `T->vf[0x10](T, cat, i)` (= bone name); wrapper `nBodies` is Dummy_Root. `actor = *(wrapper+0x40)`
   (PxRigidDynamic, concrete type 5).
4. Joints: `*(container+0x80)`, count `*(int*)(container+0x8C)`; each is a PxD6Joint (`*(short*)(j+8) == 0x105`),
   `j->vf[0x30](j, &a0, &a1)` getActors, `vf[0xD8]` getName (name `<Parent>_To_<Child>_Constraint`).
5. Leave the critical section. Wrapper methods take the PhysX lock themselves (D §2.2).
Ragdoll body wrappers use vtable 0x5893F48, whose slots +0x08..+0x148 are the same functions as D's wrapper
0x58940D0 (dump [r12]): +0x60 StopMotion, +0x68 WakeUp, +0xB8 SetWorldTransform, +0xF8 MakeKinematic,
+0x100 MakeDynamic, +0x120/+0x130 Set linear/angular velocity. Only +0x90/+0x98 and +0x150.. differ.

---

## 4. Driving one bone of a corpse (PhysX API in this build, Confirmed)

`PxD6Joint` concrete vtable **0x5463670** (D6Joint; base JointT vtable 0x5463460 has purecalls at the D6 slots)
[r8]; slots verified by decompile [r9]:
| slot | function | RVA | evidence |
|---|---|---|---|
| +0x30 | getActors(PxRigidActor*&, PxRigidActor*&) | 0x4D06180 | used by 0x3A8AC20 / 0x3A8D610 |
| +0x38 / +0x40 | setLocalPose(actor, pose) / getLocalPose | 0x4D08BD0 / 0x4D068F0 | used by 0x3A8D610 |
| +0xE8 / +0xF0 | setMotion(axis, motion) / getMotion | 0x4CFF9E0 / 0x4CFFA20 | writes `data+0x50+axis*4`, marks dirty |
| +0x120 | setLinearLimit(axis, pair) | 0x4CFFE50 | used by 0x3A78D10 |
| +0x130 / +0x138 | setTwistLimit / getTwistLimit | 0x4D00020 / 0x4D00070 | copies 0x1C bytes to data+0xD4 |
| +0x140 | setSwingLimit (cone) | 0x4D000C0 | copies to data+0xF0, sets data+0x1DE |
| +0x150 / +0x158 | setPyramidSwingLimit / get | 0x4D00170 / 0x4D001D0 | copies 0x24 bytes to data+0x10C, sets data+0x1DF |
| +0x160 / +0x168 | **setDrive(index, PxD6JointDrive)** / getDrive | 0x4D00230 / 0x4D00280 | data+0x130+index*0x10 |
| +0x170 / +0x178 | **setDrivePosition(PxTransform, autowake)** / get | 0x4D002B0 / 0x4D00390 | normalises quat, data+0x190; wakes actors if autowake |
| +0x180 / +0x188 | setDriveVelocity(lin, ang, autowake) / get | 0x4D003E0 / 0x4D00460 | data+0x1AC |
`PxD6JointCreate(physics, a0, frame0, a1, frame1)` = 0x4CFF340 (allocates 0x78 bytes, ExtD6Joint.cpp string).
`PxGetPhysics()` = 0x4A9BBD0 (D). PhysX enums (4.1): axis eX0 eY1 eZ2 eTWIST3 eSWING1 4 eSWING2 5; motion
eLOCKED0 eLIMITED1 eFREE2; drive eX0 eY1 eZ2 eSWING3 eTWIST4 eSLERP5; drive struct {stiffness, damping, forceLimit,
flags(1=acceleration)}. PxRigidDynamic slots: D §2.3 (addForce +0x170, setKinematicTarget +0x1E8, ...).

Hand-to-bone options (all on the game thread; raw PhysX calls must bracket with `scene->lockWrite/unlockWrite`):
* **A. Kinematic hand body + D6 joint (recommended, HIGGS/Bonelab feel):** create one kinematic PxRigidDynamic per
  hand (`physics->createRigidDynamic` vf+0xA8, `setRigidBodyFlag(eKINEMATIC)`, no shapes or a small sphere in a
  non-colliding group), add it to the main scene, and each physics step `setKinematicTarget(handPose)`. On grip:
  `PxD6JointCreate(physics, hand, gripFrame, boneActor, boneLocalFrame)`, motions Locked (rigid grip) or linear
  Locked + angular Free (ball grip) — the rest of the corpse follows through the ragdoll joints. Release: `joint->release()`
  (PxBase vf+0x00) and set the body's velocity from the hand's.
* **B. Velocity drive:** keep the grabbed body dynamic and each frame set its linear/angular velocity toward the hand
  target with wrapper +0x120/+0x130 (D §10). Simple, but the 10 m/s max linear velocity of the template caps it
  (`setMaxLinearVelocity` +0x160 to lift it).
* **C. Kinematic bone:** wrapper +0xF8 MakeKinematic on the grabbed body and SetWorldTransform(+0xB8, pose, false).
  Rigid and simplest; the arm stretches the joint chain if pulled hard.
Before any of them: if entry flag bit0 (frozen) is set, call wrapper +0x100 MakeDynamic and +0x68 WakeUp on every
body wrapper; keep `entry+0x60` (elapsed) below `entry+0x58` (or raise +0x58) while held, otherwise the update
freezes it again (0x3A8CAA0). Free the tether (§2.4) if the body must travel more than about 1 m, and move the entity.

---

## 5. Spring physics: the game's own "physical animation" (Confirmed)

BG3 already runs PhysX bodies that are driven by the animated skeleton and written back into it every frame on
living characters (hair, braids, beards, tails, some creature legs):
1. Creation: main scene slot +0x140 0x3A62D00 [r16] takes a Visual, finds its default skeleton, reads
   `skeleton+0x10` (SpringResourceID) and instantiates it with factory kind 2 (container 0x3A60200, size 0x1F8,
   vtable **0x58944E8**, base `phx::PhysicsSkinnedObject` vtable 0x58C2CF0, SE name). `obj+0xF8` = skeleton,
   `obj+0x88` = visual; queued on the scene's skinned-object lists (+0xC0/+0xD0 → +0xE0).
2. Attach: obj vf+0xA0 0x3A848B0 [r20] sets **`skeleton+0x30 = obj`**, caches each body's max velocities.
3. Pre-step (physics system `phx::PhysXScenePreupdateSkinnedObjectsSystem`, registration 0x733440 → callback
   0x75BAE0 → 0x3A61950 [r14, r15]): for each skinned object vf+0xA8 0x3A84A20(dt): scales body max
   velocities by how fast the visual moved, then 0x3DD6000 (wake/sleep and kinematic driving).
4. Blueprint execute 0x3BD5E30 → 0x3F12AA0 [r29]: per skeleton with Physics: picks an update mode from options
   (`[0x604AA10]+0x3E`), vf+0xB8(flags) pushes the animated pose to the kinematic bodies.
5. Animation finish 0x3BD6670: the swap/skinning pair is **skipped** for skeletons with `+0x30 != 0` (C §1.3);
   instead 0x3F12D60 [r29] calls skeleton slot 29 (swap), then 0x3DD9AC0: under the object's lock, 0x3DD6310
   writes the simulated body poses into the skeleton's world pose with **skeleton slot 26 (+0xD0, set world matrix
   from Transform)** [r27], then 0x3DD8730, then **skeleton slot 30 (skinning)**, then attachments (0x3C68F40) and
   0x3C6B900.
So slot 30 still runs after the physics write-back: the mod's slot-30 hook (bodyik.cpp) sees and can override the
spring-driven bones. This is the exact pattern an active ragdoll needs (animated pose in → physics → bones out →
skinning), and the place where Larian does it.

### 5.1 Ragdolls on living characters in the shipped blueprints (Confirmed from data)
`Public/Shared/Assets/Blueprints/Characters_Common.lsbs` (Assets.pak, LSF; converted to `Q/gamedata/bp/cs.lsx`)
contains 2 `PhysicsNode_AddRagdoll`, 2 `PhysicsNode_RemoveRagdoll`, 6 `GrannyNode_RagdollPose`, 4 `SetSpringRestPoseNode`,
3 `SetSpringBlendNode`, and states/events `KnockedDownRagdoll_OnEnterBegin/Finish/OnExitBegin`,
`IncapacitatedRagdollEnterBegin`, `Thrown_RagdollEntered`, `RagdollEntering/RagdollEntered`, `TriggerRagdoll`,
`RaycastRagdoll`, variables `STAT_ToggleRagdollOn`, `DIE_StartRagdoll`, `DIE_bIsRagdoll`, `DIE_ImmediateTriggerRagdoll`,
`RagdollCached_KnockDown`, `RagdollCached_Incapacitated`, `EnableHorizontalRaycastRagdolls`.
The compiled AddRagdoll node (ID 2378 in `cc.lsx`) wires its input sockets to the blueprint variables
260 AngularVelocityModifier, 65 LinearVelocityModifier, 361 StiffnessMultiplier, 342 DampingMultiplier,
312 DurationHardLimit, 284 DurationSoftLimit, 61 HitImpulseThreshold, 625 **HoldLastPose**, 38 AnimationResourceID,
77 DIE_ImmediateTriggerRagdoll, …, 151 VerticalRaycastLength (default 10). This confirms the socket order of §2.1
and names the two bools: `HoldLastPose` (pose-lock mode, 0x3A8D610) and `LockLimits` (rigid mode; the serializer
0x3F3BE90 names its fields AngularVelocityModifier … HitImpulseThreshold, `HoldLastFrame`, `AnimationResourceID`,
`LockLimits` [r37]). **So the game already ragdolls living characters (knocked down / prone, incapacitated, thrown)
and returns them to animation with RemoveRagdoll** — the precedent for re-ragdolling and for an active ragdoll.
What sets these graph variables (status code vs. animation text keys) was not traced (no exe string references them;
they are data). `TriggerRagdoll` / `RaycastRagdoll` are graph event ids.

---

## 6. Phase 1: grabbing and throwing corpses with the game's ragdoll

### 6.1 Finding the corpse under the hand
* Ray/overlap: D §8 RaycastClosest. The prefilter is 0x3A8A890 (vtable at `[0x5EF2050]` = 0x5894810, slot 0;
  the "any" variant 0x3A8A830) [r40, D:phx 40709]: a shape passes when `(word0 == −1 || word0 != container+0x68)`,
  `(groupMask & container+0x60) != 0`, `(excludeMask & container+0x60) == 0`, `(word3 == −1 || word3 != container+0x68)`.
  Ragdoll containers get group **0x4004** (AddRagdoll body builder → `SetPhysicsGroup(0x4004, 0xE14, 0)`).
  **The exclude mask 0xC used by hold.cpp / rayprobe "broad" (copied from the AddRagdoll ground probe) removes
  ragdoll bodies (0x4004 & 0xC = 4)** — that probe is designed to skip the ragdoll itself. For corpses use query
  flags with the dynamic bit (5 or 0xF), group 0xFFFFFFFF or 0x4000, and an exclude mask without bit 0x4.
* Hit → wrapper (`hit+0x28`) → container (`wrapper+0x38`) → manager entry with `entry+0 == container` (§3). The hit
  body is the grabbed bone; its name via the template (§3 step 3) or by index.
* E: a corpse stays an `esv::Character`; client entity from the container's +0x20 / entry+0x10.

### 6.2 Grab, drag, throw (recipe; each call cited in §3/§4/D)
1. Wake: for every body wrapper `w`: if entry flag bit0 → `w->vf[0x100]()` MakeDynamic; `w->vf[0x68]()` WakeUp.
   Raise `entry+0x58` (hard limit) or keep resetting `entry+0x60` (elapsed) under `M+0x18` while held.
2. Tether: if the drag should exceed about 0.2 m horizontally or 1 m down / 0.5 m up from the entity origin
   (tether frame axes: X = up, Y = −Z, Z = −X, from the actor0 local rotation of `Dummy_Root_To_Root_M_Constraint`),
   set its linear motions to eFREE (`joint->vf[0xE8](joint, 0..2, 2)`).
3. Attach to the hand (option A/B/C of §4). Lift the template's max linear velocity 10 m/s / max angular 5 rad/s for
   throws (PxRigidDynamic +0x160 / +0x150, the same slots the spring code 0x3A84A20 adjusts).
4. Release: set the hand velocity on the grabbed body (and optionally a fraction on the others), WakeUp.
5. Settle and commit: when the bodies sleep (PxRigidDynamic +0x1F8 isSleeping), the corpse's new place is the Root_M
   body. Commit it through the server (E §10: `Osi.TeleportToPosition(corpse, x, y, z)` → 0x35F09A0 character
   branch). When the client entity transform then changes, the manager update (0x3A8CAA0) teleports Dummy_Root to
   the entity transform and **moves every sleeping body by the same delta** — which would shift the settled corpse.
   Avoid it by first moving Dummy_Root to the committed transform yourself (setGlobalPose under lockWrite, same
   position and rotation as the entity will get) or by keeping the bodies awake (WakeUp each frame) until the
   replicated entity transform matches Dummy_Root. Then restore the tether motions (Limited).
6. The visible body follows automatically: GetRagdollPose → RagdollPose node → skeleton (§2.5).
Server side: there is no ragdoll on the server (only the query scene `phx::SimplePhysXScene`, D §7). The server keeps
the corpse's character transform only; limb poses are client state.

### 6.3 Persistence (partly open)
* The corpse position persists only through the server transform (6.2 step 5). Confirmed by D §7 + E §10.
* Whether the limb pose survives save/load or leaving/re-entering streaming range is **Unconfirmed**: after load the
  death graph re-runs AddRagdoll (likely with `HoldLastPose`) or plays a dead pose; `ls::animation::StoredPoseComponent`
  (type id 0x605BBF0, component index 0x60AB1F0, users 0x3A44C20, 0x3B2A180, 0x19F0040, 0x1975090/0x19756B0/0x1975CD0)
  and the stored-pose apply 0x3C52E10 (C) are the candidates. Runtime log to settle it: hook AddRagdoll 0x3A8AF90 and
  log desc+0x78 entity, +0x5C/+0x60/+0x68/+0x70 and whether desc+0xB8 is set, during a save load next to a corpse;
  also log 0x3C52E10 calls with the visual.

---

## 7. Phase 2: active ragdoll (PLANCK-style) for living characters

### 7.1 Where the pose is final, and the write-back point (Confirmed)
* Normal characters: graph → plugin 0x3A051F0 (local pose → world pose in `[skel+0xE8]`) → finish 0x3BD6670:
  slot 29 swap, **slot 30 (0x3A20590) skinning**, attachments 0x3F12D60/0x3F12F40, visual 0x3C6B900 (C §1.3).
* Characters with a spring object (`skel+0x30`): slot 29, physics write-back 0x3DD9AC0 → 0x3DD6310 (slot 26),
  then slot 30, then attachments (§5).
* In both, the mod's slot-30 hook (bodyik.cpp) runs **after every animation/ragdoll/spring writer and before
  skinning**: it is the write-back point for an active ragdoll, the same one the arm IK uses. Read the animated
  targets from `[skel+0xE0]` at entry, write the simulated bones back into `[skel+0xE0]`, call the original.
  Because world poses are not hierarchical, every non-simulated child bone (fingers, twist bones, `Dummy_*` attachment
  bones, face) must be re-derived: `childSim = parentSim * inverse(parentAnim) * childAnim` (same as bodyik's
  followers). Attachments (weapons) then follow (K).
* Thread: slot 30 runs on animation workers (C §1.3); results from a simulation running elsewhere must be handed
  over lock-free (double buffer per character), exactly like bodyik's targets.

### 7.2 Using the game's own PhysX as the active-ragdoll simulator (recommended first)
Everything needed exists and is confirmed above:
* Bodies/joints/limits/masses: the character's own ragdoll template (`skeleton+8` RagdollResourceID) via
  AddRagdoll with a descriptor built like 0x3DD4FC0 (or call the node path); body i ↔ bone by name (§2.2).
* Motors: every joint already has D6 drives; set `setDrive(eSLERP, {k, d, maxForce, 0})` (+0x160) and each step
  `setDrivePosition(relative target, true)` (+0x170) where the target is the child's animated frame relative to the
  parent's in joint space: `target = inv(parentJointFrame) * inv(parentAnim) * childAnim * childJointFrame` (joint frames
  = getLocalPose +0x40 for actor 0/1). Use `setDriveVelocity` (+0x180) for damping toward animated velocity.
  Root: drive the tether joint's linear axes (drives eX/eY/eZ) toward the animated pelvis so the body stays on the
  game's (server-owned) root, and let hits/contacts push the rest. Recovery = drive stiffness ramps back up.
* Keep it alive: raise `entry+0x58` (DurationHardLimit) to a huge value and clear bit0 so the manager never freezes
  it; do not use the game's RagdollPose node (the living graph has none active): write the pose in slot 30 from the
  actors' `getGlobalPose` (as GetRagdollPose 0x3A8B880 does: entity-space via inverse entity transform, times the
  per-body offset entry+0x20[i], then the bone's rest from slot 14).
* Step: the main scene steps at 60 Hz with up to 3 substeps on a job worker (D §3.2). Set drive targets on the game
  thread before the step is scheduled: `phx::PhysXSceneUpdateSystem` update 0x75BA10 (it calls the ragdoll update
  0x3A8CAA0 and schedules job 0x3A74AC0) is the pre-physics hook. Kinematic hand bodies: queue poses with wrapper
  +0xB8 SetWorldTransform(pose, false) (interpolated per substep, D §3.2).
* Collision with the world: automatic (same scene as the level statics, group/mask from SetPhysicsGroup(0x4004,
  0xE14)). Self-collision between ragdoll bodies of one character: joint `CollisionEnabled=false` disables only the
  jointed pairs.
* Switch off / hand back: RemoveRagdoll (mgr vf+0x10, or vf+0x18 MarkForRemoval) and stop writing bones.

### 7.3 What stays authoritative where
* Server: character position/rotation, movement, attack/hit results, damage (E §0.1, I: `Osi.Attack`). Characters'
  transforms only accept steering from the controlling peer (E). The server has no dynamics (D §7).
* Client-only: ragdoll bodies, spring bodies, bone poses, the active-ragdoll offsets. Keep the simulated pelvis tethered
  to the server-driven root (tether joint drives) so the visual never diverges by more than a stagger; route gameplay
  consequences (shove, knock prone, damage) through server calls, not through physics.

---

## 8. External simulator (Jolt / Box3D / delirium-physics): what BG3 provides and where it plugs in

| Need | BG3 source (confirmed) | Notes |
|---|---|---|
| Skeleton: names, hierarchy | Skeleton slot 18 name, slot 19 parent, bone count `+0x98` (C §1.4); Granny bone `+0x00 name`, `+0x08 parent`, `+0x0C LocalTransform`, `+0x50 InverseWorld4x4` | bodyik/skeleton.cpp already read names/parents |
| Bind (rest) pose, bone lengths | Granny bone LocalTransform / InverseWorld4x4 (0xA4 stride from `[skel+0xD8]+0x0C`), or rest world pose `+0xF8` (slots 13/14) | lengths = distance parent→child in the bind pose |
| Animated target pose per frame (model space) | `[skel+0xE0]` at slot-30 entry (slots 10/11) | previous frame `+0xE8` gives velocities (as 0x3DD3440 does) |
| Character world transform | Visual+0x08 (SE, bodyik `kVisualTransform`), or entity Transform | GrannyGetBoneWorldTransform node 0x3A41700 multiplies by it |
| Body shapes, masses, joint frames, limits | the character's ragdoll resource: PhysX binary (`*_Ragdoll.bin`), at runtime via PhysX getters on the instantiated template or a live ragdoll | HUM_M: 20 capsules, masses 2-10 kg, D6 limits (§1.2, `Q/hum_m_ragdoll_summary.txt`) |
| Read shapes at runtime | PxRigidActor getNbShapes +0xB0 / getShapes +0xB8 (D); **NpShape vtable 0x5430F08** [r35, r36]: +0x38 getGeometryType (`shape+0x98`), +0x48 getGeometry, +0x58 getSphere, +0x60 getCapsule (type 2 → radius, halfHeight), +0x78 getTriangleMesh (type 5), +0x80 getHeightField (type 6), +0x88 getActor (`+0x28`), +0x98 getLocalPose | geometry types: 0 sphere, 1 plane, 2 capsule, 3 box, 4 convex, 5 triangle mesh, 6 heightfield |
| Joint data at runtime | PxD6Joint vtable 0x5463670 getters (§4): getMotion +0xF0, getTwistLimit +0x138, getPyramidSwingLimit +0x158, getLinearLimit +0x128, getLocalPose +0x40 | |
| World collision | Option 1: query the game scene each step (RaycastClosest + sweep/overlap slots +0x50..+0xC0, D §5) — fine for hands and probes, too slow as a contact generator for 20 bodies × substeps. Option 2: mirror nearby statics into the external world: overlap query around the character → hit wrappers → actor → shapes → geometry (above); PxScene getNbActors(PxActorTypeFlags&) +0x90 = 0x4AAEA40 / getActors(flags, buf, size, start) +0x98 = 0x4AAEAE0 (decompiled [r44]: flag 1 counts concrete type 6 statics, flag 2 type 5 dynamics, from the scene actor array +0x23D8/+0x23E0) | terrain/meshes are triangle meshes/heightfields; their vertex/triangle getters (PxTriangleMesh vtable) are not mapped yet (open lead) |
| Write-back | slot-30 hook (§7.1) | same as arm IK |
| Turning the game's ragdoll off for a character | (a) let it be created and immediately `MarkForRemoval` (mgr vf+0x18): GetRagdollPose then finds no entry and leaves handle+0x28 unchanged, so the RagdollPose node keeps outputting the last fetched body transforms (and only while AnimationBlueprintComponent.Flags & 2, since the flags lookup returns 0); (b) hook AddRagdoll 0x3A8AF90 and return 0xFFFF for that entity (desc+0x78): the node skips GetRagdollPose (`cmp dx,0xFFFF` at 0x3A4E142) and composes the identity transforms written by 0x3DD33C0 with each bone's slot-14 transform. In both cases the slot-30 hook must overwrite every bone. (c) keep the game ragdoll but make all bodies kinematic (+0xF8) and drive them from the external sim (keeps picking/raycasts on the corpse and the game's own pose path) | (c) is the least intrusive for corpses; what (a)/(b) look like on screen is Unconfirmed (log the bones in slot 30) |
| Turning springs off | `skeleton+0x30` = PhysicsSkinnedObject; scene removal path 0x3A617E0 (vf+0x28 remove, factory release) | not needed unless the sim also drives hair |

Rates: the game steps PhysX at fixed 60 Hz; an external sim can run at headset rate (72-120 Hz) on its own thread and
publish poses; slot 30 consumes the latest. delirium-physics is GPL-3.0: linking it into the mod's DLL makes the
combined binary GPL (licensing note, not RE).

---

## 9. Function / data index (new in this report)
| RVA | What |
|---|---|
| 0x3DCC420 | AddRagdoll node socket registration (names) |
| 0x3DD3440 | descriptor params + initial velocities from the last two animated poses |
| 0x3DD47E0 / 0x3DD4F00 | body↔bone mapping by actor name (FindBone), parent fallback |
| 0x3DD33C0 / 0x3DD5AC0 | handle arrays init (identity transforms / 0xFF map) |
| 0x3A4E0B0 | GrannyNode_RagdollPose evaluate (gated by AnimationBlueprintComponent.Flags & 2 or frozen) |
| 0x3A8E320 / 0x3A8FD70 | ragdoll body builder / kinematic (Dummy_Root) builder |
| 0x3A8D610 | HoldLastPose: joint frames from current pose, ±5° soft limits × Stiffness/Damping multipliers |
| 0x3A8AC20 | "joint links kinematic and dynamic" test (tether filter) |
| 0x3A90780 / 0x3A90940 / 0x3A90B60 | ragdoll added / removed / contact (impact) handlers |
| 0x3A78D10 | template D6 joint instantiation (PxD6JointCreate + copy) |
| 0x4CFF340 | PxD6JointCreate |
| 0x5463670 | D6Joint vtable (D6 slots +0xE8..+0x1A8) |
| 0x5430F08 | NpShape vtable |
| 0x5893F48 | ragdoll/base actor wrapper vtable (same slots as 0x58940D0 up to +0x148) |
| 0x3A8A890 / 0x3A8A830 | query prefilter (block / touch) — group/exclude test on container+0x60 |
| 0x3A62D00 / 0x3A640B0 | create spring (kind 2) / dynamic-physics objects for a visual (scene slots +0x140/+0x160) |
| 0x3A60200 | kind-2 container (PhysicsSkinnedObject, vtable 0x58944E8) |
| 0x3A848B0 | spring attach: `skeleton+0x30 = this` |
| 0x3A61950 | PreupdateSkinnedObjects system body (scene lists +0xD0/+0xE0/+0xF0/+0x100) |
| 0x3A84A20 / 0x3DD6000 | spring pre-step (velocity limits, wake/kinematic drive) |
| 0x3F12AA0 | spring: animated pose → kinematic bodies (from blueprint execute 0x3BD5E30) |
| 0x3F12D60 / 0x3DD9AC0 / 0x3DD6310 | finish-time spring write-back (slot 29, slot 26 per bone, slot 30) |
| 0x3F35F20 | skeleton resource visitor (Ragdoll/Spring/DynamicPhysics resource ids) |
| 0x3F3BE90 | serializer of the ragdoll parameter block (AngularVelocityModifier … LockLimits) |
| 0x5FD1CE8 / 0x5FD1CF0 / 0x5FCF778 | FixedString slots RagdollResourceID / SpringResourceID / DynamicPhysicsResourceID |
| 0x605BBF0 / 0x60AB1F0 | ls::animation::StoredPoseComponent type id / component index |
| 0x60A6630 | ls::AnimationBlueprintComponent component index (init 0x20B8A0) |

---

## Open leads

1. **Corpse pose across save/load and streaming** (§6.3). Runtime log: hook AddRagdoll 0x3A8AF90 (log desc+0x78
   entity, +0x5C, +0x60, +0x68, +0x6C, +0x70, desc+0xB8 != 0) and 0x3C52E10 (stored pose apply) while loading a save
   next to a moved corpse. Static next step: decompile the StoredPoseComponent users 0x19F0040, 0x3B2A180 callbacks
   (0x19F04C0, vtable 0x58A1E00) and find the component's writer.
2. **Who sets the blueprint ragdoll variables** (DurationHardLimit etc. default 0 in `cs.lsx`; with 0 the manager
   would freeze at once, so something sets them). Look for SetVariable nodes / constant inputs feeding variable
   indices 260/65/361/342/312/284/61/625 in `Q/gamedata/bp/cs.lsx`, and the code that raises the graph events
   `TriggerRagdoll` / `RaycastRagdoll` (animation text keys vs. status code). Runtime log: the float[7] block at
   0x3DD3440 entry (r8) for a death, a knock-down and a thrown character.
3. **`AnimationBlueprintComponent.Flags & 2`** (SE field +0x24) gates the RagdollPose node and the finish job; meaning
   unconfirmed (visible/active update). Log it in the slot-30 hook for a corpse on and off screen.
4. **Living-character collision bodies in the main scene** (for hand ↔ body contact and for an active ragdoll's
   contacts with other characters): whether characters own query/kinematic shapes (Softbody capsules, picking
   shapes, `ecl::controller::PhysicsSystem` 0x75BCE0 → 0x1DAB2A0 per-entity scene). Runtime log: overlap query at a
   living NPC, print each hit wrapper's container group (+0x60), owner (+0x20) and actor/shape names.
5. **PxTriangleMesh / PxHeightField getters** (vertices/triangles) for mirroring static geometry into an external
   sim: find the TriangleMesh vtable (string "PxTriangleMesh" 0x545FA50 → getConcreteTypeName → vtable) and verify
   getNbVertices/getVertices/getNbTriangles/getTriangles by decompile.
6. ~~PxScene getNbActors/getActors~~ — confirmed [r44] (§8).
7. **0x3DD6310 detail**: which bodies (category 2 dynamic only vs. also 4) the spring write-back writes and whether it
   writes `+0xE0` only (slot 26) or also `+0xE8` (slot 28 seen in 0x3DD8730). Needed only if the mod wants to mix its
   own active ragdoll with springs on the same skeleton.
8. **Ragdoll impact events**: scene listener lists `scene+0x10` / `+0x28` / `+0x58` (called by 0x3A8CAA0, 0x3A90780,
   0x3A90B60 with velocities/impulses) feed `ecl::ragdoll::SoundSystem` (`Movement_Ragdoll_Impact`,
   `Gameplay_Character_RagdollImpactForce`). Useful for haptics/sounds when a thrown corpse hits; consumers not traced.
9. **Simulation filter**: ragdoll collision mask 0xE14 vs. static group bits (D §12 lists group-bit meanings as open);
   decide which groups a hand/held-corpse actor should collide with.
10. **Articulation ragdolls**: the factory supports articulation links (category 8) but none of the checked files
    uses them: HUM_M 21 bodies/20 D6, BEH_Spectator 39/38, DJINNI 34/33, BEAR 18/17 (PhysicsTool conversions in
    `Q/gamedata`). Other creature files unchecked.
