#include "bg3vr/grab_math.hpp"

#include <algorithm>
#include <cmath>

namespace bg3vr::grab {

namespace {

using vr::Quat;
using vr::Vec3;

Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 plus(const Vec3& a, const Vec3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Vec3 scale(const Vec3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
float dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
float length(const Vec3& a) {
    return std::sqrt(dot(a, a));
}
Vec3 clamp_length(const Vec3& v, float max) {
    const float l = length(v);
    return l > max && l > 0.0f ? scale(v, max / l) : v;
}
Quat conjugate(const Quat& q) {
    return {-q[0], -q[1], -q[2], q[3]};
}

} // namespace

float follow_for_mass(float base_follow, float mass, float reference_mass, float min_follow) {
    if (!(mass > 0.0f) || !(reference_mass > 0.0f)) {
        return base_follow;
    }
    return std::clamp(base_follow / (1.0f + mass / reference_mass), min_follow, base_follow);
}

Vec3 rotation_vector(const Quat& from, const Quat& to) {
    Quat q = vr::multiply(to, conjugate(from));
    if (q[3] < 0.0f) { // the short way round
        q = {-q[0], -q[1], -q[2], -q[3]};
    }
    const float s = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (s < 1e-7f) {
        return {};
    }
    const float angle = 2.0f * std::atan2(s, q[3]);
    return scale({q[0], q[1], q[2]}, angle / s);
}

Drive drive(const vr::RigidPose& current, const vr::RigidPose& target, const Velocity& hand, float dt,
            const DriveParams& params) {
    Drive out;
    if (!(dt > 0.0f)) {
        return out;
    }
    const Vec3 error = sub(target.position, current.position);
    const Vec3 turn = rotation_vector(current.orientation, target.orientation);
    out.position_error = length(error);
    out.angle_error = length(turn);
    out.snagged = out.position_error > params.teleport_distance;
    const float follow = std::clamp(params.follow, 0.0f, 1.0f);
    out.velocity.linear = clamp_length(plus(hand.linear, scale(error, follow / dt)), params.max_linear);
    out.velocity.angular = clamp_length(plus(hand.angular, scale(turn, follow / dt)), params.max_angular);
    return out;
}

void ThrowTracker::add(double time, const vr::RigidPose& pose) {
    samples_[next_] = {time, pose};
    next_ = (next_ + 1) % kCapacity;
    count_ = std::min(count_ + 1, kCapacity);
}

std::optional<Velocity> ThrowTracker::velocity(double window) const {
    if (count_ < 2) {
        return std::nullopt;
    }
    const auto at = [&](std::size_t age) -> const Sample& { // age 0 = newest
        return samples_[(next_ + kCapacity - 1 - age) % kCapacity];
    };
    const double newest = at(0).time;
    std::size_t used = 1;
    while (used < count_ && newest - at(used).time <= window) {
        ++used;
    }
    if (used < 2) {
        used = 2; // samples further apart than the window: use the last two
    }
    // Least-squares slope per axis, times relative to their mean.
    double mean_t = 0.0;
    Vec3 mean_p{};
    for (std::size_t i = 0; i < used; ++i) {
        mean_t += at(i).time;
        mean_p = plus(mean_p, at(i).pose.position);
    }
    mean_t /= static_cast<double>(used);
    mean_p = scale(mean_p, 1.0f / static_cast<float>(used));
    double tt = 0.0;
    std::array<double, 3> tp{};
    for (std::size_t i = 0; i < used; ++i) {
        const double dt = at(i).time - mean_t;
        tt += dt * dt;
        for (int k = 0; k < 3; ++k) {
            tp[k] += dt * (at(i).pose.position[k] - mean_p[k]);
        }
    }
    if (tt <= 0.0) {
        return std::nullopt;
    }
    Velocity v;
    v.linear = {static_cast<float>(tp[0] / tt), static_cast<float>(tp[1] / tt), static_cast<float>(tp[2] / tt)};
    const Sample& oldest = at(used - 1);
    const double span = newest - oldest.time;
    if (span > 0.0) {
        v.angular = scale(rotation_vector(oldest.pose.orientation, at(0).pose.orientation),
                          static_cast<float>(1.0 / span));
    }
    return v;
}

Launch launch_velocity(const Vec3& from, const Vec3& to, float gravity, float apex) {
    Launch out;
    gravity = std::max(gravity, 0.01f);
    const float top = std::max(from[1], to[1]) + std::max(apex, 0.0f);
    const float rise = std::max(top - from[1], 0.0f);
    const float fall = std::max(top - to[1], 0.0f);
    const float up_time = std::sqrt(2.0f * rise / gravity);
    const float down_time = std::sqrt(2.0f * fall / gravity);
    out.time = std::max(up_time + down_time, 0.05f);
    // Vertical: land exactly at `to` after out.time (equals gravity * up_time unless the minimum time applied).
    out.velocity = {(to[0] - from[0]) / out.time,
                    (to[1] - from[1] + 0.5f * gravity * out.time * out.time) / out.time,
                    (to[2] - from[2]) / out.time};
    return out;
}

bool is_flick(const Vec3& hand_position, const Vec3& hand_velocity, const Vec3& item_position, float threshold) {
    const Vec3 toward_player = sub(hand_position, item_position);
    const float l = length(toward_player);
    if (l < 1e-4f) {
        return false;
    }
    return dot(hand_velocity, scale(toward_player, 1.0f / l)) > threshold;
}

} // namespace bg3vr::grab
