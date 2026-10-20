#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/vr_math.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Arms that follow the motion controllers (VRIK-style; docs/design/physical-interaction.md 3a, agent C 1.6 A, agent K
// 2d). Runs inside the Skeleton slot 30 hook (UpdateMeshBindings 0x3A20590, before skinning) for the followed
// character only, on the model-space world pose at [skeleton+0xE0], written through Granny GetWorldPose4x4 0x4A11C40:
//   - arm chain per side: hand = parent of Dummy_R_Hand / Dummy_L_Hand (the weapon bones), forearm = its parent,
//     upper arm = that one's parent (or names from [ik] in bg3vr.ini);
//   - two-bone IK (ik.hpp) puts the wrist on the controller grip (model space, through the visual's world transform)
//     with the elbow bent down; the hand turns with the controller relative to a calibration taken when IK starts
//     (and on recentre);
//   - every other bone below a chain bone (fingers, twist bones, the weapon's dummy bones) moves rigidly with its
//     nearest chain bone, so the held weapon stays in the hand.
// Off by default ([ik] arms) until the logged chain is confirmed in game.
namespace bg3vr::bodyik {

struct Bone {
    std::string name;
    short parent = -1;
};

void configure(const Config& config, const ModuleInfo& module);
bool enabled();
void set_enabled(bool on);

// Game thread, when the skeleton is (re)attached: resolves and logs the arm chains.
void set_rig(std::uintptr_t skeleton, std::uintptr_t visual, const std::vector<Bone>& bones);

// The hands' grip poses in the game world (game convention), from the headset; nullopt = not tracked.
void set_targets(std::optional<vr::RigidPose> left, std::optional<vr::RigidPose> right);

// Take the hand orientations again on the next frame (recentre).
void recalibrate();

// Slot 30 hook (animation worker thread): applies the IK when `skeleton` is the rig's.
void apply(std::uintptr_t skeleton);

void report();

} // namespace bg3vr::bodyik
