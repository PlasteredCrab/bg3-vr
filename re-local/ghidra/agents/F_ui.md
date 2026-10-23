# Agent F: UI (Noesis GUI) in bg3_dx11.exe 4.1.1.7631656

Static analysis only (Ghidra project `BG3_F`, headless request server `agents\F\FServer.java`;
`bg3vr_disasm`). Image base 0x140000000; every address is an RVA. Raw decompiles and xref dumps are in
`re-local\ghidra\agents\F\` and are cited by file name ([d13] = `d13_*.c`, [x6] = `x6_*.txt`, [f3] =
`f3_*.txt`, [str_ui] = `str_ui*.txt`). Names marked **SE** come from the Script Extender source
(`downloads\bg3se-src`: `GameDefinitions\UI.h`, `Resources.h`, `GameState.h`, `Enumerations\Engine.inl`,
`GameHooks\BinaryMappings.xml`); everything else is read from the cited decompile or disassembly.
**Confirmed** = shown by the cited code. **Unconfirmed** items say what would confirm them.

Builds on: A_rendering.md (stage list, special view, resource keys, RenderView layout),
B_splitscreen.md (player/view manager), C_animation_systems.md section 3 (SDL pump 0x57AAA0,
InputManager, InjectRaw 0x40F1F00, input event ids), docs/re-notes/stereo.md (two-view stereo).

Status (2026-10-19, final for this pass; request server stopped): sections 1-4 done as far as the exe goes. The widget XAML itself (dialogue,
hotbar, radial menus) lives in `Data\Game.pak`, not in the exe; see section 5 and Open leads.

---------------------------------------------------------------------------------------------------

## Hook points for the VR mod

| Need | RVA | How to use | Confidence |
|------|-----|-----------|------------|
| Get the UI as its own texture | resource key **0x0C419ABC** (created by 0x3CC0EA0, rendered by special-view stage 0x19 job 0x3CC04F0) | The whole Noesis UI is already rendered once per frame into its own full-frame target (key 0x0C419ABC, depth/stencil 0xFF6F1F7D). Read it after stage 0x19 has executed (e.g. when the special view's stage 0x07 job 0x3E4D2A0 runs, or at Present) through the stage resource manager (`0x4098180(mgr, 0x0C419ABC)`, A section 7) and copy it into an OpenXR quad-layer / wrist-panel texture. | Confirmed (producer and consumer); exact D3D object path from the key Unconfirmed (log the ID3D11Texture2D at runtime) |
| Remove the UI from the flat/eye composite | stage 0x07 job **0x3E4D2A0** (UI SRV bind at 0x3E4D2FE), init 0x3E4C6F0 | The composite binds the UI texture (slot 1) only when `[stage+0x2188]` and byte `[[0x604B1B8]+0x0D]` are non-zero; otherwise a default (null) SRV. Patch the bind (or clear `[stage+0x2188]` before init, which selects `CombineUI_NoUI*.bshd`) so the eye images carry no UI. | Confirmed (code path); shader effect of the null SRV Unconfirmed |
| UI size / resolution | GameUI vtable slot 12 **0x120E0E0** `SetSize(w,h)` (called by 0x585510 on resize, 0x586A80 at init with `RenderFrame+0x4328/+0x432C`) | Hook or call with the panel size you want; the Noesis view is resized in NUIM update 0x20C2FD0 (`IView::SetSize`, scale = h / 2160). Do it after every resize because 0x585510 re-applies the frame size. | Confirmed |
| Pointer position for the UI | cursor float2 at **`*(float**)0x614FB70`** (+0 x, +4 y; frame pixels, 2.1) | NUIM reads it on every UIMouseMove event (0x20C3660) and every update (0x20C2FD0) and passes `(int)x,(int)y` straight to `IView::MouseMove`. Write the laser hit point in UI-view pixels there, then inject raw mouse motion (`0x40F1F00(im, {0x80, dev 1, 1.0, 1.0, 1}, true)`) so the InputManager emits event 0x4E. The SDL pump overwrites it only on real mouse motion over the game window. | Confirmed |
| Clicks / wheel for the UI | input events **1 UILeftMouse, 2 UIRightMouse, 0x5E/0x5F UIScrollUp/Down** (registered by 0x4161610) -> 0x20C3B70 | Inject raw mouse buttons on device 1 (SE ids: left2 = 123, right2 = 125, wheel 133-136) through 0x40F1F00; the bindings produce events 1/2 and NUIM calls `IView::MouseButtonDown/Up` at the cursor global. | Event path Confirmed; raw ids from SE |
| Direct Noesis input (alternative) | IView vtable **0x534FB00**: `+0xB8` MouseButtonDown(x,y,btn), `+0xC0` MouseButtonUp, `+0xC8` MouseDoubleClick, `+0xD0` MouseMove(x,y), `+0xD8` MouseWheel(x,y,delta), `+0x108..+0x118` TouchDown/Move/Up(x,y,id); view = `*(IView**)(NUIM+0x70)`, NUIM = GameUI+0xC0 | Call under NUIM's critical section `NUIM+0x198` (every game call site takes it), on the game thread. Bypasses the game's double-click and "mouse over UI" bookkeeping (NUIM +0x90/+0xE0/+0x180), so prefer the event route above. | Confirmed (slots from decompiles of each slot) |
| Keep keyboard/mouse UI mode | **0x5E3C60** (auto input-mode switch), setters **0x5EB2B0** (controller, sets byte **0x614FBC8** = 1) / **0x5EB490** (KB/M, = 0) | Any input from a controller device (ids 3..10) switches the whole game and UI to controller mode (`_c.xaml` layouts, mouse ignored by NUIM flag 0x10). If the mod injects stick input on device 3 (C 3.2) the laser stops working. Inject movement as keyboard (device 0) or patch 0x5E3C60. | Confirmed |
| World-anchored UI projection (nameplates, overhead text, tooltips, character rects) | **0x407BF00** `RenderView::WorldToScreen(view, pos, &x, &y, mat*)`, view from **0x17F72A0** / **0x17F7330** -> 0x3BAAE20 (player's view) | Uses the view's camera controller VP (`[view+0x21D8]+0x190`) and its **upscaled viewport rect** (`view+0x21A0..+0x21AC`). In the stereo layout this is the main view = left eye half. Hook it to project with a centre-eye camera into the UI panel's pixel space. | Confirmed |
| Screen-space -> UI scale | NUIM `+0xD0` design height (2160), `+0xD4/+0xD8` view size, `+0xDC` scale | `scale = h / 2160`; MainCanvas logical size = `(w / scale, 2160)` (0x20C2FD0). | Confirmed |
| Is the pointer over the UI | NUIM `+0x180` (SE MouseOverUI), `+0x181` (SE PointOverUI), `+0x190` pointer in content space | Set by 0x20C3EF0 / 0x20C2FD0 from the cursor global with the hit test 0x2106F90. Use it to decide whether the laser click goes to the UI or the world. | Confirmed |

---------------------------------------------------------------------------------------------------

## 1. Rendering

### 1.1 Objects (Confirmed)

| Object | Where | Evidence |
|--------|-------|----------|
| `ls::gGlobalResourceManager` (SE ResourceManager) | global **0x6047958** | its `+0x80` is used as shader manager by the Bink player (0x21B5530), `+0x88` = Wwise sound manager (vtable 0x56B0C10 next to `WwiseGlobal` strings, created in 0x586A80), `+0x90` / `+0x98` = the two Bink video players (class vtable 0x56D78C8, ctor 0x21B5200, update slot 10 0x21B6BE0 = `BinkDoFrameAsync...`, render slot 11 0x21B6DD0), **`+0x130` = GameUI** (SE `ResourceManager::UI`) [d3, d8, d4] |
| GameUI (SE `ui::GameUI`) | `[0x6047958]+0x130`; ctor **0x120BAE0**, dtor 0x120D630 | primary vtable **0x555EF78** (installed at 0x120BB8A), secondary interfaces: `+0x08` vtable 0x555F068 (input listener, see 2.3; its slot 3 0x1207740 returns the name `"NoesisGameUI"`), `+0x10` 0x555EF20, `+0x20` 0x555EF60, `+0x30` 0x555EEB0 [x5, d18] |
| GameUIParams (SE) | GameUI `+0x50` (0x38 bytes) | `+0x58` stage pointer, `+0x60/+0x64/+0x68` render-target keys, `+0x78` design height, `+0x7C/+0x80` screen width/height, `+0x84` active flag (vtable slot 8 0x120E0B0 returns it) [d10, d18] |
| NoesisUIManager (SE) = **NUIM** | GameUI **+0xC0** | `+0x38` RenderContext (`"ns::RenderContext"`), `+0x40` RenderDevice, `+0x60` MainCanvas (`MainCanvas.xaml`), `+0x70` **IView**, `+0x78` second view (SE: DebugOverlay), `+0x80` third view (updated by NUIM update), `+0xD0` design height, `+0xD4/+0xD8` view size, `+0xDC` scale, `+0x180..+0x182` mouse-over flags, `+0x188` controller point, `+0x190` view mouse position, `+0x198` CRITICAL_SECTION, `+0x1C0` flags (bit 0x01 resize pending, 0x10 controller mode / mouse disabled, 0x80 text-input mode) [d4 0x20C2780, d30 0x20C1780, d15, d13] |

The GameUI ctor sets the defaults `GameUI+0x78 = 0x870` (2160), `+0x7C = 0xF00` (3840), `+0x80 = 0x870`
and inside the NUIM copy `+0x190 = 0x870`, `+0x194 = 0xF00`, `+0x198 = 0x870`, `+0x19C = 1.0f` [d18 lines
`param_1[0xf]`, `param_1[0x32]`]. So the UI is authored for a 3840x2160 reference and the design height
(NUIM+0xD0) is 2160. No other writer of GameUI+0x190 was found in 0x1200000-0x1220000 [f4]
(Unconfirmed for the rest of the exe; a runtime read of NUIM+0xD0 settles it).

### 1.2 Where Noesis renders: special view, stage 0x19, key 0x0C419ABC (Confirmed)

Stage 0x19 (vtable 0x58B4FC8, the special/compositing view's first stage, A 4.2):

* **Init 0x3CBFC30** [d2]: creates key **0xFF6F1F7D** (depth-stencil, fmt 0x5230002) with 0x40955F0, and
  via **0x3CC0EA0** creates/binds key **0x0C419ABC** (colour, fmt 0x5120002 / 0x5410002) with
  `0x409B0D0(mgr, 0x0C419ABC, divisor 1, ...)`, i.e. sized from the frame (A section 7). Then
  `GameUI->vf[0x80](0x0C419ABC, 0xFF6F1F7D, 0x1AF4071A)` (slot 0x120FCE0 stores them in
  GameUIParams +0x60/+0x64/+0x68) and `GameUI->vf[0x78](stage)` (0x120FCC0, stores the stage at
  GameUIParams +0x58). Both forward the params to the RenderDevice with 0x20C2530 [d10, d11].
* **RenderDevice** (NUIM+0x40, init **0x2128E90**, called from NUIM init 0x20C1780): stores the command
  buffer `+0x38`, stage `+0x40`, colour key `+0x4C` (0x0C419ABC), depth key `+0x48` (0xFF6F1F7D), key
  `+0x50`; builds the on-screen pipeline state for RT `[+0x4C]` (fmt 0x5410002) + depth `[+0x48]`
  (0x212FAF0 -> `+0x88/+0x98`), binds it (0x212FF30 -> `+0xA8`), and sizes its offscreen
  (layer/effect) targets from the screen size (0x212EB70, grow-only resize 0x212F6D0 under base key
  0x179E264). Noesis shaders are `Noesis/v3_1/*.bshd` (0x212B570). [d12, d11, d30]
* **Job 0x3CC04F0** (render thread) [d2]: if the main view's stage 0x0B is not running it records an
  operation on key 0x1AF4071A (0x4097F70 then 0x40C9690; disasm 0x3CC0566-0x3CC058D); Bink player `[0x6047958+0x98]` renders if active;
  Bink player `+0x90` renders if active (`vf[0x58]`, letterboxed full-screen video, 0x21B6DD0), otherwise
  the UI target is cleared to 0 (`0x40979D0(mgr, .., 0x0C419ABC)` at 0x3CC0596-0x3CC059F, then opcode 9);
  then **`GameUI = [0x6047958+0x130]; if (GameUI->vf[0x40]()) GameUI->vf[0x98]()`**.
* **GameUI vf[0x98] = 0x120FD10** (Render) [d10]: if active (`+0x84`) and state `+0x2D94 == 5`: for the
  main view (`NUIM+0x70`) and the second view (`NUIM+0x78`) takes `IView::GetRenderer` (IView slot +0x30)
  and calls renderer `vf[0x58]` and `vf[0x68](0,0)` (by Noesis' IRenderer order these are
  RenderOffscreen and Render(flipY = false, clear = false); slot names Unconfirmed) with `RenderDevice+0x6B428 = 1` around it. While the UI is being
  reloaded (`+0x2DB4 != 0`, values 2..6 written by 0x12112D0 / 0x1211740 [f11]) it renders `+0x140`'s
  view or the main view after `vf[0x48]` (render-tree update), 0x20C35E0.

So **the UI is drawn once per frame, into its own full-frame texture 0x0C419ABC, in the special view;
it does not depend on the number of player/eye views.**

### 1.3 Composite onto the back buffer: special view stage 0x07 (Confirmed)

Stage 0x07 (vtable 0x58C70C0): init **0x3E4C6F0**, prepare 0x3E4D0F0, job **0x3E4D2A0** [d1].

* Init picks the shader pair by `[stage+0x2188]`: non-zero -> `CombineUI.bshd` / `CombineUI_HDR.bshd`,
  zero -> `CombineUI_NoUI.bshd` / `CombineUI_NoUI_HDR.bshd`; plus `SoftCursor.bshd` (stage+0x2150).
* Prepare sets `[stage+0x2189]` when a scene view is active and then binds key 0x684EE890 (scene colour)
  as input; otherwise unbinds it.
* Job: viewport = the **display** viewport of the special view (`0x407B8C0(view,&r,[frame+0x30],
  [frame+0x34],..,2)`, A 3.2), i.e. the whole back buffer; first SRV = 0x684EE890 (scene, disasm
  0x3E4D363), second SRV = **0x0C419ABC** (UI, disasm 0x3E4D2FE) **only if `[stage+0x2188] != 0` and
  `byte [[0x604B1B8]+0x0D] != 0`**, else the default SRV; CB {1.0 or 300.0 (HDR), 1/gamma
  `[0x604AA10+0x2C]`, `[+0x30]`, `[+0x60]`}; `Draw(3)` full-screen triangle.
* **Soft cursor**: `cursor = (*0x6046F98)->vf[0xF8]()`; if `cursor->vf[0x70](&pos, &size, &scale)` and the
  texture `[[0x5F9DEA0]+0x48]` exist, draws a 6-vertex quad with CB `{2/w, -2/h, -1, 1, ..., pos}` in
  back-buffer pixels with the SoftCursor shader. (Which modes use the soft cursor instead of the SDL
  hardware cursor is Unconfirmed; the OS cursor is set through SDL_SetCursor, refs 0x57C2B0, 0x57D4A0,
  0x57EC40, 0x57EEA0 [s1].)

Consequence for the current stereo layout (stereo.md): the eye views draw the left/right halves of
the scene target, and stage 0x07 then lays the single UI texture over the **whole** back buffer, so the
UI is split across both eyes (each eye sees one half of the UI). This matches stereo.md's observation
"UI still full screen".

### 1.4 UI resolution and scaling (Confirmed)

* Size source: `GameUI vf[0x60]` = **0x120E0E0** `SetSize(w,h)` writes GameUIParams `+0x7C/+0x80` and
  NUIM `+0xD4/+0xD8`, marks the resize (`GameUI+0x280 |= 1`, `+0x2D96 |= 2`) and resizes the device's
  offscreen targets (0x212F6D0). Called by **0x586A80** (client init) and **0x585510** (renderer resize
  handler, after the swap chain / frame resize) with **`RenderFrame+0x4328 / +0x432C`** [d3]. GameUI
  vf[0x08] = 0x120DCE0 copies a whole GameUIParams block (size from params +0x2C/+0x30) [d10].
* Applied in **NUIM update 0x20C2FD0** when `NUIM+0x1C0 & 1`: `scale = h / designH` (`+0xDC`),
  `IView::SetSize(w, h)` (IView+0x48, args from disasm 0x20C33BA/0x20C338F), MainCanvas
  `+0x2F0 = (int)(w / scale)`, `+0x2F4 = designH` (canvas layout size) [d15].
* Theme/scale presets: **0x12126D0** loads `Theme/DefaultTheme(_c).xaml`, `Theme/Scaling/Scaled
  {Minimal,Small,Medium,Large,ExtraLarge}(_c).xaml` by `[[0x604B1B8]+0x34]` = 0..4 (SE: `[0x604B1B8]` is `GlobalSwitches`, +0x34 `UIScaling`), colour-blind palette by `[+0x780]`; `_c` variants when GameUI `+0x2D88 == 1`
  (controller layout, 2.4) [d4].
* View flags set at view creation 0x20C2780: `IView::SetFlags(0x30)` (PPAA | LCD) and
  `SetTessellationMaxPixelError(0.2)` (high quality) [d4, d7].

**For a VR panel**: call/hook 0x120E0E0 with the panel resolution (e.g. 1600x1200 or 2048x1152);
the layout then reflows to `(w*2160/h) x 2160` logical units. Unconfirmed: whether the on-screen
target 0x0C419ABC (frame-sized) is then drawn only in its top-left `w x h` region; check by reading
back the texture after a SetSize smaller than the frame.

### 1.5 Redirecting / drawing once

The UI already is a separate texture drawn once per frame (1.2). What the mod needs:
1. take 0x0C419ABC after stage 0x19 (render-thread order is special view last: A 1.5, so at stage 0x07
   job time or at Present it is complete), copy into the VR panel swapchain;
2. stop compositing it into the eye halves (1.3, patch the slot-1 bind or force the NoUI shader);
3. optionally give it its own size (1.4).
Noesis also supports a per-view 3D projection: **IView+0x98 = 0x4704530 `SetProjectionMatrix(Matrix4)`**
sets the view's projection (flag 0x8000 in view+0x12C so SetSize no longer overwrites it) [d7]. This
would let one draw the UI directly in world space per eye, but the game renders the UI only once
(1.2), so the texture route is the practical one.

### 1.6 Two player views / split screen

The UI has one main Noesis view (NUIM+0x70) for all local players; per-player content is selected by
PlayerId inside the widgets/state machine (SE `UIWidget::PlayerId`, `UIStateMachine` per-player maps).
Rendering (1.2) and compositing (1.3) do not iterate player views. The only per-player-view
dependency of the UI is the world-anchored projection (section 3), which uses the player's own render
view. With the mod's second (eye) view, which is not registered as a player view, the UI is
unaffected except that world-anchored elements follow the main (left-eye) view.

---------------------------------------------------------------------------------------------------

## 2. Input

### 2.1 Mouse path end to end (Confirmed)

```
SDL pump 0x57AAA0 (C 3.1)
  SDL_GetGlobalMouseState -> qword [cursor+0x08]
  SDL_MOUSEMOTION (0x400), not in relative mode ([pump+0x48]==0), event window == game HWND:
     cursor float2 [cursor+0x00] = app->vf[0x108](window point)      (window -> UI pixels)
     InjectRaw 0x40F1F00(im, {id 0x80 motion, device 1, 1.0, 1.0, 1}, 1)
  [cursor+0x10] byte = result of an HWND == game-window test made in SDL window events
     where cursor = *(void**)0x614FB70 (pointer set once by 0x3AA8EE0)
InputManager::Update 0x40F0450 -> bindings -> engine input events (0x4161610 registers them):
     1 UILeftMouse, 2 UIRightMouse, 3 UIMiddleMouse, 0x4E UIMouseMove, 0x4F-0x5A nav keys,
     0x5E UIScrollUp, 0x5F UIScrollDown, 0x60 UIBaseCancel           [disasm of 0x4161610]
GameUI input listener (GameUI+8, vtable 0x555F068 slot 13) = 0x1214430 and GameUI update path 0x1214070
  -> NUIM::OnInputEvent 0x20C3660(NUIM, &handled, event+0xC)
       0x4E: IView::MouseMove((int)cursor.x, (int)cursor.y)    (IView+0xD0)
       1/2:  0x20C3B70 -> IView::MouseButtonDown/Up/DoubleClick(x, y, btn 0/1)  (+0xB8/+0xC0/+0xC8),
             double click from GetDoubleClickTime() and a 20 px box (NUIM +0x90/+0xE0/+0xE8)
       0x5E/0x5F: IView::MouseWheel(x, y, value * +-120)       (+0xD8)
       handled = Noesis returned true, or hit test 0x2106F90 found an element
```
[r31 in agents\C, d13, d19, d14, x4]. `0x57F6F0` (enter/leave relative mouse mode) saves the cursor
global into `[mgr+0x50/+0x54]` and warps; `0x57F470` (SetCursorPosition) converts UI pixels back to
window pixels with the inverse transform (`vf[0x100]`) and calls SDL_WarpMouseInWindow [d14].

The cursor global is also read by picking (0x1C45A90), camera code (0x1E155E0), 0x12F4940, 0x15A1270,
0x1395E10, 0x1B3F680, 0x1D0CC60, 0x3BDC7E0, 0x4245770, 0x4261630 [x4]: writing it moves the game's
whole notion of "mouse position" (world picking included), not just the UI's.

Coordinate space (Confirmed): the pump's `[+0x10]` object is the renderer/app object (vtable
0x5499F58, the one whose slot 0x288 is the client update 0x5889E0; SDLWindowManager ctor 0x57A1A0 copies
it from global 0x5F9BD58). Its `vf[0x108]` = **0x3AC77D0** maps a window point to
`point * frameSize / windowSize`, frameSize = active RenderFrame `+0x20/+0x24` (render size) when the
upscaler setting `[0x604AA10+0x77] < 1`, else `+0x28/+0x2C` (upscaled size) (0x3AC76C0 / 0x3AC7640);
`vf[0x100]` = 0x3AC7740 is the inverse [d34, d35]. So the cursor global is in **frame pixels**, and NUIM
passes it unchanged to `IView::MouseMove`, whose space is the UI view size (`RenderFrame+0x4328/+0x432C`,
1.4). Both agree as long as the frame size equals the UI size; if the mod gives the UI its own size it
must write UI-view pixels for UI clicks (world picking reads the same global as frame pixels).

### 2.2 Noesis IView vtable (Confirmed, every slot decompiled) [d7]

IView pointer = `*(NUIM+0x70)` = ViewImpl+0x18 (CreateView 0x45C3A10 allocates 0x148 bytes, ctor
0x4703540, class vtable 0x534FAD8, IView vtable **0x534FB00**).

| off | rva | method (from behaviour) |
|-----|-----|-------------------------|
| +0x30 | 0x4703E50 | GetRenderer |
| +0x38 | 0x4703E70 | GetContent |
| +0x40 | 0x4703E80 | SetScale(float) |
| +0x48 | 0x4703EF0 | SetSize(w,h) (builds the default projection unless flag 0x8000) |
| +0x50 / +0x58 | 0x47042F0 / 0x47043B0 | Set/GetTessellationMaxPixelError |
| +0x60 / +0x68 | 0x47043D0 / 0x4704480 | Set/GetFlags |
| +0x70..+0x90 | 0x4704490..0x4704510 | touch/holding/double-tap thresholds (forward to the touch manager +0x68) |
| +0x98 | 0x4704530 | SetProjectionMatrix(Matrix4) |
| +0xA0 / +0xA8 | 0x4704660 / 0x4704680 | Activate / Deactivate |
| +0xB0 | 0x47046D0 | SetEmulateTouch(bool) |
| +0xB8 | 0x47046E0 | MouseButtonDown(x, y, button) |
| +0xC0 | 0x4704730 | MouseButtonUp(x, y, button) |
| +0xC8 | 0x4704780 | MouseDoubleClick(x, y, button) |
| +0xD0 | 0x47047D0 | MouseMove(x, y) -> bool |
| +0xD8 / +0xE0 | 0x4704820 / 0x4704840 | MouseWheel / MouseHWheel |
| +0xE8..+0x100 | 0x4704890, 0x4704860, 0x47048F0, 0x47048C0 | Scroll / HScroll overloads (value x dt x 10) |
| +0x108 / +0x110 / +0x118 | 0x4704920 / 0x4704950 / 0x4704970 | TouchDown / TouchMove / TouchUp |
| +0x120 / +0x128 / +0x130 | 0x47049A0 / 0x47049C0 / 0x47049E0 | KeyDown / KeyUp / Char |
| +0x138 | 0x4704A00 | Update(double seconds) |
| +0x140..+0x160 | 0x4704BB0.. 0x4704C50 | Rendering/Updated events, GetStats |

Noesis helpers (SE names, confirmed by SE's signature hitting 0x467868B): `Visual::PointFromScreen`
**0x45FC390**, `VisualTreeHelper::HitTest` **0x4634D30**; filtered hit test used by the game
**0x2106F90** (wraps 0x4634D60 with filter callbacks 0x2106640/0x2106680). `GUI::LoadXaml` call site
0x20C27A6 (SE sig) -> 0x45C38A0.

### 2.3 Hit testing and "mouse over UI" (Confirmed)

* NUIM update 0x20C2FD0: `NUIM+0x190 = PointFromScreen(content, cursor)` (SE ViewMousePosition).
* 0x20C3EF0: `NUIM+0x180` (SE MouseOverUI) = hit test at the cursor (first against a pinned element
  `+0x88` if any, else the content); in controller mode (flag 0x10) always 0.
* 0x20C3B70 / 0x20C3660 return "handled" when Noesis consumed the click/wheel or the hit test hits;
  0x1214430 then marks the event consumed so the world (picking/character tasks) does not get it.
  For the laser: a click is UI-only if `NUIM+0x180` is set after the move.

### 2.4 Keyboard/mouse vs controller mode (Confirmed)

* Global flag **byte 0x614FBC8** = controller mode; read by ~150 sites (picking 0x1C43820/0x1C45A90,
  camera 0x1E0EE20, UI, ...) [x8].
* **0x5EB2B0** `SetControllerMode(client, device)`: requires `[0x6046F98+0x231/+0x232] == 0`, sets the
  flag to 1, assigns player 0 to the device (`0x40F4330(im, 0, dev)`), notifies the callbacks at
  `EoCClient+0x30` (array of 9-qword entries) [d20]. **0x5EB490**: flag 0, resets the InputManager
  player->device mapping, same callbacks with 0.
* **0x5E3C60** (listener slot, vtable data 0x549B7B8) switches automatically on input from a device:
  device 0/1 -> KB/M, 3..10 -> controller, respecting `[[0x604B1B8]+0x38]` (SE `GlobalSwitches::ControllerMode`; 2 = forced by
  `-startInControllerMode`, 0x5878A0; Steam Deck path also forces controller via 0x5EB2B0 when byte `+0x39` is set) and skipping when the main
  view's camera controller id `[[view]+0x80]` equals 0x5EED3AC / 0x5EED3A8 [d21]. Value names of the
  option are Unconfirmed (string "InputMode" 0x5644630 is registered in 0x1B4D260).
* GameUI side: **GameUI vtable slot 22 = 0x1211EB0** `SetInputDevice(dev)` sets `GameUI+0x2D88`
  (0 KB/M, 1 controller), `+0x2D8C` controller type from the InputManager device info
  (`[im+0x43C+dev*0x28]`: 2 -> 0, 3 -> 1, 7 -> 2; option `[+0x1D6]` forces it), marks a layout reload
  (`+0x2D96 |= 0x10`); the reload 0x12126D0 loads the `_c.xaml` themes and `StateMachine_c.xaml`
  (0x20CBCB0) and puts NUIM into controller mode via **0x20C2C50(NUIM, 1)**, which sets NUIM flag 0x10,
  clears the mouse-over state and moves the Noesis mouse to (0,0) [d22, d4, d13]. With flag 0x10 NUIM
  ignores mouse move/click unless `NUIM+0x78` (second view) exists [d13].
* The ToggleInputMode event (0xC0, C 3.3) handler was not located (its id does not appear as an
  immediate near 0x5E3C60); Unconfirmed whether it calls 0x5EB2B0/0x5EB490 directly.

**VR consequence**: injecting controller (device 3) stick input for locomotion (C 3.2's recommendation)
flips the game into controller mode, reloads the controller UI and disables the mouse/laser path.
Either inject movement as keyboard (device 0, events 0x9D-0xA0) or neutralise 0x5E3C60 for device 3.

### 2.5 Controller-mode UI (radials, focus) (partly Confirmed)

* In controller mode UI navigation is not Noesis mouse/keyboard input: GameUI's listener 0x1214430
  routes the game's input events (UIAccept 0xDF, UIBack 0xE0, UIRadialLeft..Down 0xE8-0xEB,
  UISelection* 0xF7-0xFA, ... C 3.3) to per-player widget handlers (hash map at `GameUI+0x2EA8 +
  player*0x40`, handler `vf[0x28]`), to the listener list `GameUI+0x8B8` (count `+0x8B4`), and to the UI
  state machine (`0x20C8340(GameUI+0x810, ...)`) [d19] (offsets in the decompile are relative to the
  interface at GameUI+8). Focus moves through the widget's focus events (SE `UIWidget`
  FocusDown/Left/Right/UpEvent) and NoesisGUIExtensions `MoveFocusAction` / `GamepadTrigger` (type
  names 0x53ADE00, 0x53A8520).
* A software cursor exists for controller mode: `ls.SoftwareCursor`, `ls.CursorService`,
  `CursorSpeed`, `CursorLeft/Right/Up/DownEvent` (strings 0x56D0188-0x56D02D8), components
  `ecl::ui::PadCursorVisibleComponent` / `PadCursorVisibilitySystem`; the UIMouseMove branch of
  0x1214070 copies `GameUI+0x250` (cursor position) / `GameUI+0x19C` (scale) into each player's cursor
  widget (`+0x6B8`, fields +0x78/+0x7C and +0xD8/+0xDC) [d19]. Writer of `GameUI+0x250` not located
  (Unconfirmed).
* Radial menus are Noesis controls (`ls.Radial`, `ls.LSRadialList` / `LSRadialList.cpp` 0x56CBDC0,
  `ls.LSRadialListItem`, `ui::RadialItemCollection`) with data properties `AreRadialsOpen`,
  `OpenActionRadials`, option `UseRadialContextMenu` (0x54A3EE0). Their XAML is in Game.pak.

### 2.6 Cleanest laser injection (recommendation, from the confirmed paths)

1. Keep KB/M mode (2.4).
2. Each frame on the game thread before InputManager::Update (0x3F77333, C 3.5): intersect the laser
   with the UI panel, convert to UI pixels `(u*w, v*h)` of the size set in 1.4, write the float2 at
   `*(float**)0x614FB70`, then `0x40F1F00(im, {0x80, 1, 1.0f, 1.0f, 1}, true)` so event 0x4E fires.
3. Trigger -> raw left2 (123) down/up on device 1 (SE id), grip -> right2 (125); thumbstick -> wheel
   (133-136) or event 0x5E/0x5F.
4. When the laser points at the world instead, stop writing the global (or write the projected point
   for world picking, which reads the same global, see 2.1).
Direct `IView` calls (2.2) work too but skip the game's double-click/hover bookkeeping and "handled"
flags; if used, take `EnterCriticalSection(NUIM+0x198)` like every game call site.

---------------------------------------------------------------------------------------------------

## 3. World-space UI (overheads, nameplates, tooltips, damage text)

### 3.1 One projection function for all of it (Confirmed)

**0x407BF00 `RenderView::WorldToScreen(view, float3* pos, float* x, float* y, float4x4* m = 0)`** [d26]:
* if `m == 0`: uses the view's ReadState camera controller `[view+0x21D8]`; if its `vf[0x80]` is true it
  refreshes view/projection (0x4085150 / 0x40851F0, the same updates the mod hooks, camera.md) and
  uses the controller's matrix at **`+0x190`** (view-projection by its use: row-vector multiply, divide
  by w);
* `x = (ndc.x + 1)/2 * [view+0x21A0] + [view+0x21A8]`, `y = (1 - ndc.y)/2 * [view+0x21A4] +
  [view+0x21AC]`, i.e. the view's **upscaled viewport** in ReadState (A 3.2: +0x48 int4, here read as
  w,h,x,y); returns `w >= 0` (in front).

Callers [x10]:
| caller | what |
|--------|------|
| 0x12E7F70 (body of `gui::tooltips::WorldTooltipsSystem`, reg 0x11F92A0 -> cb 0x11FAD50) | world tooltips: entity anchor (0x13E57D0) -> player view (0x17F72A0) -> 0x407BF00 -> divided by the float `[*plVar30+0x80]` (likely the UI scale; Unconfirmed) [d25 lines 1141-1163] |
| 0x13E5C50 | generic "entity -> screen point" for UI view-models (used by the tooltip body) |
| 0x13E5D20 (from 0x13065C0, `ls.Character` view-model update) | character screen rect: anchor and anchor + min(height, 3.0); on-screen test against the view rect; uses player view 0x17F7330 [d27] |
| 0x1752380 (from 0x174E560, overheads update: types "Dialogue", "DialogueNPC", "Default", "Status", "Experience", "ExperienceOverflow") | **overhead text / dialogue bubbles / floating status text** position -> VM property (0x20EE2D0) [d29] |
| 0x1C49DF0 (from mouse picking 0x1C48750) | picking |
| 0x1E13270, 0x1FEA480, 0x1F219E0, 0x407BCC0 | camera / other game code |
| 0x3F88090 (post-process steps 0x3EE46C0, 0x3EEFD20, 0x3EF2860) | sun position for screen-space effects |

Player -> view: **0x17F72A0(playerMgr 0x60487A0, entity, playerId)** and **0x17F7330(playerMgr, id)**
pick the first local player's id (or 0) and call **0x3BAAE20** (view manager: player id -> render view,
A section 11, B) [d31].

### 3.2 Stereo consequences (Confirmed from 3.1 + 1.3)

* All world-anchored UI is positioned **once**, with the **main (player 0) view's camera** and that view's
  **upscaled viewport rectangle**. In the current stereo layout (Scale 0.5, view 0 = left half) the
  nameplates/overheads are placed in the left half of the UI canvas, at left-eye positions, and the UI
  texture is drawn over both halves (1.3). They cannot be placed per eye: there is one UI render
  (1.2) and one projection per player.
* What the mod can do:
  1. hook 0x407BF00 (or its UI callers 0x13E5C50 / 0x13E5D20 / 0x1752380 / 0x12E7F70) to project with a
     **centre-eye camera into the full UI panel pixel space**, so that overheads drawn on a head-locked
     UI panel line up with the entity from the head's centre (correct direction, wrong depth: they sit
     at panel depth);
  2. or suppress them (options `ShowOverheadText`, `ShowOverheadDamage`, `ShowOverheadDialog`,
     `OverheadZoomEnabled`, strings 0x54A3A38-0x54A3C10) and draw VR-native labels from the same
     view-model data;
  3. per-eye world-space labels would need the UI rendered twice with two projections (not supported
     by the game's single render, 1.2).
* The controller VP at `+0x190` is the controller's own matrix (refreshed via 0x4085150/0x40851F0);
  the per-eye matrices written by the stereo hook into the PerCamera blocks (stereo.md) do not reach
  it. If the camera-module per-eye projection override is active on the controller, the UI uses
  whichever eye's projection the controller last had (Unconfirmed: depends on the mod's hook order;
  log `controller+0x190` at a 0x407BF00 call).

### 3.3 Cursor/target decals and the movement path preview

These are not Noesis: `CursorSystem` / `CursorArrow*` / `Cursor_*` (strings 0x54ECB88-0x55F6B28) are
hardware/soft cursor images (SDL_SetCursor, 1.3 soft cursor), and the ground target decals / path
preview are scene objects drawn by the render views (deferred game decals, A stage 0x46 sub-renderer
0x0D / 0x3B), hence already per eye in the stereo setup. Not traced further here (picking/targeting
agent's topic); Unconfirmed which system draws the path line.

---------------------------------------------------------------------------------------------------

## 4. Dialogue UI, hotbar/combat UI, radial menu

### 4.1 What the exe contains (Confirmed by strings; behaviour by SE layouts)

* The UI is a XAML state machine: `StateMachine.xaml` / `StateMachine_c.xaml` (loaded by 0x20CBCB0 from
  the theme reload 0x12126D0), `ls.StateMachine`, `ls.StateEvent`, `OnCreateStateEvent`
  (0x56B10E0-0x56C3288). SE: `ui::UIStateMachine` holds states (`UIState`: Name, Layout, Owner,
  IsModal, ForceSplitscreen, Widgets...), each state lists widgets (`UIStateWidget`: Filename, Layer,
  IgnoreHitTest...). Widgets are `UIWidget : UserControl` instances created by the WindowManager and
  placed in MainCanvas layers (SE `UIWidget::LayerIndex/IndexInLayer`).
* States are opened/closed by state events fired through the GameUI state machine: GameUI calls
  `0x20C8340(GameUI+0x810, &result, &eventArgs, &callback)` with event structs (ids 1-0xC
  seen in the game-state listener 0x12155D0 for game states 7, 0x10, 0x12, 0x15, 0x16, 0x17)
  [d18]. SE notes `Ext.UI.SetState` as deprecated; SE `UIStateMachine::EventArgs` = {EventIndex,
  StateEvent FixedString, SubState, RemoveState, PlayerId}.
* View-models the widgets bind to: dialogue `ls.DialogueParticipant`, `ls.VMDialogueAnswerTag`,
  `ls.DialogueHistory`, `ls.VMDialogStartEntry`, data `Dialogues`, `DialogueLines`,
  `ShowDialogCinematicContext`, `DialogTimelinesEnabled`, `DialogueFontSizeModifier`; hotbar/combat
  `ls.VMHotBar`, `ls.VMHotBarSlot`, `HotBarType`, `HotbarLocked`, `ls.VMCombat`,
  `ls.VMActionGroup`, `ls.VMActionResource`, `ls.VMCharacterAction`, input events UISelectSlot0-12,
  UIEndTurn 0xE3; radial `ls.Radial`, `AreRadialsOpen`, `OpenActionRadials` [str_ui2, str_ui3].
* The widget XAML (`Public/Game/GUI/...` in `Data\Game.pak`) defines which file is the dialogue panel,
  the hotbar, the radial menu, and their layout; it is not in the exe (Open lead 1).

### 4.2 Moving / scaling individual widgets

* Each widget is an ordinary Noesis FrameworkElement under MainCanvas (`NUIM+0x60`). Moving or scaling
  one independently = setting its RenderTransform / Margin / Canvas position at runtime. Script
  Extender exposes exactly this from Lua: `Ext.UI.GetRoot()` returns
  `ResourceManager->UI->NoesisUIManager.MainCanvas` (SE `Lua\Libs\ClientUI\Module.inl`), and SE can
  read/write Noesis dependency properties and register types. So: find the widget by name under the
  root and set a transform (SE route), or call Noesis directly from the native mod.
* For a split VR layout (hotbar on the wrist, dialogue in front), the simplest exe-level approach is
  still one UI texture (1.2) cropped into several quads by sub-rectangles; independent placement only
  needs the widget's screen rectangle (Noesis `Visual::PointToScreen`, SE sig, or the element's
  `ActualWidth/Height` + transform), which changes only when the layout changes.

---------------------------------------------------------------------------------------------------

## 5. Function / data index

| rva | name |
|-----|------|
| 0x6047958 | ls::gGlobalResourceManager (SE); `+0x130` GameUI, `+0x90/+0x98` Bink players, `+0x88` Wwise |
| 0x120BAE0 / 0x120D630 | GameUI ctor / dtor; vtables 0x555EF78 (+0), 0x555F068 (+8), 0x555EF20 (+0x10), 0x555EF60 (+0x20), 0x555EEB0 (+0x30) |
| 0x120DCE0 | GameUI vf[0x08] SetParams |
| 0x120E0B0 | GameUI vf[0x40] IsActive (`+0x84`) |
| 0x120E0E0 | GameUI vf[0x60] SetSize(w,h) |
| 0x120FCC0 / 0x120FCE0 | GameUI vf[0x78] SetStage / vf[0x80] SetTargets(color, depth, key3) |
| 0x120FD10 | GameUI vf[0x98] Render (render thread) |
| 0x1211EB0 | GameUI vf[0xB0] SetInputDevice (layout mode `+0x2D88`) |
| 0x1210210 | GameUI vf[0xC0] Update (calls NUIM update 0x20C2FD0, 0x1214070) |
| 0x1214430 | GameUI input listener OnInputEvent (interface +8, slot 13) |
| 0x1214070 | GameUI mouse-event routing to NUIM + per-player cursor widgets |
| 0x12126D0 | theme / UI-scale / colour-blind / controller layout reload |
| 0x1210880 | GameUI init (calls NUIM init 0x20C1780, 0x12126D0) |
| 0x20C1780 | NUIM init (RenderContext, RenderDevice 0x2128E90) |
| 0x20C2780 | NUIM create view (MainCanvas.xaml, CreateView, flags, renderer init) |
| 0x20C2530 | NUIM set params -> RenderDevice |
| 0x20C2AF0 | NUIM set pinned element + MouseMove at cursor |
| 0x20C2C50 | NUIM set controller mode (flag 0x10) |
| 0x20C2FD0 | NUIM update (pointer, keys, SetSize/scale, IView::Update) |
| 0x20C35E0 | NUIM render main view (reload path) |
| 0x20C3660 | NUIM OnInputEvent (mouse/wheel/nav keys -> IView) |
| 0x20C3B70 | NUIM mouse button (down/up/double click) |
| 0x20C3D20 | NUIM release modifier keys |
| 0x20C3EF0 | NUIM update mouse-over flag (+0x180) |
| 0x2128E90 | Noesis RenderDevice init (targets from params) |
| 0x212B570 | RenderDevice shader setup (`Noesis/v3_1/*.bshd`) |
| 0x212EB70 / 0x212F6D0 | RenderDevice offscreen target sizing / resize |
| 0x212FAF0 / 0x212FF30 | RenderDevice on-screen PSO for keys +0x4C/+0x48 / bind |
| 0x2106F90 | filtered UI hit test |
| 0x45C3A10 / 0x4703540 | Noesis CreateView / ViewImpl ctor |
| 0x534FB00 | Noesis IView vtable (2.2) |
| 0x45FC390 / 0x4634D30 | Noesis Visual::PointFromScreen / VisualTreeHelper::HitTest (SE names) |
| 0x45C38A0 | Noesis GUI::LoadXaml (SE) |
| 0x3CBFC30 / 0x3CC0490 / 0x3CC04F0 | stage 0x19 init / prepare / job (UI + Bink render) |
| 0x3CC0EA0 | create/bind key 0x0C419ABC |
| 0x3E4C6F0 / 0x3E4D0F0 / 0x3E4D2A0 | stage 0x07 init / prepare / job (composite + soft cursor) |
| 0x21B5200 | Bink video player ctor (vtable 0x56D78C8) |
| 0x585510 | renderer resize handler (frame + UI SetSize) |
| 0x586A80 | client init part (Bink, Wwise, UI SetSize) |
| 0x5878A0 | client init (`-startInControllerMode`, listeners) |
| 0x5E3C60 | auto input-mode switch on device input |
| 0x5EB2B0 / 0x5EB490 | SetControllerMode / SetKeyboardMouseMode |
| 0x614FBC8 | byte: controller mode active |
| 0x614FB70 | pointer to cursor struct {float2 UI pos, int2 global mouse, byte over-window} |
| 0x57AAA0 | SDL pump (cursor writes) |
| 0x57F470 / 0x57F6F0 | set OS cursor position / relative mouse mode |
| 0x4161610 | engine input events (UI mouse/nav ids) |
| 0x407BF00 | RenderView::WorldToScreen |
| 0x17F72A0 / 0x17F7330 / 0x3BAAE20 | player -> render view |
| 0x12E7F70 | WorldTooltipsSystem body (reg 0x11F92A0, cb 0x11FAD50) |
| 0x174E560 / 0x1752380 | overheads update / overhead position |
| 0x13E5C50 / 0x13E5D20 | entity -> screen point / rect for UI view-models |

---------------------------------------------------------------------------------------------------

## Open leads

1. **Widget XAML** (dialogue panel, hotbar, radial menu, overhead templates): read
   `Public/Game/GUI/*.xaml` and `StateMachine.xaml` from `Data\Game.pak` (LSPK; needs a pak reader) to
   get widget names, layers and the state events that open/close them.
2. **On-screen viewport when the UI view is smaller than 0x0C419ABC**: decompile the RenderDevice's
   begin-onscreen / viewport code (the RenderContext vtable, NUIM+0x38 `vf[0x58]` returns the device) or
   read back the texture at runtime after `SetSize` with a smaller size.
3. **RenderFrame +0x4328/+0x432C** (UI size source) vs `+0x20..+0x2C` (cursor space): confirm at runtime
   that they are equal in normal play (else the game's own mouse-to-UI mapping would be off).
4. **`[[0x604B1B8]+0x0D]`** (SE `GlobalSwitches::field_D`, unnamed) gates the UI texture in the composite: find its writer/name (a "hide UI"
   switch would let the mod drop the UI from the eye images without patching code).
5. **ToggleInputMode (0xC0) handler** and the meaning of option `[[0x604B1B8]+0x38]` (0/1/2).
6. **Writer of GameUI+0x250** (controller software-cursor position) and the soft cursor provider
   `(*0x6046F98)->vf[0xF8]` used by stage 0x07.
7. **Which matrix sits in the camera controller at +0x190** under the mod's per-eye projection override
   (log at a 0x407BF00 call) - decides where nameplates land after the stereo hook.
8. **Bink cutscenes** render into the same UI target in stage 0x19 (0x21B6DD0, letterboxed to the
   target); for VR they arrive with the UI texture, so the UI panel doubles as the video screen.
