#pragma once

#include "bg3vr/module_info.hpp"

// Freeze diagnosis: a background thread that watches the presented-frame count. If frames were
// coming and then none arrive for a few seconds, it logs every other thread's call stack once
// (suspend, GetThreadContext with control + integer registers only, unwind with the modules' unwind
// data), so a hang shows where each thread is. No debug registers are touched.
namespace bg3vr::watchdog {

void start(const ModuleInfo& module);

} // namespace bg3vr::watchdog
