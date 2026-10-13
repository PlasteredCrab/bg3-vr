#pragma once

#include "bg3vr/module_info.hpp"

#include <cstdint>

// In-game reverse-engineering probes on a camera controller (!vr_cam_probe). Blocks for about 5 s.
namespace bg3vr::probe {

// Counts calls to each of the controller's vtable slots for 3 s, then watches for 2 s which
// instructions write its pose and matrices, logging each with its call stack.
void run(const ModuleInfo& module, std::uintptr_t controller);

} // namespace bg3vr::probe
