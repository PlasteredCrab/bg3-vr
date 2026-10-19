# BG3 split-screen / player-view system (agent B, static RE)

Build: bg3_dx11.exe 4.1.1.7631656, image base 0x140000000, all addresses are RVAs.
Method: Ghidra headless on project BG3_B (read-only) through a query server
(`agents/B/scripts/BServer.java`); raw outputs in `agents/B/req/*.out`, full decompiles in
`agents/B/dec/*.c` (file names are cited per section). Nothing was run. Every statement below is
backed by decompiled code or disassembly unless it is marked **unconfirmed**.

---------------------------------------------------------------------------------------------------

## 0. Short answer and recipe

### What the game does for a second local player (confirmed chain)

```
input/profile event ─► 0x17F73F0 join(mgr, device)                    [pops free input slot 1..3]
                       └► 0x17F6AC0 (mgr+0xF0 iface, 1, device, &profileId)
                          └► 0x17F5CB0 AddLocalPlayer(mgr, slot, &profileId)
                             ├► 0x3BAA380 base AddLocalPlayer(mgr, slot)      → new player id
                             │   ├ listeners +0x28 (slot assigned)  ─► player_management::TrackingSystem
                             │   └ slot!=0: 0x3BAA1A0 AddPlayerView(mgr,id)  (or reuse a pooled view)
                             │        ├ [deferred to command queue [2] of DAT_14604B1C0 if needed]
                             │        ├ view = mgr->vtbl[1](mgr, mainFrame) = 0x17F82F0
                             │        │     alloc 0x2D90, ctor 0x17F1E60(view, frame, scale=(0,1), offset=(0,0), cfg)
                             │        │     0x4073230(frame, view, 0)    (add to RenderFrame + stage-manager record)
                             │        ├ view[0x2268] = 1                  (split-screen index)
                             │        ├ if id != slot-0 player: WriteState.Enabled(+0x21E0)=0, [[+0x48]+0x80]=0
                             │        ├ pipeline->vtbl[+0x2C0](pipeline, view) = 0x588E50 (stage build)
                             │        ├ mgr[+0xD8 or +0xE0] = view; map(+0x30)[id].view = view
                             │        └ set(+0x40) += id   ─► next PlayerManager::Update 0x17F4BF0 fires
                             │                                listeners +0x30 (view ready) ─► RenderViewTrackingSystem
                             ├ creates camera entity "GameCamera_playerN" (ls::CameraComponent …), stores it in map(+0x130)
                             ├ 0x17F7E10: attach that camera to the view (0x17F7CD0 → 0x3CC90B0 SetCameraController, +0x2CA0)
                             ├ 0x17F8160: scene = [level+0x28] → 0x3CCC550; level atmosphere/lighting 0x3CC9D30/0x3CCAFA0
                             ├ profile manager vtbl+0xF8 (assign profile to slot/player), input bindings …
                             └ listeners +0x08 (player added)
server message 0x23 ─► 0x1FDCC60 ─► 0x17F5100 SetPlayerCharacter(mgr, characterHandle, playerId)
ECS each frame:  TrackingSystem / RenderViewTrackingSystem → SplitReasonsSystem → GoalSystem →
                 AnimationSystem → ViewSystem → RenderViewportSystem (sets Scale/Offset/Enabled of both views)
```

### Recipe for an extra, fully set-up view without a second controller (game functions only)

This is the game's own add-player-view (0x3BAA1A0) minus the player bookkeeping. Every call is a
game function that the game itself calls in this order; preconditions are what those call sites
guarantee.

1. **Thread.** Run on the thread that owns command queue `[(DAT_14604B1C0+0x148)]->[2]`: 0x3BAA1A0
   tests `queue2->vtbl[1]()` and, if it returns true, re-queues itself (executor 0x3BAB450) instead
   of touching views. The view creators of the holder class (0x1B75A90) and the view destroyer
   (0x3BA9E80) use the same queue [2]. Which OS thread that is: **unconfirmed** (most likely the
   render thread; confirm by reading the executor that drains queue [2], or by logging
   `GetCurrentThreadId()` in a hook of 0x3BAB450). The mod's `!vr_views build` already runs the
   setup on the render thread and that worked.
2. **Main RenderFrame**, exactly as 0x3BAA1A0 / 0x3AC7600 compute it:
   `pipeline = *(u8**)0x146046F98; idx = *(int*)(pipeline+0x98)` (−1 → no frame);
   `frameMgr = *(u8**)( *(u8**)(*(u8**)(pipeline+0x1D8) + 0x30) + (u32)DAT_14604C6FC * 0xF8 )`;
   `frame = ((u8**)*(u8**)(frameMgr+0x18))[idx]`. (Equivalent: call 0x3AC7600(pipeline).)
3. **Create the view:** `view = 0x17F82F0(mgr, frame)` where `mgr = *(void**)0x1460487A0`
   (the manager's vtable slot 1; any first argument works, it is unused). It allocates 0x2D90,
   constructs an `ecl` GameRenderView (vtable 0x55F42F8) with the same shadow/cfg block the game
   uses for player views, scale (0,1), offset (0,0), and **adds it to the frame** with
   `0x4073230(frame, view, 0)` (own view-list entry and stage-manager record, the step whose absence
   caused the earlier stage-registration failures). Because cfg+0x60 = 1, the base ctor gives the
   view its own occlusion helper (+0x48) when `[frame+0x40]==0`.
4. **Split-screen index:** `*(u8*)(view+0x2268) = 1` (0x3BAA1A0 does this for every player view).
5. **Stage build:** `pipeline->vtbl[0x2C0/8](pipeline, view)` = 0x588E50 (calls 0x3AC4EA0 then
   adds the game stages). Same thread as step 1.
6. **Camera:** the game issues this from AddLocalPlayer (0x17F5CB0 → 0x17F7E10 → 0x17F7CD0); when
   the view is not registered yet, 0x17F7CD0 re-queues itself on command queue [0] (thread
   **unconfirmed**). What it does on the view is:
   `if (*(u64*)(view+0x2C98)==0) 0x3CC90B0(view, controller, keep); *(u64*)(view+0x2CA0)=controller;
   *(u8*)(view+0x2962)=p6`. `controller` = `*(ptr*)(ls::CameraComponent + 8)` of a camera entity.
   0x3CC90B0 stores it at +0x2260 (detach old via ctrl vtbl+0x10, attach new via ctrl vtbl+0x08),
   then recomputes the viewports (0x407BE10).
7. **Scene and environment:** `0x3CCC550(view, scene)` with `scene = *(level+0x28)`,
   `level = *(DAT_146007A28+0x90)`; then `0x3CC9D30(view, *(*(level+0x18)+8)+0x28, &fs, 0, 0)` and
   `0x3CCAFA0(view, *(*(level+0x18)+8)+0x24, &fs, 0, 0)` with `fs = -1` (0x17F8160).
8. **Viewport and enable:** `0x407B800(view, &scale{w,h}, &offset{x,y})` (the call
   RenderViewportSystem makes; writes WriteState +0x21E8 scale and +0x21F0 offset, clamps, then
   recomputes pixel viewports), then `*(u8*)(view+0x21E0) = 1` and, if `*(ptr*)(view+0x48)`,
   `*(u8*)(*(ptr*)(view+0x48)+0x80) = 1` (exactly what RenderViewportSystem writes per player view).
9. **Do not** register it in the manager's maps or notify the listeners if you do not want the
   ECS split-screen systems to drive it (see §8.2); RenderViewportSystem only ever touches the two
   views it finds through `PlayersSingletonComponent` player ids (0x3BAAE20 lookups).
10. **Teardown** (game's way, 0x3BA9E80): on the queue-[2] thread `0x40733E0(frame, view)` then
    `view->vtbl[0](view, 1)`.

The previous approach (copying the special view) bypassed steps 3-4: it never ran the GameRenderView
ctor, had no frame registration, shared the special view's job context/stage array, had index 0 and
no player-view cfg. Steps 3-5 are the game's own replacement for all of those hacks.

### Can a fake second local player be added instead?

Mechanically yes: `0x17F5CB0(mgr, slot, &profileId, 0)` with a free slot (1..3) does everything,
and `0x17F73F0(mgr, device)` does the whole join including the slot and the profile. But then:

* The ECS treats it as a real player: TrackingSystem creates a `player_management::UserComponent`
  entity; `PlayerIdToPlayerInputIndexMapping` lists it; SplitReasonsSystem picks it as player B.
* It has **no character** until the server assigns one (message 0x23 → 0x1FDCC60 → 0x17F5100);
  a local-only player gets none. With no character for B, GoalSystem keeps `Merged=1`, ViewSystem
  sets mode 1 ("A full-screen") and RenderViewportSystem then writes **Enabled=0 into B's view**
  (and scale (1,1)) whenever its state changes. So the fake player's view is turned off by the game.
* Profile/input: 0x17F5CB0 calls the profile manager (vtbl+0x108/+0xF8) with the profile id and the
  input manager (0x40F3BA0) assigns a device to the slot; with no device/profile these paths are
  untested (**unconfirmed** behaviour).
* Many systems iterate local players (sound listener, camera, UI) — side effects unknown.

So for VR an extra view via the recipe above is the clean path; a fake player brings the ECS
split-screen machinery, which would fight the mod over the viewport/enable state.

---------------------------------------------------------------------------------------------------

## 1. Globals and objects

| Global (RVA) | What it is | Evidence |
|---|---|---|
| `DAT_1460487A0` | **player manager** (derived class, 0x1F0 bytes, vtable 0x55F4808). Engine init 0x3AA8EE0 first creates the base (0xF0 bytes, ctor 0x3BA9AB0); client init 0x1818E40 deletes it and creates the derived one (ctor 0x17F46C0). | globals_init.c lines 109-120, 1106-1120 |
| `DAT_1460488A0` | **player-manager listener registry** (0x20 bytes, vtable 0x551EA58): +0x08 array of listener pointers (each = object+8), +0x14 count, +0x18 SRW lock | 0x3AA8EE0, 0x3C14C90 |
| `DAT_146046F98` | rendering pipeline (game class, vtable 0x5499F58; base ctor 0x3ABCE00): +0x98 main frame index, +0xA0 main view, +0xA8 special view, +0x1D8 world/frame manager, vtable +0x2C0 = 0x588E50 setup, +0x288 = 0x5889E0 per-frame update | 0x3BAA1A0, refs 0x588E50/0x5889E0 |
| `DAT_14604B1C0` | job/thread system (created in 0x40F5BD0, init 0x40F8390); +0x138 SRW lock, +0x148 array of command queues; queue vtbl+8 = "must defer?"; queue +0x10 allocator, +0x40 critical section | 0x3BAA1A0, 0x17F7CD0 |
| `DAT_14604B1B8` | **ls::gGlobalSwitches** (pointer) | SE BinaryMappings pattern `mov rax,[gGlobalSwitches]; cmp [rax+111h]` found at 0x5CD1BE and 0x30A2995 |
| `DAT_145FFD8B8` | profile/user manager (vtbl +0x20 current profile?, +0x28 current profile id, +0x48, +0x90 find profile by name, +0xC0 create, +0xF8 assign profile to player, +0x108 get profile) | 0x17F73F0, 0x17F5CB0 (names **unconfirmed**) |
| `DAT_14604B1A8` | input manager (0x40F3BA0 assign device→slot, 0x40F4060, +0x180 device→slot map, +0x398 active device) | 0x17F73F0, 0x17F6AC0 (**names unconfirmed**) |
| `DAT_146007A28` | level manager: `+0x90` current level; level+0x28 scene, level+0x18 → +8 → env (+0x24 lighting, +0x28 atmosphere ids) | 0x17F8160 |
| `DAT_145FDA240` | ecl client (EoCClient): +0x1A8 ECS world, +0x98, +0x90 game state, +0x1C0 input bindings | 0x17F5CB0 |
| `DAT_146047958` | client-side UI/manager object: +0x88 (vtbl+0x40 "player added", +0x48 "player removed"), +0x130, +0x140 | 0x17F5CB0, 0x17F63F0 |
| `DAT_146008BC0` | second listener registry: "player character changed" listeners (called from 0x17F5100) | 0x17F5100 |

---------------------------------------------------------------------------------------------------

## 2. The player manager

Decompiles: `dec/mgr_eoc.c` (0x3BA9E00-0x3BB600), `dec/mgr_eoc_ctor.c`, `dec/mgr_ecl.c`
(0x17F3000-0x17FB800, all 86 functions).

### 2.1 Classes and vtables

* Base (eoc/ls level, name **unconfirmed**): ctor 0x3BA9AB0, dtor 0x3BA9DC0 / deleting 0x3BA9D80,
  vtable 0x58A4F48: `[0] 0x3BA9D80 delete`, `[1] 0x3BAAF90 CreatePlayerView(frame)` (allocates a
  plain rf::RenderView 0x2BE0 with base ctor 0x3CC5AA0, same cfg block, then 0x4073230(frame, view, 0)).
* Derived (ecl level, name **unconfirmed**; no RTTI in this build): ctor 0x17F46C0, dtor 0x17F4A90.
  * primary vtable 0x55F4808: `[0] 0x17F4A90 dtor`, `[1] 0x17F82F0 CreatePlayerView(frame)` (allocates
    an ecl GameRenderView 0x2D90; see §3).
  * +0xF0 interface vtable 0x55F4800: `[0] 0x17F6AC0` (input/profile event → add player, see §4).
  * +0xF8 interface vtable 0x55F4818: `[0] 0x17F9490` (message-box result handler for split-screen
    join errors), `[1] 0x579DA0`.

### 2.2 Layout (offsets from the object)

Base part (0x3BA9AB0):

| Off | Type | Meaning | Evidence |
|---|---|---|---|
| +0x00 | vtbl | | |
| +0x08 | u16 | next new player id (starts 1) | 0x3BAA380 |
| +0x10/+0x18/+0x1C | u16[] buf/cap/count | recycled player ids | 0x3BAA380, 0x3BAA680 |
| +0x20/+0x28/+0x2C | u16[] buf/cap/count | **all local player ids** | 0x3BAA380, 0x3BA9FC0 |
| +0x30 (count +0x30, buckets +0x34 n, +0x38 ptr) | hash map u16 id → {u8 slot (+0x10 of node), RenderView* (+0x18)}; 37 buckets | **player → (input slot, view)**; 0x3BAAE20 lookup; insert 0x3BAB270 (node 0x20, slot default 0xFF) |
| +0x40..+0x6C | hash set u16 (index +0x40, keys +0x60, count +0x6C) | players whose view was just created; drained by 0x17F4BF0 → listeners +0x30 | 0x3BAA1A0 (`FUN_140795c20(param_1+8,…)`), 0x17F4BF0 |
| +0x70 (count), +0x74 n, +0x78 buckets | hash map s8 slot → u16 player id (node +8 slot, +0xA id) | **input slot → player**; slot 0 = primary player; insert 0x3BAB350 | 0x3BAACE0, 0x3BAADF0 |
| +0x80/+0x88/+0x8C | RenderView*[] buf/cap/count | **pool of spare views** (views of removed players) | 0x3BAA680, 0x3BAA380 |
| +0x90..+0xC8 | hash map u16 → RenderView* (index +0x90 size +0x98, next +0xA0, keys +0xB0, count +0xBC, values +0xC0) | player → pooled view (consulted by 0x3BAAE20 as fallback) | 0x3BAA680, 0x3BAAE20 |
| +0xD0 | u16 | player id of input slot 0 | 0x3BAA380, 0x3BAA680 |
| +0xD8 / +0xE0 | RenderView* | view of player id 1 / of the other player | 0x3BAA1A0, 0x3BAA380 |
| +0xE8 / +0xE9 | s8 | input slot of player id 1 / of the other player (0xFF none) | 0x3BAA380, 0x3BAA680 |

Derived part (0x17F46C0):

| Off | Meaning | Evidence |
|---|---|---|
| +0xF0, +0xF8 | interface vtables (above) | ctor |
| +0x100..+0x114 | array of 0x48-byte functors called on character change (count +0x114) | 0x17F5100 |
| +0x120..+0x12C | array | ctor (use **unconfirmed**) |
| +0x130 (count), +0x134 n=3, +0x138 buckets | hash map u16 id → node 0x30: +0x10 character entity handle (u64, null 0xFFC0000000000000), +0x18 int profile/user id (-1), +0x20 camera entity handle (16 bytes; default DAT_145EF3A08/10) | 0x17FA3B0, 0x17F5CB0, 0x17F4F30, 0x17F7E10 |
| +0x140 (n=37 buckets +0x148) | hash map u16 → u64 | 0x17F4E80, 0x17F5100 (use **unconfirmed**) |
| +0x150/+0x158/+0x15C | s8[] **free input slots**, initialised {1,2,3}, kept sorted | ctor, 0x17F6850 (push back), 0x17F73F0 (pop) |
| +0x160 | s8 pending join slot (0xFF none) | 0x17F73F0, 0x17F6AC0 |
| +0x162 | s16 pending device | 0x5E3720 |
| +0x168/+0x170/+0x174 | u16[] players queued for removal | 0x17F6320 |
| +0x178 | u8 join-dialog state (5, 6) | 0x17F8940, 0x17F9490 |
| +0x179 / +0x17A | u8 flags | 0x17F8860, 0x17F4BF0 |
| +0x180..+0x1B0 | map u16 id → character handle pending component write | 0x17F5100, 0x17F9C60 |
| +0x1C0..+0x1EC | set of entity handles pending component removal | 0x17F63F0, 0x17F9C60 |

### 2.3 Methods (all functions of both classes)

Base (0x3BA9AB0 … 0x3BB470):

| RVA | What it does (confirmed from code) |
|---|---|
| 0x3BA9AB0 | ctor (fields above) |
| 0x3BA9D80 / 0x3BA9DC0 | deleting dtor / dtor |
| 0x3BA9E80 | DestroyView(view): if queue[2] says defer → queue 0x3BAB430; else `0x40733E0(frame, view)` (remove from main frame) and `view->vtbl[0](view,1)` |
| 0x3BA9FC0 | RemoveAllButPrimary: calls 0x3BAA680(id, 1) for every player except slot 0's, destroys all pooled views (0x3BA9E80), clears +0x90 map and pool. Caller 0x1867C60 |
| 0x3BAA1A0 | **AddPlayerView(id)** — see §3.2 |
| 0x3BAA380 | **AddLocalPlayer(slot)** → id — see §4.2 |
| 0x3BAA680 | **RemoveLocalPlayer(id, notify)** — see §5 |
| 0x3BAACE0 | GetPlayerForSlot(slot) (map +0x70) |
| 0x3BAAD20 | GetPlayerForDevice(device) via input manager map (DAT_14604B1A8+0x180) then +0x70 |
| 0x3BAADB0 | GetSlotOfPlayer(id) (map +0x30 node+0x10; 0xFF if none) |
| 0x3BAADF0 | GetPrimaryPlayer() (= player of slot 0) |
| 0x3BAAE20 | **GetPlayerView(id)**: map +0x30 → node+0x18; else pooled map +0x90/+0xC0; else 0. 69 callers |
| 0x3BAAEE0 | IsPlayerXxx(id): `DAT_146047958+0x130` object vtbl+0x40 and vtbl+0xB8(slot) (meaning **unconfirmed**) |
| 0x3BAAF90 | base vtable [1] CreatePlayerView (rf::RenderView, 0x2BE0) |
| 0x3BAB100 | SetPlayerSlot(&id, &slot): updates +0xE8/+0xE9 and map +0x30 slot, fires listeners +0x28 |
| 0x3BAB1C0 / 0x3BAB210 | iterator over map +0x30 skipping views whose ReadState flag (+0x2158) or frame flag (+0x42F0) is 0; used by 0x19E0AE0 |
| 0x3BAB270 / 0x3BAB350 | map insert helpers (+0x30, +0x70) |
| 0x3BAB430 / 0x3BAB450 | queued thunks for 0x3BA9E80 / 0x3BAA1A0 |

Derived (0x17F3xxx-0x17FBxxx) — functions of the player manager (others in the range belong to
GameRenderView, see §3.4):

| RVA | What it does |
|---|---|
| 0x17F46C0 / 0x17F4A90 | ctor / dtor |
| 0x17F4BF0 | **Update** (called every frame by pipeline slot 0x5889E0): fires listeners +0x30 (view ready) for ids in set +0x40 and clears it; handles flag +0x17A (re-adds primary player via 0x1C3FCF0); 0x17F9C60 (flush character→player-id component writes); for every id in `ecl::multiplayer::DestroyPlayerRequestsSingletonComponent` (query DAT_1460A4B34) calls 0x17F6320 |
| 0x17F4E80 | store mapping in map +0x140 for a character |
| 0x17F4F30 / 0x17F4F90 | character component of player (two different component getters) |
| 0x17F4FF0 / 0x17F5050 | character handle of player |
| 0x17F5100 | **SetPlayerCharacter(handle, id)** (caller: network handler 0x1FDCC60 case 0x23); returns if `id & 0xFFFF0000`; notifies DAT_146008BC0 listeners and the +0x108 functors; writes ECS component (via +0x180) |
| 0x17F5CB0 | **AddLocalPlayer(slot, &profileId, ?)** — §4.2 |
| 0x17F6320 | RequestRemovePlayer(id): if `[[DAT_145FDA240+0x98]+0x418] != -1` queues id at +0x168 and posts message type 0xB {id} (0x4031600); else removes immediately (0x17F63F0) |
| 0x17F63F0 | **RemovePlayer(id)** — §5 |
| 0x17F6850 | return input slot to the free list (+0x150), sorted |
| 0x17F6950 | set profile id of a player |
| 0x17F69F0 | (helper, not read) |
| 0x17F6A40 | find player by profile id |
| 0x17F6AC0 | +0xF0 interface: input/profile event (see §4.1) |
| 0x17F6D50 / 0x17F6DA0 | camera entity handle of player (node+0x20) / of focused player |
| 0x17F6F00 / 0x17F7020 | MergedFocus helpers (query `MergedFocusSingletonComponent`) → focused player id |
| 0x17F7230 / 0x17F72A0 / 0x17F7330 | view of focused player (0x3BAAE20) |
| 0x17F73F0 | **Join(device)** — §4.1 |
| 0x17F76A0 | primary player if a condition on DAT_145FDA240+0x88 holds |
| 0x17F7710 | per-player sound/listener setup (0x1C53F50 / 0x1C54590); may defer to queue [0] |
| 0x17F7A00 | pick a player id (primary, or first non-primary) |
| 0x17F7AD0 / 0x17F7B90 / 0x17F7C60 / 0x17F85A0 / 0x17F86C0 / 0x17F9960 | iterators over map +0x130 |
| 0x17F7CD0 | **SetPlayerViewCamera(id, controller, keep, immediate, p6)** — view = 0x3BAAE20(id); if none and !immediate → queue [0] (0x17FA690); else `if(view+0x2C98==0) 0x3CC90B0(view, controller, keep)`; `view+0x2CA0 = controller; view+0x2962 = p6` |
| 0x17F7E10 | attach player's camera entity: `ctrl = *(0x17FAB50(world, cameraHandle) + 8)` (component `ls::CameraComponent`), 0x17F7CD0; optional extra (p5) |
| 0x17F8160 | **SetPlayerViewScene(id, scene, immediate)** — view = 0x3BAAE20(id); if none and !immediate → queue [0] (0x17FA6C0); else 0x3CCC550(view, scene), 0x3CC9D30 (atmosphere), 0x3CCAFA0 (lighting) |
| 0x17F82F0 | vtable [1] CreatePlayerView — §3.1 |
| 0x17F8460 / 0x17F8500 | iterate players with a functor |
| 0x17F87E0 | character component of primary player |
| 0x17F8860 / 0x17F8940 / 0x17F9490 | join/split-screen error dialog (localized handles h0434b959…, h1e904717…, h82ca9ca2…) |
| 0x17F93B0 | device-check for a player (DAT_14614FBC8 gate) |
| 0x17F9880 | reassign device to primary player |
| 0x17F9A40 | rumble/feedback for a player's slot (DAT_146047958+0x88) (**unconfirmed**) |
| 0x17F9C10 | player exists in map +0x130 |
| 0x17F9C60 | flush pending character→player-id component (type id DAT_146052E44) |
| 0x17FA020 | select active input device for the primary player |
| 0x17FA1F0 / 0x17FA2D0 / 0x17FA3B0 / 0x17FA4B0 | map helpers (+0x140, dtor, +0x130 insert, characters of a profile) |
| 0x17FA670 / 0x17FA690 / 0x17FA6C0 | queued thunks for 0x17F7710 / 0x17F7CD0 / 0x17F8160 |
| 0x17FAB50 | get component `ls::CameraComponent` (type DAT_146060A08) of entity |

---------------------------------------------------------------------------------------------------

## 3. Player views

### 3.1 Creation: vtable slot 1 = 0x17F82F0(mgr, frame) (disassembly checked)

```
view = alloc(0x2D90, 16)
cfg (0x64 bytes on stack) = 16 bytes at 0x55F4830..0x55F488F + word 0x0101 at cfg+0x60
     ints 0x2000,0x800,0x80,0x400 / floats 40,55 / 0x100 / 8.0 / 0x1000,0x400,0x80,0x200 /
     20,35 / 0x80 / 16 / 0x800,0x200,0x80,0x100 / 20,25 / 0x40 / 24  (cascaded-shadow style table, meaning unconfirmed)
0x17F1E60(view, frame, xmm2 = 0.0, xmm3 = 1.0 (0x591CDE8), [rsp+20] = 0.0, [rsp+28] = 0.0, [rsp+30] = &cfg)
0x4073230(frame, view, 0)        ; add to RenderFrame (not special)
return view
```

* 0x17F1E60 (GameRenderView ctor) → 0x3CC5AA0 (rf::RenderView ctor) → 0x407B080 (root ctor).
  0x407B080 passes (p3,p4) → `+0x21E8` and (p5,p6) → `+0x21F0` via 0x407B800. 0x407B800 clamps
  `+0x21F0` to [0,1] and `+0x21E8` to [0, 1-(+0x21F0)], so **+0x21E8 = Scale, +0x21F0 = Offset**
  of WriteState (+0x21E0). A new player view therefore starts with Scale (0, 1), Offset (0, 0):
  zero width until RenderViewportSystem lays it out.
* Compare the pipeline's main view: 0x582BD0 calls the same ctor with scale (1,1), offset (0,0) and
  `0x4073230(frame, view, isSpecial)`; the base pipeline 0x3AC7500 does the same with the rf ctor.
* The base ctor copies cfg into view+0x2B70..+0x2BD3; cfg+0x60 = 1 → `[view+0x2BD0] = 1`, so the
  occlusion helper (+0x48, ctor 0x41A1BD0, 0x160 bytes) is created when `[frame+0x40] == 0`
  (viewctor_base.c tail). The main view gets it the same way; the special view's copy had 0.
* GameRenderView part (0x17F1E60): vtable 0x55F42F8 (+0x2280 secondary 0x55F4380); clears
  +0x2BE0.., +0x2C00..+0x2CA0, see-through blocks +0x2CA8/+0x2CB8 (0x17F3760), focus blocks
  +0x2CC8/+0x2D00, 5 light slots +0x2D38..; +0x2C80 = 0x80-byte constant buffer (0x40D0C30).

### 3.2 AddPlayerView 0x3BAA1A0(mgr, id) (playerview: `addplayerview.c`, `dec/mgr_eoc.c`)

1. `q = (DAT_14604B1C0+0x148)[2]` (under SRW +0x138); if `q->vtbl[1](q)` → push {0x3BAB450, mgr,
   id} (0x18 bytes) into q's buffer under q+0x40 critical section and return.
2. frame = main frame (pipeline+0x98 index; 0 if -1).
3. `view = mgr->vtbl[1](mgr, frame)`.
4. `view[0x2268] = 1`.
5. if `id != primary` (slot-0 player): `view[0x21E0] = 0`, `[[view+0x48]+0x80] = 0`.
6. `pipeline->vtbl[0x2C0/8](pipeline, view)` (0x588E50).
7. `mgr[+0xD8]` (id 1) or `mgr[+0xE0]` = view; `map30[id].view = view`; `set40 += id`.

No camera, scene or listener call here; those come from 0x17F5CB0 (camera/scene) and
0x17F4BF0 (listeners +0x30 on the next manager update).

### 3.3 View fields used by this system

| Off | Meaning | Writers / readers |
|---|---|---|
| +0x10 | owning RenderFrame | ctor |
| +0x48 | occlusion helper; +0x80 = enabled byte | 0x3BAA1A0, 0x192ED80, 0x3BAA680, 0x1B76070 |
| +0x2158 | ReadState (enabled byte at +0) | iterator 0x3BAB1C0 |
| +0x21E0 | WriteState: +0 Enabled (u8), +0x8 Scale (2×f32), +0x10 Offset (2×f32) | 0x407B800, 0x192ED80, 0x3BAA1A0 |
| +0x2260 | camera controller | 0x3CC90B0 |
| +0x2268 | split-screen index (u8) | 0x3BAA1A0 (=1); ~60 stage readers |
| +0x226C | **viewport side** (int, 0 = player A/left, 1 = player B/right) | written by RenderViewportSystem 0x192ED80 in split mode; read by stage code 0x3D30E40, 0x3DD21D0, 0x3E16830, 0x3E17490 |
| +0x2270 | view index | ctor |
| +0x2274 | byte set by the holder class (0x1B75D50); read 0x3DFB6A0 | |
| +0x2962 | byte p6 of 0x17F7CD0 | |
| +0x2B10 | scene | 0x3CCC550 |
| +0x2BD0 | cfg flag → occlusion helper | base ctor |
| +0x2C98 | camera override (if set, 0x17F7CD0 does not change +0x2260) | 0x17F2B30, 0x1D11AE0, 0x1B77520 |
| +0x2CA0 | game camera controller of this view | 0x17F7CD0, 0x17F2B30 |
| +0x2CC8 / +0x2D00 | see-through data of own / other player (filled by vtbl slot 2 0x17F2230 from `ls::SeeThroughStateComponent`: entry goes to "own" only if `0x3BAAE20(player) == this view`) | 0x17F2230 |

### 3.4 GameRenderView vtable 0x55F42F8 (first 24 slots)

`[0] 0x17F2060 dtor, [1] 0x17F21E0, [2] 0x17F2230 per-frame see-through/light update, [3] 0x3CC6FD0,
[4] 0x3CC7010, [5] 0x3CC7070, [6] 0x3CC86D0, [7] 0x17F2910, [8] 0x3CC9190, [9] 0x17F2670 (called
as +0x50 by RemovePlayer), [10] 0x17F2B30 (camera/override), [11] 0x407BEB0, [12] 0x3CCCB10,
[13] 0x17DF050 (called as +0x70 by RemovePlayer), [14] 0x17F2180, [15] 0x17F3280 (copies focus data
into the occlusion helper), [16] 0x17F26F0, …` (slot meanings beyond the cited call sites are
**unconfirmed**).

---------------------------------------------------------------------------------------------------

## 4. Adding a local player

### 4.1 Triggers

* 0x5E3720(…, out, type, device): if `type` is 6 or 7 and `profileMgr->vtbl[+0x20]()` is non-null
  → `0x17F73F0(mgr, device)`; else if `mgr+0x178 == 5` stores the device at mgr+0x162 (input event
  handler; the event-type numbering is **unconfirmed**).
* 0x185DE70: profile/user sign-in completion (calls profileMgr vtbl+0x50) → `0x17F73F0(mgr, device)`
  when no player has that profile (0x17F6A40).
* 0x1C3FCF0: adds the **primary** player: `0x17F5CB0(mgr, slot 0, profileMgr->vtbl[+0x28]())` or
  re-links an existing primary player (0x17F6950). Callers 0x5E7540, 0x17F4BF0, 0x1C3FD60, 0x18479A0.

### 4.2 Join 0x17F73F0(mgr, device)

1. returns unless `0x5EB670(DAT_145FDA240)`, `mgr+0x160 == 0xFF`, free-slot list non-empty.
2. pops the first free slot (1..3) from +0x150; `0x40F3BA0(inputMgr, device, slot)`.
3. `mgr+0x160 = slot`; obtains a profile named "PublicSplitScreen" (or profileMgr+0x50 string)
   via profileMgr vtbl+0x90 (find) / +0xC0 (create) / +0x108 (get; sets byte +0xA8) / +0xB0.
4. calls the +0xF0 interface `0x17F6AC0(mgr+0xF0, 1, device, &profileId)` → since +0x160 != 0xFF
   and no player owns that profile: `0x17F5CB0(mgr, slot, &profileId)`; `mgr+0x160 = 0xFF`.
   (0x17F6AC0 bails if `[DAT_146047B40+8] != 0`.)

### 4.3 0x17F5CB0(mgr, slot, int* profileId, p4)

1. `id = 0x3BAA380(mgr, slot)`; 0 → return 0.
2. `node = map130[id]`; profile id → node+0x18 (ref-counted string ids).
3. Creates camera entity named `"GameCamera_" + "player<slot>"` (0x3B96260 on the ECS world), adds
   components (0x3B7CFB0, 0x3B7D020, 0x6D0930, 0x6D11E0, 0x1C631E0, 0x1C63170; types **unconfirmed**),
   sets the camera component's player field (+0x38) = id, copies a value (+0xAC) from the first
   existing player's camera. Stores the entity handle (16 bytes) at node+0x20.
4. If a level is loaded (`DAT_146007A28+0x90`): `0x17F7E10(mgr, id, &node.camera, 1, 1, 0)`
   (camera → view) and `0x17F8160(mgr, id, level+0x28 scene, 0)` (scene + environment).
5. slot != 0: profileMgr vtbl+0x108 / +0xF8 (assign profile to slot/player), input bindings
   (DAT_145FDA240+0x1C0, GlobalSwitches+0x113 gate).
6. If `[[DAT_145FDA240+0x98]+0x418] != -1` (a client session id; interpretation): calls
   0x1C53F50(x, id, profile, …, id==primary, id==focused) (purpose **unconfirmed**; 0x17F7710 makes
   the same call for every player).
7. Listeners +0x08 (player added) (0x3C14AB0); `DAT_146047958+0x88` vtbl+0x40(slot).

### 4.4 0x3BAA380(mgr, slot, p3) (base)

1. id = recycled id or next id; append to +0x20.
2. slot → +0xE8 (id 1) / +0xE9; `map30[id].slot = slot`.
3. Listeners +0x28 ({id, slot}).
4. `map70[slot] = id`; slot 0 → `+0xD0 = id`.
5. slot 0: view = pipeline main view (pipeline+0xA0) → `+0xD8/+0xE0`, map30, listeners +0x30
   immediately.
   slot != 0: pool empty → 0x3BAA1A0; else take pool[0] → `+0xD8/+0xE0`, map30, listeners +0x30,
   remove from pool and from map +0x90.

---------------------------------------------------------------------------------------------------

## 5. Removing a local player

0x17F63F0(mgr, id): removes ECS components of its character (+0x1C0), listeners +0x10 (removing),
for slot != 0 unassigns the profile (vtbl+0xF8 with 0) and input binding, returns the slot to the
free list (0x17F6850), `DAT_146047958+0x88` vtbl+0x48; **0x3BAA680(mgr, id, 0)**; destroys the
camera entity; listeners +0x18 (removed) (0x3C14BF0); removes the map +0x130 node; removes the
level's per-player object (`[level+0xA0]` map, byte +0x16A = 0, vtbl[0]).

0x3BAA680(mgr, id, notify): optional listeners +0x10; removes from +0x70/+0x20; if the player's view
is not the primary player's view: `view->vtbl[+0x50](view,0,0)`, **0x3CCC550(view, 0)** (disasm
0x3BAA8F6..0x3BAA906), if its slot != 0 `view->vtbl[+0x70](view)`, `view[0x21E0] = 0`,
`[[view+0x48]+0x80] = 0`, pushes the view to the **pool** (+0x80) and map +0x90, listeners +0x38
(view released). Clears +0xD8/+0xE8 or +0xE0/+0xE9, removes the map +0x30 node, recycles the id.
Views are never destroyed on player removal; only 0x3BA9FC0 destroys pooled views.

---------------------------------------------------------------------------------------------------

## 6. Listener interface (registry DAT_1460488A0)

Registry stores `object+8`; dispatch subtracts 8 and calls the object's vtable (`dec/listeners.c`,
`dec/listener_users.c`). Base listener ctor 0x75C030 (registers, vtable 0x54E39C8), base dtor
0x71FF60 (unregisters with 0x4124960).

| Slot | Event | Fired by |
|---|---|---|
| +0x08 | player added (u16 id) | 0x3C14AB0 ← 0x17F5CB0 |
| +0x10 | player removing (id) | 0x3C14B50 ← 0x17F63F0, 0x3BAA680(notify) |
| +0x18 | player removed (id) | 0x3C14BF0 ← 0x17F63F0, 0x3BAA680(notify) |
| +0x20 | (none found) | |
| +0x28 | input slot assigned ({u16 id, s8 slot}) | 0x3BAA380, 0x3BAB100 |
| +0x30 | player view ready (id) | 0x3C14C90 ← 0x17F4BF0 (set +0x40), 0x3BAA380 |
| +0x38 | player view released to pool | 0x3BAA680 |

Implementations (vtable → overridden slots; others default 0x579DA0):

| Vtable | Owner (registration / ctor) | Overrides |
|---|---|---|
| 0x54C6620 | `ecl::player_management::TrackingSystem` (reg 0x74F580) | +08 0x794F90, +10 0x794FE0, +18 0x795030, +28 0x7950D0 (queue events) |
| 0x54C6698 | `ecl::player_management::RenderViewTrackingSystem` (reg 0x74F7E0) | +30 0x794F90 (view ready), +38 0x794FE0 (view released) |
| 0x54C6CF0 | ECS system reg 0x730780 (system id DAT_146058FF0 = `ecl::sound::MusicPrioritySystem`) | +18 0x1D2D0A0 |
| 0x56069E0 / 0x56069A0 | system reg 0x756BB0 | +10 0x1901B60 / +18 0x1901B70 |
| 0x562A440 | 0x1A41A80 | +08 0x1A478E0 |
| 0x568F470 / 0x568F430 | 0x1E66DF0 | +08 0x1E6DC50 / +10 0x1E6D600 |
| 0x55F3A18 | 0x17DF080 (object vtable 0x55F3A58) | +08 0x17E3FA0, +10 0x17E3FC0 |
| 0x5658A60 | 0x1C882B0 | +08 0x1C8B630, +18 0x1C8B7C0 |
| 0x55F60D8 | client init 0x1818E40 | +18 0x180B250 |
| 0x5642CD0 | client init 0x1818E40 | +08 0x1B2FBE0, +18 0x1B2FC30 |
| 0x564C500 | client init 0x1818E40 | +08/+18 0x1B98100 |
| 0x55F75A0 | 0x1828F10 | +08 0x5D68B0 (ret 1), +18 0x1829720 |
| 0x55FDCC0 | 0x18A62A0 | none |

Only **RenderViewTrackingSystem** reacts to view ready/released.

---------------------------------------------------------------------------------------------------

## 7. ECS systems

All registered by `0x70EC90` (ecl system registration; calls at 0x70F5F5…0x70F6A6). A system is a
functor object (vtable: [0] dtor, [1] 0x5D68B0, [2] OnInit (creates the singleton), [3] secondary)
plus an update function passed as `local_120`; each update wrapper (0x757300…0x757580) builds a
query context `{world, world, world+0x210, world+0x198, world+0x308}` and calls the real update.
Ordering edges are added by tiny functions `world->systems[A].deps += B` (0x71ADE0…0x71F1E0).

### 7.1 Type ids (names from GetTypeName initialisers; tool `scripts/names.sh`)

Systems: AnimationSelector DAT_146058F30, Animation F34, HeightSplit F38, Request F3C,
SplitReasons F40, Goal F44, View F48, RenderViewport F4C, FullscreenTurn F50;
player_management::Tracking DAT_1460592E8, RenderViewTracking 592EC.
Components: Players DAT_14605B9A0, View BAA8, Letterbox BAAC, Animation BA70, MergedFocus B998,
SplitReasons BA6C, Goal BA68, HeightSplitReasons BA74, SplitTimer BA78, SelectedAnimation BA64,
Requests BA84, RequestCounts BA88, FullscreenTurnActive BAA0, PlayerCharacterChangedOneFrame BA7C,
SoundCamera BF58; player_management Events DAT_146053BC0, PlayerIdToPlayerInputIndexMapping 3BC8,
PlayersWithRenderViewAssigned 3BD0, UserComponent 3BD8.
Query ids (`Spec<TypeList<X>>`): Players DAT_146095068, View 095060, Animation 0A938C,
MergedFocus 0A4B2C, Letterbox 0A9718, ScreenFadeSync 0A8C24, PlayerIdToPlayerInputIndexMapping
0A960C, character::AssignedComponent 0950B4, PlayersWithRenderViewAssigned 0660E4,
timeline::ActorParticipation 095074, {EocCameraBehavior,GameCameraBehavior} 0A961C,
{EocCameraBehavior,camera::SelectorMode} 0A9618, SplitReasons 095064, DropInApprovedPlayers 0A9610,
DestroyPlayerRequests 0A4B34, SeeThroughState 0A4A94.

### 7.2 Systems

| System | Reg | Update | Writes | Reads | Summary |
|---|---|---|---|---|---|
| player_management::TrackingSystem | 0x74F580 (functor 0x140 B, listener at +0x10) | 0x764C80 → 0x795130, 0x7958A0 | Events, PlayerIdToPlayerInputIndexMapping, UserComponent | – | drains listener queues: Added/Removing/Removed id lists into EventsSingleton; creates/destroys an entity with UserComponent{id} per player; slot events → input-index mapping |
| player_management::RenderViewTrackingSystem | 0x74F7E0 (0xC0 B) | 0x764E10 → 0x797A60 | PlayersWithRenderViewAssigned | – | view ready(id) → add to set +0x00, remove from set +0x30; released → the reverse |
| SplitReasonsSystem | 0x726720 (functor holds unique_ptr param_2 and **player manager** [R14+0x30]) | 0x19293D0 | Players, SplitReasons, PlayerCharacterChangedOneFrame | ActiveTurn, floor::Info, DropInApproved, EocCameraBehavior, GameCameraBehavior, camera::SelectorMode, character::Assigned, … | picks players A/B from the input-index mapping (A = slot-0 player; B = last other player, filtered by DropInApproved), finds their characters via `character::AssignedComponent`, writes Players entries; computes reason bits: 0x2 distance² > 225, 0x4 > 56.25, 0x8 timeline participation differs, 0x10/0x20 camera selector mode A/B, 0x40 camera directions > 30° (GlobalSwitches+0x68D == 2), 0x80, 0x100, 0x200, 0x400 |
| GoalSystem | 0x726A20 | 0x192CF80 | Goal, MergedFocus | ActiveTurn, DropInApproved, Players, Requests, SplitReasons, HeightSplitReasons, FullscreenTurnActive, CC, combat, ftb, ReservedFor, photo mode, casting, TimelineActorData… | Goal {u8 Merged, u8 HeightSplit}, see §7.4 (`dec/goal_full.c`) |
| AnimationSelectorSystem | 0x725F00 | 0x1922DC0 | SelectedAnimation | gamestate::Running, Players, Goal, SplitReasons | writes SelectedAnimation {u32,u32} = {1,1} or {2,2} (or only [1]=2) depending on GlobalSwitches+0x68D != 1, Players, Goal and a SplitReasons bit; details not traced |
| AnimationSystem | 0x726100 | 0x1924850 | Animation | Goal, MergedFocus, SelectedAnimation | Animation {u8 State, u8 Changed, f32 Progress}: State 2 merged, 4 opening (Progress += dt to 0.5 → 0x20 split), 8/0x10 closing towards A/B (Progress -= dt to 0 → 2) |
| HeightSplitSystem | 0x7262F0 | 0x1925E50 | HeightSplitReasons, SplitTimer | Players, PlayerCharacterChanged, floor::Info, Transform | needs both Players characters; compares their heights (reason when \|Δ\| ≥ 3.0) and floor ids (0x140B23990); details not traced |
| RequestSystem | 0x726520 | (functor vtbl 0x54C7358 [2] 0x1927880 creates the singleton, [3] 0x1927A90) | Requests, RequestCounts | – | the system object (world systems[DAT_146058F3C]) keeps a u8 array at +0x10 (count +0x1C); 0x1BA28A0 / 0x1BA2A30 push 1, 0x1BA2950 / 0x1BA2AA0 push 0 (other users of the id: 0x11F1A70, 0x1C9A810, 0x1C9ACE0, 0x120BAE0, 0x12B5630, 0x424EB80). How the array becomes Requests {u8 r0, u8 r1} was not read (**unconfirmed**) |
| ViewSystem | 0x726CC0 | 0x192E260 | View | Animation, MergedFocus, Players | View {u8 Mode, u8 PrevMode}: Mode = 3 if `!(Animation.State & 2)`; else if `MergedFocus[1]==0`: (both Players characters valid) ? 0 : 1; else `(MergedFocus[0]!=0)+1` |
| RenderViewportSystem | 0x726EB0 (functor+0x10 = **player manager**) | 0x192ED80 | Letterbox | Animation, Players, View, ScreenFadeSync, TimelineActorData | §7.3 |
| FullscreenTurnSystem | 0x7270C0 | 0x1930440 | FullscreenTurnActive | turn components | not read |

Singleton layouts (confirmed parts): **Players** 0x30 = 2 × {+0x04 u16 player id, +0x08 u64
character handle, +0x10 u8 "in PlayersWithRenderViewAssigned"} (setter 0x1929150).
**Letterbox** {u8 A, u8 B} = view's camera (`+0x2260` → +0x318 entity) has
`eoc::TimelineActorDataComponent`… entry byte +0x20 == 1 (0x192F940). **ScreenFadeSync** 0x90:
hash map u16 player → u8 at +0x00/+0x10/+0x20/+0x30, +0x88 dirty byte.
**PlayersWithRenderViewAssigned** 0x60 = two hash sets of u16 (+0x00, +0x30).
**PlayerIdToPlayerInputIndexMapping** 0x40: +0x20 u16 ids, +0x2C count, +0x30 s8 input index.

### 7.4 GoalSystem 0x192CF80 (when the screen splits)

Bindings (query names resolved): R = Requests {u8,u8}, SR = SplitReasons {u16 Reasons, u16 Same},
HS = HeightSplitReasons (u8), P = Players, MF = MergedFocus (written), G = Goal (written).

```
reasons = SR.Reasons; if (G.Merged == 1) reasons &= ~0x4        // 7.5 m bit ignored while merged (hysteresis)
heightSplit = (reasons & 0x30) && !(reasons & ~0x30)
merged      = (reasons == 0 && HS == 0)
if (R[1] == 0 && GlobalSwitches.GameCameraSplitscreenUserSplit(+0x684) == 0) {
    0x1B2F100(&f, ctx); if (f.byte1) { merged = 1; MF = f }      // focus request
} else { heightSplit = 0; merged = 0 }                           // user setting "split" / request: no auto-merge
if (DropInApproved non-empty && P[1].id && P[1].id not in it)  merged = 1
if (SR.Same != 0) merged = 1                                    // both players on one character
if (R[0] != 0)    merged = 1
if ((!characterCreationFlag && P[0].character invalid) || P[1].id == 0) merged = 1
if (P[0].flag == 0 || P[1].flag == 0) merged = 1                 // flag = player has a render view (RenderViewTracking)
if (merged) heightSplit = 0
photo-mode session active → MF = {0,1}, merged = 1
G = {merged, heightSplit}
```

GlobalSwitches fields used here (names from the load function 0x5D0360 FixedStrings):
+0x683 GameCameraSplitscreenFullscreenTurn, +0x684 GameCameraSplitscreenUserSplit,
+0x68D GameCameraSplitAnimation (u8 enum; SplitReasons adds reason 0x40 only when it is 2).

### 7.3 RenderViewportSystem 0x192ED80 (the layout code)

```
A = Players[0].id (0 → mgr.GetPlayerForSlot(0)),  B = Players[1].id
vA = mgr.GetPlayerView(A),  vB = B ? mgr.GetPlayerView(B) : 0
Letterbox = {letterbox(vA), letterbox(vB)}  (marks dirty on change)
early out unless Animation.Changed || View.Mode != View.PrevMode || letterbox changed || ScreenFade.dirty
Mode < 2 : vA scale(1,1) off(0,0) enabled;  vB scale(1,1) off(0,0) DISABLED
Mode == 2: vB scale(1,1) off(0,0) enabled;  vA scale(1,1) off(0,0) DISABLED
Mode == 3: t = max(0, (2*Progress)^3); swap = Animation.State & 0x10
           vA scale((1-t)*(swap?0:1) + t/2, letterboxA ? 0.85 : 1), off(0,0); vA[0x226C] = 0
           vB scale((1-t)*(swap?1:0) + t/2, letterboxB ? 0.85 : 1),
              off((1-t)*(swap?0:1) + t/2, 0);                       vB[0x226C] = 1
           enabled A = 1, B = (scale.x >= 1.19e-7)
per player: ScreenFadeSync map[player] != 0 → that view disabled
vX[0x21E0] = enabled; [[vX+0x48]+0x80] = enabled
frame = 0x3AC7600(pipeline) (main frame); frame->vtbl[+0x40](frame, A && B && bothEnabled)
```
(0x407B800(view, &scale, &offset) for every placement.) The meaning of the frame's vtbl+0x40 is
**unconfirmed**: the base RenderFrame vtables 0x58EDA30/0x58EDD18 have a no-op there; the game frame
class (created by `pipeline+0x10`->vtbl+0x20) was not identified.

---------------------------------------------------------------------------------------------------

## 8. ForceSplitscreen

* `ls::gGlobalSwitches` = `*(ptr*)0x14604B1B8` (SE BinaryMappings signature; two code matches).
* "ForceSplitscreen" is a static FixedString (0x40CE0 → DAT_145FBE598). Its only users are the
  GlobalSwitches load/save functions 0x5D0360 (load: `*(bool*)(switches+0x6B6) = …`), 0x5D22A0 and
  0x5D3EE0 (save: reads `[DAT_14604B1B8]+0x6B6`). So **ForceSplitscreen = byte GlobalSwitches+0x6B6**.
* A displacement search over all instructions found +0x6B6 only in those three functions plus
  0xEF32B0 (writes it, GlobalSwitches defaults: `+0x6B0 = 0x01010001`, `+0x6B4 = 0x0101`, `+0x6B6 = r14b`),
  0x3F26D80 / 0x3F29220 (other structs). **No code reads ForceSplitscreen at run time** (no
  wider access at +0x6B0/+0x6B4 in a function that loads gGlobalSwitches either). The input event
  `ToggleForceSplitscreen` (0x5E0) is registered at 0x15B643C; its handler was not located.
* 0x2137110 is **not** GlobalSwitches: it registers the property map of a UI state definition
  (Layout, Owner, DisableStatesBelow, HideStatesBelow, IsModal, IsModalCameraControlAllowed,
  ForceVisible, ForceSplitscreen, DisableSoundLayersAbove, TutorialLayer, ModType, IsModdable,
  InitialSubstates, Events, Widgets). The earlier note "GlobalSwitches property at +0x110" is wrong:
  0x110 is that UI-state property's slot value.
* Whether the Script Extender's `GlobalSwitches.ForceSplitscreen` property maps to +0x6B6 was not
  checked (the SE binary only has the name list; its offsets are compiled code). The split-related
  switches the split-screen systems do read are +0x683 GameCameraSplitscreenFullscreenTurn,
  +0x684 GameCameraSplitscreenUserSplit (forces split in GoalSystem, §7.4) and +0x68D
  GameCameraSplitAnimation.
* Therefore the "third view" seen after `!vr_split on` cannot be attributed to ForceSplitscreen from
  the code. Known creators of render views: player views (0x17F82F0 / 0x3BAAF90), the pipeline's
  main/special views (0x582BD0 / 0x3AC7500), and the **BasicGameObjectRenderer holder** (0x1B75D50:
  creates its own RenderFrame "BasicGameObjectRenderer" via 0x3BB1880 with frame+0x42F0 = 0,
  a GameRenderView with a smaller cfg, `0x4073230(frame, view, 0)`, view+0x2274 = p4, and runs only
  the base setup 0x3AC4EA0). Which of these produced the observed view is **unconfirmed**; a hook on
  0x4073230 logging (frame, view, caller) would settle it.

---------------------------------------------------------------------------------------------------

## 9. Function index

| RVA | Name (descriptive) |
|---|---|
| 0x17F1E60 | GameRenderView ctor (view, frame, scaleX, scaleY, offX, offY, cfg) |
| 0x17F2060 | GameRenderView dtor |
| 0x17F2230 | GameRenderView vtbl[2] per-frame see-through/lights |
| 0x17F46C0 / 0x17F4A90 | player manager (derived) ctor / dtor |
| 0x17F4BF0 | player manager Update (from pipeline 0x5889E0) |
| 0x17F5100 | SetPlayerCharacter |
| 0x17F5CB0 | AddLocalPlayer (derived) |
| 0x17F63F0 / 0x17F6320 | RemovePlayer / RequestRemovePlayer |
| 0x17F6AC0 | input/profile event (+0xF0 iface) |
| 0x17F73F0 | Join(device) |
| 0x17F7CD0 / 0x17F7E10 | SetPlayerViewCamera / attach camera entity |
| 0x17F8160 | SetPlayerViewScene |
| 0x17F82F0 | CreatePlayerView (derived vtbl[1]) |
| 0x1922DC0 / 0x1924850 / 0x1925E50 / 0x19293D0 / 0x192CF80 / 0x192E260 / 0x192ED80 / 0x1930440 | splitscreen AnimationSelector / Animation / HeightSplit / SplitReasons / Goal / View / RenderViewport / FullscreenTurn updates |
| 0x192F940 | letterbox test for a view |
| 0x1B75A90 / 0x1B75D50 / 0x1B76070 / 0x1B76620 | BasicGameObjectRenderer holder ctor / create frame+view / dtor / destroy view |
| 0x3AC7500 / 0x582BD0 | pipeline CreateView (base / game) |
| 0x3AC7600 | pipeline main RenderFrame |
| 0x3BA9AB0 | player manager (base) ctor |
| 0x3BA9E80 | DestroyView |
| 0x3BA9FC0 | RemoveAllButPrimary |
| 0x3BAA1A0 | AddPlayerView |
| 0x3BAA380 | AddLocalPlayer (base) |
| 0x3BAA680 | RemoveLocalPlayer (base) |
| 0x3BAACE0 / 0x3BAADB0 / 0x3BAADF0 / 0x3BAAE20 | slot→player / player→slot / primary player / player→view |
| 0x3BAAF90 | CreatePlayerView (base vtbl[1]) |
| 0x3BB1880 / 0x3BB1A20 | RenderFrame create / destroy (frame manager) |
| 0x3C14AB0 / B50 / BF0 / C90 | listener dispatch +08 / +10 / +18 / +30 |
| 0x3CC5AA0 | rf::RenderView ctor |
| 0x3CC90B0 | SetCameraController (view+0x2260) |
| 0x3CCC550 | SetScene (view+0x2B10) |
| 0x3CC9D30 / 0x3CCAFA0 | set atmosphere / lighting |
| 0x4073230 / 0x40733E0 | RenderFrame add view / remove view |
| 0x407B080 | root view ctor |
| 0x407B800 | SetViewport(view, &scale, &offset) |
| 0x588E50 | game pipeline setup (vtbl +0x2C0) |
| 0x5889E0 | game pipeline per-frame update (vtbl +0x288) |
| 0x70EC90 | ecl ECS system registration |
| 0x74F580 / 0x74F7E0 | player_management Tracking / RenderViewTracking registration |
| 0x725F00 … 0x7270C0 | splitscreen system registrations |
| 0x75C030 | listener base ctor (registers in DAT_1460488A0) |

---------------------------------------------------------------------------------------------------

## 10. Open items (unconfirmed, and how to confirm)

* Thread of command queue [2] (view create/setup/destroy) and queue [0] (camera/scene/sound): read
  the drain loop of `(DAT_14604B1C0+0x148)[i]` or log thread ids in hooks of 0x3BAB450 / 0x17FA690.
* Game RenderFrame class and its vtbl+0x40 (called by RenderViewportSystem with "two views shown").
* Exact meaning of Players entry flag (+0x10) w.r.t. the two sets of PlayersWithRenderViewAssigned.
* RequestSystem / HeightSplit / AnimationSelector / FullscreenTurn internals (not needed for the
  recipe).
* Origin of the extra view seen after `!vr_split on` (hook 0x4073230).
