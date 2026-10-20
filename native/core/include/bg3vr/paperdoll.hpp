#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

// The 3D characters inside UI screens (inventory / equipment / character sheet, party, companions, examine, trade):
// every one is an ls.LSPaperdoll Noesis control showing a live offscreen render of a copy of the character in its
// own small scene (agent P, re-local/ghidra/agents/P_ui_character_3d.md). Step 1 of showing them in true 3D in VR:
//   - [ui] paperdoll = diagnose (default): hooks the paperdoll manager's per-frame update (0x13E1A40, PM = GameUI+0x2B90)
//     and logs every doll once: its renderer's frame, view, camera, scene, yaw, and its output texture (the D3D11
//     texture found in the render-frame texture object, its size, format and flags, and the alpha of a corner
//     pixel read back on the presenting thread: whether the background is transparent);
//   - [ui] paperdoll = hide: also skips LSPaperdoll::OnRender (0x155D9D0) so the flat picture disappears from the UI
//     (for testing the stereo doll, step 2);
//   - [ui] paperdoll = normal: nothing.
namespace bg3vr::paperdoll {

void install(const Config& config, const ModuleInfo& module);
// Presenting thread: the pending corner-alpha readback.
void on_present();
void report();

} // namespace bg3vr::paperdoll
