# BG3 VR

First-person, full-body VR for Baldur's Gate 3: you see through your character's eyes, your arms follow your
hands, you point, grab, swing and draw a bow, while the game's own D&D rules still decide every roll.

> **Early and experimental.** This is a work in progress by one tester and an AI pair programmer. Most features
> are built but have been tried in a headset only a few times, or not yet; expect rough edges, low frame rates and
> crashes. Please read [Status](#status) before trying it, and report what you see in an issue (with
> `<game>\bin\bg3vr\bg3vr.log`).

- **How to play** (setup, controls, every setting): [docs/playing.md](docs/playing.md)
- **Requirements:** Baldur's Gate 3 **4.1.1.7631656** (the mod checks the game's code and switches features off on
  any other build), DirectX 11, [BG3 Script Extender](https://github.com/Norbyte/bg3se), a PC VR
  headset through an OpenXR runtime (tested: Quest 2 through SteamVR), Windows. No release download yet: build from
  source (below).

## Status

Legend: ✅ seen working in a headset run · 🧪 built, not yet tried in the headset · 🗓️ planned

### Seeing

| Feature | Status |
|---|---|
| First-person view at your character's eyes, full head tracking | ✅ |
| Stereo: the game renders both eyes in the same frame (two render views) | ✅ |
| Lighting and outlines the same in both eyes | ✅ lighting, 🧪 outlines |
| Real stereo depth: each eye drawn from its own position (decals and blood pools at the right depth) | 🧪 |
| Camera locked to your character's eyes, also through cutscene teleports and scene stand-ins | 🧪 |
| Render resolution from the headset (80% by default), DLSS Balanced while in VR | ✅ resolution, 🧪 DLSS profile |
| Your own head hidden from your view | ✅ |
| Your shadow keeps its head | 🧪 |
| Third-person toggle: the game's camera on a big screen in front of you, the laser as the mouse | ✅ |
| Comfort vignette, snap or smooth turning, recentre | ✅ turning, 🧪 the rest |
| Characters in UI screens (inventory, character sheet, trade) in 3D | 🧪 |
| Items in UI screens in 3D | 🗓️ |

### Hands and body

| Feature | Status |
|---|---|
| Controllers: bindings for every button, combos, haptics | ✅ |
| Arms follow your hands (IK), weapon in your hand, natural elbows, shoulders, finger curl | ✅ arms, 🧪 elbows / shoulders / fingers |
| Aiming laser (styles, colour, opacity), stops on the UI and the menu | ✅ laser, 🧪 stopping on panels |
| Game UI hidden until X shows it in front of you; up by itself in combat, conversations and when a game window opens (or always in front, or on the wrist) | 🧪 hidden by default |
| Holsters: draw and stow weapons over the shoulders and at the hips; torch at the chest; bag behind the back | ✅ draw, 🧪 stow and the rest |
| Hand tracking (point, pinch, close the hand to click or grab) | 🧪 |
| Grab and throw dead bodies (the game's ragdolls) | 🧪 (within ~1 m of where they fell for now) |
| Gravity gloves: point at an item to highlight it, grip and flick to pull it into your hand, hold, throw, stow over the shoulder | 🧪 (on by default) |
| Physics-driven (active ragdoll) bodies | 🗓️ |

### Playing

| Feature | Status |
|---|---|
| Walking with the left stick | ✅ |
| Melee by swinging at who you look at; bows by nocking, drawing and releasing; spells by aiming | 🧪 |
| Conversations: answers in front of you, sticks + A / B to pick, or point and click | 🧪 |
| Cutscenes and conversations kept in first person | ✅ conversations, 🧪 cutscenes |
| In-game VR menu (all settings, rebinding, animated guide) | 🧪 (the opener changed to the left stick click) |

### Performance (the main problem today)

The last measured run was about **26 fps** in stereo against 49 fps flat. The game renders a whole second view for
the second eye. Since then, all 🧪:

- the right eye reuses the left eye's sun shadows;
- the eyes record their draw calls in parallel;
- the headset no longer stalls the game's render thread;
- DLSS Balanced replaces DLAA (full-resolution DLSS) while in VR;
- depth of field and lens flare are off in VR;
- the GPU frame queue is shorter;
- head-pose prediction is fixed.

The analysis behind them is in [re-local/ghidra/agents/](re-local/ghidra/agents/) (T, W, X).

### Known issues

- Low frame rate (see above). Lower **VR menu → View → Render resolution** and the game's shadow / fog quality if it
  stutters.
- A Quest 2 through SteamVR keeps the left ☰ button for its own menu: the VR menu opens with the **left stick
  click** instead.
- Cutscenes that use a stand-in for your character don't hide the stand-in's head yet.
- Opening the game's graphics menu while in VR can save the VR graphics settings (DLSS Balanced) as yours.

## Controls (Quest Touch, defaults)

| Control | Does |
|---|---|
| Right trigger | Click: move, talk, loot, attack where the right hand points; press UI buttons |
| Right grip | Right-click (context menu); with an item highlighted it grabs instead (gravity gloves) |
| A / B | Inventory / cancel (back) |
| X / Y | Show / hide the game UI / first ↔ third person |
| Left stick | Walk where you look |
| Right stick left / right | Snap turn (or smooth) |
| Right stick up / down | Scroll |
| Hold right stick click | Recentre |
| **Left stick click** | **Open / close the VR menu** |
| Left grip + left stick click | Map |
| Left trigger (hold) | Highlight items |
| Left grip + A / B / X / Y | Character sheet / journal / spellbook / party |
| Left grip + right trigger / right stick click | End turn / sneak |
| Reach over a shoulder or to a hip, squeeze, pull | Draw a weapon (let go back at the holster to stow) |
| Swing at the enemy you look at | Melee attack (the game rolls) |
| Reach for or point at an item, squeeze a grip | Take it (close by) or lock on (far, the laser turns amber); flick the hand back to pull it |
| Let go of a held item | Put it down (it drops onto what is below); mid-swing throws it; over a shoulder or behind your back puts it in the inventory |
| **In the VR menu** | sticks up/down choose, left/right change, A change / press, B close; or point and pull the trigger |
| **In a conversation** | sticks up/down choose an answer, A picks, B skips the line; or point and pull the trigger |

Every button can be rebound in the VR menu (Buttons and Combos pages) or `[bindings]` / `[combos]` in `bg3vr.ini`.
Hand tracking, holsters, bows and the rest: [docs/playing.md](docs/playing.md).

## Build and install

Requires Visual Studio 2026 (C++ desktop workload, MASM; C++23) and CMake 3.28+.

```bash
cmake --preset vs2026
```

```bash
cmake --build --preset release
```

```bash
ctest --preset debug
```

Deploy into the game (`-GameDir` is your Baldur's Gate 3 folder; the default is the maintainer's), then enable
**BG3 VR** in the in-game Mod Manager (the game owns the load order):

```bash
powershell -File tools/deploy.ps1 -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Baldurs Gate 3"
```

```bash
powershell -File tools/deploy.ps1 -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Baldurs Gate 3" -Uninstall
```

The first deploy writes `<game>\bin\bg3vr\bg3vr.ini`: set `[xr] enabled = true` and `mode = mono`, start SteamVR,
then the game. The core logs to `<game>\bin\bg3vr\bg3vr.log`. To turn the mod off without uninstalling, create an
empty file `<game>\bin\bg3vr\disabled`.

## Reverse-engineering notes

What the game's functions and structures are for, with addresses for **bg3_dx11.exe 4.1.1.7631656** (RVAs relative to
0x140000000): [re-local/ghidra/agents/](re-local/ghidra/agents/). There is one report per topic:

- rendering, split screen, animation, physics, items, UI, camera, LOD, picking, locomotion, weapons;
- stereo lighting, render resolution, UI characters, ragdolls, outlines, decals;
- shared eye work, CPU and pipelining, pixel cost;
- cutscene actors, stereo UI characters.

There are more notes in [docs/re-notes/](docs/re-notes/) and [docs/offsets.md](docs/offsets.md). Raw decompiler
output, Ghidra projects and extracted game files are not in the repository.

## Layout

```
config/        default bg3vr.ini and signatures.txt (copied to the game on first deploy)
docs/          the player guide, phase plans, decisions (ADRs), RE notes, offset table
lua/Mods/BG3VR Script Extender mod: the gameplay side (camera follow, holsters, combat, UI events)
native/
  proxy/       version.dll proxy: forwards to the system DLL, loads the core
  core/        bg3vr_core.dll: stereo, camera, input, IK, UI panel, VR menu, everything native
  xr/          OpenXR session, swapchains, input, hand tracking
third_party/   vendored libraries (SafetyHook + Zydis), pinned and digest-checked
tools/         deploy script, RE helpers (headless Ghidra, disassembler)
tests/         native unit tests
re-local/ghidra/agents/   the reverse-engineering reports (the rest of re-local stays local)
```

## Rules

- Never commit game files or extracted assets (`.gitignore` blocks binaries and paks).
- Every confirmed address goes in [docs/offsets.md](docs/offsets.md) or an agent report with the game version.
- Every hooked or called game function is checked byte for byte at start-up; on a mismatch the feature switches off.

## License

MIT, see [LICENSE](LICENSE). Vendored libraries keep their own licenses ([docs/third-party.md](docs/third-party.md)).

Baldur's Gate 3 is a trademark of Larian Studios. This is an unofficial fan project, not affiliated with or endorsed by
Larian Studios; it contains no game files, and you need your own copy of the game.
