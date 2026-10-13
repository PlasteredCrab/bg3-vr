#include "bg3vr/vr_math.hpp"

#include <cmath>
#include <numbers>

namespace bg3vr::vr {

Quat multiply(const Quat& a, const Quat& b) {
    return {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
}

Vec3 rotate(const Quat& q, const Vec3& v) {
    // v' = v + 2w(u x v) + 2u x (u x v), u = (x, y, z)
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz),
            v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}

Quat yaw_quat(float degrees) {
    const float half = degrees * std::numbers::pi_v<float> / 360.0f;
    return {0.0f, std::sin(half), 0.0f, std::cos(half)};
}

void camera_matrices(const RigidPose& pose, camera::Mat4& world, camera::Mat4& view) {
    const Vec3 right = rotate(pose.orientation, {1.0f, 0.0f, 0.0f});
    const Vec3 up = rotate(pose.orientation, {0.0f, 1.0f, 0.0f});
    const Vec3 forward = rotate(pose.orientation, {0.0f, 0.0f, 1.0f});
    const Vec3& p = pose.position;
    const auto dot = [](const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

    world = {right[0], right[1], right[2], 0.0f, up[0], up[1], up[2], 0.0f,
             forward[0], forward[1], forward[2], 0.0f, p[0], p[1], p[2], 1.0f};
    view = {right[0], up[0], forward[0], 0.0f, right[1], up[1], forward[1], 0.0f,
            right[2], up[2], forward[2], 0.0f, -dot(right, p), -dot(up, p), -dot(forward, p), 1.0f};
}

RigidPose from_openxr(const RigidPose& xr) {
    return {{xr.position[0], xr.position[1], -xr.position[2]},
            {-xr.orientation[0], -xr.orientation[1], xr.orientation[2], xr.orientation[3]}};
}

RigidPose anchor(const camera::Pose& anchor, const RigidPose& local) {
    const Quat yaw = yaw_quat(anchor.yaw_deg);
    const Vec3 offset = rotate(yaw, local.position);
    return {{anchor.x + offset[0], anchor.y + offset[1], anchor.z + offset[2]}, multiply(yaw, local.orientation)};
}

camera::Mat4 projection(float angle_left, float angle_right, float angle_up, float angle_down, float near_z,
                        float far_z) {
    const float l = std::tan(angle_left), r = std::tan(angle_right);
    const float u = std::tan(angle_up), d = std::tan(angle_down);
    const float a = 2.0f / (r - l), b = -(r + l) / (r - l);
    const float c = 2.0f / (u - d), e = -(u + d) / (u - d);
    const float z_scale = near_z / (near_z - far_z);
    const float z_offset = -far_z * near_z / (near_z - far_z);
    return {a, 0.0f, 0.0f, 0.0f, 0.0f, c, 0.0f, 0.0f, b, e, z_scale, 1.0f, 0.0f, 0.0f, z_offset, 0.0f};
}

} // namespace bg3vr::vr
