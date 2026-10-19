# Physical interaction design (draft, 2026-10-19)

Goal, from the user: interaction in the style of Half-Life: Alyx, Bonelab and Skyrim's VRIK + HIGGS + PLANCK:

- **Grab** items, props and loose objects with either hand, and hold them physically.
- **Pull** a distant item into the hand (gravity gloves).
- **Hold** items with weight, collision and two-handed grips.
- **Pick up dead bodies** (ragdolls) by a limb or the torso, drag, lift and **throw** them.

D&D rules still decide outcomes (damage, checks, theft). Physics decides how things move.

This draft fixes the architecture and the questions the reverse engineering has to answer. Sections
marked **(RE)** wait on agent reports, and nothing in them is claimed about the engine yet:

- `re-local/ghidra/agents/D_physics.md` (physics engine, bodies, ragdolls, queries)
- `re-local/ghidra/agents/E_items_interaction.md` (items, inventory, throw, corpses, authority)
- `re-local/ghidra/agents/C_animation_systems.md` (skeleton, bone override, input)

## 1. Building blocks

| Block | What it is | Depends on |
|---|---|---|
| Hand presence | Two tracked hands in the world: pose from OpenXR controllers, mapped into game space like the head (recentre, world scale) | **done (untested on hardware):** `headset::hands()` with `world_grip` via `camera::tracked_to_world` |
| Hand collision proxy | A kinematic shape per hand (sphere or capsule) used for queries, and later for pushing things | physics queries (RE) |
| Grab candidates | Per hand, each frame: nearby graspable objects (overlap query around the palm), and a pointing ray for distant ones | physics overlap and ray queries (RE), item picking (RE) |
| Grab state machine | idle → hover → grab (near) / pull (far) → held → release / throw | — |
| Held-object driver | Keeps a held object at the hand through the physics engine, kinematically or with velocity-tracked constraints, so it still collides | rigid body control (RE) |
| Ragdoll driver | Grabs one ragdoll bone body and drives it to the hand, so the rest of the body follows through the joints | ragdoll structure (RE) |
| Authority bridge | Makes held and thrown results real for the game: final position, ownership, damage | client/server model (RE), Lua/Osiris |
| Body IK | VRIK-style: the character's arms follow the hands and the head follows the headset | Skeleton slot 30 hook (section 3a) |
| Feedback | Controller haptics on hover, grab and impact | **done:** `headset::haptic(hand, amplitude, seconds)` |

## 2. Interaction model

### Near grab
1. Every frame, each hand runs an overlap query (radius about 8 cm around the palm) for graspable bodies:
   items, props, ragdoll bones.
2. The closest candidate is highlighted. The game's own item highlight can be used if it is reachable (RE).
3. On grip press: record the grip offset (the object pose relative to the hand), so objects keep the
   orientation they were picked up in (like HIGGS). Optional snap points later for weapons and handles.
4. While held: drive the body toward `hand * gripOffset` every physics step. The drive is
   velocity-based (linear and angular velocity from the pose error, as in HIGGS/PLANCK), so held objects
   collide, can't pass through walls, and heavy objects lag behind. Fall back to kinematic if the engine
   only allows that (RE).
5. On release: hand the body back to simulation with the hand's tracked velocity (averaged over a few
   frames) as the throw velocity.

### Far pull (gravity gloves)
1. Pointing ray from the hand (or a cone search) finds the target item; it is highlighted.
2. A flick gesture (the hand's velocity toward the body over a threshold) launches the item along a
   ballistic arc that ends at the hand position, like Half-Life: Alyx: solve for the initial velocity
   given gravity and flight time.
3. Catch: when it arrives within grab range while grip is held, it becomes a near grab.

### Bodies (ragdolls)
1. A dead character's ragdoll bones are grab candidates. Gripping a limb grabs that bone's body.
2. Two hands can grab two bones; the joints carry the rest.
3. Throw = release with hand velocity, applied to the grabbed bone (or spread across the body).
4. Open questions **(RE)**: does a corpse stay a simulated ragdoll or freeze after settling, and how is
   it woken? Do its final position and inventory location sync to the server when it is moved?

### Two-handed and weight
- Mass from the physics body (or item weight from the game data) scales how fast the drive follows the
  hand, so a heavy chest lags behind and a bottle follows tightly.
- Two hands on one object: drive toward the average of both grip targets, with orientation from the
  hand-to-hand axis (like Alyx's two-handed props).

## 3. Game integration (RE dependent)

- **Which objects:** items (ecl/esv item entities) and loose props with physics bodies. Whether
  scenery props are simulated at all is **(RE, agent D)**.
- **Authority:** BG3 runs a client and a server even in single player. While held, the client drives
  the visual and physics body. At release, the final position (and any damage from impacts or throws)
  must reach the server, for example through the Script Extender's server Lua and Osiris calls, or the
  game's own move-item path **(RE, agent E)**. The draft plan is:
  - client-side physics while held;
  - a server teleport/settle at release;
  - the game's throw resolution reused for damage where possible.
- **Inventory:** gripping an item from a container or the world doesn't put it in the inventory. A
  "stow" gesture (bring it to the hip or over the shoulder) calls the game's pickup.
- **Theft and ownership:** holding an owned item should trigger the game's own checks, through the
  same functions the game uses for pickup **(RE)**.
- **Combat:** melee hits with held weapons, and thrown impacts, call into the game's attack and damage
  resolution so dice and rules still decide. Separate design later.

## 2a. Physics (agent D, `re-local/ghidra/agents/D_physics.md`; D's sections 7+ still pending)

- **Engine:** NVIDIA PhysX 4.1, statically linked; Larian's layer is `phx::`. The main scene (vtable
  0x5891B30, `PxScene*` at +0x98, ragdoll manager at +0x90) steps at a **fixed 60 Hz** with at most 3 substeps
  per frame, on a job worker thread (`phx::PhysXSceneUpdateSystem` 0x75BA10 schedules job 0x3A74AC0, step
  0x3A61B70). Every PhysX write in the game is bracketed by `lockWrite`/`unlockWrite` (PxScene +0x318/+0x320).
- **Per-actor wrapper** (vtable 0x58940D0; `PxActor::userData` points to it; `PxRigidActor*` at +0x40; cached
  pose quat +0x0C / pos +0x1C; owner container at +0x38). The slots the grab system needs, all taking the
  lock themselves:
  - `+0xB8 SetWorldTransform(pose, kinematicTarget=false)`: queues the pose; the physics job interpolates
    it per substep and applies it as a kinematic target. This is **the kinematic hold path**, the same
    one the game uses for animated bodies.
  - `+0x120 SetLinearVelocity(v)` / `+0x130 SetAngularVelocity(w)`: **the velocity-drive hold path**
    (HIGGS-style, keeps collisions). Readers: `+0x128` / `+0x138`.
  - `+0x100 MakeDynamic`, `+0xF8 MakeKinematic`, `+0x68 WakeUp`, `+0x60 StopMotion`, `+0x08 Teleport`.
  - `+0xC8 IsDynamic`, `+0xD0 IsKinematic`, `+0xD8 IsStatic`, `+0xE8 IsArticulationLink`.
  - Mass: PxRigidBody `getMass` (+0xE8 on the PxRigidDynamic vtable 0x5433838) through the actor at +0x40.
- **Queries:** `RaycastClosest` 0x3A85AA0 (PxScene*, from, to, out hit, flags, masks, ...) returns normal,
  position, the hit fraction and, at hit+0x28, **the hit actor's wrapper**. That gives the container, and
  from there the entity (pending in D's §7). Sphere/box sweeps and overlaps sit next to it (main scene slots
  +0x50..+0xC0).
- **Ragdolls:** AddRagdoll/RemoveRagdoll/GetRagdollPose on the manager (vtable 0x5894878). The per-frame
  update 0x3A8CAA0 moves a ragdoll rigidly when its entity moves, and **freezes it** once every body sleeps
  or a timer runs out: it makes every body kinematic (wrapper `MakeKinematic`) and sets entry flag bit0. To
  grab a corpse, make its bodies dynamic (`MakeDynamic`) and wake them. Then either keep them awake while
  held or reset the entry timer (+0x60 elapsed vs +0x58 limit), or the update freezes them again.
- **Impacts:** the contact callback 0x3A91A50 reports pairs above the actor's contact-report threshold to the
  owning container's slot +0x98. That's a hook point for thrown-object impacts (damage, sound).

**Decision for held objects:** drive the body with velocities (`SetLinearVelocity` / `SetAngularVelocity`)
computed each frame from the pose error (section 2, "Near grab", step 4). Fall back to `SetWorldTransform`
kinematic targets for very heavy or jittery cases. The math is in `native/core/src/grab_math.cpp`.

## 3a. Body and input (agent C, `re-local/ghidra/agents/C_animation_systems.md`)

Answered from agent C's report; the parts our code uses are re-checked in `docs/re-notes/skeleton.md`.

- **Bones.** The game's Skeleton (vtable 0x588D4A8) wraps a Granny 3D 2.12 skeleton. Poses are model space,
  in row-major 4x4 matrices. `GetBoneWorldMatrix` is slot 10, `FindBone(FixedString)` slot 17,
  `SetBoneLocalTransform` slot 20, and slots 22-28 set world-pose matrices. The head bone is `Head_M`.
  Other bone names are data, so `!vr_skel` logs them.
- **Override point for IK (arms follow the hands).** Hook Skeleton slot 30 `UpdateMeshBindings` (0x3A20590).
  At entry, `[skel+0xE0]` holds this frame's model-space pose. Write the IK chain's matrices (clavicle, upper
  arm, forearm, hand, plus head and neck), then let the original rebuild skinning. Attachments (held weapons,
  shields) read the edited pose afterwards, because 0x3F12D60 / 0x3F12F40 run after slot 30 in the finish step
  0x3BD6670. It runs on animation worker threads, so filter by the player's skeleton pointer. The
  alternative is local space: a post-hook on the Granny plugin 0x3A051F0, then rebuild `[skel+0xE8]`.
- **Held object attached to the visible hand.** After the IK write, the hand bone's model matrix composed
  with the visual's world transform is where the held object goes. The game does the same for attachments.
- **Hiding the head per view.** Visual slot 14 (0x3C6B6F0) takes a render view and sets cull bit 0x200 or
  0x400 by the view's split index. We already hide through slot 15 (both bits) in `cull.cpp`.
- **Input injection** (VR buttons to game actions). `InputManager::InjectRaw` 0x40F1F00 on `[0x604B1A8]`
  with a 16-byte raw change `{u16 id, u16 device, float value, float value2, u8 state}` (controller device
  3; raw ids 138-167 for sticks, triggers and buttons). The InputManager drains the queue at the start of
  each frame (0x3F77333). Game events include Interact 0xAD, ActionCancel 0xAE, Throw 0xCE,
  ContextMenu 0xD6 and CharacterMove 0x9D-0xA0. Whether raw input for an unregistered device id is
  accepted is **unconfirmed**.
- **Movement and facing.** Steering yaw at `eoc::SteeringComponent+0xC` (radians); the game's forward is
  `(-sin yaw, 0, -cos yaw)`. Root motion is applied through 0x3C78110 (set entity world transform; its
  server-side effects are unconfirmed).

## 4. Order of work

1. Hand presence: controllers tracked in game space (done in code: logged as `xr input:` lines with world
   poses). Visible hands come with body IK (step 7) or a debug marker.
2. Queries: ray and overlap from the hand against the game's physics; log what is hit (item, character,
   ragdoll bone).
3. Near grab of a loose item: kinematic first, then velocity drive. Release with throw velocity.
4. Authority at release, so items stay where they were dropped after saving and reloading.
5. Far pull.
6. Ragdoll grab and throw.
7. Body IK (arms follow hands), then weight, two-handed grips and haptics.

Each step gets the same evidence-first treatment as the rendering work: logs and traces that show what
the engine did, before building the next layer.

## 5. Questions for the RE reports

- ~~Physics engine and its API layer~~ (answered, section 2a). Remaining: overlap variants' exact semantics,
  constraints for two-handed holds.
- Ragdoll structure: which wrapper belongs to which bone (GetRagdollPose maps bodies to bone transforms;
  agent D §7 pending).
- Item ↔ physics body link; transform sync direction (physics → ECS or ECS → physics).
- Client/server: who simulates item physics in single player, and how positions replicate.
- Throw action: trajectory and damage resolution we can reuse.
- Skeleton bone override point (for arms following the hands) and where the hand bone pose comes from
  (to attach held objects to the visible hand).
