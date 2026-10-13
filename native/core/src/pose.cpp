#include "bg3vr/pose.hpp"

#include <cmath>
#include <numbers>

namespace bg3vr::camera {

namespace {

struct Vec3 {
    float x, y, z;
};

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 normalize(const Vec3& v) {
    const float length = std::sqrt(dot(v, v));
    return {v.x / length, v.y / length, v.z / length};
}

} // namespace

void pose_matrices(const Pose& pose, Mat4& world, Mat4& view) {
    const float yaw = pose.yaw_deg * std::numbers::pi_v<float> / 180.0f;
    const float pitch = pose.pitch_deg * std::numbers::pi_v<float> / 180.0f;
    const Vec3 forward = normalize({std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)});
    const Vec3 right = normalize(cross({0.0f, 1.0f, 0.0f}, forward));
    const Vec3 up = cross(forward, right);
    const Vec3 p{pose.x, pose.y, pose.z};

    world = {right.x, right.y, right.z, 0.0f, up.x, up.y, up.z, 0.0f,
             forward.x, forward.y, forward.z, 0.0f, p.x, p.y, p.z, 1.0f};
    view = {right.x, up.x, forward.x, 0.0f, right.y, up.y, forward.y, 0.0f,
            right.z, up.z, forward.z, 0.0f, -dot(right, p), -dot(up, p), -dot(forward, p), 1.0f};
}

std::array<float, 4> pose_quaternion(const Pose& pose) {
    Mat4 world, view;
    pose_matrices(pose, world, view);
    // Rotation matrix m[row][col] with columns right, up, forward.
    const float m00 = world[0], m10 = world[1], m20 = world[2];
    const float m01 = world[4], m11 = world[5], m21 = world[6];
    const float m02 = world[8], m12 = world[9], m22 = world[10];
    const float trace = m00 + m11 + m22;
    float x, y, z, w;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        w = 0.25f * s;
        x = (m21 - m12) / s;
        y = (m02 - m20) / s;
        z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        w = (m21 - m12) / s;
        x = 0.25f * s;
        y = (m01 + m10) / s;
        z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        w = (m02 - m20) / s;
        x = (m01 + m10) / s;
        y = 0.25f * s;
        z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        w = (m10 - m01) / s;
        x = (m02 + m20) / s;
        y = (m12 + m21) / s;
        z = 0.25f * s;
    }
    return {x, y, z, w};
}

} // namespace bg3vr::camera
