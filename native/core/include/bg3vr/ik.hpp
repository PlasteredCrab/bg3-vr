#pragma once

#include "bg3vr/vr_math.hpp"

// Inverse kinematics for the body (Phase 2, VRIK-style arms). Pure math on positions in one space (model
// or world), independent of the game's bone axis conventions: results are positions and rotation deltas
// that take each bone's old direction to its new one, applied on top of the bone's animated rotation.
namespace bg3vr::ik {

// The shortest rotation taking direction `from` to direction `to` (need not be normalised).
vr::Quat from_to(const vr::Vec3& from, const vr::Vec3& to);

struct TwoBone {
    vr::Vec3 elbow{};
    vr::Vec3 wrist{};
    vr::Quat upper_delta{0.0f, 0.0f, 0.0f, 1.0f}; // old (elbow - shoulder) direction -> new
    vr::Quat lower_delta{0.0f, 0.0f, 0.0f, 1.0f}; // old (wrist - elbow) direction -> new
    bool reached = false;                          // the target is within the arm's length
};

// Two-bone IK (shoulder-elbow-wrist, or hip-knee-ankle): keeps both bone lengths from the current pose,
// puts the wrist on `target` (or as close as the straight arm reaches) and bends the elbow toward `pole`
// (a point the elbow should point at, e.g. below and behind the shoulder). If the pole is on the
// shoulder-target line, the current elbow's side is kept.
TwoBone solve_two_bone(const vr::Vec3& shoulder, const vr::Vec3& elbow, const vr::Vec3& wrist,
                       const vr::Vec3& target, const vr::Vec3& pole);

} // namespace bg3vr::ik
