#pragma once

#include "bg3vr/module_info.hpp"

#include <cstdint>
#include <vector>

// Finding the game's render views (rf::RenderView / ecl::GameRenderView) at runtime, for Phase 1
// step 5 (split screen as same-frame stereo) and for matching presented frames to camera poses.
//
// The Script Extender's layout (GameDefinitions/RenderFramework.h) puts two RenderViewStates
// (ReadState, WriteState) in each render view, each ending in a CameraController*. So the views
// that render our camera can be found by scanning memory for pointers to the driven controller
// and checking that each hit sits at the end of something shaped like a RenderViewState. The scan
// covers committed private read-write memory once, on a background thread, and logs everything it
// finds so the layout can be checked against this build.
namespace bg3vr::views {

// Starts a scan for render views pointing at `controller` (no-op while one is running).
void find(const ModuleInfo& module, std::uintptr_t controller);

// The scan itself, synchronously: the ReadState addresses found (also logged and kept for found()).
std::vector<std::uintptr_t> scan_now(std::uintptr_t controller);

// Logs the current state of the views found by the last scan (cheap; no scan).
void dump();

// The ReadState addresses of the render views found by the last scan.
std::vector<std::uintptr_t> found();

} // namespace bg3vr::views
