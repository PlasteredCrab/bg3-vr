#include "bg3vr/bodyik.hpp"

#include "bg3vr/ik.hpp"
#include "bg3vr/rigid.hpp"
#include "bg3vr/health.hpp"
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

using namespace rigid;

ModuleInfo g_module;
std::atomic<bool> g_enabled{false};
std::array<std::string, 6> g_names{}; // [ik] overrides: right upper/lower/hand, left upper/lower/hand

constexpr std::uintptr_t kGetWorldPose4x4 = 0x4A11C40; // Granny thunk: float* (world_pose*, int bone); slot 10 calls it
constexpr std::size_t kSkeletonWorldPose = 0xE0;
constexpr std::size_t kVisualTransform = 0x08; // quat, translate +0x10, scale +0x1C (SE MoveableObject layout)

struct Side {
    short upper = -1, lower = -1, hand = -1;
    short dummy = -1; // the weapon bone below the hand
    short index = -1, middle = -1, pinky = -1; // knuckles (IndexFinger1 / MiddleFinger1 / PinkyFinger1): the hand's own frame
    std::vector<std::pair<short, int>> followers; // bone, chain slot (0 upper, 1 lower, 2 hand)
    bool ok() const { return upper >= 0 && lower >= 0 && hand >= 0; }
};
struct Rig {
    std::uintptr_t skeleton = 0;
    std::uintptr_t visual = 0;
    std::array<Side, 2> sides; // 0 left, 1 right
};
std::atomic<std::shared_ptr<const Rig>> g_rig; // replaced on the game thread, read in the hook

// [ik] hands: the weapon bone follows the controller absolutely (weapon) or the hand turns relative to a
// calibration (relative); the controller -> weapon bone rotation offset per side (0 left, 1 right).
struct Hands {
    bool weapon = true;
    std::array<vr::Quat, 2> offset{vr::Quat{0.0f, 0.0f, 0.0f, 1.0f}, vr::Quat{0.0f, 0.0f, 0.0f, 1.0f}};
};
std::atomic<std::shared_ptr<const Hands>> g_hands{std::make_shared<const Hands>()};

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
std::atomic<int> g_logged_hands{0};

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
    side.dummy = static_cast<short>(dummy_bone);
    const std::string suffix = std::string(dummy).find("_R_") != std::string::npos ? "_R" : "_L";
    side.index = static_cast<short>(find(bones, "IndexFinger1" + suffix));
    side.middle = static_cast<short>(find(bones, "MiddleFinger1" + suffix));
    side.pinky = static_cast<short>(find(bones, "PinkyFinger1" + suffix));
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
    log::info("ik: {} hand frame from knuckles: index {} middle {} pinky {}{}", label, side.index, side.middle, side.pinky,
              side.index >= 0 && side.middle >= 0 && side.pinky >= 0 ? "" : " (MISSING: weapon hands fall back to the animated hand)");
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
        health::set("arm IK", health::State::Unavailable, "Granny getter changed");
        log::error("ik: the Granny world-pose getter isn't where it was checked (exe+0x{:X}); arm IK unavailable",
                   kGetWorldPose4x4);
        g_module = {};
        return;
    }
    health::set("arm IK", config.get_bool("ik", "arms", true) ? health::State::Ready : health::State::Off, "[ik] arms");
    configure_hands(config);
    set_enabled(config.get_bool("ik", "arms", true));
}

void configure_hands(const Config& config) {
    auto hands = std::make_shared<Hands>();
    hands->weapon = text::to_lower(config.get("ik", "hands", "weapon")) != "relative";
    const auto degrees = [&](const char* key, const char* fallback_key) {
        std::string v = config.get("ik", key);
        if (v.empty() && fallback_key) {
            v = config.get("ik", fallback_key);
        }
        return v.empty() ? 0.0f : std::clamp(std::strtof(v.c_str(), nullptr), -180.0f, 180.0f);
    };
    const auto axis = [](float deg, int a) {
        const float h = deg * 0.5f * 3.14159265f / 180.0f;
        vr::Quat q{0.0f, 0.0f, 0.0f, std::cos(h)};
        q[static_cast<std::size_t>(a)] = std::sin(h);
        return q;
    };
    float logged[2][3]{};
    for (int s = 0; s < 2; ++s) {
        const bool left = s == 0;
        const float pitch = degrees(left ? "left_weapon_pitch" : "weapon_pitch", left ? "weapon_pitch" : nullptr);
        const float yaw = degrees(left ? "left_weapon_yaw" : "weapon_yaw", left ? "weapon_yaw" : nullptr);
        const float roll = degrees(left ? "left_weapon_roll" : "weapon_roll", left ? "weapon_roll" : nullptr);
        // In the grip's own frame: yaw about +Y, then pitch about +X, then roll about the blade (Z).
        hands->offset[static_cast<std::size_t>(s)] = vr::multiply(vr::multiply(axis(yaw, 1), axis(pitch, 0)), axis(roll, 2));
        logged[s][0] = pitch;
        logged[s][1] = yaw;
        logged[s][2] = roll;
    }
    g_hands.store(std::shared_ptr<const Hands>(hands));
    log::info("ik: hands {} (weapon angle right pitch {:.0f} yaw {:.0f} roll {:.0f}, left {:.0f} {:.0f} {:.0f} deg)",
              hands->weapon ? "follow the controllers (the weapon bone at the grip)" : "turn relative to a calibration",
              logged[1][0], logged[1][1], logged[1][2], logged[0][0], logged[0][1], logged[0][2]);
    g_recalibrate.store(true, std::memory_order_relaxed);
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
    const std::shared_ptr<const Hands> hands = g_hands.load();

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

        // Weapon hands: the hand turned so that its own frame, taken from the knuckles of this frame's animated pose
        // (wrist -> middle knuckle = where the fingers point, pinky knuckle -> index knuckle = up the fist), matches the
        // controller's grip frame (OpenXR: -Z along the pointing index finger, +Y up the fist; game convention after
        // the Z mirror: fingers +Z, up +Y), times the [ik] weapon angle. A held weapon then sits in the fist the way the
        // game's animations hold it. The wrist goal is moved so the weapon bone (the grip point) lands on the
        // controller grip.
        bool weapon = false;
        float weapon_hand[3][3]{};
        vr::Vec3 wrist_goal = goal;
        if (hands && hands->weapon && side.dummy >= 0 && side.index >= 0 && side.middle >= 0 && side.pinky >= 0) {
            float* md = get(pose, side.dummy);
            float* mi = get(pose, side.index);
            float* mm = get(pose, side.middle);
            float* mp = get(pose, side.pinky);
            if (md && mi && mm && mp) {
                const Rigid dummy = from_matrix(md), index = from_matrix(mi), middle = from_matrix(mm), pinky = from_matrix(mp);
                const auto sub = [](const float* a, const float* b) { return vr::Vec3{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
                const auto dot3 = [](const vr::Vec3& a, const vr::Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
                const auto cross3 = [](const vr::Vec3& a, const vr::Vec3& b) {
                    return vr::Vec3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
                };
                const auto unit = [&](vr::Vec3 v) {
                    const float n = std::sqrt(dot3(v, v));
                    return n > 1e-6f ? vr::Vec3{v[0] / n, v[1] / n, v[2] / n} : vr::Vec3{0.0f, 0.0f, 0.0f};
                };
                const vr::Vec3 fingers = unit(sub(middle.t, hand.t));
                vr::Vec3 up = sub(index.t, pinky.t);
                const float along = dot3(up, fingers);
                up = unit({up[0] - along * fingers[0], up[1] - along * fingers[1], up[2] - along * fingers[2]});
                if (dot3(fingers, fingers) > 0.5f && dot3(up, up) > 0.5f) {
                    float hand_t[3][3];
                    transpose(hand.R, hand_t);
                    const auto local = [&](const vr::Vec3& v) {
                        return vr::Vec3{hand_t[0][0] * v[0] + hand_t[0][1] * v[1] + hand_t[0][2] * v[2],
                                        hand_t[1][0] * v[0] + hand_t[1][1] * v[1] + hand_t[1][2] * v[2],
                                        hand_t[2][0] * v[0] + hand_t[2][1] * v[1] + hand_t[2][2] * v[2]};
                    };
                    const vr::Vec3 f_l = local(fingers), u_l = local(up), b_l = cross3(f_l, u_l);
                    float c[3][3];
                    quat_to_matrix(vr::multiply(vr::multiply(inverse, target->orientation), hands->offset[static_cast<std::size_t>(s)]), c);
                    const vr::Vec3 cf{c[0][2], c[1][2], c[2][2]}, cu{c[0][1], c[1][1], c[2][1]};
                    const vr::Vec3 cb = cross3(cf, cu);
                    for (int i = 0; i < 3; ++i) {
                        for (int j = 0; j < 3; ++j) {
                            weapon_hand[i][j] = cf[static_cast<std::size_t>(i)] * f_l[static_cast<std::size_t>(j)] +
                                                cu[static_cast<std::size_t>(i)] * u_l[static_cast<std::size_t>(j)] +
                                                cb[static_cast<std::size_t>(i)] * b_l[static_cast<std::size_t>(j)];
                        }
                    }
                    const vr::Vec3 d = local(sub(dummy.t, hand.t));
                    vr::Vec3 moved{};
                    for (int i = 0; i < 3; ++i) {
                        for (int j = 0; j < 3; ++j) {
                            moved[static_cast<std::size_t>(i)] += weapon_hand[i][j] * d[static_cast<std::size_t>(j)];
                        }
                    }
                    wrist_goal = {goal[0] - moved[0], goal[1] - moved[1], goal[2] - moved[2]};
                    weapon = true;
                    if (g_logged_hands.load(std::memory_order_relaxed) < 6) {
                        g_logged_hands.fetch_add(1, std::memory_order_relaxed);
                        log::info("ik: {} hand frame (model): fingers ({:.2f}, {:.2f}, {:.2f}) up ({:.2f}, {:.2f}, {:.2f}); "
                                  "controller forward ({:.2f}, {:.2f}, {:.2f}) up ({:.2f}, {:.2f}, {:.2f}); grip point "
                                  "{:.3f} m from the wrist",
                                  s ? "right" : "left", fingers[0], fingers[1], fingers[2], up[0], up[1], up[2], cf[0],
                                  cf[1], cf[2], cu[0], cu[1], cu[2], std::sqrt(dot3(d, d)));
                    }
                }
            }
        }

        const vr::Vec3 S{upper.t[0], upper.t[1], upper.t[2]};
        const vr::Vec3 E{lower.t[0], lower.t[1], lower.t[2]};
        const vr::Vec3 W{hand.t[0], hand.t[1], hand.t[2]};
        const vr::Vec3 pole{E[0], E[1] - 0.25f, E[2]}; // elbows bend down
        const ik::TwoBone solved = ik::solve_two_bone(S, E, W, wrist_goal, pole);

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
        if (weapon) {
            std::memcpy(new_hand.R, weapon_hand, sizeof(weapon_hand));
        } else {
            // Hand rotation: the controller's turn since calibration, on top of the hand's rotation then.
            float cal_inverse[3][3], turn[3][3];
            transpose(cal.controller, cal_inverse);
            mul(controller, cal_inverse, turn);
            mul(turn, cal.hand, new_hand.R);
        }

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
