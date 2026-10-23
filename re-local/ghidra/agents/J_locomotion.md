# Agent J: locomotion and character movement (bg3_dx11.exe 4.1.1.7631656)

Static RE only (Ghidra project `BG3_B`, headless, request server `agents/J/JServer.java` = agent E's EServer
command set + agent C's runScript dispatch; disassembler `bg3vr_disasm.exe`). Image base 0x140000000; every
address is an RVA. Raw decompiles are in `re-local/ghidra/agents/J/dec/` (cited as [file.c]), request
outputs in `agents/J/req/`. SE = name/layout taken from the Script Extender source (`downloads/bg3se-src`).
Builds on C (input pipeline, task ids, system bodies), E (messages, Osiris teleport, picking), F
(controller-mode switch), G (type-id map `agents/G/typeidmap.txt`).

Status (2026-10-19): complete for the static part; runtime checks listed per item and in "Open leads".

## Hook points for the VR mod

| Need | RVA | How to use | Confidence |
| --- | --- | --- | --- |
| Smooth stick locomotion, analog, without gamepad input | GlobalSwitches (`*(void**)0x604B1B8`) `+0x136C` byte enable, `+0x1364` float x (right), `+0x1368` float z (forward); only reader **0x425E7F0** | Write each frame before the client update; magnitude <= 1 blends Stroll..Run speed; vector is rotated by the camera entity yaw (2.1) | Code Confirmed; never used by the game, runtime untested |
| Let MoveController run in KB/M (laser UI) mode | **0x425EEEF** `jz` after `cmp byte [0x614FBC8],0` at 0x425EEE8 (sig `80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 49 8B 4F 68`) | NOP the 6-byte jz (or hook CanExecute 0x425EC30) | Check Confirmed; side effects Unconfirmed (task priorities in KB/M mode) |
| Movement direction / yaw source | **0x425E7F0** (sig `48 8B C4 53 55 41 56 48 81 EC B0 00 00 00`) | Yaw = camera entity `ls::TransformComponent` right axis. Drive that Transform from the HMD, or hook this function and return a world vector | Confirmed |
| Speed tables | Template (`char+0x58`) `+0x590/+0x598/+0x5A0/+0x5A8`; 0x1BF6AC0 | Stroll/Walk/Run/Sprint (SE order); MoveController lerps 0x590 -> 0x5A0 by stick | Offsets Confirmed, names from SE order |
| Direct movement request (room-scale, point move without task) | 0x1BD67C0(m,2) -> **0x1BED4C0**(state,&target,400,1,&speed) -> **0x20064B0**(rdx=m, r9=state); m = `char+0x78` | Same sequence MoveController/MoveInDirection use; pathing + server sync (0xEB) for free (5) | Confirmed (calls); runtime untested |
| Stop movement | 0x1BD6FF0(char+0x78, force) | | Confirmed |
| Character facing | **0x1BBD650**(steerState, yaw, speedMul, flag) writes `eoc::SteeringComponent+0xC`; 0x1BBD500(char+0x80,0) selects the Steer state | Turn character to head yaw when idle (3) | Confirmed (code); replication of idle turns Unconfirmed |
| Instant rotation | 0x1C65700(char, quat*) | Snap; used by client teleport | Confirmed (callers) |
| Walkability for room-scale | **0xE4CEC0**(aiGrid, &flags, &pos, r), mask `0x40000000400084`; aiGrid = `*(*(*(void**)0x6007A28+0x90)+0x80)`; 0xE3C970 blocked test | Refuse to follow the HMD into blocked cells | Calls Confirmed, bit meanings Unconfirmed |
| Can the character move (turn-based) | 0x1CAB8E0; "Movement" resource GUID at 0x5FBFC50, amount 0x1102510 | Gate locomotion / show remaining movement | Confirmed |
| Point-to-move (game's own) | MoveTo task vtable 0x591A400, OnInputEvent 0x4265950 on event 4 "Action1" | Make the picking helper hit come from the controller ray, inject left mouse (device 1) | Task path Confirmed; ray hook Unconfirmed |
| Teleport (authoritative) | Osiris TeleportToPosition -> 0x35F09A0 -> 0x35E74D0; client applies 0x34 via 0x1FA9100 | From SE Lua `Osi.TeleportToPosition`; not 0x3C78110 (client-only transform write) | Confirmed |
| Sneak / climb toggles | events 0xCD / 0xAA | Inject the bound keyboard key on device 0 | Event ids Confirmed (C); handlers Unconfirmed |

---

## 1. Character movement end to end (keyboard / gamepad)

### 1.1 Who consumes input events 0x9D-0xA0 (CharacterMoveForward/Backward/Left/Right)

* `ecl::InputController` vtable **0x5643038** (GetName 0x1B3E720 returns "InputController"); the player
  character's controller is the derived `CharacterInputController`, vtable **0x5918678** (GetName 0x4244E10
  "CharacterInputController", task factory slot 19 = **0x4244E20**). Slot layout (this build; differs from the
  SE header): 4 Update **0x1B3F9F0**, 11 OnInputEvent **0x1B3F680**, 13 GetTask(i) 0x1B3EF70
  (`return Tasks[i]`). The controller hangs off the client character at `char+0xA0` (SE `InputController`;
  all SE `ecl::Character` offsets are +8 in this build: `+0x58` Template, `+0x68`..`+0x80` AiAction /
  AiBehaviour / **AiMovement (MovementMachine) +0x78** / **AiSteering (SteeringMachine) +0x80**, `+0xA0`
  InputController, `+0xC0` CharacterMover). Confirmed by every task below using exactly these offsets
  ([movectrl.c], [moveindir.c], [svchartp.c] 0x1FA9100).
* Task factory 0x4244E20 [inputctrl.c] creates the task array `ctrl+0x30` (count `+0x3C`), each task
  constructed with `0x20061D0(task, type, flags)`:
  `[0]` MoveTo (0x4262C40, size 0x358, vtable **0x591A400**), `[1]` ControllerSelection (vtable 0x5919878),
  `[2]` **MoveController** (vtable **0x591A158**, size 0x108), `[3]` **MoveInDirection** (vtable
  **0x591A288**), `[4]` Spell (0x1FB2730) ... `[0xE]` SheathUnsheath (vtable 0x591A5D0) ... up to `+0xB8`.
  Slot `+0xA8` (type 21 "CharacterMove") is **not** created by this factory (Confirmed: no store to
  `+0xA8` in 0x4244E20; the only `0x20061D0` callers are listed in [req/s7.out]).
* Task vtable slots used by the scheduler (SE CharacterTask order, confirmed by the calls in 0x1B41320):
  7 CanExecute `+0x38`, 10 GetExecutePriority `+0x50`, 17 OnInputEvent `+0x88`, 25 Enter `+0xC8`,
  26 Update `+0xD0`, 27 Exit `+0xD8`, 35 `+0x118` (second "can run" test).
* **MoveController OnInputEvent 0x4260360**: consumes events 4 and 0x9D-0xA0 (returns handled `0x101`)
  [movectrl.c]. A scan of every instruction with immediate 0x9D in .text ([req/s2.out]) finds only two
  readers of these events: 0x425E907 in **0x425E7F0** (direction) and 0x425ED93 in **0x425EC30**
  (CanExecute). So **0x9D-0xA0 drive the character only through the MoveController task** (Confirmed for
  immediate operands; an id taken from a table would not show up in that scan).
* **MoveController CanExecute 0x425EC30** returns 0 unless the global controller-mode byte
  **0x614FBC8** is set (`cmp byte [0x14614fbc8],0; jz` at **0x425EEE8**, disassembly in [req/s14.out]).
  Confirmed. Consequence: in keyboard/mouse mode WASD never moves the character through 0x9D-0xA0; the
  task only runs in controller mode (F: that mode disables the mouse/laser UI). C's option "inject keyboard
  w/a/s/d on device 0" therefore does **not** move the character while the game stays in KB/M mode.
  Further CanExecute conditions (all must pass): character not owned by another system (0x783E80 /
  0xB23D90 / 0xA923B0 / 0xF0EC30 checks), `char+0x128 != DAT_145EF3A2C`, InputController not busy
  (0x1B41980), the running task is not a Spell task with flag 8, the UI does not hold input for the player
  (0x3BAAEE0), either `GlobalSwitches+0xE70 != 0` or the LocomotionComponent/player checks, then the
  direction from 0x425E7F0 has length^2 >= 0.01 (or the existing LocomotionComponent `+0x34` vector does),
  then `char+0x18` bit 13 set and bit 5 clear and **0x1CAB8E0** (can-move, 6.1) true.
* **MoveInDirection (type 3)** CanExecute 0x4261440 is the opposite: requires KB/M mode
  (`0x614FBC8 == 0`) and `InputController+0x23` (mouse held) - it is the "hold left mouse to walk toward
  the cursor" mode: Update **0x4261630** takes the picking helper's ray (`0x17F7330(player)+0x21D8`), aims
  at a point up to 3 m along it, and issues the same MoveTo movement request as below with speed class 2/3
  [moveindir.c]. Confirmed.

### 1.2 Movement direction: which yaw

**0x425E7F0 `GetMoveInput(out vec3, short playerId, bool ignoreInput)`** [dir.c], Confirmed:

```c
if (GlobalSwitches->byte[0x136C] != 0) {           // GlobalSwitches = *(void**)0x604B1B8 (SE GlobalSwitches)
    x = GlobalSwitches->float[0x1364];  z = GlobalSwitches->float[0x1368];   // override, see 2.1
} else if (!ignoreInput) {
    dev = playerInputDevice(playerId);                // map [0x60487A0]+0x38, byte at node+0x10
    x = GetInputValue(0xA0 Right) - GetInputValue(0x9F Left);      // 0x40EFB10
    z = GetInputValue(0x9D Forward) - GetInputValue(0x9E Backward);
}
if (|(x,z)| > 1) normalize;                          // analog magnitude kept, clamped to 1
ref = PlayerMap(playerId) -> EntityRef at value+0x10 // 0x17F6DA0, map [0x60487A0]+0x138
T   = ls::TransformComponent(ref)                    // 0xE59570, type id 0x6053D78
r   = T.rotation * (1,0,0)                           // 0xE331D0: entity right axis
yaw = atan2(-r.z, r.x)                               // 0x4128260 = atan2f(y, x)
out = R_y(yaw) * (x, 0, z)   = (x cos + z sin, 0, -x sin + z cos)
```

* The entity at player-map `value+0x10` is the **player's camera entity**: the camera system body
  `ecl::CameraSystem` 0x1E0A160 (C) calls the same lookup 0x17F6DA0 and reads `ecl::GameCameraBehavior`
  from that entity through 0x7049E0 (type id 0x605319C = `ecl::GameCameraBehavior` in G's map) [pcam.c].
  The type id 0x6053D78 belongs to `ls::TransformComponent`: its initialiser 0x8B900 falls back to the
  name string 0x54C1C60 = `ls::TransformComponent` [req/s10.out, s11.out]. Confirmed.
* So **stick movement is relative to the yaw of the camera entity's ECS Transform**, the same component
  camera.md found the view reads (slot 13 / slot 9 of the camera controller). The yaw is taken from the
  right axis, so camera pitch does not change it. If the VR mod writes the HMD orientation into that
  Transform, movement follows the head automatically; if the mod only overrides the view matrix at
  0x4085150, movement follows the game's orbit camera instead. (The value+0 slot of the same map entry is
  the character handle, as C used in 0x1C43820.)
* Sign convention: input right (x=+1) maps to the camera's +X axis; input forward (z=+1) maps to
  `(sin yaw, 0, cos yaw)`. Combined with C's character convention (forward = (-sin yaw, 0, -cos yaw) for a
  steering yaw) whether "forward" is the camera's look direction cannot be settled statically.
  **Unconfirmed**: log `out` of 0x425E7F0 and the camera's view forward while pushing the stick forward once.

### 1.3 MoveController task: Enter / Update / Exit

* Enter **0x425F160**: adds `eoc::controller::LocomotionComponent` (type id 0x605349C) to the character
  (0x6D14F0 = add component), fills it with the direction from 0x425E7F0: `+0x00` magnitude, `+0x04`
  normalised dir, `+0x10` raw dir; calls 0x1C6AAB0 (ends repose / sitting: message 0x2E). [movectrl.c]
* Update **0x425F2C0** each frame [movectrl.c]:
  1. re-runs CanExecute; reads the character Transform (pos `+0x10`, forward via 0xEF7160);
  2. refreshes the LocomotionComponent direction (0x705EA0 = get-for-write);
  3. **speed**: `speed = lerp(Template+0x590, Template+0x5A0, (clamp(|dir|,0.1,0.9)-0.1)/0.8)`
     (Template = `char+0x58`, 0x1C6B540). 0x1BF6AC0 maps speed classes 1/2/3/4 to Template `+0x590/+0x598/
     +0x5A0/+0x5A8`; with SE `CharacterTemplate` order (MovementSpeedStroll, Walk, Run, Sprint, Dash,
     8 bytes each) these are Stroll/Walk/Run/Sprint. So the stick deflection blends **stroll -> run**
     continuously: analog speed is native to this task. (Field names Unconfirmed: read the four floats of a
     template at runtime);
  4. target = position + speed * dir (+ dt-limited smoothing, max 16 m/s step), height from the AI grid
     (0xE4AE40 walk along the grid cells; 6.3), stored in the LocomotionComponent (`+0x28` target, `+0x2C`
     ground height, `+0x34` smoothed offset, `+0x5C` look point);
  5. if the target is >= 0.01 m away: builds movement settings (0x1BFB710 init; type byte 2) and calls
     **0x1BED4C0(MovementMachine.MoveTo state, &settings, 400, 1)** then **0x20064B0(state, machine)**
     (activate); else 0x1BD6FF0(machine, 0) (stop). The MoveTo state is `MovementMachine->cached[3]`
     created by **0x1BD67C0(machine, 2)**;
  6. steering: SteerTunnel (type 6, `0x1BBD130(steerMachine, 6)`, `0x1BEAA10(state, dir)` sets
     TargetDir `+0x18`), activated by 0x2006580.
* Exit **0x4260100**: stops movement (0x1BD6FF0) and steering (0x1BBD500), removes the
  LocomotionComponent.
* Scheduler: InputController Update 0x1B3F9F0 -> **0x1B41320** picks, every frame, the task with the
  highest `GetExecutePriority >= 0` whose CanExecute and slot-35 test pass, Enters it, and calls its Update
  every frame; a non-zero return means "finished" and leads to ClearFlags (vf `+0x18`) + Exit (vf `+0xD8`)
  [tasksel.c 0x1B417D8]. MoveController Update returns 1 when CanExecute fails and 0 while moving.
  MoveController's priority slot is the shared 0x5D92F0.

### 1.4 Client movement / steering machines (char+0x78 / char+0x80)

| Machine | Cached states (type: vtable) | Active layers | API |
| --- | --- | --- | --- |
| MovementMachine `char+0x78` | 2 MoveTo 0x5650940 (`[3]`), 3 0x5650608 (`[4]`, MoveToSync per SE numbering), 4 Idle 0x5650988 (`[5]`), 5 Climbing 0x5650A50 (`[0]`), 6 Falling 0x564F770 (`[1]`), 7 Repose 0x5650A98 (`[2]`) | `+0x38`, `+0x40` | get/create 0x1BD67C0(m,type); set target 0x1BED4C0; activate 0x20064B0; stop 0x1BD6FF0(m, force) |
| SteeringMachine `char+0x80` | 2 Steer 0x564EC08, 3 LookAt 0x56507B0, 4 SteerTo 0x5650800, 5 (0x5650850), 6 SteerTunnel 0x56508A0 | `+0x30`, `+0x38`; entity ref `+0x40/+0x48` | get/create 0x1BBD130(m,type); SteerTunnel dir 0x1BEAA10; Steer set yaw **0x1BBD650**; activate 0x2006580; reset 0x1BBD500(m, force) |

Type numbers match SE `esv::MovementType` / `esv::SteeringType` (MoveTo 2, MoveToSync 3, Idle 4, Climbing 5,
Falling 6, Repose 7; Steer 2, LookAt 3, SteerTo 4, Repose 5, SteerTunnel 6). [mmapi.c], [ssapi.c]

Calling conventions (from the call sites, Ghidra drops arguments here): **activate** 0x20064B0 /
0x2006580 take the machine in **rdx** and the new state in **r9** (rcx/r8 unused):
`mov r9, state; mov rdx, machine; call 0x1420064b0` at 0x425FEBE [req/s30.out]. 0x20064B0 sets the
MovementMachine layer `+0x38`; 0x2006580 sets the SteeringMachine overlay layer `+0x38` (the primary
steering layer `+0x30` is set inline, see 3). **0x1BED4C0**(rcx state, rdx &target, r8w 400, r9b 1,
[rsp+0x20] &speed) (0x425FEA0-0x425FEB9). **0x1BBD650**(rcx state, xmm1 yaw, xmm2 speedMul, r9b flag)
(0x193DE44-0x193DE53). Each layer switch calls the old state's CanExit (vf `+0x28`) / Exit (`+0x30`) and
the new state's Enter (`+0x10`), or Continue (`+0x18`) if the type is unchanged.

### 1.5 What the client sends to the server

`ecl::CharacterProtocol` per-frame update **0x1FE0520** [clsync.c], for each locally controlled character
(`char+0x18` bit 13):
* **0xEB CharacterPathing**: the client-computed path (path id from the PathingComponent 0x116E4A0
  `+0x3C`, nodes from the AI path object 0xE3A0A0 `+0x278`, 0x28-byte nodes with 2 flag bits, two floats
  `+0x44/+0x48`, movement byte `+0x4C`), or the current position as a one-node path when no path exists.
  Sent when the path id changes.
* **0x24 CharacterSteering**: when the active steering type changes: {NetId, steering type, state sync data
  (state vf `+0x40`), `eoc::SteeringComponent+0xC` (TargetRotation)}.
* **0xCC CharacterFalling** (movement state 6), **0x25 CharacterAction**, **0x2B CharacterUpdateClient**
  (`char+0xC8` `+0x40` byte), 0x35 / 0x37.
* **0x2F CharacterUseMovement {NetId, float distance}**: sent by **0x1B3E630** from the MoveTo state
  helpers 0x1BEF970 / 0x1BF0050 with the distance walked along the path [usemove.c] (turn-based movement
  cost, 6.1).
Server `esv::CharacterProtocol` 0x306BBB0 (E's [E/dec/charproto.c]): case **0xEB** checks controlling peer
and copies the path into the server character (`esvChar+0x118`, `+0x88`, flag `+0x84 = 1`); case **0x24**
applies the steering state (peer-checked, else error 0x69); case **0x2F** passes the distance to
**0x3601E20**, which spends the resource keyed by the GUID at **0x5FBFC50** = ActionResource
**"Movement"** (the GUID is looked up by the FixedString "Movement", string 0x54EB840, table 0x54F4FE8,
initialiser 0xEB3220) [tbmove.c, req/s24.out]. Confirmed. So **the client is authoritative for its own
path; the server replays it**.

---

## 2. Injection for VR smooth locomotion (UI stays in mouse/laser mode)

### 2.1 Built-in move-input override (best route)

`GlobalSwitches` (`*(void**)0x604B1B8`) has three fields that only **0x425E7F0** reads (all users of
displacements 0x1364/0x1368/0x136C in .text are listed in [req/s12.out]; the only writers are the
constructors 0xEF1EF0 / 0xEF32B0, which zero them):
`+0x136C` byte **override enable**, `+0x1364` float **x (right)**, `+0x1368` float **z (forward)**.
When the byte is set the stick/keys are ignored and (x, z) is used, magnitude <= 1 gives analog speed
(stroll..run), and the vector is rotated by the camera entity yaw. Confirmed (code); never set by the game
itself.

Because MoveController refuses to run in KB/M mode, the override alone does nothing while the laser UI is
active. The minimal change is one patch: at **0x425EEEF** (`jz 0x14425f11b` after `cmp byte
[0x14614fbc8],0` at 0x425EEE8) make the jump not taken (6 bytes NOP) so CanExecute ignores controller mode.
Everything else in the task is mode-independent except: `+0x100` IgnoreMovementUntilStickIsReleased reads
the real 0x9D-0xA0 values (harmless: they are 0 in KB/M mode). **Unconfirmed** at runtime: that no other
task out-prioritises MoveController in KB/M mode while the override vector is non-zero. Statically:
MoveController's GetExecutePriority 0x5D92F0 returns **1** and its slot-35 test 0x5D68B0 returns 1;
MoveTo's priority 0x4263090 returns 3 (0x14 in some modes) whenever the picking helper has a hit, but its
slot-35 test 0x42639D0 needs `task+0xE9`, set only by Action1 (event 4); MoveInDirection returns 2 but
needs the mouse held. So a click or a mouse-hold takes over from stick movement, as in the normal game.
The priorities of the other 20 tasks were not read. Runtime check:
log `InputController+0x50` (RunningTask) type `+0x80` each frame while the override is set.

Recommended per-frame recipe (game thread, before the client ECS update, i.e. before App->vf[0x288] in
0x3F770A0, C 3.5):
```
gs = *(uint8_t**)(base+0x604B1B8);
*(float*)(gs+0x1364) = x;  *(float*)(gs+0x1368) = z;   // thumbstick, already in camera space
*(uint8_t*)(gs+0x136C) = (x*x+z*z) > 0.01f;
```
If the camera entity Transform is **not** HMD-driven, pre-rotate (x,z) by (hmdYaw - cameraYaw) using the
same formula as 1.2, or hook 0x425E7F0 and return the world vector directly (its 3 callers are 0x425EC30,
0x425F160, 0x425F2C0 [req/s16.out]).

### 2.2 Alternatives (and why not)

| Route | Verdict |
| --- | --- |
| Gamepad raw input (device 3) | Works (it is the designed path) but flips the game to controller UI (F 0x5E3C60). |
| Keyboard w/a/s/d raw on device 0 | Produces events 0x9D-0xA0 only if bound, and MoveController still refuses KB/M mode (1.1). Would need the same 0x425EEEF patch; analog lost. |
| Write `eoc::MovementComponent` Direction / SteeringComponent | Overwritten by the machines each frame; no path sent to the server (1.5). |
| Drive MovementMachine directly (0x1BD67C0 / 0x1BED4C0 / 0x20064B0 with a target 0.5 m ahead) | Works without the task and without the patch; must replicate the settings block (0x1BFB710 init, `+0x10` byte 1, speed block) and the steering; more code to keep in sync. Second choice. |

---

## 3. Turning

* Facing target = `eoc::SteeringComponent` (type id global 0x6053D74; initialiser 0x8B7C0 falls back to
  the string 0x54C1CC0 = `eoc::SteeringComponent`, [req/s17.out]) `+0x0C` **TargetRotation** (yaw, radians,
  wrapped to [0, 2pi)), `+0x10` turn speed (rad/s), `+0x18` speed multiplier, `+0x1C` flag, `+0x00` vec3 look
  point with `+0x14` "has look point" (written by SteerTunnel from LocomotionComponent+0x5C).
* Writer: **0x1BBD650 `Steer_SetTarget(steerState, yaw, speedMul, flag)`** (get-for-write 0xE2EF60, wrap,
  store `+0xC/+0x18/+0x1C`) [steer2.c]. Callers: steering SyncSystem 0x193DBA0 (server-synced yaw),
  SteerTunnel update 0x1BEAA60 (yaw = atan2(-dir.x, -dir.z), multiplier 1), type-5 state 0x1BEA130
  (multiplier = clamp(currentSpeed / RunSpeed, 0.5, 3)), SteerTo 0x1BE97A0, 0x1B8DBD0, 0x1BBE620.
* Applying: `ecl::steering::SSSteerSystem` body **0x193B550** [steersys.c] rotates the Transform forward
  (-Z axis of the rotation) toward (-sin yaw, 0, -cos yaw) by at most `turnSpeed * dt` and writes the new
  forward into `ecl::steering::SSSteerComponent` (type 0x6053784, 16 bytes: vec3 + flag `+0xC`); the Steer
  state Update **0x1BBD6F0** consumes it and calls **0x1C65700(char, quat)** (set character rotation; also
  used by the client teleport 0x1FA9100). Confirmed.
* For VR: snap/smooth **view** turning is a camera/playspace offset in the mod (no game call needed). To
  turn the **character** to the head yaw (character idle): `0x1BBD500(char+0x80, 0)` (puts the cached
  Steer state, `*(void**)(char+0x80)`, on the primary layer `+0x30` and clears the overlay `+0x38`), then
  `0x1BBD650(steer, yaw, 1.0f, 0)` with yaw = atan2(-f.x, -f.z) of the horizontal head forward. This is
  what the steering SyncSystem 0x193DBA0 does (it sets layer `+0x30` inline instead of calling 0x1BBD500).
  The turn runs at the game's turn speed (SSSteerSystem clamp) and is replicated by 0x24 when the steering
  type changes (1.5). Instant snap: additionally call 0x1C65700(char, &quat). Do not do this while
  MoveController runs: its SteerTunnel (overlay layer) retargets the yaw every frame to the move direction.
  **Unconfirmed** at runtime: that a yaw change without a steering-type change reaches the server (0x24 is
  only sent on type change / state flag; the server may take facing from the path instead) - log
  esvChar facing after an idle turn.

---

## 4. Point-to-move and teleport

* **MoveTo task (type 0, vtable 0x591A400)** [moveto_task.c]: OnInputEvent **0x4265950** reacts to input
  event **4 = "Action1"** (registered in 0x4161610, [evreg_ui.c]) when the per-player picking helper
  (`0x1CC3260(level+0xA0, player)`) has a valid hit (`helper+0x15C` bit 0) and the UI does not own the
  cursor: sets `task+0xE9 = 1` and notifies listeners. GetExecutePriority **0x4263090** copies the target
  from the helper (`helper+0xF0` vec3 -> `task+0x330`, hovered entity via 0x1C42190 -> `task+0x340`) and
  returns 3 (0x14 in some modes). Update **0x4263A30** / preview **0x4263600** do the path request and
  preview (`task+0x250` target, `+0x2C0` preview path, preview tiles 0x4264ED0). So the game's own
  click-to-move works in VR as soon as the picking helper's hit comes from the controller ray and Action1
  (left mouse, device 1) is injected. Picking-ray construction (0x1C48750 -> 0x1C473D0 / 0x1C46830 ->
  0x1C40470) is the place to substitute the controller ray: **Unconfirmed** which call builds the
  cursor ray (open lead; E's section 11 covers picking).
* **0x3C78110 `TransformSystem::SetTransform(sys, EntityRef*, Transform* /*quat, pos, scale*/, notify,
  recordPrev)`** [settransform.c]: only writes the ECS Transform (+ listeners at `sys+0x60`). It does not
  touch the movement machines, the path, or the server. On a client character the next MoveTo / path
  sync (0xEB) and server corrections fight it. Not a teleport for the player character.
* **Client character teleport 0x1FA9100(char, mgr, &pos, &euler, flag)** [svchartp.c]: rotation via
  0x1C65700, **stops movement and steering with force**, sets position through the CharacterMover
  (`char+0xC0`, 0x1BE7ED0), clears path state. Called by the client handler of **0x34 CharacterTeleport**
  (`ecl::CharacterProtocol` 0x1FDCC60 case 0x34 [clproto.c]), which also snaps the camera to the character
  (0x1E0FA30 / 0x1E1A1A0) and answers **0xCA TeleportAck**; the server counts pending acks in
  `esvChar+0x19F` (case 0xCA decrements it).
* **Server teleport**: Osiris `TeleportToPosition` character branch -> **0x35F09A0** -> **0x35E74D0**
  (queues the teleport request; also linked characters/summons), the server character protocol update
  0x306EC10 sends 0x34. This is the authoritative teleport (from SE Lua: `Osi.TeleportToPosition(char,
  x, y, z, "", 0, 0, 0, 0, 0)` per E).
* For a VR teleport-locomotion mode: use the game's MoveTo (path, keeps turn-based rules) for normal
  point-to-move; use server TeleportToPosition only for a blink/teleport option (it skips pathing and the
  Movement resource, so it would be a cheat in combat).

---

## 5. Room-scale (character follows the headset's horizontal position)

Facts the design rests on:
* The character's position is owned by the client MovementMachine (MoveTo state, path from the AI grid)
  and replicated as a **path** (0xEB) plus steering (0x24); the server replays it (1.5). Writing the
  character Transform directly (0x3C78110 / 0x3C77BD0) bypasses both, is not sent to the server and is
  overwritten by the next movement update (4). Not usable for room-scale.
* The MoveTo state accepts a new target only when it moved more than 2 cm (`0x1BED4C0`: squared XZ
  distance > 0.0004, or a different target entity / settings value) - small per-frame targets are fine
  and do not restart the path every frame [mmapi.c].
* AI grid (navigation grid) of the current client level: `aiGrid = *(*(*(void**)0x6007A28 + 0x90) + 0x80)`
  (used this way by MoveInDirection 0x4261630 and by the client protocol 0x1FE0520). Queries:
  * **0xE4CEC0(aiGrid, u64* outFlags, vec3* worldPos, float r)** -> cell flags at a world position
    [aigrid.c]; the game treats `flags & 0x40000000400084` as "not walkable for movement" (the same
    constant is the default movement collision mask written by 0x1BED4C0 at state `+0xA0`, SE
    `MSMoveToSettings::CollisionMaskMove`, and tested in 0x1BEF970). Meaning of the individual bits
    Unconfirmed.
  * **0xE3C970(aiGrid, vec3* pos, float radius, u64 mask, ...)** -> byte "blocked for this radius/mask"
    (wraps 0xE4CEC0 and a neighbourhood walk 0xE4F5B0).
  * **0xE4AE40(aiGrid, AiTilePos*, out cell, includeBlocked, ignoreHeight)** -> nearest sub-grid cell
    to a tile and height; cell = 16 bytes, byte 0 bit 0 blocked, `u16 +8` floor height (`*2/100 +
    subgrid+0xC`), `u16 +0xA` height span. MoveController uses it to put its target on the ground.
* Movement in combat costs the "Movement" action resource per metre (0x2F, 1.5; 6.1).

Recommended design (needs runtime validation; **Unconfirmed** as a whole):
1. Every frame: `d = hmdWorldXZ - characterWorldXZ` (character pos = Transform `+0x10` via 0xE59570).
2. If `|d| < ~0.05 m`: nothing (avoid jitter; the game's minimum blend speed is Stroll at 0.1 stick).
3. Else, either
   (a) **override route**: feed `d` (normalised, magnitude `clamp(|d| / 0.5, 0.1, 1)`) into the
       GlobalSwitches override (2.1) - zero extra API, the game picks stroll..run speed, or
   (b) **direct MoveTo** (precise): `st = 0x1BD67C0(char+0x78, 2)`; target block initialised by
       0x1BFB710 with the target vec3 at `+0x00` (HMD XZ, ground height) and byte `+0x60 = 2`; then
       `0x1BED4C0(st, &target, 400 /*search horizon*/, 1, &speed)` and `0x20064B0(st, char+0x78)`, as
       MoveController Update does at 0x425FEB9 and MoveInDirection at 0x42621F6. `speed` (5th argument,
       copied to state `+0xB2..+0xDA`) = `{u8 1, u8 1, pad, union{u8 class; float mps} +4, u32 index +8,
       u8 +0xC..+0xE}`: MoveInDirection passes index 0 with speed class 2/3 (walk/run, 0x1BF6AC0),
       MoveController passes index 1 with a float = length of LocomotionComponent `+0x34` (the smoothed
       `speed * dir` offset), written by 0x42603B0, which also computes the target position (its 4th
       argument, `LocomotionComponent+0x28`) on the AI grid [movectrl.c, moveindir.c, mcmisc.c].
       Pathing then refuses unreachable points by itself.
4. Before following, test `0xE4CEC0` at the HMD position; if blocked, do not follow and fade/push back the
   view (the player walked into a wall or off a ledge).
5. Keep the camera at the HMD (the mod's camera override) - the body catches up; the camera entity yaw
   drives stick movement (1.2).

## 6. Turn-based limits, jump, climb, sneak

### 6.1 Movement limits

* **0x1CAB8E0 `CanMove(char, bool skipSecondCheck)`** (called from MoveController/MoveInDirection
  CanExecute): returns 1 only if 0x1C6DAB0(char) is false and the remaining amount of the ActionResource
  keyed by **0x5FBFC50 = "Movement"** (0x1102510, via 0x11001B0) is **> 0.1**, plus 0x1101D50 unless
  `skipSecondCheck` [mcmisc.c, tbmove.c]. Confirmed that the resource is "Movement" (1.5); what 0x1C6DAB0
  and 0x1101D50 test is Unconfirmed.
* Distance walked is reported with 0x2F and charged on the server by 0x3601E20 (1.5). Speed = template
  speed class (0x1BF6AC0) x `char(+0xB8 object)+0x148` multiplier, reduced by a status of type 0x10
  (`1 - a/b` at `+0x2C8/+0x2CC`) [tbmove.c]. In turn-based mode the stick / room-scale movement is
  therefore limited exactly like click-to-move (Confirmed for the resource check; whether MoveController
  is also blocked when it is not the player's turn is Unconfirmed: log CanExecute 0x425EC30's return
  during an enemy turn).

### 6.2 Sneak, climb, jump, teleport debug keys

* **ToggleSneak 0xCD**, **ClimbingToggle 0xAA**, **Shove 0xCC**: no handler compares these ids with an
  immediate (scan [scalar_cd_aa_b8.txt]; the 0xCD hit at 0x131C380 only builds the key label for the UI);
  they are dispatched through a jump table or by id lookup. Trigger for VR: inject the raw keyboard key
  bound to the event on device 0 (keeps KB/M mode), the same way F injects mouse buttons. Handler
  Unconfirmed (lead).
* **Jump** has no input event (C's event table has none); it is the spell `Projectile_Jump` cast through
  the Spell task (type 4) or server-side `Osi.UseSpell` (E 6). VR trigger: hotbar key injection or the
  spell route. Unconfirmed which is cleaner.
* **Climbing / ladders**: client MovementMachine state 5 (Climbing, vtable 0x5650A50) and task
  ClimbTo (type 23, created at `tasks+0xB8` by 0x4258470). Climbing is entered from a MoveTo path
  (SE `MSMoveToSettings::PathClimbingMode`), so stick/room-scale movement toward a ladder uses it as the
  game does. Details not decompiled (lead).
* **TeleportPlayer 0xB8 / TeleportParty 0xB9** (handler **0x4250910**, [ev_b8.c]): teleports the
  character(s) to the picking helper's hit (`helper+0xF0`, valid bit `+0x15C`) by sending
  **0xB5 ServerCommandMessage** {command `+0x28 = 0xE`, NetId `+0x78`, pos `+0x88`, flag `+0x94 = 1`}.
  Gated by byte **0x5F9DEB1**, which has ~30 readers and no writer in .text (developer flag); the server
  side (`esv::ServerCommandProtocol`) was not checked. Lead only - do not rely on it.

---

## Function index (this report)

| RVA | What |
| --- | --- |
| 0x5643038 / 0x5918678 | vtables ecl::InputController / CharacterInputController |
| 0x4244E20 | CharacterInputController: create tasks |
| 0x1B3F9F0 / 0x1B41320 / 0x1B3F680 | InputController Update / task scheduler / OnInputEvent |
| 0x591A158 | MoveController task vtable; 0x4260360 OnInputEvent, 0x425EC30 CanExecute, 0x425F160 Enter, 0x425F2C0 Update, 0x4260100 Exit |
| 0x425E7F0 | move input -> world direction (override fields, camera yaw) |
| 0x591A288 | MoveInDirection task vtable (mouse-hold move); 0x4261440 CanExecute, 0x4261630 Update |
| 0x591A400 | MoveTo task vtable; 0x4265950 OnInputEvent (Action1), 0x4263090 priority/target, 0x4263A30 Update |
| 0x17F6DA0 / 0x17F6D50 | player map lookup (camera EntityRef at value+0x10 / character at value+0) |
| 0xE59570 / 0xE331D0 / 0x4128260 | get Transform / quat right axis / atan2f |
| 0x6D14F0 / 0x705EA0 / 0x1BEA7F0 | LocomotionComponent add / get-for-write / get |
| 0x1BD67C0 / 0x1BED4C0 / 0x20064B0 / 0x1BD6FF0 | MovementMachine get state / MoveTo set target / activate / stop |
| 0x1BBD130 / 0x1BBD650 / 0x1BEAA10 / 0x2006580 / 0x1BBD500 | SteeringMachine get state / Steer set yaw / SteerTunnel dir / activate / reset |
| 0x193B550 / 0x193DBA0 / 0x1937780 | SSSteerSystem / steering SyncSystem / MovementSystem bodies |
| 0x1BBD6F0 / 0x1C65700 | Steer state Update / set character rotation |
| 0x1FE0520 / 0x1FDCC60 | ecl::CharacterProtocol update (sends 0xEB/0x24/0xCC/0x2B) / receive (0x34 teleport) |
| 0x1B3E630 | send 0x2F UseMovement(distance) |
| 0x1FA9100 | client character teleport |
| 0x3C78110 / 0x3C77BD0 | TransformSystem SetTransform / SetTranslate |
| 0x35F09A0 / 0x35E74D0 / 0x306EC10 | server teleport (Osiris) / queue / send 0x34 |
| 0x1CAB8E0 / 0x1102510 | can-move check / remaining "Movement" resource |
| 0x1BF6AC0 | speed class -> template speed |
| 0xE4CEC0 / 0xE3C970 / 0xE4AE40 | AI grid flags at pos / blocked test / nearest cell |
| 0x4250910 | TeleportPlayer/Party debug handler |

## Open leads

1. **Runtime test of the override route** (2.1): set the GlobalSwitches override + patch 0x425EEEF, log per
   frame: RunningTask (`InputController+0x50`) and its type (`+0x80`), MoveController CanExecute result,
   0x425E7F0 output, character position. Confirms task selection in KB/M mode and the forward sign (1.2).
2. **Camera entity Transform vs. view override**: confirm the mod writes the HMD pose into the camera
   entity's `ls::TransformComponent` (the yaw source of 0x425E7F0); otherwise pre-rotate the input.
3. **GlobalSwitches `+0xE70`** (skips part of MoveController CanExecute) and **0x1442592C0** (checked when
   the character has no LocomotionComponent): not identified.
4. **0x1CAB8E0** sub-checks 0x1C6DAB0 / 0x1101D50, and whether MoveController is blocked outside the
   player's turn in turn-based mode.
5. **Idle-turn replication**: whether a Steer yaw change (3) reaches the server without a steering-type
   change (0x24 is sent on type change or overlay dirty flag, 0x1FE0520).
6. **Picking ray substitution** for point-to-move: the cursor ray is built inside 0x1C48750 -> 0x1C473D0
   / 0x1C46830 (-> 0x1C40470); which call produces the ray was not settled (shared with E's picking work).
7. **ToggleSneak 0xCD / ClimbingToggle 0xAA / Shove 0xCC handlers** (jump-table dispatch, not found by
   immediate scans); **Jump** trigger route (Spell task vs. server UseSpell); climbing state 5 / ClimbTo
   task 23 internals.
8. **Debug teleport** 0x4250910 / ServerCommand 0xB5 cmd 0xE: gated by byte 0x5F9DEB1 (no writer); server
   acceptance in release not checked.
9. AI grid cell flag bits (`0x40000000400084`) meaning; agent D/H may have the AI grid layout.
10. LocomotionComponent full layout (0x84 bytes; fields used: `+0x00` mag, `+0x04` dir, `+0x10` raw dir,
    `+0x1C` target dir, `+0x28` target pos, `+0x2C` ground height, `+0x34` smoothed offset, `+0x5C` look
    point, `+0x68/+0x74` smoothing quats, `+0x80` timer) - enough for logging, names Unconfirmed.

