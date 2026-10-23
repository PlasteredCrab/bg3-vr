#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

// Sun shadows rendered once for both eyes (agent T, re-local/ghidra/agents/T_shared_work.md, 1 / 2.4).
// The sun's cascaded shadow stage (0x02) fits its cascades from the camera entity (the head, with the game's FOV),
// not from the view's eye, so both eye views compute the same world-space cascades; only the camera-relative
// translation in the four cascade matrices differs (by the eye offset). The right eye's stage is switched off with
// its own run flag after its prepare (the game's own "no sun" path) and gets the left eye's results, the matrices
// re-based to its origin. The textures stay valid: the left eye's commands run before the right eye's.
// [views] share_shadows (default on). The first frames of every stereo session only validate: both eyes compute
// their own cascades and the re-based copy is compared with the right eye's; sharing starts only if they agree.
namespace bg3vr::shadows {

void install(const Config& config, const ModuleInfo& module);
void configure(const Config& config); // the VR menu
void report();

} // namespace bg3vr::shadows
