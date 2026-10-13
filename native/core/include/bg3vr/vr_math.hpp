#pragma once

#include "bg3vr/pose.hpp"

#include <array>

// Math for driving the game camera from a headset (Phase 1). Game convention: left-handed, +Y up,
// a camera looks along its local +Z, right is local +X. OpenXR: right-handed, +Y up, a view looks
// along its local -Z. Quaternions are (x, y, z, w); matrices column-major (glm layout).
namespace bg3vr::vr {

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;

struct RigidPose {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

// a * b: rotate by b, then by a.
Quat multiply(const Quat& a, const Quat& b);
Vec3 rotate(const Quat& q, const Vec3& v);

// Rotation about +Y that turns +Z toward +X by `degrees` (the game's yaw, as camera::Pose uses it).
Quat yaw_quat(float degrees);

// Camera-to-world and world-to-camera matrices for a camera at `pose` (game convention).
void camera_matrices(const RigidPose& pose, camera::Mat4& world, camera::Mat4& view);

// An OpenXR pose (view or head) expressed in game camera convention: the space is mirrored in Z,
// so positions become (x, y, -z) and orientations (-x, -y, z, w). The view's look direction (-Z)
// becomes the game's +Z; right and up are unchanged.
RigidPose from_openxr(const RigidPose& xr);

// Places a head pose that is relative to the tracking origin (already in game convention) into the
// game world: rotated by the anchor's yaw and offset from the anchor's position. The anchor's pitch
// is ignored, so the horizon stays level with the real one.
RigidPose anchor(const camera::Pose& anchor, const RigidPose& local);

// Per-eye projection in the game's form (left-handed, reversed-Z, finite far plane: the near plane
// maps to depth 1) from OpenXR field-of-view angles in radians (left and down are negative).
camera::Mat4 projection(float angle_left, float angle_right, float angle_up, float angle_down, float near_z,
                        float far_z);

} // namespace bg3vr::vr
