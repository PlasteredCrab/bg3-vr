#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

// Where the frame goes, per eye and per render stage (agents AT 4.1 and AU 5.6: no per-pass timing existed). Two
// hooks, both from code read in 4.1.1.7631656:
//  * 0x3CC7010(view), a view's end of recording: executes the view's own command buffer (view+0x50 when
//    view+0x2150) and then calls slot 8 (+0x40: wait for the stage's job, execute its buffers) of every stage in
//    [view+0x28] (count +0x34). The mod runs those same steps itself, which gives exact view and stage windows (CPU
//    time per stage, waits included) on the thread that runs them.
//  * 0x407F240(cmd), the command-buffer executor: runs the buffer's opcodes against D3D11 inside the renderer's
//    critical section [[cmd+0x38]+0x78] (taken only when cmd+0x122D is set). The mod enters that same (re-entrant)
//    section around the call and places D3D11 timestamp queries before and after, so they never interleave with another
//    thread's use of the immediate context; the GPU time goes to the view and stage whose window it ran in.
// Results are read back five frames later (no stall) and logged every [perf] stage_timing_frames frames: GPU and CPU
// ms per frame per eye, the top stages of each eye and what the right eye costs on top of the left.
namespace bg3vr::stage_timing {

void install(const Config& config, const ModuleInfo& module);

// The render window the calling thread is in (set by the view executor whether or not timing is on): eye 0 / 1 for
// the left / right eye view, -1 any other view or none; stage the stage id (0..0xFF), 0x100 the view's own buffer,
// -1 none. Foveation gates its shading rates on it.
struct Window {
    int eye = -1;
    int stage = -1;
};
Window window();
void configure(const Config& config);
void report();

} // namespace bg3vr::stage_timing
