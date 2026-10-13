#pragma once

#include <array>

// Camera pose math in the game's convention: left-handed, +Y up, the camera looks along +Z.
namespace bg3vr::camera {

using Mat4 = std::array<float, 16>; // column-major, like glm

struct Pose {
    float x = 0, y = 0, z = 0;
    float yaw_deg = 0;   // about +Y; 0 looks along +Z
    float pitch_deg = 0; // positive looks up
};

// Automatic look-around around a held pose: yaw swings +/- yaw_deg once per period, pitch nods
// +/- pitch_deg twice per period.
struct Sweep {
    float yaw_deg = 60.0f;
    float pitch_deg = 20.0f;
    float period_s = 8.0f;
};

// A per-eye field of view, as OpenXR reports it (XrFovf), in degrees: left and down are negative.
struct EyeFov {
    float left_deg = -45.0f;
    float right_deg = 45.0f;
    float up_deg = 45.0f;
    float down_deg = -45.0f;
};

// Camera-to-world and world-to-camera matrices for `pose`.
void pose_matrices(const Pose& pose, Mat4& world, Mat4& view);

// The rotation of `pose` as a quaternion (x, y, z, w) that maps +X, +Y, +Z to the camera's right,
// up and forward axes, as the game's view builder expects.
std::array<float, 4> pose_quaternion(const Pose& pose);

} // namespace bg3vr::camera
