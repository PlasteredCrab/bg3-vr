# Agent AX: idle animations of the controlled character (bg3_dx11.exe 4.1.1.7631656)

Static RE only (Ghidra project `BG3_G`, headless; `bg3vr_disasm.exe`) plus static reading of the game's own data
files (the compiled client animation blueprint and `Attitudes.lsf`, extracted from the user's paks to
`re-local/ghidra/agents/AX/data/`, never modified). Image base 0x140000000; every address is an RVA. Raw decompiles
are `AX/x*.c|txt` (cited as [xN]); the decoded blueprint bytecode is `AX/bp_idle_sequences.txt` and
`AX/bp_seq675_choose_idle.txt` (cited as [bp]). SE = Script Extender source (`downloads/bg3se-src`).

Status: complete. The game has its own per-entity off switch for idle variations (the genome event `IdleTurnOff`,
which the game itself sends to character-creation dummies), and the matching on switch (`IdleTurnOn`) exists in the
blueprint. Attitude fidgets are a second, independent layer with their own timer variables.

---------------------------------------------------------------------------------------------------

## 0. Fix for the VR mod

The user's requirement (coordinator, during the task): **only in first person**. Third person (chase camera) and
the free camera keep the game's normal idles. The switch must work in both directions at any moment, with no level
reload and no new animation request. The table covers that.

| # | Mechanism | RVA / data | Exact action | What it changes | Thread | Risk | Confidence |
|---|-----------|------------|--------------|-----------------|--------|------|------------|
| F1 | **Idle variations OFF** (entering first person): queue the genome gameplay event `IdleTurnOff` to the controlled character's **client** entity | `ls::AnimationBlueprintSystem::QueueGameplayEventToGenome` **0x3BD4B90** (SE `Ext.System.AnimationBlueprint:QueueGameplayEventToGenome(entity, "IdleTurnOff", {})`) | One call, on entering first person. | Blueprint handler (seq 534, `on IdleTurnOff`) sets `IDLE_IsOn = false` (var 435). This (a) blocks the Still to RandomIdle transition (check seq 355 needs `IDLE_IsOn`); (b) **ends a random idle that is playing immediately** (RandomIdle to Still, check seq 359 `!IDLE_IsOn`, blend time **0.0**, a snap); (c) blocks additive-random rolls (check seq 381); (d) drives the additive-idle layer weight `IDLE_Additives_Alpha` to 0 (seq 692 br5, interpolated). The base standing idle (Still), neutral stills, status idles, look-at, reactions and attitude fidgets keep playing. | Any thread (locks the system's CS +0xF0). It applies when the queue is next drained (0x3BD3990 to 0x3BD73C0). SE Lua: client Lua tick. | Low: the game uses the same event (0x2038C80, character-creation dummies). | Mechanism **Confirmed** (exe and blueprint bytecode). SE Lua route: **Unconfirmed** (one log line, see §4). |
| F2 | **Idle variations back ON** (leaving first person, or the VR toggle switched off) | Same queue | Queue **`DisallowIdle`, then `IdleTurnOn`**, in that order (the queue is FIFO). | `DisallowIdle` sets `IDLE_Trigger Idle = false` (var 664). `IdleTurnOn` sets `IDLE_IsOn = true`. While OFF, the idle timer keeps running (seq 692 br0 is not gated). Without `DisallowIdle`, a trigger that was latched while OFF would fire a random idle within 0.5 s of switching back on. With it, the next random idle comes after the remaining `IDLE_IdleDelay - IDLE_CurrentTime` (normal cadence, 20–45 s); the additive layer fades back in. | Same as F1 | Low | **Confirmed** (blueprint seq 534 handlers. The exe never sends `IdleTurnOn`; the mod creates that FixedString itself, see §3.4.) |
| F3 | **Attitude fidgets OFF** (optional, if the log in §4 shows they move the body) | Instance variables (Instance+0x30, 16-byte `GenomeVariant`), then the event `ATT_Reset_Additive_Timer` | Cache the current float values of `ATT_MinTimeBeforeAdditveRandom` (var 212, default 6), `ATT_MaxTimeBeforeAdditveRandom` (150, 12), `ATT_MindMelt_MinTimebeforeAdditiveRandom` (609, 6) and `ATT_MindMelt_MaxTimebeforeAdditiveRandom` (123, 12). Write 1e6 to all four, then queue `ATT_Reset_Additive_Timer`. | The handler re-rolls `ATT_TimeBeforeAdditveRandom` (271) from min/max, so it becomes about 1e6 s, and zeroes the timer and its gate. The wait state (1052) never reaches its exit check (seq 405), so no new attitude fidget is rolled. A fidget that is already playing (state 1051, additive, bone-group masked) finishes normally. | Variable writes on the game thread. The animation job reads them on workers; aligned 4-byte stores, so the race is benign. | Low–medium (instance offsets come from SE plus 0x3FF6580) | Bytecode **Confirmed**. Variable indices **Confirmed for `Characters_Common.lsbc`**: check the name per instance (§3.3). |
| F4 | **Attitude fidgets back ON** | Same | Write the cached four values back, then queue `ATT_Reset_Additive_Timer`. | `ATT_TimeBeforeAdditveRandom` is re-rolled to a normal 6–12 s, and the fidget loop resumes. | Game thread | Low | **Confirmed** (bytecode seq 697) |
| F5 | **Re-apply triggers** (nothing else is cached) | `ls::AnimationBlueprintComponent` (SE: `Instance` +0x00, `Blueprint` +0x10) | While first person is active, every frame (or on change) compare the controlled entity's handle and its `AnimationBlueprintComponent.Instance` pointer with the last ones applied. On a change, re-send F1 (and F3). Send F2/F4 to the **previous** entity if it still exists. | A new blueprint instance (visual rebuilt, shapeshift, polymorph, load, resurrect, character switch) starts with the defaults (`IDLE_IsOn = true`, ATT timers 6/12). Nothing in the blueprint other than the two handlers writes `IDLE_IsOn` (exhaustive search of all bytecode and node sockets, §2.4). | Game thread | Low | **Confirmed** for the data. Which game actions recreate the instance: **Unconfirmed** (that is why the pointer is compared). |
| F6 | **Fallback only, default off**: pin the body horizontally under the camera | Skeleton slot 30 entry **0x3A20590** (C §1.6 A; the mod already hooks it) | For the followed skeleton, only in first person, and only while standing still (not in a timeline or status idle), subtract Root_M's model-space horizontal (X, Z) offset from a reference from the translation row of every bone in `[skel+0xE0]`. | Keeps the whole mesh centred on the visual root, whatever animation plays. | Animation worker threads | Medium: sitting, lying, kneel and climb poses deliberately offset the root, so this must stay gated | Code path **Confirmed** (C report). Need: **Unconfirmed** until §4 shows displacement after F1. |

**On and off at any moment, summary.**
- Turn ON (entering first person): F1, plus F3 if wanted. A random idle that is playing stops at once (0.0-blend
  transition). The additive layer fades out. An attitude fidget that is playing finishes.
- Turn OFF (leaving first person): F2, plus F4. To restore exactly the game's own behaviour, the only state to put
  back is `IDLE_IsOn = true`, `IDLE_Trigger Idle = false` and the four ATT values. Nothing else was changed: the
  timers (`IDLE_CurrentTime`, `IDLE_IdleDelay`, `ATT_AdditiveRandomTimer`) are game-owned and keep running.
- Per entity, the mod keeps only: entity handle, instance pointer, the four cached ATT floats, and an "applied" flag.
  Do not cache variable indices across blueprints; resolve them per `Instance->Parent`.
- Third person and free camera need nothing special: just send F2/F4 when leaving first person.

---------------------------------------------------------------------------------------------------

## 1. How BG3 picks and plays idles (client)

### 1.1 Everything is in the client animation blueprint (Genome), per entity

- Each animated entity has `ls::AnimationBlueprintComponent` (SE: `Instance`, `Resource`, `Blueprint`). The client
  updates it with `ecl::AnimationUpdateSystem` 0x1F14C80, through 0x3BD35B0 and the job 0x3BD5E30, which runs the
  blueprint sequences (C §1.3).
- Player and NPC humanoids use `Public/Shared/Assets/Blueprints/Characters_Common.lsbc` (client) and `.lsbs`
  (server), from `Assets.pak`. The resource banks point at `Mods/Shared/Assets/Blueprints/Characters_Common.lsabp`
  (`[PAK]_Female_Body/_merged.lsf`).
- `Characters_Creatures`, `Characters_Common_MindflayerPlayer` and `Characters_Creatures_Quadrupeds` have the same
  `IdleTurnOff` / `IdleTurnOn` handlers and `IDLE_IsOn` at index 435. Checked by extracting them.
- The idle logic is blueprint bytecode, not exe code. I decoded the genome VM to read it (§2).

### 1.2 The idle state machine (Characters_Common, StateMachine sequence 348, entry state 972)

From the `GrannyNode_State` nodes and their transitions in the blueprint (Confirmed, data):

| State (node) | Enter events | Plays | Leaves when |
|---|---|---|---|
| 972 NeutralStill | `NeutralStill_Entering` | Neutral loop or transition after stopping | 361/363/366/369/376 (neutral timers, `IDLE_SkipNeutral`) |
| 973 NeutralTransition | | | 362/364/367 |
| **968 Still** (base idle) | `EnteringStill`, `Turn_On_Additive` | `IDLE_CurrentStillShortName` (default `07e81afc-…`), plus the additive layer `AdditiveRandoms` weighted by `IDLE_Additives_Alpha` (seq 350 br4: `GrannyNode_BlendAdditive` node 976) | **355** to 969 (random idle), 371/372/373 |
| **969 RandomIdle** | `RandomIdle_Entering`, `DisallowIdle` | `IDLE_SetRandomAnim` (seq 350 br20, node 990 `PlayAnimationFromActiveSet`, full body) | 356 (anim time remaining ≤ 0.2) or **359** (interrupt, includes `!IDLE_IsOn`, blend **0.0**) to 968 |
| 970/971 StatusIdle | `StatusIdleStarting` | Status idles (`STAT_CurrentStatus`) | 357/358/360/370/374 |

- **Timer** (seq 692, every update):
  - `IDLE_CurrentTime += dt`;
  - when it is ≥ `IDLE_IdleDelay`: `IDLE_Trigger Idle = true` and the timer resets;
  - `IDLE_IdleDelay = RandomFloat(IDLE_SetMinIdleDelay 20, IDLE_SetMaxIdleDelay 45)` (seq 665).
- **Random-idle gate** (seq 355), all of:
  - `IDLE_Trigger Idle`;
  - `STAT_CurrentStatus == 0` and `!STAT_EndStatus`;
  - **`IDLE_IsOn`**;
  - `!AD_IsADsActive`, `!AD_IsWorldBehaviour`, `!bIsInAction`, `!bSteeringIsLookAtEnabled`, `!IDLE_DebugAdditiveRandoms`.
- **Which animation** (seq 675, [bp] `bp_seq675_choose_idle.txt`):
  - In peace or in combat, `IDLE_Random` (1–5) picks `IDLE_SetRandomAnim` (short names such as `b5d99946-…`,
    `9e02c309-…`, `1c364663-…`, `aa91db27-…`, `9e15d485-…`), gated by `IDLE_Random_Peace_0N_Valid`.
  - `IDLE_SetIdlePose` (1–3) picks the base still `IDLE_DesiredStillShortName`.
  - `STAT_CurrentStatus` (0..36) overrides start, still and end idles for statuses.
- **Additive idles**:
  - additive random every `RandomFloat(IDLE_SetMinAdditiveDelay 5, IDLE_SetMaxAdditiveDelay 15)` (seq 440, 382, 381);
  - layer weight = `And(IDLE_IsOn, !bIsInAction, !lookAt, !AD…, …, IDLE_Additive Toggle, …)`, interpolated
    (seq 692 br5–13).
- **Blueprint off switch** (seq 534 event handlers, routed by the blueprint's global EventMap:
  `IdleTurnOn` / `IdleTurnOff` / `DisallowIdle` to sequence 534):
  `on IdleTurnOff: IDLE_IsOn = false`, `on IdleTurnOn: IDLE_IsOn = true`, `on DisallowIdle: IDLE_Trigger Idle = false`.
  [bp]

### 1.3 Attitude fidgets (a second, independent layer)

- Data: `Public/Shared/Animation/Attitudes.lsf` (`Shared.pak`). Per attitude pose (`Default`, `Ready_L`, `Ready_R`,
  `Kneel`, `MindMeld`, `Crouch_L/R`, `Supine`, `CrossLegs`, …) it lists:
  - `Fidget`: a list of animation GUIDs (`Default` has about 24);
  - `AdditiveIdle`: one GUID;
  - `BoneGroup`.
- Exe:
  - Manager `DAT_14600f368` (loaded by 0x3D2D0C0 / 0x3D2E690, keys `Attitudes`, `Fidget`, `AdditiveIdle`).
  - Client genome nodes are registered by 0x1BFF790 [x5]:
    - `Client_AttitudeRandomFidget`: node vtable 0x5651478, Execute 0x3D6CD10, then vf+0x38 0x2014BD0, then
      **0x1F14800**;
    - `Client_AttitudeRandomAdditiveIdle`: vtable 0x5651588, then 0x2014960, then **0x1F14930**;
    - `Client_AnimationAttitudeToBoneGroupNode`.
  - 0x1F14800 looks up the attitude's fidget list, picks a random entry without immediate repeats (0x1F13E10, LCG on
    per-instance state at `plugin data +0xA8 + 4`) and returns the FixedString (0x3D2D7F0). It returns empty
    `DAT_145eeb6e8` if the attitude has none. [x8, x9, x11]
  - `ecl::AnimationAttitudeSystem` (type id 0x6059204, registration 0x716CD0, object built by 0x1F13EB0) has **no
    update callback**. It is only the service these nodes call (reg 0x716CD0 passes no callback) [x11].
- Blueprint (seq 697, states 1051/1052) [bp]:
  - state 1052 enters with event `ATT_Roll_Random`: node 2425 picks `ATT_AdditiveRandom`, node 2427 picks
    `ATT_FidgetBoneGroup`;
  - it waits until `ATT_AdditiveRandomTimer ≥ ATT_TimeBeforeAdditveRandom` (seq 405), then goes to 1051;
  - 1051 enters with `ATT_Reset_Additive_Timer` (timer = 0, `TimeBefore = RandomFloat(min 6, max 12)`, or the
    MindMelt pair while mind-melding), plays `ATT_AdditiveRandom` (seq 402), and returns to 1052 when its time remaining
    is ≤ 0.1 (seq 404);
  - weight `ATT_AdditiveFidgetBlend` = `!ATT_BIsTransitionActive` (seq 620 br147);
  - applied as an additive layer masked to `ATT_FidgetBoneGroup` (node 1761 `GrannyBlendLayeredWithBoneGroupNode`).
  - **Not gated by `IDLE_IsOn`.** `ATT_IsAttitudeActive` (events `DIAG_AttitudeOn/Off`) is written but read nowhere
    in the client blueprint, so it is not a switch.

### 1.4 Other idle-like sources (not touched by F1)

| Source | Notes |
|---|---|
| Emotes | `eoc::photo_mode::EmoteAnimations`. Photo mode only. |
| Timelines / dialogues | They drive their own animation, with `ActorIdleOverride(s)` in timeline data (strings 0x58BC9D0); see AV/U. |
| `Plugin*IdleAnimation*` strings (0x3AA49B0) | Timeline-editor plugin strings. |
| `ecl::movement::MSIdleComponent/System` | Movement-state idle, not animation selection. |

---------------------------------------------------------------------------------------------------

## 2. Evidence

### 2.1 The genome VM (decoded to read the blueprint)

- Instance and context:
  - The execution context is built by 0x3FF6580: `ctx+0x18 = instance`, **`ctx+0x78 = instance + 0x30`** (the
    global variable array; SE `GenomeBlueprintInstance::Variables`), `ctx+0x80` = stack [x20].
  - A variable reference is a u16: bit 15 set means stack, else global; the index is `& 0x7FFF`; each variant is
    16 bytes (0x3D6CD10, 0x3DD1B60) [x8, x32].
- Interpreter **0x3FE2B30**: `op = *pc++; handler = table[op]` at **0x58E7090** [x29, x31]. Opcodes:

| Op | Handler | Operation |
|---|---|---|
| 0 | | end |
| 1 | 0x3FE1F90 | PreExecute node |
| 2 | 0x3FE1FE0 | Execute node `[node, nInR, nOutR, nIn, nOut]`, then ranges and variable references |
| 3 | 0x3FE2130 | Call a native function (8-byte function slot patched from a type hash, then the same layout) |
| 4 | | goto branch |
| 5 | 0x3FE22D0 | if bool |
| 6–0xC | | int compares |
| 0xD–0x12 | | float compares |
| 0x13/0x14 | | switch |
| 0x15 | | return |
| 0x16 | 0x3FE2B00, then 0x3DD1B60 | mov dst ← src |

  "goto" pushes a branch frame (0x3FE1F30), and 0x15 pops it.
- Native function names: the op-3 hash is the FixedString hash of the node type name. FixedString creation
  0x4194CA0 uses MurmurHash3_x86_32 with seed **0xDEAD5EED** [x38]. Hashing the exe's strings with it named **all
  87** hashes in the blueprint (`AX/fnnames.txt`, e.g. `GenomeNodeType_And`, `GenomeNode_RandomFloat`,
  `GenomeGameplayNode_IsIdling`). The whole blueprint (10958 branches plus event handlers) disassembled with no
  unknown opcode. The format is therefore **Confirmed**.
- Tools: `AX/lsfdump2.py` (LSF v7, dependent LZ4 frames), `lsfparse*.py`, `bpload.py`, `bpdis.py`.

### 2.2 The game's own use of `IdleTurnOff` (Confirmed)

- String 0x58A73A0. The FixedString global **0x5EF2710** is initialised in 0x3BB3860.
- Its only gameplay reader is **0x2038C80** [x41]. That function:
  1. builds a `GenomeParametrizedEventArgs` (vtable 0x58DF288, 22 variants);
  2. puts the event FixedString in front of it;
  3. calls **0x3BD4B90**(AnimationBlueprintSystem, &entity, &event).
- Caller chain: 0x2037EC0, 0x2037670, 0x1D0F450. The queries there are
  `ecl::character_creation::TargetVisualComponent/CurrentVisualComponent/…` (0x60CF974, 0x60A7DE0) [x49]. So the
  game turns idles off for the **character-creation** preview body.
- 0x3BD4B90 [x43]:
  - moves the FixedString (sets the source to -1) and copies the arguments (0x3FA25B0);
  - pushes a 0x178-byte record into the ring queue at system+0xD8 (count +0xE4, read index +0xE8, write index +0xEC)
    under CS +0xF0. This is SE `GameplayEventsToGenome`.
- Drain [x45, x47]: **0x3BD3990** (AnimationBlueprintSystem vtable method, data reference 0x58A82D8) calls 0x3BD73C0.
  0x3BD73C0 validates each record's entity handle and dispatches it to that entity's blueprint, then resets the
  queue.
- SE exposes the same queue (`AnimationBlueprintSystem::LuaQueueGameplayEventToGenome`, Animation.inl:291; Lua
  `Ext.System.AnimationBlueprint:QueueGameplayEventToGenome`, IdeHelpers 2033 / 35228).

### 2.3 Fidget node implementation (Confirmed) [x5–x11]

- The node's Execute 0x3D6CD10 calls `vf+0x38`, passing:
  - the input variable `var[node+8]`;
  - `plugin data = *instance + [0x6046FCC] + 0xA8`;

  then writes the result to `var[node+0xA]` (0x3D6CD80).
- The fidget thunk 0x2014BD0 gets the system as `systems[0x6059204 * 0xF8]`, where
  `systems = [[0x5FDA240]+0x1A8]+0x30` (the client world), and jumps to 0x1F14800.
- 0x1F14800: under CS sys+0x10 it looks up the attitude in `DAT_14600f368+0xF8..0x128` (0xC-byte entries), takes the
  per-collection used-bitset in sys+0x38, picks with 0x1F13E10, and returns `0x3D2D7F0(mgr, entry, idx)`.

### 2.4 Who writes `IDLE_IsOn` / reads the switches (Confirmed, exhaustive over the data)

- Every `mov`, node output and function output in all 710 sequences and their event handlers was searched, plus
  every node socket.
- `g435 IDLE_IsOn`:
  - written **only** by `on IdleTurnOn` and `on IdleTurnOff` (seq 534);
  - read in seq 355 (gate), 359 (abort), 381 (additive roll gate) and 692 (additive weight).
- `g664 IDLE_Trigger Idle`: written by seq 692 br1 (true) and `on DisallowIdle` (false); read in seq 355.
- `ATT_TimeBeforeAdditveRandom`: written only in `on ATT_Reset_Additive_Timer` (seq 697); read only in seq 405.

### 2.5 Server side

- `Characters_Common.lsbs` contains the same variables (`IDLE_IsOn` index 435, `IDLE_IdleDelay`,
  `ATT_TimeBeforeAdditveRandom`), the `IdleTurnOn/Off` handlers, `Server_AttitudeRandomFidget` (exe init 0x4BBEA0)
  and `ServerNode_*` states. The server runs its own instance with its own local timers.
- The client's random idle is decided only by client variables (seq 692: local `dt`, local `RandomFloat`).
  No replicated component feeds `IDLE_*` (Confirmed for the bytecode). The only exe senders of idle events are
  0x2038C80 (client, character creation).
- So a client-only change is enough for what the user sees. It does not desynchronise gameplay: idles have no
  gameplay effect, and root motion is discussed in §3.1.
- Anubis (server AI): `DefaultPartyMember.ann` / `DefaultCharacter.ann` contain no animation calls for party members
  (follow logic only). Data, Confirmed.
- Osiris `PlayAnimation` (eoc/ecl `script::PlayAnimationComponent`) is story-driven, not an idle variation, and
  `IDLE_IsOn` does not block it. Out of scope.

---------------------------------------------------------------------------------------------------

## 3. What moves the body away from the VR camera

### 3.1 Root motion does not separate body and camera (Confirmed)

- Post plugin 0x3A051F0, skeleton slot 0 only [x20]: if the final pose variant carries root motion (byte +0x1C) and
  it is not identity, it sends `RootMotionEvent` (quat plus translation) to the instance listener.
- 0x3BD1AB0 drains the queue (system+0x180, CS +0x198) and calls **0x3C78110** SetTransform (notify) on the
  **entity**. Through 0x3BF9900 that runs Visual::SetWorldTransform 0x3C65100, which writes Visual+0x08 (U §1.2).
- The mod's body anchor reads Visual+0x08. In `camera.cpp` `update_follow`, `translate = head->visual_position` and
  `g_pose = translate + height` (lines 141–149, 222–224). So the camera follows root motion.

### 3.2 Bone-only displacement does separate them

- Bones are model space, relative to the visual root (C §1.4).
- A full-body animation that translates Root_M / pelvis inside the pose, with no root-motion extraction, moves the
  mesh while Visual+0x08 stays put. Body anchor mode does not follow this (only head/eyes mode does, via `eyes_model`).
- That matches the symptom: the body steps or leans out in front of the camera.
- Candidates, in order:
  1. `RandomIdle` (969, full-body `PlayAnimationFromActiveSet` of `IDLE_SetRandomAnim`, no bone mask);
  2. the full-body additive randoms on Still (`AdditiveRandoms`, weight `IDLE_Additives_Alpha`);
  3. the neutral and status stills;
  4. attitude fidgets (additive, masked to a bone group, so least likely).
- F1 removes 1 and 2. Whether a given GR2 clip carries Root_M translation is in the animation data, not the exe:
  **Unconfirmed**, measured by §4.

### 3.3 Instance and variable access for F3/F4 and the logging

| Step | Detail | Source |
|---|---|---|
| Entity to instance | `ls::AnimationBlueprintComponent` +0x00 `Instance` (SE Visual.h:115) | SE |
| Variables | `Instance+0x30` = Array{buf +0x30, capacity u32 +0x38, size u32 +0x3C}; element i at `buf + i*16` = {Value 8 bytes inline for bool/int/float/FixedString, `GenomeVarTypeDesc*` 8 bytes} | Confirmed by 0x3FF6580 and 0x3D6CD10; Array field order from SE |
| Parent | `Instance+0x40` = Parent `GenomeBlueprint*` (0x3FF6580 passes `*(instance+0x40)`) | Confirmed |
| Name check | SE: `GenomeBlueprint::Variables` (Array<GenomeVariableTemplate*>, `Name` STDString) and `VariableIndices` (Guid to index) | SE only, **Unconfirmed** offsets |

- The compiled indices equal the runtime array indices: bytecode `g435` addresses `buf + 435*16`.
- Because the template offsets are unconfirmed, **prefer SE Lua for F3/F4 and the name check**, or match the
  variable GUIDs from the file:
  - `IDLE_IsOn a712fa80-460b-43d5-a0bb-32b5958ec527`
  - `ATT_MinTimeBeforeAdditveRandom 4c87774c-9de0-4f04-798d-7ca1900b77f3`
  - `ATT_MaxTimeBeforeAdditveRandom 37e30261-d893-4be1-7db5-7a49f23ee64b`
  - `ATT_MindMelt_MinTimebeforeAdditiveRandom`: index 609 (its GUID is in `AX/data/bp/cc.txt`),
    `ATT_MindMelt_MaxTimebeforeAdditiveRandom 2f5f581c-0d3b-459d-7480-750ea4a01282`
  - `ATT_TimeBeforeAdditveRandom 676521e7-0c4f-4682-cf8e-05fe242590fa`
  - `IDLE_Trigger Idle 664`
- F1/F2 need none of this: they are routed by event name.

### 3.4 Native route for F1/F2, if not done from Lua

- Client AnimationBlueprintSystem: `[[[0x5FDA240]+0x1A8]+0x30] + typeId*0xF8`, the same pattern as 0x2014BD0.
  `typeId = [0x604C928]`, the ls::AnimationBlueprintSystem type id (C §1.2). Confirmed pattern; the type-id global
  comes from C.
- Event record: {u32 FixedString; pad; GenomeParametrizedEventArgs (vptr 0x58DF288, 22 × 16-byte variants built with
  0x7A09C0 / destroyed with 0x3FEBF00)}, exactly as 0x2038C80 builds it.
- 0x3BD4B90 consumes the FixedString reference.
- FixedStrings:
  - `IdleTurnOff` = `[0x5EF2710]`;
  - create `IdleTurnOn`, `DisallowIdle` and `ATT_Reset_Additive_Timer` with **0x4194CA0**`(DAT_14604b2e0+0xC600, &out, &{char*, len})`,
    the same call as 0x1BFF790.

---------------------------------------------------------------------------------------------------

## 4. One-run logging plan (heavy; one run should settle it)

Log prefix `idle:`. Everything is for the controlled client entity, plus the followed stand-in if any.

1. **On every first-person enter/leave and every F5 re-apply:**
   - entity handle, `AnimationBlueprintComponent.Instance`, `Instance+0x40` Parent, variable array size;
   - for indices 435, 664, 347, 20, 157, 68, 191, 269, 506, 437, 69, 271, 476, 103, 475, 513, 212, 150, 609, 123:
     the template name (SE Lua, or the GUID check), the type name (`GenomeVarTypeDesc+0x40` FixedString per SE, or
     log the TypeHash) and the value;
   - which events were queued, and the result of the queue call. If the SE route is used, log
     `Ext.System.AnimationBlueprint ~= nil`.
   - **Abort F3 when a name does not match.**
2. **Every 0.25 s and on any change** of `IDLE_IsOn`, `IDLE_Trigger Idle`, `IDLE_SetRandomAnim`, `ATT_AdditiveRandom`
   or the state of `IDLE_Additives_Alpha`:
   - those variables;
   - `IDLE_CurrentTime` / `IDLE_IdleDelay`, `ATT_AdditiveRandomTimer` / `ATT_TimeBeforeAdditveRandom`.
   - Expected: with F1 on, `IDLE_IsOn = 0`, alpha falling to 0, no change of `IDLE_SetRandomAnim` taking effect.
     After F2, `IDLE_Trigger Idle = 0` right after re-enable.
3. **Every frame in first person**, from the slot-30 watch the mod already has:
   - Root_M, pelvis (find by name, log the name once) and Head_M model-space translation;
   - horizontal |XZ| of Root_M and of Head_M−Root_M;
   - Visual+0x08 world position and the entity Transform position.
   - Write a line when |Root_M XZ| changes by > 3 cm from its 2-second median, with the current `IDLE_*` and `ATT_*`
     state names.
   - This attributes each displacement to RandomIdle, additive, attitude fidget or something else, and shows whether
     F6 is needed.
4. **Root motion while idle:** log the entity Transform delta per frame while there is no movement input. Any
   non-zero delta while idle is root motion (moves the camera too, harmless).
5. **Optional hook counts:** 0x1F14800 / 0x1F14930 (on animation workers; thread-safe counters only). Log the
   attitude name and the returned FixedString for the controlled entity's instance, matched through the plugin-data
   pointer (`*instance + [0x6046FCC] + 0xA8`).
6. **Run script for the user:**
   - 90 s in third person, idle (expect 2–4 random idles);
   - switch to first person for 90 s (expect none; additives fade out);
   - back to third person for 90 s (expect the normal cadence, no idle right after switching).
   - Repeat once while a random idle is visibly playing at the moment of switching, to see the 0.0-blend abort.

---------------------------------------------------------------------------------------------------

## 5. Open leads

1. Whether SE's `Ext.System.AnimationBlueprint` resolves the **client** system in client Lua. One log line in §4.1;
   the native route in §3.4 is the fallback.
2. The exact frame the queue is drained: the slot of 0x3BD3990 in the AnimationBlueprintSystem vtable (data
   reference 0x58A82D8) was not mapped to the ECS update. The effect is expected within one or two frames; §4.2 shows
   it.
3. The SE `GenomeBlueprint` / `GenomeVariableTemplate` field offsets used for the name check are SE-only. Decompile
   the template reader (the blueprint loader near 0x3FA6840 / 0x3FA71D0) if native name checks are wanted.
4. Root_M translation of each clip (`IDLE_SetRandomAnim` short names, Attitudes fidget GUIDs). Measured by §4.3, or
   statically by reading the GR2 tracks (not done).
5. Neutral stills (972/973, after stopping) and status idles (970/971) also play full-body clips and are not covered
   by F1. Check them in §4.3. If they displace the body, the next lever is `IDLE_SkipNeutral` (var 514, read by
   seq 353/361/369; its writer was not traced) or F6.
6. The `FloatInterpolation` rate argument (5.0) in seq 692, which sets the additive fade time on F1/F2. Decompile the
   node function for `GenomeNodeType_FloatInterpolation` (hash 0x0AE000E8, registered like 0x3FA96C0).
