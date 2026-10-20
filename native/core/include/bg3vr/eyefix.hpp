#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

#include <cstdint>

// Keeping the two eye views consistent (2026-10-20, first headset run: lighting differed between the eyes and
// changed while looking around, plus some flicker). Both eye views render with split-screen index 0 (index 1 has
// no lights or per-player resources without a second player; docs/re-notes/stereo.md), so per-index and shared
// state is touched by both every frame (re-local/ghidra/agents/A_rendering.md 7, 8, 12):
//   - exposure: both views run the exposure step (post-process step 5, job 0x3EDE160), each measuring the whole
//     shared frame and updating the same adaptation buffers (GPU trace: 214x120 luminance, then the buffers ending
//     in the EV buffer), so the second eye moves the exposure again before its own tonemap, every frame.
//     shared_exposure: the second eye's exposure job is skipped, so both eyes use the main eye's value of the frame.
//   - per-object GPU data: stage 0x47 (job 0x3E53FA0) stores each object's ring-buffer offset per split index; two
//     views on index 0 write the same slot from parallel jobs, and a view can record draws with the other's offsets.
//     serialize_eyes: the second eye's stage 0x47 counter ([ctx+0x58]) waits for the main view's end of recording
//     ([ctx+0x78]) through the game's own job-graph edge 0x41074A0, added once when the eye view is created.
namespace bg3vr::eyefix {

void install(const Config& config, const ModuleInfo& module);

// Render thread, when the eye view has been created (or reused): adds the serialisation edge if wanted.
void link_eyes(std::uintptr_t main_view, std::uintptr_t eye_view);

void report();

} // namespace bg3vr::eyefix
