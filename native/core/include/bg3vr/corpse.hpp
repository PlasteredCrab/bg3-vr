#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/vr_math.hpp"

#include <cstddef>
#include <optional>

// Grabbing dead bodies with the hands, through the game's own ragdoll (agent Q, re-local/ghidra/agents/Q_active_ragdoll.md
// 6, agent D 4.3). A grip closing near a corpse (a short ray from the hand, ragdoll group 0x4000, excluding only 0x8)
// finds the body part under it: the hit wrapper's container is a ragdoll in the main scene's ragdoll manager
// ([scene+0x90], entries +0x40 x 0x70, lock +0x18). The ragdoll is woken (MakeDynamic + WakeUp on every body, its freeze
// timer held at 0), and every game frame the grabbed body's velocity is set toward the hand (wrapper +0x120, which locks
// the scene itself); the game's joints pull the rest of the body along, and the visible body follows through the
// game's ragdoll pose node. Letting go mid-swing throws it (the hand's velocity).
// Version 1: the game keeps every ragdoll within about a metre of the character's position (the Dummy_Root tether),
// so a body can be lifted, dragged a little and flung, not carried away; freeing the tether and committing the new
// place to the server is the next step.
namespace bg3vr::corpse {

void install(const Config& config, const ModuleInfo& module);
void set_enabled(bool on);
// Presenting thread: a hand's grab (closed grip, not taken by a holster), its grip and aim in the world, and its
// velocity (world, m/s).
void set_hand(std::size_t hand, bool grab, std::optional<vr::RigidPose> grip, std::optional<vr::RigidPose> aim,
              std::optional<vr::Vec3> velocity);
// Whether a hand holds a body (hold.cpp then leaves that hand alone).
bool holding(std::size_t hand);
// Game thread.
void on_frame();
void report();

} // namespace bg3vr::corpse
