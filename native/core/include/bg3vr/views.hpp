#pragma once

#include "bg3vr/config.hpp"

#include "bg3vr/module_info.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
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

// Step 5 experiment: finds the game pipeline (the object holding the main view at +0xA0 and the
// special 3-stage view at +0xA8) and builds a normal view's stage lists into found view `index` on the
// render thread (not the full setup, which crashes; see views.cpp); a view without a camera gets the
// driven one, through the view's own SetCamera on the game thread.
// kAutomatic picks the first found view that is neither of those.
inline constexpr std::size_t kAutomatic = static_cast<std::size_t>(-1);
// compositing also builds stages 0x45 and 0x07 (crashed the game on 2026-10-13).
// With kAutomatic it first scans (repeatedly, up to a minute) until the split view exists.
void build(const ModuleInfo& module, std::uintptr_t controller, std::size_t index, bool compositing);

// The game's own way to an extra view (player manager slot 1, 0x17F82F0, then the pipeline setup), on the render
// thread; the camera, scene and environment follow on the game thread as for build. !vr_stereo on then places
// the main and new view with the game's 0x407B800.
void create(const ModuleInfo& module, std::uintptr_t controller);

// Whether build runs the game's full view setup (0x588E50; [views] full_setup, default true) or only the
// stage lists (0x3AC4EA0).
void set_full_setup(bool on);
// The split-screen index the split view renders with after the build (which uses 1): [views] split_index,
// default 0 (render as player 0, the same lights, exposure and per-player resources as the main view).
void set_split_index(int index);

// After a build that completed: the pipeline's main view and the built split view (view addresses,
// not ReadStates), the pair that draws the two eyes in same-frame stereo.
std::optional<std::array<std::uintptr_t, 2>> eye_views();
// Stops applying the stereo layout without touching the views (a level is unloading: they may be gone). The next
// create() reuses the eye view if it is still live, or makes a new one.
void forget_layout();
// Same-frame stereo layout: the main view in the left half, the split view in the right half, both
// enabled (on), or both back to full screen (off). Needs a completed build.
bool set_stereo_layout(bool on);
// Logs every 8-byte field (0..0x2D00) that differs between the main and the split view.
void diff_eye_views(const char* why);

// What points at each found view (memory scan, background thread): a list that holds view 0 but
// not view 1 is a candidate for the list of views that get drawn.
void refs();
// The small values (flags, counts, indices) that differ between view 0 and view 1.
void diff();

// Logs the current state of the views found by the last scan (cheap; no scan).
void dump();

// A value seen somewhere (a register of a frozen thread's frame), to mark in dump_jobs.
struct Hint {
    std::uintptr_t value = 0;
    std::string where;
};
// Logs every view in the presenting RenderFrame's view list with its job context (17 job counters:
// count, flags, predecessors, successors, queue indices) and its stages (id, vtable, wait function,
// async flag, the counter each waits on and which view's context slot that is), plus the frame-wide
// counter. Reads only (safe while other threads are suspended or running). Values in `hints` are
// marked where they appear.
void dump_jobs(const char* why, const std::vector<Hint>& hints = {});

// One job counter's state, as dump_jobs prints it.
std::string describe_counter(std::uintptr_t counter);
// "main", "SPLIT", "special" for the views a build knows, otherwise "view".
const char* view_role(std::uintptr_t view);
// The view context slot a counter is ("view 1 ctx+0x78"), searching the RenderFrame's views.
std::string counter_name(std::uintptr_t counter);
// A stage's id (its vtable slot 1), or -1.
int stage_id_of(std::uintptr_t stage);

// The ReadState addresses of the render views found by the last scan.
std::vector<std::uintptr_t> found();

// [views] eye_fog_layers: give the eye view its own fog layers after each environment copy (agent Z 1).
void configure(const Config& config);

} // namespace bg3vr::views
