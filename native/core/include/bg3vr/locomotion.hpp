#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

#include <cstddef>
#include <cstdint>

// Smooth thumbstick locomotion through the game's own movement-input override (agent J,
// re-local/ghidra/agents/J_locomotion.md). The MoveController task's direction function 0x425E7F0 reads, when
// GlobalSwitches+0x136C is set, a camera-relative stick vector from +0x1364 (x, right) and +0x1368 (z, forward);
// magnitude up to 1 blends stroll..run, and the vector is turned by the camera entity's yaw (which the mod drives from
// the headset). The game never writes these fields. The task only runs in controller mode (check at 0x425EEE8 on
// byte 0x614FBC8), and controller-device input would switch the UI away from the mouse/laser (agent F), so while
// stick locomotion is on the 6-byte jz after that check is replaced with a NOP (restored when it is turned off).
namespace bg3vr::locomotion {

// 4.1.1.7631656 (re-disassembled 2026-10-19).
namespace layout {
inline constexpr std::uintptr_t kGlobalSwitches = 0x604B1B8; // GlobalSwitches*
inline constexpr std::size_t kOverrideX = 0x1364;            // float, right
inline constexpr std::size_t kOverrideZ = 0x1368;            // float, forward
inline constexpr std::size_t kOverrideEnable = 0x136C;       // byte
inline constexpr std::uintptr_t kControllerModeByte = 0x614FBC8;
} // namespace layout

void install(const Config& config, const ModuleInfo& module);

// Stick locomotion on or off ([locomotion] stick, bridge move=stick|off). Takes effect on the next game frame.
void set_enabled(bool on);

// The left thumbstick, -1..1 (from the headset, any thread).
void set_stick(float x, float y);

// Game thread (camera view update): applies or removes the patch and writes the override.
void on_frame();

// The current stick speed 0..1 (0 when off or idle), for the comfort vignette (any thread).
float speed();

void report();

} // namespace bg3vr::locomotion
