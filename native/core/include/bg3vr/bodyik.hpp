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
//   - two-bone IK (ik.hpp) with the elbow bent down. [ik] hands = weapon (default): the weapon bone (Dummy_R_Hand /
//     Dummy_L_Hand, where the game attaches what the hand holds) takes the controller grip's position and orientation
//     (times the [ik] weapon_pitch / _yaw / _roll offset), so a held weapon sits in the fist at the controller's
//     angle, and the wrist goes wherever that puts it; hands = relative: the wrist on the grip, the hand turned by
//     the controller's turn since a calibration taken when IK starts (and on recentre);
//   - every other bone below a chain bone (fingers, twist bones, the weapon's dummy bones) moves rigidly with its
//     nearest chain bone, so the held weapon stays in the hand.
// On by default ([ik] arms); the chain was confirmed in game on 2026-10-20 (Shoulder / Elbow / Wrist, 29 followers).
namespace bg3vr::bodyik {

struct Bone {
    std::string name;
    short parent = -1;
};

void configure(const Config& config, const ModuleInfo& module);
// [ik] hands and the weapon angle offsets; again whenever the VR menu changes one (any thread).
void configure_hands(const Config& config);
bool enabled();
void set_enabled(bool on);

// Game thread, when the skeleton is (re)attached: resolves and logs the arm chains.
void set_rig(std::uintptr_t skeleton, std::uintptr_t visual, const std::vector<Bone>& bones);

// The hands' grip poses in the game world (game convention), from the headset; nullopt = not tracked.
void set_targets(std::optional<vr::RigidPose> left, std::optional<vr::RigidPose> right);

// Take the hand orientations again on the next frame (recentre).
void recalibrate();

// Finger curl per hand (0 left, 1 right), 0 open .. 1 closed: the index finger and the other three ([ik] fingers).
// From the controllers (trigger, grip) or a tracked hand; ignored while a weapon is drawn (the game's grip pose).
void set_fingers(std::size_t hand, float index, float others);
void set_weapon_drawn(bool drawn);

// Slot 30 hook (animation worker thread): applies the IK when `skeleton` is the rig's.
void apply(std::uintptr_t skeleton);
// The eye bones' midpoint (Dummy_EyeFX_01 / 02, model space) as the animation posed it, read in apply() before the
// body turn moves every bone: the camera sits there, so turning the body toward the hands can't move the camera
// (which would move the hands relative to the body again). nullopt unless apply() read it for this skeleton in the
// last 0.25 s (arms off: nothing moves the bones, the skeleton's own eye read is right).
std::optional<vr::Vec3> animated_eyes(std::uintptr_t skeleton);

void report();

} // namespace bg3vr::bodyik
