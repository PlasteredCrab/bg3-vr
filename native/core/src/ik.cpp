#include "bg3vr/ik.hpp"

#include <algorithm>
#include <cmath>

namespace bg3vr::ik {

namespace {

using vr::Quat;
using vr::Vec3;

Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 add(const Vec3& a, const Vec3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Vec3 scale(const Vec3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
float dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float length(const Vec3& a) {
    return std::sqrt(dot(a, a));
}
Vec3 normalized(const Vec3& a, const Vec3& fallback) {
    const float l = length(a);
    return l > 1e-6f ? scale(a, 1.0f / l) : fallback;
}

// Any unit vector perpendicular to n.
Vec3 any_perpendicular(const Vec3& n) {
    const Vec3 axis = std::abs(n[0]) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    return normalized(cross(n, axis), {0.0f, 0.0f, 1.0f});
}

// The part of v perpendicular to unit n, normalised; zero length if v is (nearly) along n.
Vec3 perpendicular_part(const Vec3& v, const Vec3& n, bool& ok) {
    const Vec3 p = sub(v, scale(n, dot(v, n)));
    const float l = length(p);
    ok = l > 1e-5f;
    return ok ? scale(p, 1.0f / l) : Vec3{};
}

} // namespace

Quat from_to(const Vec3& from, const Vec3& to) {
    const Vec3 a = normalized(from, {0.0f, 0.0f, 1.0f});
    const Vec3 b = normalized(to, {0.0f, 0.0f, 1.0f});
    const float d = dot(a, b);
    if (d < -0.999999f) {
        const Vec3 axis = any_perpendicular(a); // 180 degrees about any perpendicular axis
        return {axis[0], axis[1], axis[2], 0.0f};
    }
    const Vec3 c = cross(a, b);
    Quat q{c[0], c[1], c[2], 1.0f + d};
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& v : q) {
        v /= l;
    }
    return q;
}

TwoBone solve_two_bone(const Vec3& shoulder, const Vec3& elbow, const Vec3& wrist, const Vec3& target,
                       const Vec3& pole) {
    TwoBone result;
    const float a = length(sub(elbow, shoulder));
    const float b = length(sub(wrist, elbow));
    if (a < 1e-6f || b < 1e-6f) {
        result.elbow = elbow;
        result.wrist = wrist;
        return result;
    }
    const Vec3 to_target = sub(target, shoulder);
    const float distance = length(to_target);
    const Vec3 n = normalized(to_target, normalized(sub(wrist, shoulder), {0.0f, 0.0f, 1.0f}));
    // Keep a sliver of bend at full stretch and never fold completely, so the elbow stays well defined.
    const float reach = a + b;
    const float d = std::clamp(distance, std::abs(a - b) + 1e-4f, reach * 0.9999f);
    result.reached = distance <= reach;

    // Bend plane from the pole; fall back to the current elbow's side, then to anything.
    bool ok = false;
    Vec3 u = perpendicular_part(sub(pole, shoulder), n, ok);
    if (!ok) {
        u = perpendicular_part(sub(elbow, shoulder), n, ok);
    }
    if (!ok) {
        u = any_perpendicular(n);
    }
    const float cos_alpha = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
    const float sin_alpha = std::sqrt(std::max(0.0f, 1.0f - cos_alpha * cos_alpha));
    result.elbow = add(shoulder, add(scale(n, a * cos_alpha), scale(u, a * sin_alpha)));
    result.wrist = add(shoulder, scale(n, d));
    result.upper_delta = from_to(sub(elbow, shoulder), sub(result.elbow, shoulder));
    result.lower_delta = from_to(sub(wrist, elbow), sub(result.wrist, result.elbow));
    return result;
}

} // namespace bg3vr::ik
