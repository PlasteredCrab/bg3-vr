# Agent P: 3D characters in UI screens (paperdoll, portraits, character creation) - bg3_dx11.exe 4.1.1.7631656

Static analysis only (Ghidra project `BG3_F`, headless, read-only, request server `agents\P\PServer.java`;
UI XAML read from agent M's extraction `agents\M\gamedata\game\`). Image base 0x140000000; every address is an
RVA. Raw decompiles / xref dumps: `re-local\ghidra\agents\P\` (cited [dN] = `dN_*.c`, [xN] = `xN_*.txt`,
[qN] = vtable dumps `qN_*.txt`). "SE" = name or layout from the Script Extender source (`downloads\bg3se-src`).
Builds on A_rendering.md (views, frames, PerCamera), B_splitscreen.md (view creation, 0x1B75D50 first noted
there), F_ui.md (GameUI, Noesis), M (XAML extraction, state events), N (per-eye PerView/PerCamera rewrite),
O (frame sizes, frame+0x42D8).

Status: complete for this pass (2026-10-20); request server stopped.

---------------------------------------------------------------------------------------------------

## Hook points for the VR mod

| Need | RVA | How to use | Confidence |
|---|---|---|---|
| Every 3D character inside a UI screen | Noesis control **`ls.LSPaperdoll`** (type reg 0x12411D0 / 0x155E6E0, ctor 0x155D5B0, vtable 0x55B8748) | Used by CharacterPanel (inventory/equipment/character sheet), PartyPanel, CompanionsPanel, Examine, Trade, SpellBook_c, PreloadedCharacterPanel_c (table in 0). Each instance renders its own live offscreen 3D scene. | Confirmed (XAML + code) |
| Hide the 2D picture (all screens at once) | **0x155D9D0** `LSPaperdoll::OnRender` (vtable +0x360) | It only does `DrawGeometry(ctx, brush=+0x290, null, geometry=+0x2A0)`. Detour it to return without drawing while VR dolls are on. Layout, mouse-rotate hit testing and the offscreen render keep working. | Confirmed [d13] |
| Hide via SE Lua instead | LSPaperdoll dependency properties | Walk `Ext.UI.GetRoot()` with `VisualChild`/`Find` (SE NsHelpers) and set `Opacity = 0` on elements of type `ls.LSPaperdoll` (keeps arrange, hit-test and the offscreen render). `Visibility = "Collapsed"` also stops ArrangeOverride 0x155DEE0, so no renderer is created (use only for the world-clone option). | SE API Confirmed; runtime behaviour Unconfirmed |
| Where the paperdoll sits on the UI panel | element `ActualWidth/Height`, Noesis `PointToScreen` (SE `P_FUN(PointToScreen)`) | Gives the 2D rect (UI pixels) where the game would draw the doll: anchor the 3D doll in front of that rect on the VR panel. | Confirmed (SE API) |
| All live paperdoll renderers | **PM = `*(GameUI + 0x2B90)`**, GameUI = `[[0x6047958]+0x130]`; entries `PM+0x270` (array of ptr), count `PM+0x26C` | Entry (vtable 0x55EE6F0, 0x90 B): `+0x08` URI FixedString, `+0x10` output texture (rf), `+0x44/+0x48` w/h, `+0x58` EntityRef {handle,+0x60 world}, `+0x68` UseCombatPose, **`+0x70` PaperdollRenderer**, `+0x78` dirty. Created by 0x13E1520 -> ctor 0x178A8E0, released 0x178AAE0. Hook 0x178A8E0 / 0x178AAE0 to track them. | Confirmed [d4, d9] |
| Per-frame update (game thread) | **0x13E1A40** (PM update, called from GameUI vf[0x68] 0x120E140 at 0x120F1E2) | After it returns all renderers have this frame's pose, camera and texture. Put the mod's per-doll work right after. | Confirmed [x4] |
| PaperdollRenderer object | vtable **0x5646D78** (size 0x530), base BasicGameObjectRenderer ctor **0x1B75A90** (vtable 0x5646EE0), visual-clone base ctor 0x1888C10 (vtable 0x55FC810) | Fields (2.3): `+0x08` clone entity, `+0x10` source entity, `+0x348` own scene entity, `+0x3A0` RenderFrame index, `+0x3A8` GameRenderView*, `+0x3B0/+0x3B8` camera entity/world, `+0x400/+0x404/+0x408` doll pitch/yaw/roll (rad), `+0x40C` spin speed (rad/s), `+0x464` hide-output, `+0x524` FoV (rad). | Confirmed [d1, d3, d5, d6] |
| Its offscreen frame and view | **0x1B75D50** creates RenderFrame "BasicGameObjectRenderer" (0x3BB1880, desc type 2, size = control px) + GameRenderView (0x17F1E60, scale 1, offset 0, cfg +0x2BD0 = 1), `0x4073230(frame, view, 0)`, `0x3AC4EA0` stages | `frame = [[[[0x6046F98]+0x1D8]+0x30 + [0x604C6FC]*0xF8]+0x18][idx]`. Output texture: **0x1B77C80** = `frame+0x42D8` if set else `frame+0x42C8[frame+0x42C0]` (null while `+0x464`). | Confirmed [d1, d5, d14] |
| Enable / disable the doll render | **0x1B79150(frameIdx, bool)** writes `frame+0x42F0`; 0x1B79290(renderer, bool) does scene + frame | Turn the offscreen render off when the mod does not need it (e.g. world-clone option). The PM update re-enables it when the entity changes (0x13E1BDE). | Confirmed [d11] |
| Doll turning | yaw `renderer+0x404` integrated by **0x1B784D0** (`+0x404 += +0x40C*dt`, writes the clone's rotation via TransformSystem 0x3C78A40) | Set `+0x40C` (rad/s) for spin, or write `+0x404` and the clone transform for an absolute angle (VR grab-to-turn). Game input path: MouseRotate drag 0x155E5D0/0x155E670 -> 0x138AC00 -> PM map `+0x318` -> 0x13E1D30 -> `+0x40C`. | Confirmed [d6, d8, d2] |
| Doll camera | camera entity `renderer+0x3B0` (made in 0x1B76330 via ecl::CameraSystem 0x3B96300, then `SetCamera 0x17F2B30(view, cam, 0)`); fit by **0x1B748F0**, applied by 0x1B74E90 -> **0x1B77520** -> camera request 0x1E1A900 | For stereo override the matrices per eye at render time (existing hook 0x3CC6CB9, N) or the controller; the game re-requests only on changes. | Confirmed [d5, d6, d8, d17] |
| Make the doll stereo (recommended) | add a 2nd GameRenderView to the doll's frame: ctor **0x17F1E60**(view, frame, 0.5,1, 0.5,0, cfg) + **0x4073230** + **0x3AC4EA0** + `0x1B75FC0(renderer, view)` + `0x17F2B30(view, cam, 0)`; resize frame with frame `vf[1]` (as 0x178AC60) to 2W x H; view 1 WriteState Scale 0.5 | Two eye images side by side in one texture from one clone/scene/light set; per-eye matrices through the same 0x3CC6CB9 rewrite the main eyes use. Section 3.1. | Code paths Confirmed; the combination is a design (Unconfirmed until run) |
| Party / dialogue portraits | `Background="{Binding Icon}"` (DataTemplates.xaml:4571, 2064/2100) | Static images: `Assets/Portraits/...` (0x13D27A0) or a one-shot captured custom icon (`Assets/Uri/CustomPlayerIcon/%d`, 0x1977600; rendered once by ecl::CharacterIconRenderSystem 0x740370 -> 0x1E99CB0, 300x300). Not live: nothing to make 3D. | Confirmed [x14, d3, d16] |
| Character creation / magic mirror / respec | `CharacterCreation*.xaml` (no LSPaperdoll); VM commands `SetCameraMode` / `SetCameraOffsets`; `DummyCharacter` | The character is a dummy in the CC level, drawn by the normal world views with a game camera: already 3D/stereo once the mod drives the camera. | Confirmed (XAML); camera code: G 0x1C9ACE0 |

---------------------------------------------------------------------------------------------------

## 0. Which UI screens show a 3D character (Confirmed from data and code)

All in-UI 3D characters are one Noesis custom control, **`ls:LSPaperdoll`** (type name `ls.LSPaperdoll`
0x55B8650, next to `Assets/Uri/Paperdoll` 0x55B8630):

| file:line (Game.pak GUI) | x:Name | notes |
|---|---|---|
| Pages/CharacterPanel.xaml:390 | CSPaperdoll | inventory / equipment / character sheet; `EntityRef="{Binding EntityHandle}"`, `UseCombatPose`, `MouseRotate`, `MouseRotateMultiplier=10`, `RenderScale`; rotate buttons set `RotationSpeed` +-1.2 (lines 396-417) |
| Pages/CompanionsPanel.xaml:198 | CSPaperdoll | camp companions |
| Pages/PartyPanel.xaml:40 | CSPaperdoll | party overview (Tab), 828x1080 |
| Pages/Examine.xaml:36 | ExaminePaperdoll | examine a character |
| Pages/Trade.xaml:529, :1046 | PlayerPaperdoll, CSPaperdoll | trade (player side and trader side) |
| Pages/Examine_c, SpellBook_c, Trade_c, Library/PreloadedCharacterPanel_c | | controller layouts, `noesis:Element.BlendingMode="Additive"`, `Zoom` |

Not paperdolls: party portraits, turn order, dialogue participant portraits (`Border/LSEntityObject
Background="{Binding Icon}"`, DataTemplates.xaml:4571 `CharacterPortraitTemplate`, :2064/:2100
`Dialogue.InactivePortrait/ActivePortrait`, PlayerPortraits.xaml:397), and character creation (section 4).

---------------------------------------------------------------------------------------------------

## 1. How a paperdoll is rendered

### 1.1 Noesis side: LSPaperdoll (Confirmed [d1, d2, d9, d12, d13, q6/q7])

* Properties registered by 0x155E6E0: `RotationSpeed` (DP 0x6005DF0, float), `EntityRef` (0x6005E00, an
  `ls.EntityRef` object: `+0x10` entity handle, `+0x18` world, compared in 0x155DB00), `UseCombatPose`
  (0x6005DF8, bool), `MouseRotate` (0x6005DE0), `MouseRotateMultiplier` (0x6005DD8, default 6.0), `RenderScale`
  (0x6005DE8, default 1.0, clamped <= 1), `ZoomModifier` (0x6005E18), `Zoom` (0x6005E08), `FoV` (0x6005E10,
  wheel-clamped 28.1..60.1 in 0x155DC30), `ScrollType` (0x6005E20, "Zoom" or "FoV").
* Ctor 0x155D5B0: makes a unique URI FixedString **`Assets/Uri/Paperdoll/<uuid>`** at `+0x298`, an
  ImageBrush at `+0x290` (0x4621220) and a RectangleGeometry at `+0x2A0` (0x4672200).
* OnConnectToView 0x155DA40 (vtable +0x90): stores the IView at `+0x2A8` and subscribes the IView
  Rendering event (`view->vf[0x140]()->vf[0x38]`, handler object `+0x2B0`, vtable 0x55B8700, target
  0x155DB00). 0x155DB00 invalidates arrange when the pixel size (`ActualSize * RenderScale`) or the
  EntityRef changes.
* **ArrangeOverride 0x155DEE0** (vtable +0x418): `w,h = finalSize * min(RenderScale,1)`;
  **0x155E160(this, w, h)** on first use creates the texture entry (below), sets the brush's ImageSource to
  the URI (0x46213E0 / 0x46220D0(3) / 0x46221F0(1) / Viewbox 0,0,1,1), pushes rotation 0x138AC00, zoom
  0x138AC70, FoV 0x138ACE0; on size change stores `(w,h)` in PM's resize map (`PM+0x2D8`, read by
  0x13E1D30 -> 0x178AC60) and sets the geometry rect.
* **OnRender 0x155D9D0** (vtable +0x360): `if geometry and rect > 0 and brush: DrawGeometry(ctx, brush,
  null, geometry)` (0x4673470). That is the only thing that puts the 3D render on screen.
* Mouse: +0x1C8 0x155E5D0 (button down: capture, `+0x2D9 = 1`, start x `+0x2E0`), +0x1D8 0x155E670 (move:
  `RotationSpeed = (start - x) * MouseRotateMultiplier / ActualWidth` via 0x155E560 -> 0x138AC00), +0x1D0
  0x155E630 (up: speed 0), +0x1F8 0x155DC30 (wheel: Zoom or FoV).
* OnDisconnect 0x155DAC0 -> 0x155E400 -> 0x13E16D0 queues the entry for removal (PM `+0x3D8` list).

`[LSPaperdoll+0x278]` is an object whose `+0x88` is the paperdoll manager PM (all of 0x13E1520,
0x138AC00/0x138AC70/0x138ACE0, 0x13E16D0 dereference `[+0x278]+0x88`). Its writer was not found inside the
LSPaperdoll code range (FindInDecompiled [f1] empty); Unconfirmed what it is (log `[el+0x278]` and compare
`[[el+0x278]+0x88]` with `*(GameUI+0x2B90)`).

### 1.2 The texture provider / paperdoll manager PM (Confirmed [d4, d9, d10, x4])

* PM is created in GameUI init 0x1210880 (`0x13E0EC0`, stored at **GameUI+0x2B90**, disasm 0x121097F/
  0x1210991) and updated every frame by GameUI vf[0x68] 0x120E140 -> **0x13E1A40(PM, ?, dt)** (0x120F1E2).
* PM derives from the game's Noesis **TextureProvider** (base ctor 0x20F0F00, vtable 0x5584E88: slot 5
  0x20F1710 GetTextureInfo(uri), slot 6 0x20F1F20 LoadTexture(uri); both strip `file://` and look the URI
  FixedString up in `+0x88`). So Noesis gets the paperdoll image by the URI in the ImageBrush; there is no
  named XAML resource key.
* 0x13E1520(PM, entityRef+0x10, uri, w, h, combat): under CS `PM+0x2B0`, map `PM+0x240` (URI -> entry,
  values `PM+0x270`, count `PM+0x26C`); new entry = 0x90-byte object, ctor **0x178A8E0** (base 0x211AC00,
  vtable 0x55EE6F0) which allocates the **PaperdollRenderer** (0x530 bytes, `0x1B75A90(r, w, h, combat, ..,
  1, 0)`, vtable **0x5646D78**) into `entry+0x70` and subscribes `0x178AE60` (sets `entry+0x78` dirty when
  the visual of `entry+0x58` changes).
* 0x13E1A40 (per frame): processes the removal queue, then for each entry: if dirty and not yet done this
  frame (one per frame), `0x1B74250(renderer, entry+0x58)` (set character) and `0x1B79290(renderer, 1)`
  (enable); if not set up `0x1B72D50` (scene setup); `0x1B744A0(renderer, dt)` (camera / spin); `entry+0x10
  = 0x1B77C80(renderer)` (output texture) and a texture view for it (0x40BEFE0) at `entry+0x18..+0x2F`; then
  0x13E1D30 applies the per-URI maps: size `+0x2D8` -> 0x178AC60, spin `+0x318` -> `renderer+0x40C`, zoom
  `+0x358` -> 0x1B78940, FoV `+0x398` -> camera FOV `[[cam]+8]+0x14` (degrees -> rad, +5 deg if flag).

### 1.3 PaperdollRenderer = BasicGameObjectRenderer (Confirmed [d1, d3, d5, d6, d8, d11, d14])

Class chain: visual-clone base (ctor 0x1888C10, vtable 0x55FC810) -> BasicGameObjectRenderer (ctor
0x1B75A90, vtable 0x5646EE0, dtor body 0x1B76070) -> PaperdollRenderer (vtable 0x5646D78, dtor 0x178ABD0).
A second derived class, vtable 0x59180B8 (300x300), is the character-icon renderer (section 4).

Vtable 0x5646D78: +0x08 0x1B77FC0 init(visual template), +0x10 0x1B78150, +0x18 0x1B77DD0 set from
ecl::Character (-> base 0x188A4C0), +0x20 0x1B77E10 set from item (-> 0x188A320), +0x28 0x188A0F0, +0x30
0x1889EE0, +0x38 0x1B72D00 is-ready, +0x40 **0x1B72D50 scene setup**, +0x48 0x1B740F0 attach view to scene,
+0x50 **0x1B74250 set EntityRef**, +0x58 0x1B782C0, +0x60 **0x1B744A0 per-frame update**, +0x68 **0x1B77520
set camera**, +0x70 0x1B74810 set FoV.

**Offscreen frame and view, 0x1B75D50** (called from the ctor, or queued as 0x1B79390 when not on the right
thread):
```
desc = { +0x00 0, +0x08 w,h (render), +0x10 w,h (upscaled), +0x18 w,h (display), +0x20 0, +0x28 type 2 }
idx  = 0x3BB1880(frameMgr = [[[0x6046F98]+0x1D8]+0x30 + [0x604C6FC]*0xF8], &desc, "BasicGameObjectRenderer")
                 // type 2 -> new 0x43A0-byte RenderFrame, ctor 0x4072580(frame, desc, 5), vtable 0x58EDD18
renderer+0x3A0 = idx;  frame+0x42F0 = 0 (not rendering yet)
view = alloc(0x2D90); 0x17F1E60(view, frame, 1.0, 1.0, 0.0, 0.0, cfg)  // cfg 0x64 B on stack, byte +0x60 = 1
renderer+0x3A8 = view;  0x4073230(frame, view, 0);  view+0x2274 = p4;  0x3AC4EA0([0x6046F98], view)
renderer->vf[0x48](renderer, view)                                       // 0x1B740F0: scene/atmo/lighting
```
So each paperdoll is a **separate RenderFrame (type 2, own output) with one full GameRenderView** (full deferred
pipeline: A's normal stage list, plus 0x45/0x07 because `frame+0x40 != 0`), sized to the control's pixel
size. It is independent of the player/eye views, the special view and the UI render (F 1.2).

**Scene setup 0x1B72D50** (once):
* `0x1B76330`: creates a **camera entity** (name = random GUID, `0x3B96300` = ecl::CameraSystem
  (type idx 0x6058D2C) create, components set to 3/5/6/0xC) into `+0x3B0/+0x3B8`, then
  **`0x17F2B30(view, camera, 0)`** (GameRenderView::SetCamera, A slot 10) and `+0x414 = 1`.
* Creates a new entity with an **`ls::Scene`** component (type idx 0x60A567C, name from the initialiser
  0x203FD0) via 0x4017CE0 + 0x4005520, stores it at `+0x348` (0x18890D0) and binds the view to it:
  `0x3CCC550(view, 0x1852350(world, +0x348))` (0x3CCC550 = set view scene, `view+0x2B10`).
* Atmosphere `+0x468` (0x3D512F0, 0x378 B) and lighting `+0x470` (0x3D51980, 0x2F0 B; near 0.1, far 300 at
  +0x280/+0x284) bound with 0x1B76990 -> `0x3CC9700(view, atmo)` and 0x1B77150 -> `0x3CCAAC0(view,
  lighting)` (names `Atmosphere_Paperdoll` 0x54EE6B0 / `LightProbe_Paperdoll` 0x54EE5D0 sit in the same data
  table; which one is passed (DAT_145EECD88) is Unconfirmed).
* IBL probe 0x1B77B00 with `Public\Game\Assets\Textures\Paperdoll\Paperdoll_IBL_{Specular,Diffuse}_{RGB,
  Alpha}.dds` (0x5646BD0.., table 0x5646DF0) -> `+0x3F0` list.
* **Three lights** 0x1B77810 -> `0x3C75390(lightMgr +0x420, ..., desc, scene)` -> `+0x3E0` list: directions
  (3, 2.5, -3), (-2, 2.5, -2), (-3, 1, 4) (x4, +1.5 up), colours (1, .9, .7) / (1, .95, .85) / (.4, .7, 1),
  intensities 200000 / 40000 / 400000 (floats 0x48435000, 0x471C4000, 0x48C35000). They follow the camera
  (0x1B74E90 re-positions them from the camera rotation every time the camera changes).
* `+0x528 = 1`.

**The character shown is a clone, not the real entity** (base class, [d11]):
* 0x1B74250(renderer, EntityRef): character -> vtable +0x18 with `0x7841A0(world, handle)` (ecl::Character),
  else item -> +0x20 (0x793890).
* 0x188A4C0 (from character): takes the character template's visual resource (0x1C6B540, resource type 0x1A)
  and calls init (+0x08). 0x188B510 then **creates a new ECS entity** (`+0x08`), adds components incl.
  **`ecl::PaperdollComponent {Entity = source +0x10, Combat = +0x331}`** (0x6D18D0, which reads the
  PaperdollComponent type index 0x605388C, registration 0x7EFC0), copies the source's appearance, equipment
  and material lists (four `param_2` arrays) and requests the visual load (0x3BF4F20); the visual goes into
  the scene entity `+0x348` (0x3CF1E00 -> 0x1852350 -> 0x3CF11E0 = add visual to ls::Scene).
* Equipment changes reach clones: the visual/equipment code path 0x198A770 checks for PaperdollComponent on
  the entity and fires the clone listeners (`+0x30` list) (Confirmed for the check; full propagation path
  Unconfirmed).
* Pose: `UseCombatPose` -> PaperdollComponent.Combat; animation is the clone's own (normal animation systems
  run on it).

**Per-frame 0x1B744A0**: 0x1B784D0 (spin: `+0x404 += +0x40C*dt`, wrap 2pi, quaternion from `+0x400/+0x404/
+0x408` written with TransformSystem 0x3C78A40, type idx 0x6058CBC); then camera fit **0x1B748F0** from the
clone visual's bounds (`vis+0x40..+0x54`), vertical FOV base 2*0.2452 rad = 28.1 deg; smooth 0.05 s blend
(`+0x478` current, `+0x4CC` target, `+0x520` timer); **0x1B74E90** -> **0x1B77520(this, FoV, rot, eye,
rot2, target, 0.1, 300)** stores `+0x428..+0x463` and issues the camera request 0x1E1A900 -> 0x1E11480
(ecl::CameraSystem); it also sets `view+0x2CA0` (0x3CC90B0).

**Output**: 0x1B77C80 = `frame+0x42D8` (output override target, O 1.6) if set, else
`frame+0x42C8[frame+0x42C0]`; null while `+0x464` (set during a deferred resize, 0x178AC60). The PM stores it
in `entry+0x10` and LoadTexture hands it to Noesis. Exact rf-texture -> ID3D11Texture2D path Unconfirmed
(log the object and its D3D pointer at runtime).

### 1.4 Rotation input summary (Confirmed)

Mouse drag on the LSPaperdoll -> `RotationSpeed` (signed, rad/s scale) -> PM map `+0x318` -> 0x13E1D30 ->
`renderer+0x40C` -> 0x1B784D0 rotates the **clone entity's Transform**. The UI rotate buttons set
`RotationSpeed` +-1.2 the same way. Zoom/FoV only change the doll camera.

---------------------------------------------------------------------------------------------------

## 2. Hiding the 2D image

| method | effect | cost / risk |
|---|---|---|
| **A. Detour 0x155D9D0** (LSPaperdoll OnRender) to skip `DrawGeometry` when VR dolls are on | All paperdolls vanish from every screen; layout, hit-testing (mouse-rotate), the PM entry and the offscreen 3D render keep running | One function, no per-screen work. Confirmed [d13]. Recommended for options 3.1 / 3.2. |
| B. SE Lua: set `Opacity = 0` on each `ls.LSPaperdoll` (find by walking `Ext.UI.GetRoot()` VisualChild/Child, type name, or `Find("CSPaperdoll")` inside each widget) | Same as A per element; needs re-applying when widgets reload | Opacity-0 elements still hit-test in WPF semantics (Noesis follows WPF; Unconfirmed in-game). |
| C. SE Lua `Visibility = "Collapsed"` | No ArrangeOverride -> 0x155E160 never runs -> no renderer, no GPU cost | Use only with option 3.3 (world clone), which does not need the game's renderer. |
| D. Disable the offscreen render: `0x1B79150(renderer+0x3A0, 0)` / `renderer+0x464 = 1` | Saves GPU but the brush then shows a stale/empty texture | Combine with A; the PM re-enables on entity change (0x13E1BDE). |

---------------------------------------------------------------------------------------------------

## 3. Showing the same character in 3D in VR

Common facts: the doll's world is a private `ls::Scene` (renderer `+0x348`) containing only the clone, three
lights, atmosphere/lighting and an IBL probe; its camera is a normal ecl camera entity (`+0x3B0`) driving a
normal GameRenderView (`+0x3A8`). The mod's existing per-view camera rewrite (hook 0x3CC6CB9, PerCamera
`view+0x2660/+0x2740`, PerView `view+0x2340/+0x24D0`, N) runs for every view's begin-recording, the doll view
included (begin-recording 0x3CC6C00 is the base for all GameRenderViews, A 2).

### 3.1 (a)+(c) Recommended: keep the game's paperdoll renderer, add a second eye view, drive both per eye

What the code supports (each step is a game function used the same way by 0x1B75D50 / B's add-view path):
1. Track renderers: hook 0x178A8E0 (entry created) / 0x178AAE0 (released) or scan `PM+0x270` after 0x13E1A40.
   Wait for `0x1B72D00(renderer) != 0` (setup done: camera, scene, `+0x528`).
2. Add view 2 to the same frame (game thread or the renderer's own deferral path, see 0x1B75A90):
   `v2 = alloc 0x2D90 (0x4175960(0x2D90,0x10))`; `0x17F1E60(v2, frame, 0.5, 1.0, 0.5, 0.0, cfg)` with cfg =
   the 0x64-byte block copied from view 1 `+0x2B70..+0x2BD3` (A 3.1); `0x4073230(frame, v2, 0)`;
   `v2+0x2274 = view1+0x2274`; `0x3AC4EA0([0x6046F98], v2)`; `0x1B75FC0(renderer, v2)` (scene, atmosphere,
   lighting); `0x17F2B30(v2, cameraController, 0)` with the same camera as view 1 (0x1B76330 shows how it gets
   it: `0x17F2B30(view, [component(+0x3B0)]+8, 0)`).
3. Make view 1 the left half: WriteState Scale (0.5, 1) / Offset (0, 0) at `view+0x21E0+0x08/+0x10` (A 3.2;
   same layout as stereo.md's eye views).
4. Double the frame width: frame `vf[1](frame, 2W, H, 2W)` as 0x178AC60 does (game thread uses the deferred
   path 0x1B79500 with `renderer+0x464 = 1` while pending). The PM resizes back only when the LSPaperdoll's
   arranged size changes (map `+0x2D8`), so re-apply after each 0x178AC60 call (hook it).
5. Per-eye cameras: in hook 0x3CC6CB9, when `rdi` is one of the doll views, write PerCamera/PerView like for
   the main eyes (N's table) with **doll-space eye matrices**: pick the VR anchor pose A of the doll (next to
   or in front of the panel, scale s, e.g. 0.25) and the clone's own position (renderer camera target
   `+0x448..+0x450` or the clone transform); eye camera in doll space `C_e = T_clone * S(1/s) * A^-1 * E_e`
   (E_e = tracked eye pose), projection = the HMD eye FOV (full-FOV projection layer) or an off-axis frustum
   through the doll's quad (quad layer). Also N's stage 0x1B (0x3E32C44) and 0x40 (0x3E12730) hooks for these
   views, or keep TAA off for them.
6. Present: output texture (1.3) holds left | right; copy each half to an OpenXR swapchain image and submit
   either (i) a projection layer with `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` (+ optional depth
   via XR_KHR_composition_layer_depth from the view's depth target) or (ii) two quad layers with
   `XR_EYE_VISIBILITY_LEFT/RIGHT` at the doll anchor (off-axis cameras). Needs a transparent background:
   whether the doll frame's alpha is 0 outside the character is **Unconfirmed** (the KB/M XAML draws it with
   normal alpha over the panel; the controller XAML uses Additive) - read back one corner pixel's alpha.
7. Hide the 2D draw (2.A).

Lighting: the game's own paperdoll lights + IBL (exactly what the 2D view shows), consistent per eye because
both views share the scene. Turning/posing: unchanged game behaviour (spin `+0x40C`, combat pose). Cost: one
extra small deferred render per doll (frame size = 2 x control pixels x RenderScale; the mod may raise the
size through 0x178AC60 for sharpness). Only one paperdoll is normally visible per screen (Trade has two).

### 3.2 (c-cheap) Reuse the single doll view, alternate eyes per frame

No new view: in 0x3CC6CB9 write the left-eye matrices on even frames, right on odd, and keep the last image
per eye. Zero new engine objects, but each eye updates at half rate and animation/rotation is temporally
offset between eyes. Not recommended except as a first test of the per-eye doll camera math.

### 3.3 (b) Clone in the main world

The visual-clone base (0x1888C10) puts its visual into whatever scene entity is at `+0x348`
(0x18890D0 sets it; 0x3CF1E00 adds the visual to that entity's `ls::Scene`). A clone in the main world would
need: an `ls::Scene` entity for the current level (SE: `ls::Scene` is a proxy component with an
`ls::SceneRoot` tag; the level root from the character's `LevelComponent.RootLevel` is the likely owner -
**Unconfirmed**, check `0x1852350(world, rootLevel) == mainView+0x2B10`), a helper object of the base class
(vtable 0x55FC810; other game users: 0x69EC20, 0x19CBD00, 0x1EDB480, 0x1BD4CB0, 0x1BD5F60 - not identified),
then `+0x18` 0x188A4C0(helper, ecl::Character*) and the clone transform each frame (TransformSystem
0x3C78980 position / 0x3C78A40 rotation as 0x1B74E90 / 0x1B784D0 do).
Pros: native stereo and depth, no extra render. Cons: lit by the level (dark interiors), occluded by world
geometry, more engine objects created by the mod, and no game-provided rotation/camera. Do NOT redirect the
paperdoll renderer's own `+0x348`: its three 200000-400000 intensity lights would land in the world scene
(0x1B77810 uses `+0x348`).

### 3.4 Placement in VR

The doll's UI rect: LSPaperdoll `ActualWidth/Height` and `PointToScreen` (SE) or the arranged size stored in
PM map `+0x2D8`. Map the rect centre through the panel's UV to the VR panel pose and put the doll anchor in
front of / beside it. Rotation by grab: write `renderer+0x40C` (spin) or `+0x404` (absolute yaw) on the game
thread before 0x13E1A40 (0x1B784D0 only writes the transform when `|+0x40C| > eps`).

---------------------------------------------------------------------------------------------------

## 4. Portraits, dialogue, character creation, mirror

* **Party / turn-order / dialogue portraits are static textures**: XAML binds the VM `Icon` brush
  (DataTemplates.xaml:4571, 2064, 2100; PlayerPortraits.xaml:397). Origins/NPCs: `Assets/Portraits/` +
  name (0x5573040, formatted in 0x13D27A0 and 9 other VM functions [x1]). Custom characters / hirelings:
  `Assets/Uri/CustomPlayerIcon/%d` (0x56110E0 in 0x1977600) - an image produced once by
  **ecl::CharacterIconRenderSystem** (registration 0x740370, type idx 0x6059144, update 0x75E990 ->
  0x1E99CB0): a 300x300 BasicGameObjectRenderer (vtable 0x59180B8) poses the character, waits up to 3 s /
  60 frames, then queues a capture of its frame (0x1E9A7E0 -> 0x3BB31B0 on the frame manager) and disables
  the frame. Not per-frame (Confirmed); where the capture is stored (file vs memory) Unconfirmed.
  Related types: ecl::CustomPortraitTextureComponent, ecl::CustomIconTextureComponent,
  eoc::CustomIconComponent (strings 0x54B7DC0, 0x54B7AC0, 0x54C9660).
* **Dialogue**: no LSPaperdoll in Dialogue.xaml; speakers are the real world characters (G covers the
  dialogue camera). Portrait strip = static icons as above.
* **Character creation, respec, magic mirror**: CharacterCreation(_c).xaml / CharacterRespec /
  CharacterFullRespec have no LSPaperdoll; they bind `DummyCharacter` and call `SetCameraMode`
  ("Overview", ...) / `SetCameraOffsets` (CharacterCreation_c.xaml:125-163, CharacterCreation.xaml:1156),
  states `CharacterCreation` / `CharacterRespec` (Keyboard.xaml:248, 367, `ForceSplitscreen="True"`). The
  character is a dummy in the CC level (`CharacterCreationLevelName` 0x590C8D0, `IsCharacterCreationLevel`
  0x5868ED0) rendered by the normal player view with a game camera (G: CC camera 0x1C9ACE0). In VR it is
  already 3D and stereo if the mod keeps driving the camera from the headset; nothing to hide. Unconfirmed:
  that the mirror uses the same CC state (the `OpenCharacterCreation` events at Keyboard.xaml:294/524 are
  the candidates).
* `ecl::portrait_painting::*` / `eoc::portrait_painting::DataComponent` are the in-world painted portrait
  objects (camp), unrelated to UI.

---------------------------------------------------------------------------------------------------

## 5. Function / data index

| rva | what |
|---|---|
| 0x155D5B0 / 0x155D8C0 | LSPaperdoll ctor / dtor (vtable 0x55B8748; +0x18 0x55B8C28, +0x1A8 0x55B8BD0, +0x270 0x55B8B70) |
| 0x155E6E0, 0x12411D0 | LSPaperdoll DP registration / type `ls.LSPaperdoll` |
| 0x155DA40 / 0x155DAC0 | OnConnectToView / OnDisconnect |
| 0x155DB00 | IView Rendering handler (size/entity change) |
| 0x155DEE0 | ArrangeOverride (creates/resizes the entry) |
| 0x155D9D0 | OnRender (DrawGeometry with the paperdoll brush) |
| 0x155E160 | create entry + brush (calls 0x13E1520) |
| 0x155E5D0 / 0x155E670 / 0x155E630 / 0x155DC30 | mouse down / move / up / wheel |
| 0x155E560 | set RotationSpeed -> 0x138AC00 |
| 0x138AC00 / 0x138AC70 / 0x138ACE0 | PM maps: spin / zoom / FoV per URI |
| 0x13E0EC0 | PM ctor (GameUI+0x2B90), base TextureProvider 0x20F0F00 (vtable 0x5584E88) |
| 0x20F1710 / 0x20F1F20 | TextureProvider GetTextureInfo / LoadTexture |
| 0x13E1520 / 0x13E1790 / 0x13E16D0 | PM add entry / remove entry / queue removal |
| 0x13E1A40 / 0x13E1D30 | PM per-frame update / apply maps |
| 0x178A8E0 / 0x178AAE0 / 0x178ABD0 | entry ctor (vtable 0x55EE6F0) / release / renderer dtor |
| 0x178AC60 | entry resize -> frame vf[1] |
| 0x1B75A90 / 0x1B75D50 / 0x1B76070 | BasicGameObjectRenderer ctor / frame+view creation / dtor body |
| 0x1B72D50 | paperdoll scene setup (scene, camera, atmosphere, lighting, IBL, 3 lights) |
| 0x1B76330 | create camera entity + SetCamera |
| 0x1B744A0 / 0x1B784D0 | per-frame update / spin |
| 0x1B748F0 / 0x1B74E90 / 0x1B77520 | camera fit / apply / set camera (request 0x1E1A900) |
| 0x1B78940 / 0x1B74810, 0x1B78BC0 | zoom / FoV |
| 0x1B74250 | set EntityRef (character or item) |
| 0x1B77C80 | output texture of the doll frame |
| 0x1B79150 / 0x1B79030 / 0x1B79290 | frame enable / scene enable / both |
| 0x1888C10 / 0x188A4C0 / 0x188A320 / 0x188B510 | visual-clone base ctor / from character / from item / create clone entity + visual |
| 0x6D18D0 | add ecl::PaperdollComponent (type idx 0x605388C) |
| 0x3BB1880 | RenderFrame create (type 2 = own offscreen frame) |
| 0x3CCC550 / 0x1852350 | set view scene / get ls::Scene of entity (type idx 0x60A567C) |
| 0x3CF1E00 / 0x3CF11E0 | add visual to scene entity / to scene |
| 0x740370 -> 0x75E990 -> 0x1E99CB0 | ecl::CharacterIconRenderSystem (300x300 icon renderer, vtable 0x59180B8) |
| 0x1E9A7E0 | icon capture request (0x3BB31B0) |

---------------------------------------------------------------------------------------------------

## Open leads

1. **Background alpha of the doll frame** (decides projection/quad layer blending): read back the output
   texture (0x1B77C80) at a corner while the inventory is open; also log its DXGI format and the
   rf-texture -> ID3D11Texture2D pointer.
2. **Second view in a type-2 frame**: run 3.1 steps 2-4 once with heavy logging (frame size before/after,
   both views' ReadState rects, stage lists from 0x3AC4EA0, `view+0x2B10` scene equal for both). The frame
   `vf[1]` argument meaning (4th arg) is inferred from 0x178AC60 only.
3. **`[LSPaperdoll+0x278]` owner** (the object whose +0x88 is PM): log it and compare with GameUI+0x2B90.
4. **Main-world `ls::Scene` entity** for option 3.3: log `0x1852350(world, LevelComponent.RootLevel)` against
   the main view's `+0x2B10`.
5. Identify the other users of the visual-clone base (0x69EC20, 0x19CBD00, 0x1EDB480, 0x1BD4CB0, 0x1BD5F60)
   - one may already be a world-space clone (timeline/photo-mode dummies) the mod could reuse.
6. Atmosphere/lighting resource ids used by the doll (DAT_145EECD88; `Atmosphere_Paperdoll` /
   `LightProbe_Paperdoll` strings) - only needed if the mod wants to relight the doll to match the world.
7. Where CharacterIconRenderSystem's capture (0x3BB31B0 request) ends up (file in the profile or memory
   image) - only relevant if live 3D portraits are ever wanted.
8. Whether the magic mirror opens the `CharacterCreation` state (Keyboard.xaml `OpenCharacterCreation`) or a
   respec state; both are world-rendered, so this only affects camera handling (G).
