#pragma once

#include "bg3vr/module_info.hpp"

#include <cstdint>
#include <optional>
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

// Where a view goes on screen, as fractions of it (RenderViewState Scale and Offset). The game
// honours it: Scale (0.5, 1) draws the view's whole image into half the screen (2026-10-13).
struct Placement {
    float scale_x = 1.0f, scale_y = 1.0f;
    float offset_x = 0.0f, offset_y = 0.0f;
};

// Step 5 experiments, written into both states of the views every frame. Views are numbered by
// their place in found().
enum class Layout { SideBySide, SideBySideSwapped, Full };
// SideBySide: view 0 left, view 1 right; Swapped: the other way round; Full: everything back to
// full screen and enabled (written once).
void set_layout(Layout layout);
// One view's placement and/or Enabled flag.
void set_view(std::size_t index, std::optional<Placement> placement, std::optional<bool> enabled);
// Per-frame work on the game thread (the camera's frame callback).
void on_frame();

// What points at each found view (memory scan, background thread): a list that holds view 0 but
// not view 1 is a candidate for the list of views that get drawn.
void refs();
// The small values (flags, counts, indices) that differ between view 0 and view 1.
void diff();

// Logs the current state of the views found by the last scan (cheap; no scan).
void dump();

// The ReadState addresses of the render views found by the last scan.
std::vector<std::uintptr_t> found();

} // namespace bg3vr::views
