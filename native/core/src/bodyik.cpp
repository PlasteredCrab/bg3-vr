#include "bg3vr/bodyik.hpp"

#include "bg3vr/ik.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/text.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

namespace bg3vr::bodyik {

namespace {

ModuleInfo g_module;
std::atomic<bool> g_enabled{false};
std::array<std::string, 6> g_names{}; // [ik] overrides: right upper/lower/hand, left upper/lower/hand

constexpr std::uintptr_t kGetWorldPose4x4 = 0x4A11C40; // Granny thunk: float* (world_pose*, int bone); slot 10 calls it
constexpr std::size_t kSkeletonWorldPose = 0xE0;
constexpr std::size_t kVisualTransform = 0x08; // quat, translate +0x10, scale +0x1C (SE MoveableObject layout)

struct Side {
    short upper = -1, lower = -1, hand = -1;
    std::vector<std::pair<short, int>> followers; // bone, chain slot (0 upper, 1 lower, 2 hand)
    bool ok() const { return upper >= 0 && lower >= 0 && hand >= 0; }
};
struct Rig {
    std::uintptr_t skeleton = 0;
    std::uintptr_t visual = 0;
    std::array<Side, 2> sides; // 0 left, 1 right
};
std::atomic<std::shared_ptr<const Rig>> g_rig; // replaced on the game thread, read in the hook

std::mutex g_targets_mutex;
std::array<std::optional<vr::RigidPose>, 2> g_targets{}; // world, game convention

// Hook-thread state (slot 30 runs on animation workers, one skeleton at a time per worker; the player's skeleton is
// processed by one job per frame, so its state is only touched by one thread at a time).
struct Calibration {
    bool valid = false;
    float controller[3][3]{}; // controller rotation in model space at calibration
    float hand[3][3]{};       // the hand bone's animated rotation then
};
std::array<Calibration, 2> g_calibration{};
std::atomic<bool> g_recalibrate{true};
std::atomic<std::uint64_t> g_applied{0};
std::atomic<std::uint64_t> g_skipped{0};
std::atomic<int> g_logged{0};

// Rigid transform, column-vector form: world = R * local + t. Granny world-pose matrices are row-major with the
// translation in row 3 (row-vector convention), so R = transpose of their 3x3 and t = row 3.
struct Rigid {
    float R[3][3];
    float t[3];
};

Rigid from_matrix(const float* m) {
    Rigid r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.R[j][i] = m[i * 4 + j];
        }
        r.t[i] = m[12 + i];
    }
    return r;
}

void to_matrix(const Rigid& r, float* m) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            m[i * 4 + j] = r.R[j][i];
        }
        m[12 + i] = r.t[i];
    }
}

void mul(const float a[3][3], const float b[3][3], float out[3][3]) {
    float r[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
    }
    std::memcpy(out, r, sizeof(r));
}

void transpose(const float a[3][3], float out[3][3]) {
    float r[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i][j] = a[j][i];
        }
    }
    std::memcpy(out, r, sizeof(r));
}

void quat_to_matrix(const vr::Quat& q, float out[3][3]) {
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

vr::Vec3 apply_r(const float R[3][3], const vr::Vec3& v) {
    return {R[0][0] * v[0] + R[0][1] * v[1] + R[0][2] * v[2], R[1][0] * v[0] + R[1][1] * v[1] + R[1][2] * v[2],
            R[2][0] * v[0] + R[2][1] * v[1] + R[2][2] * v[2]};
}

// Delta that takes `before` to `after`, applied to `child`.
Rigid carry(const Rigid& before, const Rigid& after, const Rigid& child) {
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

int find(const std::vector<Bone>& bones, const std::string& name) {
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (text::to_lower(bones[i].name) == text::to_lower(name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

Side resolve(const std::vector<Bone>& bones, const char* dummy, const std::string* overrides, const char* label) {
    Side side;
    const auto parent = [&](int b) { return b >= 0 && b < static_cast<int>(bones.size()) ? bones[b].parent : short(-1); };
    const int dummy_bone = find(bones, dummy);
    side.hand = static_cast<short>(!overrides[2].empty() ? find(bones, overrides[2]) : parent(dummy_bone));
    side.lower = static_cast<short>(!overrides[1].empty() ? find(bones, overrides[1]) : parent(side.hand));
    side.upper = static_cast<short>(!overrides[0].empty() ? find(bones, overrides[0]) : parent(side.lower));
    if (!side.ok()) {
        log::warn("ik: {} arm chain not found ({} is bone {}; set [ik] {}_upper/_lower/_hand)", label, dummy, dummy_bone,
                  label);
        return side;
    }
    // Every other bone below a chain bone follows its nearest chain ancestor.
    for (int b = 0; b < static_cast<int>(bones.size()); ++b) {
        if (b == side.upper || b == side.lower || b == side.hand) {
            continue;
        }
        for (int a = parent(b), guard = 0; a >= 0 && guard < 256; a = parent(a), ++guard) {
            const int slot = a == side.hand ? 2 : a == side.lower ? 1 : a == side.upper ? 0 : -1;
            if (slot >= 0) {
                side.followers.emplace_back(static_cast<short>(b), slot);
                break;
            }
        }
    }
    log::info("ik: {} arm: upper {} '{}', forearm {} '{}', hand {} '{}' (from {} bone {}), {} bones follow", label,
              side.upper, bones[side.upper].name, side.lower, bones[side.lower].name, side.hand, bones[side.hand].name,
              dummy, dummy_bone, side.followers.size());
    return side;
}

} // namespace

void configure(const Config& config, const ModuleInfo& module) {
    g_module = module;
    const char* keys[] = {"right_upper", "right_lower", "right_hand", "left_upper", "left_lower", "left_hand"};
    for (std::size_t i = 0; i < 6; ++i) {
        g_names[i] = std::string(text::trim(config.get("ik", keys[i])));
    }
    // Skeleton slot 10 (0x3A1F170) calls the Granny getter at 0x3A1F19C; it must still be the one used here.
    std::uint8_t opcode = 0;
    std::int32_t rel = 0;
    const std::uintptr_t call = module.base + 0x3A1F19C;
    const bool ok = mem::try_copy(&opcode, call, 1) && mem::try_copy(&rel, call + 1, 4) && opcode == 0xE8 &&
                    call + 5 + static_cast<std::intptr_t>(rel) == module.base + kGetWorldPose4x4;
    if (!ok) {
        log::error("ik: the Granny world-pose getter isn't where it was checked (exe+0x{:X}); arm IK unavailable",
                   kGetWorldPose4x4);
        g_module = {};
        return;
    }
    set_enabled(config.get_bool("ik", "arms", false));
}

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void set_enabled(bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
    g_recalibrate.store(true, std::memory_order_relaxed);
    log::info("ik: arms {}", on ? "on (follow the controllers)" : "off ([ik] arms = false)");
}

void set_rig(std::uintptr_t skeleton, std::uintptr_t visual, const std::vector<Bone>& bones) {
    auto rig = std::make_shared<Rig>();
    rig->skeleton = skeleton;
    rig->visual = visual;
    if (skeleton) {
        rig->sides[1] = resolve(bones, "Dummy_R_Hand", &g_names[0], "right");
        rig->sides[0] = resolve(bones, "Dummy_L_Hand", &g_names[3], "left");
    }
    g_rig.store(std::shared_ptr<const Rig>(rig));
    g_recalibrate.store(true, std::memory_order_relaxed);
}

void set_targets(std::optional<vr::RigidPose> left, std::optional<vr::RigidPose> right) {
    std::scoped_lock lock(g_targets_mutex);
    g_targets = {left, right};
}

void recalibrate() {
    g_recalibrate.store(true, std::memory_order_relaxed);
}

void apply(std::uintptr_t skeleton) {
    if (!g_enabled.load(std::memory_order_relaxed) || !g_module.base) {
        return;
    }
    const std::shared_ptr<const Rig> rig = g_rig.load();
    if (!rig || rig->skeleton != skeleton) {
        return;
    }
    std::array<std::optional<vr::RigidPose>, 2> targets;
    {
        std::scoped_lock lock(g_targets_mutex);
        targets = g_targets;
    }
    std::uintptr_t pose = 0;
    float visual[10]{};
    if (!mem::try_copy(&pose, skeleton + kSkeletonWorldPose, sizeof(pose)) || !pose ||
        !mem::try_copy(visual, rig->visual + kVisualTransform, sizeof(visual))) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    using GetMatrixFn = float* (*)(std::uintptr_t, int);
    const auto get = reinterpret_cast<GetMatrixFn>(g_module.base + kGetWorldPose4x4);
    // World -> model: inverse of the visual's transform (rotation, translation, scale).
    const vr::Quat vq{visual[0], visual[1], visual[2], visual[3]};
    const vr::Quat inverse{-vq[0], -vq[1], -vq[2], vq[3]};
    const float scale = visual[7] != 0.0f ? visual[7] : 1.0f;
    const bool recalibrate = g_recalibrate.exchange(false, std::memory_order_relaxed);

    for (int s = 0; s < 2; ++s) {
        const Side& side = rig->sides[static_cast<std::size_t>(s)];
        const auto& target = targets[static_cast<std::size_t>(s)];
        if (!side.ok() || !target) {
            continue;
        }
        float* mu = get(pose, side.upper);
        float* ml = get(pose, side.lower);
        float* mh = get(pose, side.hand);
        if (!mu || !ml || !mh) {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const Rigid upper = from_matrix(mu), lower = from_matrix(ml), hand = from_matrix(mh);
        // The controller in model space.
        const vr::Vec3 rel{target->position[0] - visual[4], target->position[1] - visual[5], target->position[2] - visual[6]};
        const vr::Vec3 rotated = vr::rotate(inverse, rel);
        const vr::Vec3 goal{rotated[0] / scale, rotated[1] / scale, rotated[2] / scale};
        float controller[3][3];
        quat_to_matrix(vr::multiply(inverse, target->orientation), controller);

        Calibration& cal = g_calibration[static_cast<std::size_t>(s)];
        if (recalibrate || !cal.valid) {
            std::memcpy(cal.controller, controller, sizeof(controller));
            std::memcpy(cal.hand, hand.R, sizeof(hand.R));
            cal.valid = true;
        }

        const vr::Vec3 S{upper.t[0], upper.t[1], upper.t[2]};
        const vr::Vec3 E{lower.t[0], lower.t[1], lower.t[2]};
        const vr::Vec3 W{hand.t[0], hand.t[1], hand.t[2]};
        const vr::Vec3 pole{E[0], E[1] - 0.25f, E[2]}; // elbows bend down
        const ik::TwoBone solved = ik::solve_two_bone(S, E, W, goal, pole);

        Rigid new_upper = upper, new_lower = lower, new_hand = hand;
        float r[3][3];
        quat_to_matrix(solved.upper_delta, r);
        mul(r, upper.R, new_upper.R);
        quat_to_matrix(solved.lower_delta, r);
        mul(r, lower.R, new_lower.R);
        for (int i = 0; i < 3; ++i) {
            new_lower.t[i] = solved.elbow[static_cast<std::size_t>(i)];
            new_hand.t[i] = solved.wrist[static_cast<std::size_t>(i)];
        }
        // Hand rotation: the controller's turn since calibration, on top of the hand's rotation then.
        float cal_inverse[3][3], turn[3][3];
        transpose(cal.controller, cal_inverse);
        mul(controller, cal_inverse, turn);
        mul(turn, cal.hand, new_hand.R);

        // Followers first (they need the old chain transforms), then the chain.
        const Rigid* before[3] = {&upper, &lower, &hand};
        const Rigid* after[3] = {&new_upper, &new_lower, &new_hand};
        for (const auto& [bone, slot] : side.followers) {
            if (float* m = get(pose, bone)) {
                to_matrix(carry(*before[slot], *after[slot], from_matrix(m)), m);
            }
        }
        to_matrix(new_upper, mu);
        to_matrix(new_lower, ml);
        to_matrix(new_hand, mh);
        if (g_logged.load(std::memory_order_relaxed) < 4) {
            g_logged.fetch_add(1, std::memory_order_relaxed);
            log::info("ik: {} arm: shoulder ({:.3f}, {:.3f}, {:.3f}) elbow ({:.3f}, {:.3f}, {:.3f}) wrist ({:.3f}, {:.3f}, "
                      "{:.3f}) -> goal ({:.3f}, {:.3f}, {:.3f}){}; new elbow ({:.3f}, {:.3f}, {:.3f}) wrist ({:.3f}, "
                      "{:.3f}, {:.3f}) [model space]",
                      s ? "right" : "left", S[0], S[1], S[2], E[0], E[1], E[2], W[0], W[1], W[2], goal[0], goal[1],
                      goal[2], solved.reached ? "" : " (out of reach)", solved.elbow[0], solved.elbow[1],
                      solved.elbow[2], solved.wrist[0], solved.wrist[1], solved.wrist[2]);
        }
        g_applied.fetch_add(1, std::memory_order_relaxed);
    }
}

void report() {
    log::info("ik: arms {}, {} arm solves, {} skipped", g_enabled.load(std::memory_order_relaxed) ? "on" : "off",
              g_applied.load(std::memory_order_relaxed), g_skipped.load(std::memory_order_relaxed));
}

} // namespace bg3vr::bodyik
