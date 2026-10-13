#pragma once

#include "bg3vr/config.hpp"

// OpenXR in the game (Phase 1 step 2): with [xr] enabled = true in bg3vr.ini, the first presented
// frame opens an OpenXR session on the game's own D3D11 device, and every frame after that copies
// the finished back buffer onto a virtual screen (an OpenXR quad layer) in front of the player.
// Off by default; openxr_loader.dll is only loaded when enabled.
namespace bg3vr::headset {

void install(const Config& config);
void report();

} // namespace bg3vr::headset
