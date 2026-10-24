#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

// Work the right eye repeats that it can take from the left eye (agent AT, AT_perf_sharing.md), from code read in
// 4.1.1.7631656:
//  * Blend shapes, 0x3E15C50(stage 0x41, sets): per morphing mesh it uploads the weights into a buffer the mesh owns
//    and dispatches into the mesh's own UAV `*[obj+0x98]`; no camera, no per-view output, no CPU state but its thread's
//    scratch and the stage's "has commands" byte. Both eyes write identical data into the same buffers, the left eye's
//    stream first; the right eye's skinning reads what the left wrote. [views] share_blendshapes skips the right eye's.
//  * The cull feedback job 0x3F7A510 -> 0x3F7A700(stage, params, scene): a second full scene query (0x3CD27F0) and
//    the "visible" marks per view. Shared by every stage built on 0x3F7A530, and its marks feed gameplay, so it is
//    timed per eye and stage first; [views] skip_eye_cull_feedback (off) skips the right eye's.
namespace bg3vr::sharing {

void install(const Config& config, const ModuleInfo& module);
void configure(const Config& config);
void report();

} // namespace bg3vr::sharing
