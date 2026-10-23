# The character the player is acting with - agent AF, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_G, read-only, -noanalysis, request server
`agents\AF\AFServer.java`) plus `bg3vr_disasm.exe`. RVAs are relative to 0x140000000. Raw output is in
`re-local\ghidra\agents\AF\` (af1..af24). "SE" = Script Extender source (`downloads\bg3se-src`).
**Confirmed** = shown by the cited code. **Unconfirmed** = not shown by code; each one is in the logging plan (section 4).

## Fix table

| need | read / hook | thread | confidence |
|------|-------------|--------|------------|
| **The character the hotbar and the UI act for** (recommended for camera, hand IK, hide-head, gloves) | `mgr = *(u8**)(exe+0x60487A0)`; `pid = *(u16*)(mgr+0xD0)` (= 0x3BAADF0(mgr)); `ui = *(u8**)(*(u8**)(exe+0x6047958)+0x130)`; `vp = 0x1300990(ui+0x878, &pid)` (VMPlayer*); `vc = *(u8**)(vp+0x6B8)` (SelectedCharacter, VMCharacter*); **`handle = *(u64*)(vc+0xA8)`** | game thread (UI update; VMs are written by the gui systems) | Confirmed: the hotbar's UseSlot 0x1718060 and the UseGameObject command 0x159DB00 act with `[[VMPlayer+0x6B8]+0xA8]`; the SelectCharacter command 0x159EA20 compares `VMCharacter+0xA8` with 0x17F5050's handle |
| Fallback (no UI yet, menu, `vc == 0`) | the player manager: `0x17F4FF0(mgr, &h, pid)` (what skeleton.cpp reads now) | game thread | Confirmed (B, U, here) |
| Know at once when the game changes the controlled character | hook entry of **0x17F5100** `SetPlayerCharacter(mgr, u64 handle, u32 player)`; only caller: client net handler 0x1FDCC60, message id 0x23 (`msg+8`) | game thread (net message processing; Unconfirmed which thread runs 0x1FDCC60) | Confirmed |
| Know when the hotbar's character changes | hook entry of **0x141DDB0** `VMPlayer::SetSelectedCharacter(VMPlayer*, VMCharacter*)` | UI update | Confirmed |
| A portrait click (request only) | **0x17F4E80**(mgr, u16 player, EntityRef* clicked) called by the SelectCharacter command 0x159EA20 | UI update | Confirmed (call); meaning of map `mgr+0x140` Unconfirmed |
| Does an entity carry the `ClientControl` tag | **0xA926F0**(EntityWorld*, EntityHandle*) -> bool (type index global 0x6066708) | game thread | Confirmed |
| Do NOT use for "the acting character" | SE `_C()` (first `ClientControl` entity with `ReservedUserID == 1`) | - | Ambiguous by design (section 3) |

Steady state: `handle` (hotbar) == player manager character. They differ only for a short time after a change (section 2.3).

## 1. Who holds "the controlled character", and who writes it

### 1.1 Player manager map (what `GetPlayerCharacter` 0x17F4FF0 returns)

* `0x17F4FF0(mgr, out, player)`: hash map at `mgr+0x130` (count `+0x134`, buckets `+0x138`), node `+0x8` u16 player,
  node `+0x10` character handle; null = `*(u64*)(exe+0x5EEB6E0)`. Pure read. Confirmed [B dec 0x17F4FF0].
* The node's character (`+0x10`) is written only by **0x17F5100** `SetPlayerCharacter` (`FUN_1417fa3b0(mgr+0x130,&id)`
  then `*node = handle`). Confirmed [af1: the map insert 0x17FA3B0 is called by 0x17F5100, 0x17F5CB0, 0x17F7710;
  0x17F5CB0 AddLocalPlayer writes the profile `+0x18` and camera `+0x20`, not the character; 0x17F7710 only reads
  the node (not read in full)].
* 0x17F5100 has **one** caller: the client net message handler **0x1FDCC60**, case `msg+8 == 0x23` (0x1FDD618). The
  message holds a list of 0x28-byte entries: `+0` NetID -> handle (0x4014C00 on `world+0x480`), `+8` user id,
  `+0x20` i16 player id. Confirmed [af3 l.542]. Message name Unconfirmed (SE's enum gives 35 =
  NETMSG_CHARACTER_ACTION_REQUEST_RESULT, which does not fit the payload; SE ids may be stale).
* So the player manager's character changes only when the **server** says so: combat start, a turn passing to
  another party member, a portrait click (after the round trip). The client never writes it itself. Confirmed
  (single writer). That the server sends 0x23 at combat start / turn change is Unconfirmed (the 12:45 run shows one
  switch E9F -> EA4 0.64 s after "combat started"; log line 12728).
* 0x17F5100 then calls the change functors at `mgr+0x100` (count `+0x114`, 0x48 bytes each):
  `f(this, u16 player, EntityHandle* old, EntityHandle* new)`. Confirmed [B dec l.1640].

### 1.2 The hotbar's character: `VMPlayer+0x6B8` SelectedCharacter

* Setter **0x141DDB0** `SetSelectedCharacter(vp, vc)`: stores `vp+0x6B8`, keeps the previous one in `vp+0x600`,
  fires PropertyChanged, refreshes "SelectedCharacterPartyInvList", calls the `vp+0x6D0` functors with
  `{handle of vc (vc+0xC0 -> +0x10 EntityRef)}`, and clears `vp+0x750`. Confirmed [af5].
* Callers (af6): 0x12F4940 (GUI update), 0x1339E00 (VMCharacter refresh), 0x135F290 (player re-init),
  0x141D850 (clear, with 0). Confirmed.
* **Driven by the player manager** (Confirmed):
  - GUI functor **0x1323B50**(provider, u16 player, old*, new*) is registered on **`PlayerManager+0x100`** by
    0x13281F0 (`mov rax,[0x60487A0]` at 0x1328353 -> `[rbp+0x20]` -> `rsi`; `lea rcx,[rsi+0x100]` at 0x1329918;
    handler `lea rax,0x1323B50` at 0x132988A) [disasm]. It finds the VMPlayer (0x1300990), looks up the new
    character's VMCharacter (0x1305570) and stores its handle as **pending** in `vp+0x750` (null if the VMCharacter
    does not exist yet) [af14].
  - GUI update 0x12F4940 then reads `vp+0x750` (getter 0x1389EE0 = `lea rax,[rcx+0x750]`), and if valid calls
    `0x141DDB0(vp, 0x1305570(provider, &h))` (0x12F8E97) [af7b l.1980-1987].
  - 0x1339E00 (VMCharacter refresh): `0x17F5050(provider+0x2B0 = player manager, &h, player)`; if `h == vc+0xA8`
    -> `0x141DDB0(vp, vc)` [af7 l.695-697].
  - 0x135F290 (functor built by 0x13479B0, run from 0x12F4940 when flag `provider+0xF89 & 8`): map `+0x130` lookup
    by `vp+0x128` -> `0x141DDB0(vp, VMCharacter of it)` [af7].
* **The hotbar acts with it** (Confirmed): UseSlot 0x1718060 resolves the caster as
  `0x7841A0(world, [[0x1300990(..)+0x6B8]+0xA8])` [M d10 l.363-370, l.698-709]; DC command 0x159DB00 (UseGameObject
  slot +0x70 of vtable 0x55C2A28) uses `[vp+0x6B8]+0xA8` too [af24].
* `VMCharacter+0xA8` is the character EntityHandle, the same kind as 0x17F4FF0 returns: 0x159EA20 compares
  `vc+0xA8` with 0x17F5050's handle; 0x1339E00 compares `h == vc+0xA8` [af23, af7]. Confirmed.
  `VMCharacter+0xC0 -> +0x10` is the same entity as an EntityRef {handle, world} (0x141DDB0, 0x1339E00). Confirmed.

### 1.3 Clicks, picking and other actions

* Portrait click = command `SelectCharacter` (DC vtable 0x55C2A28, command object at DC+0x2F8 built in ctor
  0x15991B0; handler thunk 0x14B39C8 -> vtable slot +0x78 = **0x159EA20**) [af22, af23, disasm]. It does **not**
  write the player manager's character. If the clicked character differs from 0x17F5050's, it calls
  `0x17F4E80(mgr, player, EntityRef)` (stores a NetID in map `mgr+0x140`) and fires a UI event (0x20C8340 path).
  0x1C57550 is called only when the clicked character belongs to another player (`ecl::Character+0x12C`).
  Confirmed (calls). How the request reaches the server: Unconfirmed. That the switch then arrives as 0x23:
  follows from 0x17F5100 being the only writer.
* Picking 0x1C43820: in controller mode (`byte exe+0x614FBC8 != 0`) the helper reads the player map
  `[0x60487A0]+0x138` by `helper+0x168`; in KB/M mode it only casts the ray (0x1C45A90). Confirmed [af10].
* Spell/target code tests "is this the player's current character" as
  `(*(u32*)(ch+0x12C) & 0xFFFF0000) == 0 && 0x17F4F30(mgr, (u16)ch[0x12C]) == ch` (0x1FA2020, 0x1FBE170,
  0x1FBF240, 0x1FBFF50, 0x1FC00D0, net handler 0x1FDCC60 case 0x72/3). Confirmed [af24, af3 l.1721].
  `ecl::Character+0x12C` = owning local player id (name Unconfirmed).
* End turn 0x1B3E3C0(playerId) and the hotbar both key on the player id (I, M). Confirmed (I/M).

### 1.4 Turn-based combat's current turn

* `ecl::ActiveTurnComponent` type index **0x605379C**; `eoc::TurnBasedComponent` 0x6058DE4 (SE `TurnBased`:
  `IsActiveCombatTurn` byte at +0); `eoc::IsInTurnBasedModeComponent` 0x605BAA4 [G typeidmap]. Who carries them and
  when is Unconfirmed (data in the logging plan).
* UI: `VMPlayer+0x650` IsCurrentTurn, `+0x668` CanEndTurn, `VMCharacter+0x628` InTurnBasedMode (AB). Confirmed (AB).
* Nothing found that makes the hotbar follow the turn character directly; the hotbar follows the player manager
  (1.2). That the server switches the player manager to the turn's party member: Unconfirmed (one switch in the
  12:45 log).

## 2. Why GetPlayerCharacter and the UI can differ

1. **Order of updates** (Confirmed): 0x23 -> 0x17F5100 writes the map, then the functor 0x1323B50 sets the pending
   `vp+0x750`; the hotbar's `vp+0x6B8` changes only in the next GUI update 0x12F4940. The map leads, the hotbar
   lags. How many frames: Unconfirmed (log it).
2. **New character without a VMCharacter yet** (Confirmed): 0x1323B50 then stores null in `vp+0x750`, so the hotbar
   keeps the **old** character until 0x1339E00 builds the VMCharacter and selects it. Duration Unconfirmed.
3. **Not a game cause, a mod cause** (Confirmed in the repo): the camera follows the player manager
   (`skeleton.cpp controlled_raw`), but FirstPerson.lua (hide-head), VRAuto.lua, Combat.lua, Holster.lua and
   CameraProbe.lua use SE `_C()` or the 45-tick "steady" `BG3VR.Controlled()`. `_C()` is not unique in combat
   (section 3), so body parts and the camera can sit on two different party members. This fits "the camera moved to
   EA4 but I acted with the other": Unconfirmed until the plan's log shows both values.
4. A real disagreement between the hotbar and the player manager that lasts more than a few frames was **not**
   found in code. If the log shows one, its cause is still Unconfirmed.

## 3. SE `_C()` and `ClientControl`

* SE client `_C()` (`BuiltinLibraryClient.lua` l.28): `Ext.Entity.GetAllEntitiesWithComponent("ClientControl")`,
  returns the **first** with `ClientCharacter.ReservedUserID == 1`. Order = ECS storage order. Confirmed (source).
* `ClientControl` = tag `eoc::ClientControlComponent`, type index **0x6066708** [G typeidmap].
  Test: **0xA926F0**(world, &handle) (pending changes at `world+0x340` via 0x400C8A0, then the entity's component bit
  in `world+0x210` storage). Confirmed [af12].
* The client adds/removes it only through generic per-component handlers (vtable slots 0x91DCE0, 0x91E150,
  0xB00C10, deserialize 0x2583190 / serialize 0x2582D90). No gameplay code was found that sets it for "the selected
  character". Whether it is replicated from the server or save data: Unconfirmed.
* The game's own query 0x606645C is `Spec<ecl::Character, ClientControl, Player, Level, TurnBased>` - a set of
  characters, and 0x1B022B0 iterates entities with `ClientControl && user == 1`. So ClientControl marks the
  characters this client may control, not one acting character. Strongly suggested by code, Unconfirmed as a rule;
  the repo's own note (Controlled.lua: `_C()` flipped between two party members four times a second in combat)
  matches it.
* Verdict: `ClientControl`, `_C()` and `GetHostCharacter` are **not** the right source. Use 1.2 (or 1.1).
  Lua equivalent of the recommended read: the hotbar's VM is not exposed natively to Lua; send the native handle to
  Lua over the bridge instead, and drop `_C()` from FirstPerson / VRAuto / Combat / Holster.

## 4. One-run logging plan

Run: load, walk, start a fight, let the turn pass to another party member, click a portrait out of combat and in
combat (shared turn), end turn, leave combat. All on the game thread (the skeleton per-frame code), under `__try`.

Each frame, log a line **when any value changes** (and once a second a summary with the same fields):
1. `pid = *(u16*)(mgr+0xD0)` and `0x3BAADF0(mgr)`.
2. `pm = 0x17F4FF0(mgr, pid)`.
3. `vp = 0x1300990(ui+0x878, &pid)`, `vc = [vp+0x6B8]`, `sel = vc ? [vc+0xA8] : 0`, `pending = [vp+0x750]`,
   `prev = [vp+0x600] ? [[vp+0x600]+0xA8] : 0`, `IsCurrentTurn = byte [vp+0x650]`, `CanEndTurn = byte [vp+0x668]`,
   `InTurnBasedMode = vc ? byte [vc+0x628] : -`.
4. `cc(pm)`, `cc(sel)` with `cc(h) = 0xA926F0(world, &h)`; world = the camera controller's `+0x320`.
5. What the camera follows (`g_followed_entity`) and the handle hide-head / gloves / hand IK use.
6. Frame counter, so lags (2.1, 2.2) can be counted in frames.

Hooks (entry, log and pass through; all game code is left unchanged):
* **0x17F5100** SetPlayerCharacter: `rdx` new handle, `r8d` player, old = 0x17F4FF0 before the call, frame number.
* **0x141DDB0** SetSelectedCharacter: `rcx` VMPlayer, `rdx` VMCharacter, `[rdx+0xA8]`, return address
  (0x12F8E9C = pending path, 0x133AE51 = VMCharacter refresh, 0x141D9F5 = clear; 0x135F290 tail-jumps in, so the
  re-init path shows its caller's return address).
* **0x17F4E80** (portrait click request): player `dx`, `[r8]` handle.
* Optional: 0x1FDCC60 entry when `*(u32*)(r9+8) == 0x23`: entry count `*(u32*)(r9+0x34)` and each entry's player
  `+0x20`, to tie each switch to a server message.

Lua side, same run (Tick, log on change): every `ClientControl` entity with `ClientCharacter.ReservedUserID`,
`UserReservedFor.UserID`, `TurnBased.IsActiveCombatTurn`, `IsInTurnBasedMode` presence; and what `_C()` returned.

What the log decides:
* `sel` vs `pm`: how long and when they differ (expect: `pm` first, `sel` 1+ frames later; longer if `pending` was 0).
* Whether the server's 0x23 switch happens at combat start / turn change / click (1.1, 1.3 Unconfirmed items).
* How many entities carry ClientControl in and out of combat (section 3).
* Which handle each VR part followed when the user felt "the other character" was controlled.

## Addresses used

| RVA | what |
|-----|------|
| 0x60487A0 | PlayerManager* |
| 0x3BAADF0 | GetPrimaryPlayer (slot 0) = `[mgr+0xD0]` |
| 0x17F4FF0 / 0x17F5050 | player -> character handle / EntityRef (map +0x130) |
| 0x17F4F30 / 0x17F4F90 | player -> character component / ecl::Character* |
| 0x17F5100 | SetPlayerCharacter (only writer of the map's character) |
| 0x1FDCC60 | client net handler; case 0x23 -> 0x17F5100 |
| 0x17F4E80 | store requested character for a player (map +0x140) |
| 0x1300990 | VMProvider (GameUI+0x878): VMPlayer by player id |
| 0x140CE90 / 0x141DDB0 | VMPlayer SelectedCharacter getter / setter (+0x6B8) |
| 0x1389EE0 | VMPlayer pending select handle (+0x750) |
| 0x1323B50 | player-manager change functor (registered at mgr+0x100 by 0x13281F0) |
| 0x12F4940 | GUI update (applies +0x750) |
| 0x1339E00 | VMCharacter refresh (selects it if it is the player's character) |
| 0x135F290 / 0x13479B0 | re-init: select the player manager's character |
| 0x1305570 | VMCharacter by entity handle |
| 0x159EA20 | SelectCharacter command (portrait click) |
| 0x1718060 | hotbar UseSlot (acts with `[[vp+0x6B8]+0xA8]`) |
| 0xA926F0 | has ClientControl (type 0x6066708) |
| 0x605379C / 0x6058DE4 / 0x605BAA4 | ActiveTurn / TurnBased / IsInTurnBasedMode type indices |
