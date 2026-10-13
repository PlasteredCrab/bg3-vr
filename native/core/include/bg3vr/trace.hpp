#pragma once

#include "bg3vr/module_info.hpp"

// Call tracing for reverse engineering (step 5): mid-hooks at the entry of a few named signatures
// that record, per target, how often it's called and the distinct (caller, rcx, rdx) combinations.
// The caller is the return address on the stack at entry. Hooks are only installed while tracing is
// on (!vr_trace on), and removed again by !vr_trace off. No debug registers.
namespace bg3vr::trace {

// Installs the hooks (the signatures render_view_begin, render_view_end, game_pipeline_setup and
// render_pipeline_setup) and clears the records.
void start(const ModuleInfo& module);
// Removes the hooks; the records stay for report().
void stop();
// Logs the records.
void report();

} // namespace bg3vr::trace
