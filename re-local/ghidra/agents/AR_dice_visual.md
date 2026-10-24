# A 3D d20 in the VR hand: model, client-only visual, face layout - agent AR, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_G, read-only, -noanalysis, request server
`agents\AR\ARServer.java`, stopped), bg3vr_disasm, and game data read with LSLib Divine / PhysicsTool / LSLib.dll
(VirtualTileSet) into `agents\AR\`. RVAs are relative to 0x140000000. "Confirmed" = read in code or data at the cited
place. "Unconfirmed" = section 4 says how one run settles it. Raw output: `agents\AR\ar1..ar23_*`, `gr2\`, `models\`,
`banks\`.

## Recommendation

| need | how | RVA / id | confidence |
|---|---|---|---|
| Model to use | **UI_Dice_D20_A**, visual resource **`a61032d1-98f3-6d0b-55f0-75e62b623f26`**. Centred on its origin, blue faces, cream numbers. Same number layout as the game's ActiveRoll dice pictures. Alternative: the world item die QUEST_Dice_d20_A, visual `81196a4e-17f6-938d-3fdc-751c29847bbe` (ivory, origin on its bottom face). | banks: Shared.pak `Public/Shared/Content/UI/[PAK]_UI/_merged.lsf` / `.../Assets/Quest/[PAK]_Dice/_merged.lsf` | Confirmed (data) |
| Show it | Client-only "visual clone" in the level scene, the same object the game uses for paperdoll and stealth clones. On the game thread right after the paperdoll-manager update **0x13E1A40** (dolls.cpp already hooks it): `obj = 0x4188700(0x40EA160()+0xE8, 0x3A0, 0x10, &tmp)`; **`0x1888C10(obj)`** ctor; **`0x18890D0(obj, **(u64**)(level+0x28))`** with `level = [[0x6007A28]+0x90]`; FixedString `fs` from the GUID text with **0x4194CA0**(`[0x604B2E0]+0xC600`, &fs, &{char*, len}); then vtable slot 2 **`0x188A8A0(obj, &fs, 0, 0)`** (last 0 = synchronous: create the visual and add it to the scene now). | 0x1888C10, 0x18890D0, 0x188A8A0, 0x188B510, 0x3CF1E00 | Code path Confirmed. Runtime Unconfirmed (4.2) |
| Move, rotate, scale every frame | **0x3C785C0**(sys, &ref, &pos, &quat, &scale3, 1, 1). `sys = *(u64*)([world+0x30] + [0x6058CBC]*0xF8)`, `world = [[0x5FDA240]+0x1A8]`. `ref` = 0x38 bytes: `+0x00` handle `obj+0x08`, `+0x08` world, `+0x10` 0 (Transform*), byte `+0x30` = 1 (look it up). Quat in the layout hold.cpp already passes to SetRotate 0x1C77130 (both end in TransformComponent `+0x00`). Same call the game uses for its template-visual clone (0x1F4EB00, purpose not identified) and stealth clones (0x19C9B10). | 0x3C785C0 | Confirmed (code). Visible result Unconfirmed (4.3) |
| Remove | `obj->vtbl[0](obj, 1)` = **0x1888FB0**: dtor 0x1888FF0 -> **0x188C450** destroys the visual (ls::VisualSystem 0x3BF7DF0) and deletes the entity (0x4003B20), then frees obj. Do it on load start, on level change and when done. | 0x1888FB0 | Confirmed (code) |
| Visual pointer (flags, debug) | **0x1889D00(obj)** returns the ls::Visual*. | 0x1889D00 | Confirmed |
| Keep the laser and the game's picking off it | Nothing to do: the clone gets no physics component, and all picking is physics raycasts. | components in 0x188B510 | Confirmed (code) |
| Shadows | Created with load flags 0xC. Keep, or turn off with AA's cull setter **0x3C6B810(v, 0x803E, false)**. | 0x3BF4F20 arg, 0x3C6B810 | Flags passed Confirmed; shadow on screen Unconfirmed (4.4) |
| Land on face N | Rotate so the face normal of N (table 3.2) points to world +Y. Face 21-N then points down. | data | Confirmed (mesh + texture) |
| Not recommended | Server item via Osiris `CreateAt` (0x3672CA0) + client `SetTranslate`. It is saved, replicated, has physics and picking, and the server copy stays where it spawned. | 3.5 | Confirmed (code, data) |

Size at scale 1: UI d20 is 0.79 m across vertices (circumradius 0.3948, inradius 0.3137). A hand die of
about 10 cm face to face needs scale about 0.16. The quest die is 0.24 m face to face (0.30 m across vertices).

---------------------------------------------------------------------------------------------------

## 1. The game's dice

### 1.1 ActiveRoll draws no 3D die (Confirmed, data)

* ActiveRoll.xaml includes `Public/Game/GUI/Library/DiceAnimation.xaml`. All dice there are `ls:AnimatedImage`
  flipbooks (277 uses, 256x256 cells). The source is the converter `ls.DiceImageSourceConverter` (type string
  0x555F390) from `CurrentDiceStyle` (0x55B35E0) and a suffix: `/d20` (spin), `/d20_<N>` (land on N),
  `/d20_faceCover`, `single_roll`, `double_roll_1/2`. Base paths `Assets/DiceSets/` (0x55B39B8) and
  `Assets/DiceSets/Default/` (0x55B4168).
* No LSPaperdoll, Viewport3D or other 3D element in ActiveRoll.xaml, ActiveRoll_c.xaml or DiceAnimation.xaml.
* Dice skins DiceSet01/02/03/06.pak hold only these DDS sheets plus `CustomDice.lsx`.
* So there is no runtime d20 mesh to borrow from the roll UI. The pictures were pre-rendered.

### 1.2 3D d20 assets in the data (Confirmed, data)

| asset | visual id | GR2 (Models.pak) | material / texture | physics | notes |
|---|---|---|---|---|---|
| **UI_Dice_D20_A** | `a61032d1-98f3-6d0b-55f0-75e62b623f26` | `Generated/Public/Shared/Assets/UI/UI_Dice_ABC/Resources/UI_Dice_D20_A.GR2` (object `UI_Dice_D20_A.UI_Dice_D20_A_Mesh.0`) | `387b2949-dece-bb2f-63cf-b7793f8bf732` "UI_Dice_A_Stub" (Base_VT), VT `4cc1e33f-fca9-e44f-1c5e-88a43934199e` = GTex `a31307cb7f9dbc4a005a21f800c199ae` (tile set `Albedo_Normal_Physical_a`, 1024x1024) | `19d937a9-4c6e-71ec-c0e5-d787f8b29983` RigidDynamic convex hull, 20 faces, mass 1 (not linked from the visual) | bounds ±0.353/±0.395/±0.336, centre 0. Not used by any root template |
| **QUEST_Dice_d20_A** | `81196a4e-17f6-938d-3fdc-751c29847bbe` | `Generated/Public/Shared/Assets/Quest/QUEST_Dice_d20_ABC/Resources/QUEST_Dice_d20_A.GR2` | `30becb9c-5aea-7e5f-8d75-3af08466560a` (Base_VT), VT `7d8d1648-c559-bf1e-f7fd-8a96b1fd8d8f` = GTex `a75de691cdefbb3ee4cb1b38517759be` (256x256, in `_a_mips.gtp`) | `6a6ec4a4-6088-c3eb-e9cf-eae962ea1f7d` RigidStatic convex | bounds y 0..0.238, Center (0, 0.1192, 0): origin on the "17" face |
| UI_Dice_D6_A | `61223ded-1904-1df6-f700-b01799448725` | `.../UI_Dice_ABC/Resources/UI_Dice_D6_A.GR2` | same stub material | `f756ff47-...` | d6 |
| KeyArt_Dice_D20_A | - | none found | - | `.bin` static only | not usable |

Item template using a d20 (Gustav.pak `Public/Gustav/RootTemplates/_merged.lsf`): **Quest_D20**, MapKey
`0b8b0139-300e-4133-9e6c-6807067d02d3`, VisualTemplate `81196a4e-...`, PhysicsTemplate `6a6ec4a4-...`, Stats
`OBJ_GenericImmutableObject`, Hostile True, WalkThrough, CanShootThrough, tag `c86e0114-...`. No template in Shared,
SharedDev, Gustav, GustavDev, Honour or GustavX uses the UI die.

The UI die matches the ActiveRoll pictures: the Default set's `d20_20.DDS` shows 20 with neighbours 2, 14, 8 and
then 18, 4, 12, 10, 16. The UI mesh table (3.2) gives exactly those neighbours. So it is the die the roll UI shows.

---------------------------------------------------------------------------------------------------

## 2. Client-only visual: how the code works

### 2.1 The visual-clone object (vtable 0x55FC810, 0x3A0 bytes) (Confirmed, ar1/ar2)

* Allocation as the game does it (0x1BD4CB0): `0x4188700(0x40EA160() + 0xE8, 0x3A0, 0x10, &tmp)`.
* Ctor **0x1888C10**: handles `+0x08` (clone entity), `+0x10` (source entity) = null, `+0x20` visual FixedString
  = -1, `+0x348` scene entity = null. It needs the client world `[[0x5FDA240]+0x1A8]` and registers with
  ecl::EquipmentVisualsSystem (type idx 0x604C67C), id kept at `+0x340`.
* **0x18890D0(obj, sceneEntity)**: stores the scene entity at `+0x348` (releases an older clone first).
* Vtable: `+0x00` 0x1888FB0 deleting dtor, `+0x08` 0x188A720 init(template, visual id, ...), `+0x10` **0x188A8A0
  set visual id**, `+0x18` 0x188A4C0 from character, `+0x20` 0x188A320 from item, `+0x28` 0x188A0F0 from template.
* **0x188A8A0(obj, const FixedString* id, bool force, bool async)**: releases the old clone (0x188C450), stores the
  id at `+0x20`, sets `+0x32A = 1` (item-like), then **0x188B510(obj, block, async)**.
* **0x188B510** creates the clone entity in the client world and adds: ls::TransformComponent (0x188E360, idx
  0x6053D78), ls::VisualComponent (0x1855CB0, idx 0x60A542C), ecl::PaperdollComponent (0x6D18D0, idx 0x605388C,
  `{Entity = obj+0x10, Combat = obj+0x331}`), ls::AnimationWaterfallComponent, ls::AnimationSetComponent,
  ls::AnimationBlueprintComponent. Copies from a source character run only when the source is valid. **No physics
  component.**
  * `async == 0`: ls::VisualSystem (idx 0x604C88C) **0x3BF4F20**(sys, desc, &obj+0x20, ..., loadFlags **0xC**, ...),
    then for `+0x32A` **0x3CF1E00(desc, obj+0x348)**: add the visual to that entity's `ls::Scene` (idx 0x60A567C,
    0x1852350 -> 0x3CF11E0).
  * `async == 1`: 0x3BF4BB0 with callback **0x1892AE0**. The callback adds to the scene only when the source
    entity is valid (byte `+0xD0` of the 0x188DC10 block). With no source it never adds. **So use `async = 0`.**
* The game's own source-less, sync path is a template-visual clone (purpose not identified): 0x1F4EB00 -> 0x1B50830 -> slot `+0x28`
  0x188A0F0 -> **0x188BF00** (same VisualSystem create, same 0x3CF1E00 add, then 0x3C785C0 with scale 1 each
  update). It needs a template pointer and level data. 0x188A8A0 needs only the visual id. Both Confirmed.

### 2.2 Which scene (Confirmed)

* Level scene entity = `**(u64**)(level + 0x28)` (B; used this way by 0x19C97A0, 0x1BD4CB0 and 0x1B50830).
  `level = [[0x6007A28]+0x90]`, null while loading (D).
* Paperdoll private scenes (`renderer+0x348`, P) are a different scene. Do not use them: they have their own
  3 strong lights and only render to the doll frame.

### 2.3 FixedString (Confirmed)

* Create: **0x4194CA0**(table = `[0x604B2E0]+0xC600`, u32* out, `{const char*, u32 len}*`). Found from SE's
  `ls::FixedString::CreateFromString` pattern at 0x4262B6D (call at 0x4262BA9).
* AddRef 0x4170950(id), release **0x41950A0**(table, &id). 0x188A8A0 takes its own reference, so the mod
  releases its copy after the call.

### 2.4 Transform (Confirmed, ar2/ar23)

* **0x3C785C0(sys, ref, pos*, quat*, scale3*, notify, recordPrev)**: writes Transform `+0x00` quat, `+0x10` pos,
  `+0x1C` scale (each axis clamped to >= 0, no upper limit). `recordPrev` stores the old values first
  (0x3C78980 / **0x3C78A40** / 0x3C78B20: these only record the previous value for the frame, they do not set it).
  `notify` calls the listeners at `sys+0x60`.
* Single setters: SetTranslate 0x3C77BD0, SetRotate 0x3C77D20, uniform SetScale **0x3C77FA0**(sys, ref, float,
  notify).
* The world for the clone and for `sys` is `[[0x5FDA240]+0x1A8]` (0x188B510 creates the entity there; 0x1F4EB00
  builds `sys` from the same world).

### 2.5 Thread (Confirmed where the game does it)

The game builds and frees these clones inside the paperdoll-manager update **0x13E1A40** (P: 0x1B74250 -> 0x188A4C0
-> 0x188B510) and in ECS updates. Doing create, move and destroy in the post-hook of 0x13E1A40 matches the game. The
mod's grab code runs on the event-pump main thread; whether that is the same thread is Unconfirmed (4.1).

### 2.6 Safety (Confirmed unless marked)

* Client only: the entity lives in the client world; nothing is sent to the server.
* Not in saves: saves hold server state; this entity has no server side. (Not traced in save code; same kind of
  entity as the paperdoll clones.)
* Picking: game picking runs four physics raycasts (I 1.4, physics scene slot +0x40). The mod's laser uses
  rayprobe (physics). The clone has no physics, so neither sees it.
* Cleanup: the clone is not tied to the level. Destroy it before the level goes away. Destroying after the scene
  is gone is untested (4.6).
* The clone carries ecl::PaperdollComponent with a null Entity. Twelve functions reference that component type
  (0x64F7C0, 0x6C0A70, 0x6D7E80, 0x6E9BC0, 0x706B70, 0x70C850, 0x72D460, 0x733660, 0x7471B0, 0x7828C0,
  0x19876C0, 0x198A770); their handling of a null Entity is Unconfirmed (4.2).

### 2.7 Scale, lighting, shadows (mixed)

* Scale: Transform scale only, clamped >= 0 (Confirmed). Whether the visual's world bound grows with it, so
  culling stays right at 0.1-4x, is Unconfirmed (4.3).
* Lighting: the visual is in the level scene, so the level's lights, sun and probes light it like any prop. It is
  dark in dark places. It renders in both eyes with real depth.
* Shadows: load flags 0xC (SE names CastShadow 0x4 | PlayEffects 0x8). Cull bits per AA: shadow passes need
  0x803E. Turn shadows off with `0x3C6B810(v, 0x803E, false)`; turn them back with slot 23 0x3C64730.

---------------------------------------------------------------------------------------------------

## 3. Face layout

### 3.1 How it was read (Confirmed)

* Divine `convert-model` fails: these GR2s use Granny compression 4 (BitKnit2) and LSLib needs `granny2.dll`,
  which the game does not ship (Granny 2.12 is linked into the exe).
* I ported the exe's own decoder to Python (`agents\AR\bitknit.py`): one-shot 0x4A35590 -> 0x4A7CE50, stream
  0x4A7F100, quantum 0x4A7E810, models 0x4A81880 / 0x4A7E340 / 0x4A7E170 / 0x4A7DDF0, adaptation 0x4A7D260 /
  0x4A7DA60 / 0x4A7D3B0 (dispatch 0x4A4DC30, file read 0x4A4D310). Every section decoded and both rANS states ended
  at 0x10000, the decoder's own check. `gr2dec.py` writes the sections.
* Mesh: section 5, indices (u16) at 364, vertices at 484, stride 24: Position float3, QTangent int16x4,
  TextureCoordinates0 half2 (names from the GR2). UI die: 60 vertices, 20 triangles. Quest die: 26 vertices.
  Positions are an exact icosahedron (UI: one vertex on +Y).
* Texture: extracted from the virtual-texture set with LSLib.dll `VirtualTileSet.ExtractTexture`
  (`models\UI_Dice_stub_L0.png`, `models\QUEST_d20_L0.png`).
* Each triangle's UV area was cropped (`gr2\ui_d20_facegrid.png`, `gr2\quest_d20_facegrid.png`) and read.
  Check: for both dice every face N is exactly opposite face 21-N (dot -1.000), as on a standard d20. That also
  settles 6 vs 9.

### 3.2 UI_Dice_D20_A (local space, Y up, centred; face centre = 0.3137 x normal)

| N | outward normal (x, y, z) | N | outward normal (x, y, z) |
|---|---|---|---|
| 1 | (+0.9822, -0.1876, +0.0000) | 11 | (+0.1876, -0.7947, +0.5773) |
| 2 | (-0.7947, -0.1876, -0.5774) | 12 | (-0.3035, +0.1876, -0.9342) |
| 3 | (+0.4911, +0.7947, +0.3568) | 13 | (+0.6071, -0.7947, +0.0000) |
| 4 | (-0.4911, -0.7947, +0.3568) | 14 | (-0.7947, -0.1876, +0.5774) |
| 5 | (+0.1876, -0.7947, -0.5773) | 15 | (+0.3035, -0.1876, -0.9342) |
| 6 | (-0.3035, +0.1876, +0.9342) | 16 | (-0.1876, +0.7947, +0.5773) |
| 7 | (+0.7947, +0.1876, -0.5774) | 17 | (+0.4911, +0.7947, -0.3568) |
| 8 | (-0.6071, +0.7947, +0.0000) | 18 | (-0.4911, -0.7947, -0.3568) |
| 9 | (+0.3035, -0.1876, +0.9342) | 19 | (+0.7947, +0.1876, +0.5774) |
| 10 | (-0.1876, +0.7947, -0.5773) | 20 | (-0.9822, +0.1876, +0.0000) |

The 12 vertices: (0, +0.3948, 0), (0, -0.3948, 0), (+0.3531, +0.1766, 0), (-0.3531, -0.1766, 0),
(-0.2857, +0.1766, ±0.2076), (+0.2857, -0.1766, ±0.2076), (+0.1091, +0.1766, ±0.3358), (-0.1091, -0.1766, ±0.3358).
`gr2\ui_d20_faces.json` has each face's vertices and UVs.

### 3.3 QUEST_Dice_d20_A (origin on face 17; rotate about Center (0, 0.1192, 0); face centre = Center + 0.1192 x normal)

| N | normal | N | normal |
|---|---|---|---|
| 1 | (+0.3568, -0.3338, -0.8725) | 11 | (+0.0000, +0.7450, -0.6670) |
| 2 | (+0.3568, +0.3338, +0.8725) | 12 | (+0.5774, -0.3330, +0.7455) |
| 3 | (-0.5774, -0.7455, -0.3330) | 13 | (+0.5774, +0.3330, -0.7455) |
| 4 | (+0.0000, +1.0000, -0.0005) | 14 | (-0.5774, +0.7455, +0.3330) |
| 5 | (+0.9342, +0.3333, -0.1275) | 15 | (+0.9342, -0.3333, +0.1275) |
| 6 | (-0.9342, +0.3333, -0.1275) | 16 | (-0.9342, -0.3333, +0.1275) |
| 7 | (+0.5774, -0.7455, -0.3330) | 17 | (+0.0000, -1.0000, +0.0005) |
| 8 | (-0.5774, -0.3330, +0.7455) | 18 | (+0.5774, +0.7455, +0.3330) |
| 9 | (-0.5774, +0.3330, -0.7455) | 19 | (-0.3568, -0.3338, -0.8725) |
| 10 | (-0.0000, -0.7450, +0.6670) | 20 | (-0.3568, +0.3338, +0.8725) |

### 3.4 Landing on a chosen face

Let `n` be the table normal of the wanted number. The resting orientation is any quat `q` with `q * n = (0,1,0)`
(BG3 world is Y up). Pick the yaw about +Y freely (e.g. keep the tumble's last yaw). For the last part of the
simulated roll, slerp the die's orientation toward that `q` while it settles. The die's ground contact is the
opposite face (21 - N), at `-0.3137 x scale` below the centre for the UI die.

### 3.5 Server item option (Confirmed, not recommended)

* Osiris `CreateAt` handler **0x3672CA0**: args template, x/y/z (`+0x18/+0x28/+0x38`), temporary (`+0x48`), play
  spawn (`+0x58`), event (`+0x68`); creates through 0x35F1340 on the server world `[0x5FF03C0]+0x2B0`.
* That item is a server entity: replicated, saved unless deleted, part of server physics. Quest_D20 has a
  physics template, so picking and the laser hit it. Client `SetTranslate` 0x1C77050 moves only the local copy
  (E): the server copy and other players stay at the spawn point.
* `RequestDeleteTemporary` (0x365CF60) deletes only items with item-flag bit 25 set and bit 32 clear;
  `RequestDelete` 0x3627E90.
* SE has no Lua call that creates a visual (`VisualHelpers` only edits blend shapes; `Ext.Entity.Create` makes an
  empty entity). So the clone must be made natively.

---------------------------------------------------------------------------------------------------

## 4. One-run logging plan

One session: load a save, spawn the die (menu toggle), hold it, throw it twice, walk into shadow and light,
point the laser at it, then load another save. Log first occurrence, then every 300th frame.

4.1 **Thread.** `GetCurrentThreadId()` in the 0x13E1A40 post-hook (where the die code runs), in hold.cpp's
main-thread task and in the 0x17F21E0 render hook. Informational; the die code stays in the 0x13E1A40 post-hook.

4.2 **Creation.** Around `0x188A8A0(obj, &fs, 0, 0)`: return value, time taken (QueryPerformanceCounter; a long
time means a blocking load), `obj+0x08` handle, `obj+0x20` id, `obj+0x348`, `[[0x6007A28]+0x90]`. Then
`v = 0x1889D00(obj)` now and on the next 120 frames until non-null. Dump 0x100 bytes of `v`; log `[v+0xF0]`
(VisualFlags) and `[v+0xF4]` (cull flags). Expect v non-null at once, VisualFlags bit 0x1 set, cull bits 0x803E
and 0x600 set. Wrap every game call in the mod's fault guard and log any fault (covers the null PaperdollComponent
Entity readers).

4.3 **Transform.** After each `0x3C785C0` write, log the Transform at `0x0E59570(world, handle)` (`+0x00` quat,
`+0x10` pos, `+0x1C` scale). In the next frames log the visual's world transform and bounds from the 0x100-byte
dump (SE layout: `+0x08` quat, `+0x18` pos, `+0x24` scale; bounds near `+0x40..+0x58`). Expect them to follow,
with the bound scaled. Log the frame number of the write and of the first frame the visual changes (latency).
Try scales 0.16, 0.5, 1, 4.

4.4 **Shadow.** Look for the die's shadow in sunlight. Then call `0x3C6B810(v, 0x803E, false)` once (menu switch)
and log `[v+0xF4]` before/after. Expect the shadow to go.

4.5 **Picking.** While the laser crosses the die: log the rayprobe hit entity and PlayerPickingHelper `Inner[0]`
item `+0x48` (I). Expect neither to be the die.

4.6 **Cleanup.** On load start (the mod's pause-on-load) call `vtbl[0](obj, 1)`; log it and the level pointer.
On the next frames check the handle with `0x4017E10([world+0x208], &handle)` (the liveness test 0x188A720 uses):
expect false. If the level pointer changes while a die exists, log an error and drop the pointer without calling
the game (leak instead of crash).

4.7 **Face check.** Spawn the die with face 20 up (table 3.2, q maps (-0.9822, 0.1876, 0) to +Y), then 1 up.
A screenshot confirms the numbers in game.

## Files

* `agents\AR\bitknit.py`, `gr2dec.py`: GR2 section decoder (BitKnit2 port). `faces.py`, `faces_q.py`, `facegrid*.py`.
* `agents\AR\gr2\`: decoded sections, `*_faces.json` (normals, centres, vertices, UVs per face), face grids.
* `agents\AR\models\`: physics hull XML (PhysicsTool), textures (DDS/PNG), default d20_20 picture.
* `agents\AR\banks\`: the two visual banks and the Quest_D20 root template as LSX.
* Decompiles: ar1 clone base, ar2 load/scene/transform/stealth, ar3 VisualSystem, ar4 CreateAt, ar9-ar16 Granny
  and BitKnit, ar17/ar20 callers, ar18/ar19/ar21 template-visual clone, ar22 PaperdollComponent readers, ar23
  TransformSystem helpers.
