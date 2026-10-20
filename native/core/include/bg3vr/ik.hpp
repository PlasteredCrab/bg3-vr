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

// Where the elbow points, after Parger, Mueller, Schmalstieg, Steinberger, "Human Upper-Body Inverse Kinematics for
// Increased Embodiment in Consumer-Grade Virtual Reality" (VRST 2018), section 3.3. The swivel angle phi around the
// shoulder -> hand axis is 0 with the elbow up, 90 degrees outward, 180 down:
//   1. from the hand position in shoulder coordinates (x outward, y up, z forward; metres):
//      phi = 15 + sum_i max(0, x_i w_i + b_i), w = (-50, -60, 260), b = (30, 120, 65), clamped to 13..175 degrees;
//   2. near the shoulder's vertical axis (horizontal distance below 0.5 m) and behind the shoulder (forward 0.1 .. 0 m)
//      the direction is blended linearly toward a fixed relaxed one, (0.133, -0.443, -0.886) in shoulder coordinates;
//   3. a wrist turned past its comfortable range moves the elbow: hand roll (the thumb's turn around the forearm away
//      from "elbow down, thumb up", positive outward) below 0 degrees adds -(roll)^2 / 600, above 90 degrees
//      (roll - 90)^2 / 300 (degrees).
struct ArmFrame {
    vr::Vec3 outward{}; // unit, away from the body on this arm's side
    vr::Vec3 up{};
    vr::Vec3 forward{};
};
// Swivel (degrees) from steps 1 and 2 as a direction perpendicular to the shoulder -> hand axis, and the angle.
struct Elbow {
    vr::Vec3 direction{};
    float phi_deg = 0.0f;
};
Elbow elbow_direction(const vr::Vec3& shoulder, const vr::Vec3& hand, const ArmFrame& frame);
// The direction for a swivel angle (degrees) around the shoulder -> hand axis.
vr::Vec3 swivel_direction(const vr::Vec3& shoulder, const vr::Vec3& hand, const ArmFrame& frame, float phi_deg);
// Step 3: the swivel change (degrees) for a hand roll (degrees).
float wrist_roll_correction(float roll_deg);

} // namespace bg3vr::ik
