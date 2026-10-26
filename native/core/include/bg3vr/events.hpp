#pragma once

#include "bg3vr/module_info.hpp"

#include <cstddef>

// A detailed log of what the renderer does in the frames right after something risky (a split view
// build), to see the exact order of events up to a hang: render view begin/end recording, each view's
// job release (0x407B540), each stage's wait call in end-recording (0x3CC7050), job submissions
// (0x410E5D0) and counter waits (0x410E880), each with the counter's full state. Hooks are installed on
// the first arm() and stay; logging stops after `frames` presented frames or `max_events` lines.
namespace bg3vr::events {

void arm(const ModuleInfo& module, int frames, std::size_t max_events);
void disarm();

// The automatic event logs (stereo on, view create / build) only when [dev] trace_on_stereo is set: each one logs
// thousands of lines on the render thread (a visible hitch at every view switch, 2026-10-26).
void set_auto(bool on);
bool auto_on();
void arm_auto(const ModuleInfo& module, int frames, std::size_t max_events);

} // namespace bg3vr::events
