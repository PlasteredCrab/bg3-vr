#include "bg3vr/bodyik.hpp"

#include "bg3vr/ik.hpp"
#include "bg3vr/rigid.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/text.hpp"

#include <array>
#include <vector>
#include <format>
#include <optional>
#include <algorithm>
#include <chrono>
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
    short scapula = -1; // the upper arm's parent: turned forward / up when the hand reaches far (Parger 3.2)
    std::vector<short> scapula_followers; // its other descendants (not under the upper arm): turned with it
    short dummy = -1; // the weapon bone below the hand
    short index = -1, middle = -1, pinky = -1; // knuckles (IndexFinger1 / MiddleFinger1 / PinkyFinger1): the hand's own frame
    // Finger chains for the curl, root (segment 1) to tip (end bone): index, middle, ring, pinky, thumb.
    std::array<std::vector<short>, 5> fingers;
    std::vector<std::pair<short, int>> followers; // bone, chain slot (0 upper, 1 lower, 2 hand)
    bool ok() const { return upper >= 0 && lower >= 0 && hand >= 0; }
};
struct Rig {
    std::uintptr_t skeleton = 0;
    std::uintptr_t visual = 0;
    std::array<Side, 2> sides; // 0 left, 1 right
    int bone_count = 0;
    short head = -1; // Head_M
    short root = -1; // Root_M: the body turns about its vertical axis
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
std::atomic<int> g_logged_elbow{0};
std::array<std::atomic<float>, 2> g_curl_index{}, g_curl_others{};
std::atomic<bool> g_fingers{true};     // [ik] fingers
std::atomic<bool> g_shoulder{true};    // [ik] shoulder: the shoulder girdle turns toward far reaches
std::atomic<bool> g_weapon_drawn{false};
std::atomic<float> g_stretch{1.15f};    // [ik] stretch: how much longer an arm may get to reach the hand
std::atomic<bool> g_elbow_model{true};  // [ik] elbow = natural (Parger et al. 2018) | down
std::atomic<bool> g_elbow_wrist{true};  // [ik] elbow_wrist: the wrist's roll moves the elbow
struct ElbowState {
    bool valid = false;
    float phi = 0.0f;
    std::chrono::steady_clock::time_point when{};
};
std::mutex g_elbow_mutex;
std::array<ElbowState, 2> g_elbow_state{};
struct BodyYaw {
    bool valid = false;
    bool turning = false;
    float yaw = 0.0f; // degrees about +Y, model space
    std::chrono::steady_clock::time_point when{};
};
BodyYaw g_body; // guarded by g_elbow_mutex
std::atomic<bool> g_body_yaw{true}; // [ik] body_yaw

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
    {
        const char* names[5] = {"IndexFinger", "MiddleFinger", "RingFinger", "PinkyFinger", "ThumbFinger"};
        for (int f = 0; f < 5; ++f) {
            for (int segment = 1; segment <= 3; ++segment) {
                const int b = find(bones, std::format("{}{}{}", names[f], segment, suffix));
                if (b >= 0) {
                    side.fingers[static_cast<std::size_t>(f)].push_back(static_cast<short>(b));
                }
            }
            const int end = find(bones, std::format("{}4{}_endBone", names[f], suffix));
            if (end >= 0) {
                side.fingers[static_cast<std::size_t>(f)].push_back(static_cast<short>(end));
            }
        }
    }
    side.hand = static_cast<short>(!overrides[2].empty() ? find(bones, overrides[2]) : parent(dummy_bone));
    side.lower = static_cast<short>(!overrides[1].empty() ? find(bones, overrides[1]) : parent(side.hand));
    side.upper = static_cast<short>(!overrides[0].empty() ? find(bones, overrides[0]) : parent(side.lower));
    side.scapula = parent(side.upper);
    if (side.scapula >= 0) {
        for (int b = 0; b < static_cast<int>(bones.size()); ++b) {
            if (b == side.scapula || b == side.upper) {
                continue;
            }
            // Nearest ancestor that is the scapula or the upper arm: only the scapula's own branch follows it here.
            for (int a = parent(b), guard = 0; a >= 0 && guard < 256; a = parent(a), ++guard) {
                if (a == side.upper) {
                    break;
                }
                if (a == side.scapula) {
                    side.scapula_followers.push_back(static_cast<short>(b));
                    break;
                }
            }
        }
    }
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

// Curls the fingers toward the palm by the controller's trigger (index) and grip (the others), on top of the animated
// pose: each finger segment turns about the knuckle axis (pinky -> index knuckle) at its own joint, carrying the rest of
// the finger. The palm side comes from the weapon bone (the grip point sits in the palm).
template <typename Get>
void curl_fingers(const Side& side, int s, Get get, std::uintptr_t pose, const Rigid& hand) {
    if (!g_fingers.load(std::memory_order_relaxed) || g_weapon_drawn.load(std::memory_order_relaxed) || side.index < 0 ||
        side.pinky < 0 || side.middle < 0 || side.dummy < 0) {
        return;
    }
    const float curl_index = g_curl_index[static_cast<std::size_t>(s)].load(std::memory_order_relaxed);
    const float curl_others = g_curl_others[static_cast<std::size_t>(s)].load(std::memory_order_relaxed);
    if (curl_index < 0.02f && curl_others < 0.02f) {
        return;
    }
    float* mi = get(pose, side.index);
    float* mp = get(pose, side.pinky);
    float* mm = get(pose, side.middle);
    float* md = get(pose, side.dummy);
    if (!mi || !mp || !mm || !md) {
        return;
    }
    const Rigid index = from_matrix(mi), pinky = from_matrix(mp), middle = from_matrix(mm), dummy = from_matrix(md);
    vr::Vec3 axis{index.t[0] - pinky.t[0], index.t[1] - pinky.t[1], index.t[2] - pinky.t[2]};
    const vr::Vec3 fingers{middle.t[0] - hand.t[0], middle.t[1] - hand.t[1], middle.t[2] - hand.t[2]};
    const vr::Vec3 palm{dummy.t[0] - hand.t[0], dummy.t[1] - hand.t[1], dummy.t[2] - hand.t[2]};
    const float n = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (n < 1e-4f) {
        return;
    }
    for (float& c : axis) {
        c /= n;
    }
    // Turning +angle about the axis moves the fingers toward axis x fingers; flip if that's away from the palm.
    const vr::Vec3 toward{axis[1] * fingers[2] - axis[2] * fingers[1], axis[2] * fingers[0] - axis[0] * fingers[2],
                          axis[0] * fingers[1] - axis[1] * fingers[0]};
    const float sign = toward[0] * palm[0] + toward[1] * palm[1] + toward[2] * palm[2] >= 0.0f ? 1.0f : -1.0f;
    constexpr float kMax[3] = {70.0f, 90.0f, 60.0f}; // degrees per segment at a full curl
    for (std::size_t f = 0; f < 5; ++f) {
        const std::vector<short>& chain = side.fingers[f];
        const float curl = f == 0 ? curl_index : f == 4 ? curl_others * 0.4f : curl_others;
        if (curl < 0.02f || chain.size() < 2) {
            continue;
        }
        for (std::size_t j = 0; j < 3 && j + 1 < chain.size(); ++j) {
            const float angle = sign * curl * kMax[j] * (f == 4 ? 0.5f : 1.0f) * 3.14159265f / 180.0f;
            const vr::Quat q{axis[0] * std::sin(angle * 0.5f), axis[1] * std::sin(angle * 0.5f), axis[2] * std::sin(angle * 0.5f),
                             std::cos(angle * 0.5f)};
            float r[3][3];
            quat_to_matrix(q, r);
            float* joint = get(pose, chain[j]);
            if (!joint) {
                break;
            }
            const Rigid at = from_matrix(joint);
            for (std::size_t k = j; k < chain.size(); ++k) {
                float* m = get(pose, chain[k]);
                if (!m) {
                    continue;
                }
                Rigid b = from_matrix(m);
                const float d[3] = {b.t[0] - at.t[0], b.t[1] - at.t[1], b.t[2] - at.t[2]};
                Rigid out = b;
                for (int i = 0; i < 3; ++i) {
                    out.t[i] = at.t[i] + r[i][0] * d[0] + r[i][1] * d[1] + r[i][2] * d[2];
                }
                mul(r, b.R, out.R);
                to_matrix(out, m);
            }
        }
    }
}

} // namespace

void set_fingers(std::size_t hand, float index, float others) {
    if (hand < 2) {
        g_curl_index[hand].store(std::clamp(index, 0.0f, 1.0f), std::memory_order_relaxed);
        g_curl_others[hand].store(std::clamp(others, 0.0f, 1.0f), std::memory_order_relaxed);
    }
}

void set_weapon_drawn(bool drawn) {
    g_weapon_drawn.store(drawn, std::memory_order_relaxed);
}

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
    {
        const std::string v = config.get("ik", "stretch");
        g_stretch = v.empty() ? 1.15f : std::clamp(std::strtof(v.c_str(), nullptr), 1.0f, 1.5f);
        g_elbow_model = text::to_lower(config.get("ik", "elbow", "natural")) != "down";
        g_elbow_wrist = config.get_bool("ik", "elbow_wrist", true);
        g_fingers = config.get_bool("ik", "fingers", true);
        g_shoulder = config.get_bool("ik", "shoulder", true);
        g_body_yaw = config.get_bool("ik", "body_yaw", true);
    }
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
        rig->bone_count = static_cast<int>(bones.size());
        rig->head = static_cast<short>(find(bones, "Head_M"));
        rig->root = static_cast<short>(find(bones, "Root_M"));
    }
    {
        std::scoped_lock lock(g_elbow_mutex);
        g_body = {}; // a new skeleton starts facing its own way
        g_elbow_state = {};
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

    // The body faces where the hands are (Parger et al. 2018, 3.1: the sum of the directions from the head to the
    // controllers), so turning with the stick, which turns only the view, doesn't leave the torso facing away while the
    // arms reach around it. Visual only: every bone turns about Root_M's vertical axis; the game's own facing and
    // movement are untouched. It follows once the hands are more than 25 deg off and stops within 5 deg, eased.
    if (g_body_yaw.load(std::memory_order_relaxed) && rig->head >= 0 && rig->root >= 0 && (targets[0] || targets[1])) {
        float* mhead = get(pose, rig->head);
        float* mroot = get(pose, rig->root);
        if (mhead && mroot) {
            const Rigid head_bone = from_matrix(mhead), root_bone = from_matrix(mroot);
            float dx = 0.0f, dz = 0.0f;
            for (const auto& t : targets) {
                if (!t) {
                    continue;
                }
                const vr::Vec3 rel{t->position[0] - visual[4], t->position[1] - visual[5], t->position[2] - visual[6]};
                const vr::Vec3 m = vr::rotate(inverse, rel);
                const float hx = m[0] / scale - head_bone.t[0], hz = m[2] / scale - head_bone.t[2];
                const float n = std::sqrt(hx * hx + hz * hz);
                if (n > 0.05f) {
                    dx += hx / n;
                    dz += hz / n;
                }
            }
            std::scoped_lock lock(g_elbow_mutex);
            const auto now = std::chrono::steady_clock::now();
            if (std::sqrt(dx * dx + dz * dz) > 0.5f) {
                // Yaw (about +Y) that turns the model's forward (-Z) to (dx, dz).
                const float want = std::atan2(-dx, -dz) * 57.2957795f;
                float diff = want - g_body.yaw;
                diff -= 360.0f * std::floor((diff + 180.0f) / 360.0f);
                if (std::abs(diff) > 25.0f) {
                    g_body.turning = true;
                }
                if (g_body.turning) {
                    const float dt = g_body.valid ? std::chrono::duration<float>(now - g_body.when).count() : 0.0f;
                    const float step = std::clamp(dt / 0.3f, 0.0f, 1.0f) * diff;
                    g_body.yaw += step;
                    if (std::abs(diff - step) < 5.0f) {
                        g_body.turning = false;
                    }
                }
            }
            g_body.when = now;
            g_body.valid = true;
            if (std::abs(g_body.yaw) > 0.1f) {
                const float h = g_body.yaw * 0.5f * 3.14159265f / 180.0f;
                float rot[3][3];
                quat_to_matrix(vr::Quat{0.0f, std::sin(h), 0.0f, std::cos(h)}, rot);
                const float px = root_bone.t[0], pz = root_bone.t[2];
                for (int b = 0; b < rig->bone_count; ++b) {
                    float* mb = get(pose, b);
                    if (!mb) {
                        continue;
                    }
                    Rigid r = from_matrix(mb);
                    const float x = r.t[0] - px, z = r.t[2] - pz;
                    Rigid out = r;
                    out.t[0] = px + rot[0][0] * x + rot[0][2] * z;
                    out.t[2] = pz + rot[2][0] * x + rot[2][2] * z;
                    mul(rot, r.R, out.R);
                    to_matrix(out, mb);
                }
            }
        }
    }

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
        std::optional<vr::Vec3> thumb_world; // model space: where the thumb side of the fist points
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
                    // The grip frame (logged 2026-10-20: "forward" ran up the handle): the handle axis (OpenXR -Z,
                    // here cf) is where the fist's up (pinky -> index knuckle) and a held blade point; the fingers
                    // wrap along OpenXR -Y (here -cu).
                    const vr::Vec3 target_fingers{-cu[0], -cu[1], -cu[2]};
                    const vr::Vec3 target_up = cf;
                    const vr::Vec3 target_third = cross3(target_fingers, target_up);
                    thumb_world = target_up;
                    for (int i = 0; i < 3; ++i) {
                        for (int j = 0; j < 3; ++j) {
                            const auto ii = static_cast<std::size_t>(i), jj = static_cast<std::size_t>(j);
                            weapon_hand[i][j] = target_fingers[ii] * f_l[jj] + target_up[ii] * u_l[jj] +
                                                target_third[ii] * b_l[jj];
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

        // The shoulder (Parger et al. 2018, 3.2): when the hand reaches far forward or up, the shoulder girdle turns
        // toward it (scapula about its own joint): yaw / roll = 30 deg x (reach along forward / up per arm length - 0.5),
        // clamped to 0..33 deg. The arm is turned with it before the IK.
        Rigid upper_s = upper, lower_s = lower, hand_s = hand;
        if (g_shoulder.load(std::memory_order_relaxed) && side.scapula >= 0) {
            if (float* ms = get(pose, side.scapula)) {
                const Rigid scap = from_matrix(ms);
                const auto len3 = [](float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); };
                const float arm = len3(lower.t[0] - upper.t[0], lower.t[1] - upper.t[1], lower.t[2] - upper.t[2]) +
                                  len3(hand.t[0] - lower.t[0], hand.t[1] - lower.t[1], hand.t[2] - lower.t[2]);
                const vr::Vec3 reach{wrist_goal[0] - upper.t[0], wrist_goal[1] - upper.t[1], wrist_goal[2] - upper.t[2]};
                if (arm > 1e-3f) {
                    const float yaw = std::clamp(30.0f * (-reach[2] / arm - 0.5f), 0.0f, 33.0f);  // forward is -Z
                    const float roll = std::clamp(30.0f * (reach[1] / arm - 0.5f), 0.0f, 33.0f); // up is +Y
                    if (yaw > 0.0f || roll > 0.0f) {
                        const vr::Vec3 arm_out{upper.t[0] - scap.t[0], upper.t[1] - scap.t[1], upper.t[2] - scap.t[2]};
                        // The turn about `axis` that moves the shoulder joint toward `toward` (sign by a test turn).
                        const auto turn = [&](const vr::Vec3& axis, const vr::Vec3& toward, float degrees) {
                            const auto q_of = [&](float d) {
                                const float h = d * 0.5f * 3.14159265f / 180.0f;
                                return vr::Quat{axis[0] * std::sin(h), axis[1] * std::sin(h), axis[2] * std::sin(h), std::cos(h)};
                            };
                            const vr::Vec3 moved = vr::rotate(q_of(1.0f), arm_out);
                            const float d = (moved[0] - arm_out[0]) * toward[0] + (moved[1] - arm_out[1]) * toward[1] +
                                            (moved[2] - arm_out[2]) * toward[2];
                            return q_of(d >= 0.0f ? degrees : -degrees);
                        };
                        const vr::Quat q = vr::multiply(turn({0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, roll),
                                                        turn({0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, yaw));
                        float rot[3][3];
                        quat_to_matrix(q, rot);
                        const auto apply_turn = [&](const Rigid& in) {
                            Rigid out = in;
                            const float d[3] = {in.t[0] - scap.t[0], in.t[1] - scap.t[1], in.t[2] - scap.t[2]};
                            for (int i = 0; i < 3; ++i) {
                                out.t[i] = scap.t[i] + rot[i][0] * d[0] + rot[i][1] * d[1] + rot[i][2] * d[2];
                            }
                            mul(rot, in.R, out.R);
                            return out;
                        };
                        upper_s = apply_turn(upper);
                        lower_s = apply_turn(lower);
                        hand_s = apply_turn(hand);
                        to_matrix(apply_turn(scap), ms);
                        for (const short b : side.scapula_followers) {
                            if (float* mf = get(pose, b)) {
                                to_matrix(apply_turn(from_matrix(mf)), mf);
                            }
                        }
                    }
                }
            }
        }
        const vr::Vec3 S{upper_s.t[0], upper_s.t[1], upper_s.t[2]};
        const vr::Vec3 E{lower_s.t[0], lower_s.t[1], lower_s.t[2]};
        const vr::Vec3 W{hand_s.t[0], hand_s.t[1], hand_s.t[2]};
        // Arms a little longer when the hand is out of reach ([ik] stretch): the elbow and wrist move out from the
        // shoulder; the skin stretches between the bones.
        vr::Vec3 E_s = E, W_s = W;
        {
            const auto len = [](const vr::Vec3& a, const vr::Vec3& b) {
                const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
                return std::sqrt(x * x + y * y + z * z);
            };
            const float reach = len(E, S) + len(W, E);
            const float want = len(wrist_goal, S);
            const float stretch = g_stretch.load(std::memory_order_relaxed);
            if (reach > 1e-3f && want > reach && stretch > 1.0f) {
                const float f = std::min(want / reach, stretch);
                for (int i = 0; i < 3; ++i) {
                    const auto k = static_cast<std::size_t>(i);
                    E_s[k] = S[k] + (E[k] - S[k]) * f;
                    W_s[k] = S[k] + (W[k] - S[k]) * f;
                }
            }
        }
        // Where the elbow points (Parger et al. 2018, ik.hpp): model space has +Y up, -Z forward and +X on the
        // character's left.
        vr::Vec3 pole{E[0], E[1] - 0.25f, E[2]};
        if (g_elbow_model.load(std::memory_order_relaxed)) {
            const ik::ArmFrame frame{{s ? -1.0f : 1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
            const ik::Elbow elbow = ik::elbow_direction(S, wrist_goal, frame);
            float phi = elbow.phi_deg;
            float roll = 0.0f;
            if (thumb_world && g_elbow_wrist.load(std::memory_order_relaxed)) {
                // Hand roll: the thumb side's turn around the forearm away from "elbow down, thumb up", positive
                // outward, measured on the arm this elbow direction gives.
                const ik::TwoBone first = ik::solve_two_bone(S, E_s, W_s, wrist_goal,
                                                             {(S[0] + wrist_goal[0]) * 0.5f + elbow.direction[0] * 0.4f,
                                                              (S[1] + wrist_goal[1]) * 0.5f + elbow.direction[1] * 0.4f,
                                                              (S[2] + wrist_goal[2]) * 0.5f + elbow.direction[2] * 0.4f});
                const auto sub3 = [](const vr::Vec3& a, const vr::Vec3& b) { return vr::Vec3{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
                const auto dot3v = [](const vr::Vec3& a, const vr::Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
                const auto cross3v = [](const vr::Vec3& a, const vr::Vec3& b) {
                    return vr::Vec3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
                };
                const auto perp = [&](const vr::Vec3& v, const vr::Vec3& axis) {
                    const float d = dot3v(v, axis);
                    return vr::Vec3{v[0] - axis[0] * d, v[1] - axis[1] * d, v[2] - axis[2] * d};
                };
                vr::Vec3 axis = sub3(first.wrist, first.elbow);
                const float al = std::sqrt(dot3v(axis, axis));
                if (al > 1e-4f) {
                    axis = {axis[0] / al, axis[1] / al, axis[2] / al};
                    const vr::Vec3 elbow_out = perp(sub3(first.elbow, S), axis); // toward the elbow, across the forearm
                    const vr::Vec3 reference{-elbow_out[0], -elbow_out[1], -elbow_out[2]};
                    const vr::Vec3 thumb = perp(*thumb_world, axis);
                    const vr::Vec3 outward = perp(frame.outward, axis);
                    float angle = std::atan2(dot3v(cross3v(reference, thumb), axis), dot3v(reference, thumb)) * 57.2957795f;
                    if (dot3v(cross3v(reference, outward), axis) < 0.0f) {
                        angle = -angle; // positive toward outward
                    }
                    roll = angle;
                    phi += ik::wrist_roll_correction(roll);
                }
            }
            // Smoothed over about 0.1 s so the elbow glides.
            {
                std::scoped_lock lock(g_elbow_mutex);
                const auto now = std::chrono::steady_clock::now();
                ElbowState& st = g_elbow_state[static_cast<std::size_t>(s)];
                const float dt = st.valid ? std::chrono::duration<float>(now - st.when).count() : 1.0f;
                const float k = std::clamp(dt / 0.1f, 0.0f, 1.0f);
                st.phi = st.valid ? st.phi + (phi - st.phi) * k : phi;
                st.when = now;
                st.valid = true;
                phi = st.phi;
            }
            const vr::Vec3 dir = ik::swivel_direction(S, wrist_goal, frame, std::clamp(phi, 0.0f, 180.0f));
            pole = {(S[0] + wrist_goal[0]) * 0.5f + dir[0] * 0.4f, (S[1] + wrist_goal[1]) * 0.5f + dir[1] * 0.4f,
                    (S[2] + wrist_goal[2]) * 0.5f + dir[2] * 0.4f};
            if (g_logged_elbow.load(std::memory_order_relaxed) < 12 && (g_applied.load(std::memory_order_relaxed) % 450) < 2) {
                g_logged_elbow.fetch_add(1, std::memory_order_relaxed);
                log::info("ik: {} elbow swivel {:.0f} deg (position model {:.0f}, wrist roll {:.0f} deg){}", s ? "right" : "left",
                          phi, elbow.phi_deg, roll, W_s[0] != W[0] ? " (arm stretched)" : "");
            }
        }
        const ik::TwoBone solved = ik::solve_two_bone(S, E_s, W_s, wrist_goal, pole);

        Rigid new_upper = upper_s, new_lower = lower_s, new_hand = hand_s;
        float r[3][3];
        quat_to_matrix(solved.upper_delta, r);
        mul(r, upper_s.R, new_upper.R);
        quat_to_matrix(solved.lower_delta, r);
        mul(r, lower_s.R, new_lower.R);
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
        curl_fingers(side, s, get, pose, new_hand);
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
