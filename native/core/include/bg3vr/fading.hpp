#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

// In first person the game's fading for its overhead camera is off (agent AJ): roofs, ceilings and walls between the
// camera and the character no longer fade away, and the main view's see-through hole is closed. Third person keeps it.
// [vr] first_person_no_fading.
namespace bg3vr::fading {

void install(const Config& config, const ModuleInfo& module);
void configure(const Config& config);
void report();

} // namespace bg3vr::fading
