#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/vr_math.hpp"

#include <cstdint>

// Pointing with the hand instead of the mouse (agent I, re-local/ghidra/agents/I_picking_actions.md 1.3-1.7). The
// per-player picking (0x1C45A90) unprojects the cursor through CameraController::ScreenToWorldRay 0x4085960
// (camera, u, v, float outA[3] /*start, near plane*/, float outB[3] /*end*/, bool cached), then runs its four
// raycast jobs on that ray: hovered character/item, ground position, path preview, highlight and the default
// action all follow it. An inline hook on 0x4085960 replaces the ray with the right hand's aim ray (from the
// headset, rayprobe::hand_aim) only when called from the two picking sites (returns 0x1C45CE1 / 0x1C45E21).
namespace bg3vr::picking {

// 4.1.1.7631656 (re-disassembled 2026-10-19).
namespace layout {
inline constexpr std::uintptr_t kPickReturn1 = 0x1C45CE1;
inline constexpr std::uintptr_t kPickReturn2 = 0x1C45E21;
inline constexpr float kRayLength = 40.0f; // metres
} // namespace layout

void install(const Config& config, const ModuleInfo& module);

// Hand pointing on or off (also [pick] hand in bg3vr.ini; bridge pick=hand|off).
void set_hand(bool on);

// For a moment (`seconds`), the picking ray is this one (world, game convention) whatever the hand or mouse does:
// combat aims the game's own click along the head's gaze or a drawn arrow (combat.cpp). Any thread.
void override_ray(const vr::Vec3& from, const vr::Vec3& direction, float seconds);

void report();

} // namespace bg3vr::picking
