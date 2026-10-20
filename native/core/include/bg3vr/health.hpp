#pragma once

#include <string_view>

// One place that says which features are ready, off by setting, or unavailable (and why), so the first look at
// bg3vr.log answers "what works?". Modules report as they install or start; the summary is logged after start-up,
// when VR play starts and with every report (!vr_fps).
namespace bg3vr::health {

enum class State { Ready, Off, Unavailable };

void set(std::string_view feature, State state, std::string_view detail = {});
void log_summary(std::string_view when);

} // namespace bg3vr::health
