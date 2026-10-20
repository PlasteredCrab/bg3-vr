#pragma once

#include "bg3vr/vr_math.hpp"

#include <cstring>

// Rigid transforms for editing Granny world-pose matrices (bodyik.cpp), kept in a header so the unit tests check the
// conventions. Column-vector form: world = R * local + t. Granny world-pose matrices are row-major with the
// translation in row 3 (row-vector convention: p' = p * M), so R = transpose of their 3x3 and t = row 3.
namespace bg3vr::rigid {

struct Rigid {
    float R[3][3];
    float t[3];
};

inline Rigid from_matrix(const float* m) {
    Rigid r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.R[j][i] = m[i * 4 + j];
        }
        r.t[i] = m[12 + i];
    }
    return r;
}

inline void to_matrix(const Rigid& r, float* m) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            m[i * 4 + j] = r.R[j][i];
        }
        m[12 + i] = r.t[i];
    }
}

inline void mul(const float a[3][3], const float b[3][3], float out[3][3]) {
    float r[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
    }
    std::memcpy(out, r, sizeof(r));
}

inline void transpose(const float a[3][3], float out[3][3]) {
    float r[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i][j] = a[j][i];
        }
    }
    std::memcpy(out, r, sizeof(r));
}

inline void quat_to_matrix(const vr::Quat& q, float out[3][3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    out[0][0] = 1 - 2 * (y * y + z * z);
    out[0][1] = 2 * (x * y - z * w);
    out[0][2] = 2 * (x * z + y * w);
    out[1][0] = 2 * (x * y + z * w);
    out[1][1] = 1 - 2 * (x * x + z * z);
    out[1][2] = 2 * (y * z - x * w);
    out[2][0] = 2 * (x * z - y * w);
    out[2][1] = 2 * (y * z + x * w);
    out[2][2] = 1 - 2 * (x * x + y * y);
}

inline vr::Vec3 apply_r(const float R[3][3], const vr::Vec3& v) {
    return {R[0][0] * v[0] + R[0][1] * v[1] + R[0][2] * v[2], R[1][0] * v[0] + R[1][1] * v[1] + R[1][2] * v[2],
            R[2][0] * v[0] + R[2][1] * v[1] + R[2][2] * v[2]};
}

// Delta that takes `before` to `after`, applied to `child`.
inline Rigid carry(const Rigid& before, const Rigid& after, const Rigid& child) {
    float before_t[3][3], d[3][3];
    transpose(before.R, before_t);
    mul(after.R, before_t, d);
    const vr::Vec3 dt0 = apply_r(d, {before.t[0], before.t[1], before.t[2]});
    const vr::Vec3 td{after.t[0] - dt0[0], after.t[1] - dt0[1], after.t[2] - dt0[2]};
    Rigid out{};
    mul(d, child.R, out.R);
    const vr::Vec3 ct = apply_r(d, {child.t[0], child.t[1], child.t[2]});
    for (int i = 0; i < 3; ++i) {
        out.t[i] = ct[static_cast<std::size_t>(i)] + td[static_cast<std::size_t>(i)];
    }
    return out;
}

} // namespace bg3vr::rigid
