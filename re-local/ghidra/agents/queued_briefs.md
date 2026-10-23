# Queued decompile agents (max 3 running at once, per the user, 2026-10-19)

Launch order as slots free up: G (camera/dialogue), F (UI), H (LOD/streaming). Later: picking/targeting/actions/dice,
locomotion, weapons in hand.

## Common preamble (prepend to every brief, replacing <X> and <topic>)

You are a reverse-engineering agent for a full-body 6DoF VR mod of Baldur's Gate 3 (repo `E:\Modding\Baulders Gate 3 VR`,
game `E:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\bg3_dx11.exe`, build 4.1.1.7631656, image base 0x140000000; write
every address as an RVA). Static analysis only.

Tools and rules:
- Your own Ghidra project copy: `re-local\ghidra\BG3_<X>` (a copy of the fully analyzed BG3 project). Headless only, via
  `tools\ghidra\headless.ps1 -Project BG3_<X> -MaxMem 4G` (memory on this machine is tight: about 7 GB available, so
  never more than one Java process of yours at a time, and never more than 4G). Reuse agent C's persistent request
  server instead of reloading the project per query: copy `re-local\ghidra\agents\C\CServer.java`, `req.ps1` and
  `run.ps1` into `re-local\ghidra\agents\<X>\`, change the paths and project name to yours, start it once in the
  background, send requests with req.ps1. Scripts in `tools\ghidra` (DecompileAt, DecompileContaining, FindCallers,
  FindDisplacement, FindInDecompiled: use `#` for `|`) and agent C's (XrefTree, TypeIdRefs, SymbolRefs, RangeCallers).
  Quick disassembly: `build\vs2026\bin\RelWithDebInfo\bg3vr_disasm.exe <exe> <rva> [count] | --func <rva> | --sig <rva>`.
  Stop your server (quit.req) before you finish, so its Java process exits.
- Never open the Ghidra GUI or other agents' projects, never modify the program, never launch the game, never download
  anything. All files stay on E: (headless.ps1 sets TEMP).
- Reference material: Script Extender source `downloads\bg3se-src` (names and layouts; mark anything taken from it as
  "SE"), the existing reports `re-local\ghidra\agents\A_rendering.md`, `B_splitscreen.md`, `C_animation_systems.md`,
  `D_physics.md`, `E_items_interaction.md` (if present), and `docs\re-notes\*.md` (camera.md, stereo.md, skeleton.md),
  `docs\offsets.md`. Don't redo what they confirmed; build on it and cite it.
- Standard: no guessing. Every claim cites the RVA and the decompile/disassembly/data that shows it. Mark each item
  Confirmed or Unconfirmed; for Unconfirmed say exactly what would confirm it (a further decompile, or a specific
  runtime log the mod could take). Be exhaustive within the topic, prioritising what the VR mod needs.
- Output: write the report incrementally to `re-local\ghidra\agents\<X>_<topic>.md` (start it early, update as you go),
  raw decompiles and xref dumps in `re-local\ghidra\agents\<X>\`. The report starts with a "Hook points for the VR mod"
  table (need, RVA, how to use, confidence) and ends with "Open leads". Your final message: a short summary of the
  most important findings and the hook-point table.

## G: camera system and dialogue camera (report G_camera.md)

The mod drives the gameplay camera from the headset (our hook at the camera controller view update 0x4085150 writes the
camera entity's Transform; docs/re-notes/camera.md). The user wants every NPC conversation kept in first person (the
game must not take the camera away for dialogue), while real cinematic cutscenes may still use their own camera.
Decompile the whole camera system:
1. Structure: `ecl::CameraSystem` (registration 0x738A70 -> callback 0x75D250 -> body 0x1E0A160), the camera behaviours
   in 0x1E0xxxx-0x1E1xxxx (MasterBehaviorType values; 8 = gameplay per C_animation_systems.md 3.4), camera entities and
   components (SE names: CameraComponent, GameCameraBehavior, DefaultCameraBehavior, dialog/timeline camera components),
   how the active camera is chosen each frame and handed to the render views (render view vtable slot 10 SetCamera
   0x17F2B30; the player view setup in B_splitscreen.md).
2. Dialogue vs cutscene: what happens when a dialogue starts (dialog system, `ecl::TimelineSystem` and related timeline
   systems, timeline camera shots, "dialog camera", any player-camera override component). Find the data that
   distinguishes an ordinary NPC conversation (timeline-driven dialogue with camera shots) from a full cinematic cutscene
   (fullscreen cinematic, letterbox, different timeline type or flag), and the cleanest place to keep the gameplay
   camera active during dialogues while the dialogue UI and choices still work. Also what dialogues do to the player
   character (teleport/reposition, hide, look-at), depth of field, letterboxing, fades, and the camera return afterwards.
3. Other users of the camera: obstruction fading (`ecl::obstruction::FadeSystem` registration 0x7236B0,
   `*FadeView0/1*` components, `ecl::FadeResolveSystem` 0x723CC0), camera-distance culling (`ls::CullSystem` 0x3AE5E20,
   `ls::OcclusionSystem` 0x3AE58D0), the audio listener (which camera/entity sets the Wwise listener position),
   tactical camera (input event 0xD9), camera shake, camera collision, free camera and photo mode.
4. Camera input handling beyond C's section 3.4 (0x1E0B740 / 0x1E0EE20), only as far as the VR mod needs it.

## F: UI (Noesis) (report F_ui.md)

The VR mod needs the UI on a wrist panel or a world-space panel, and a laser pointer from the controllers.
1. Rendering: where Noesis GUI renders (render target, which pipeline stage or pass in A_rendering.md's stage list,
   how it is composited onto the back buffer), whether it can be redirected into its own texture or drawn once for
   both eye halves, UI resolution and scaling.
2. Input: how mouse position and clicks reach Noesis (InputManager listener group, C_animation_systems.md 3.1; the
   cursor position source: SDL / window coordinates), hit testing, controller-mode UI (radial menus, focus navigation),
   what switches keyboard/mouse vs controller UI mode, and the cleanest way to inject a pointer position and clicks
   from a VR laser.
3. World-space UI: nameplates, overhead text, damage numbers, tooltips, cursor decals: which view/projection they use
   (critical for stereo: two eye views now render the halves), and whether they can be placed per eye.
4. Dialogue UI and the hotbar/combat UI specifically: which widgets, how they are opened and closed.

## H: LOD, streaming and model quality (report H_lod_streaming.md)

The user wants the highest-poly models and highest-quality textures always loaded near the player in VR.
1. Mesh LOD selection: the metric (distance or screen size), which camera position and FOV it uses (main view only, or
   per render view), LOD bias / distance multipliers and the graphics options that set them; the Visual ObjectDesc LOD
   byte (C_animation_systems.md 2.1: Visual+0x98 entries, LOD byte +8); HLOD / impostors / cluster systems
   (`ls::Cluster*`).
2. Streaming: texture streaming (pool size, mip bias, which settings), resource/level streaming around the player,
   virtual textures if any; what decides what is resident.
3. Other quality-by-distance systems: shadow cascade distances and resolution, foliage/detail density, culling
   distances (`ls::CullSystem` 0x3AE5E20), character animation/skinning LOD (Granny BuildWorldPose LOD variant
   0x4A753A0), cloth LOD.
4. For each: where it is computed, its inputs, the option or config value controlling it, accepted ranges (do values
   beyond the in-game Ultra preset work), where graphics options are stored (GraphicSettings / PlayerProfiles config)
   and read, and the cleanest override (setting value vs code patch).
