#include "bg3vr/dice.hpp"

#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/rayprobe.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <initializer_list>
#include <mutex>
#include <random>
#include <string>

namespace bg3vr::dice {

namespace {

// ------------------------------------------------------------------------------------------------ the roll screen
// 4.1.1.7631656, agent AQ.
constexpr std::uintptr_t kUpdateRva = 0x1548FC0;   // gui::DCActiveRoll update (dc, ctx)
constexpr std::uintptr_t kNextPhaseRva = 0x15475D0; // UserTriggeredNextPhaseCommand handler (dc, 0, BaseComponent*)
constexpr std::size_t kPhase = 0x7A8;               // u32
constexpr std::size_t kRolled1 = 0x838, kRolled2 = 0x850, kFinal = 0x880, kTarget = 0x898, kSuccess = 0x8E0; // u8
constexpr std::size_t kCanRespond = 0x928;           // u8
constexpr std::size_t kTotal = 0xD58;                // u8
constexpr std::size_t kRollEntity = 0xCC8;           // u64

using NextPhaseFn = void (*)(std::uintptr_t dc, std::uintptr_t zero, std::uintptr_t component);

ModuleInfo g_module;
safetyhook::InlineHook g_update_hook;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_roll_request{false};
std::atomic<bool> g_skip_request{false};
std::atomic<std::uint64_t> g_rolls_sent{0}, g_skips_sent{0}, g_screens{0};

std::mutex g_mutex;
std::optional<Roll> g_roll;          // under g_mutex
std::uintptr_t g_dc = 0;             // the view-model seen last
std::chrono::steady_clock::time_point g_last_update{};

std::uint8_t byte_at(std::uintptr_t dc, std::size_t offset) {
    std::uint8_t v = 0;
    mem::try_copy(&v, dc + offset, 1);
    return v;
}

int call_next_phase(std::uintptr_t dc) {
    __try {
        reinterpret_cast<NextPhaseFn>(g_module.base + kNextPhaseRva)(dc, 0, 0);
        return 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 1;
    }
}

void on_update(std::uintptr_t dc, std::uintptr_t ctx) {
    g_update_hook.call<void>(dc, ctx);
    std::uint32_t phase = 0;
    if (!dc || !mem::try_copy(&phase, dc + kPhase, sizeof(phase)) || phase > 5) {
        return;
    }
    Roll r;
    r.phase = static_cast<int>(phase);
    r.target = byte_at(dc, kTarget);
    r.can_respond = byte_at(dc, kCanRespond) != 0;
    if (phase >= 3) {
        r.has_result = true;
        r.face = byte_at(dc, kFinal);
        r.dice[0] = byte_at(dc, kRolled1);
        r.dice[1] = byte_at(dc, kRolled2);
        r.total = byte_at(dc, kTotal);
        r.success = byte_at(dc, kSuccess) != 0;
    }
    std::uint64_t entity = 0;
    mem::try_copy(&entity, dc + kRollEntity, sizeof(entity));
    std::optional<Roll> before;
    {
        std::scoped_lock lock(g_mutex);
        before = g_roll;
        if (dc != g_dc) {
            g_dc = dc;
            r.serial = g_screens.fetch_add(1, std::memory_order_relaxed) + 1;
            before.reset();
            log::info("dice: roll screen 0x{:X} (roll entity 0x{:016X}) opened, phase {}, needs {} on the d20, thread {}", dc,
                      entity, phase, r.target, GetCurrentThreadId());
        } else if (before) {
            r.serial = before->serial;
        }
        g_roll = r;
        g_last_update = std::chrono::steady_clock::now();
    }
    if (before && before->phase != r.phase) {
        log::info("dice: roll screen phase {} -> {} (can respond {}){}", before->phase, r.phase, r.can_respond,
                  r.has_result ? std::format("; natural {} (dice {} / {}), total {}, {}", r.face, r.dice[0], r.dice[1], r.total,
                                             r.success ? "success" : "failure")
                               : std::string());
    }
    if (!g_enabled.load(std::memory_order_relaxed) || !r.can_respond) {
        return;
    }
    // The physical die's requests, as presses of the game's own "Roll" button.
    if (phase == 1 && g_roll_request.exchange(false, std::memory_order_relaxed)) {
        const int faulted = call_next_phase(dc);
        g_rolls_sent.fetch_add(1, std::memory_order_relaxed);
        log::info("dice: the physical die was thrown: the game's Roll pressed{}", faulted ? " (FAULTED)" : "");
    } else if (phase == 3 && g_skip_request.exchange(false, std::memory_order_relaxed)) {
        const int faulted = call_next_phase(dc);
        g_skips_sent.fetch_add(1, std::memory_order_relaxed);
        log::info("dice: the physical die came to rest: the game's own skip pressed{}", faulted ? " (FAULTED)" : "");
    }
}

bool bytes_at(std::uintptr_t address, std::initializer_list<std::uint8_t> expected) {
    std::array<std::uint8_t, 32> actual{};
    if (expected.size() > actual.size() || !mem::try_copy(actual.data(), address, expected.size())) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), actual.begin());
}

// ------------------------------------------------------------------------------------------------ the physical die
// Agent AR (AR_dice_visual.md), 4.1.1.7631656: a visual clone (vtable 0x55FC810, 0x3A0 bytes) as the game builds its
// paperdoll and stealth clones, without a source character, in the level scene; created, moved and removed after the
// paperdoll-manager update 0x13E1A40 (where the game builds its own clones).
constexpr std::uintptr_t kAllocatorRva = 0x40EA160;   // returns the allocator; +0xE8 is the pool 0x4188700 takes
constexpr std::uintptr_t kAllocRva = 0x4188700;       // (pool, size, align, &tmp)
constexpr std::uintptr_t kCloneCtorRva = 0x1888C10;   // (obj)
constexpr std::uintptr_t kCloneSceneRva = 0x18890D0;  // (obj, scene entity)
constexpr std::uintptr_t kCloneVisualRva = 0x188A8A0; // (obj, const FixedString*, force, async): async must be 0
constexpr std::uintptr_t kCloneDeleteRva = 0x1888FB0; // vtable slot 0 (obj, 1): frees the visual and the entity
constexpr std::uintptr_t kFixedStringRva = 0x4194CA0; // (table, u32* out, {const char*, u32}*)
constexpr std::uintptr_t kFixedStringReleaseRva = 0x41950A0; // (table, u32*)
constexpr std::uintptr_t kTransformRva = 0x3C785C0;   // (sys, ref, pos*, quat*, scale3*, notify, record previous)
constexpr std::uintptr_t kCloneVisualPtrRva = 0x1889D00; // (obj) -> ls::Visual*
constexpr std::uintptr_t kCullSetterRva = 0x3C6B810;  // (visual, u16 mask, bool set): shadow bits 0x803E
constexpr std::uintptr_t kLevelHolderRva = 0x6007A28, kWorldHolderRva = 0x5FDA240, kTransformTypeRva = 0x6058CBC;
constexpr std::uintptr_t kStringTableRva = 0x604B2E0;
constexpr const char* kDieVisual = "a61032d1-98f3-6d0b-55f0-75e62b623f26"; // UI_Dice_D20_A
constexpr float kInradius = 0.3137f; // at scale 1: face centres are this far from the centre
constexpr float kGravity = 9.81f;

// Face normals of UI_Dice_D20_A in its local space (Y up), from the mesh and its texture (AR 3.2); opposite faces sum
// to 21. To land on face N, rotate so N's normal points to world +Y.
constexpr float kFaces[20][3] = {
    {+0.9822f, -0.1876f, 0.0f},     {-0.7947f, -0.1876f, -0.5774f}, {+0.4911f, +0.7947f, +0.3568f}, {-0.4911f, -0.7947f, +0.3568f},
    {+0.1876f, -0.7947f, -0.5773f}, {-0.3035f, +0.1876f, +0.9342f}, {+0.7947f, +0.1876f, -0.5774f}, {-0.6071f, +0.7947f, 0.0f},
    {+0.3035f, -0.1876f, +0.9342f}, {-0.1876f, +0.7947f, -0.5773f}, {+0.1876f, -0.7947f, +0.5773f}, {-0.3035f, +0.1876f, -0.9342f},
    {+0.6071f, -0.7947f, 0.0f},     {-0.7947f, -0.1876f, +0.5774f}, {+0.3035f, -0.1876f, -0.9342f}, {-0.1876f, +0.7947f, +0.5773f},
    {+0.4911f, +0.7947f, -0.3568f}, {-0.4911f, -0.7947f, -0.3568f}, {+0.7947f, +0.1876f, +0.5774f}, {-0.9822f, +0.1876f, 0.0f}};

using AllocatorFn = std::uintptr_t (*)();
using AllocFn = std::uintptr_t (*)(std::uintptr_t pool, std::uint64_t size, std::uint64_t align, std::uint64_t* tmp);
using ObjFn = void (*)(std::uintptr_t obj);
using CloneSceneFn = void (*)(std::uintptr_t obj, std::uint64_t scene_entity);
using CloneVisualFn = void (*)(std::uintptr_t obj, const std::uint32_t* id, bool force, bool async);
struct StringRef {
    const char* text;
    std::uint32_t length;
};
using FixedStringFn = void (*)(std::uintptr_t table, std::uint32_t* out, const StringRef* in);
using FixedStringReleaseFn = void (*)(std::uintptr_t table, std::uint32_t* id);
using TransformFn = void (*)(std::uintptr_t sys, const void* ref, const float* pos, const float* quat, const float* scale,
                             bool notify, bool record);
using VisualPtrFn = std::uintptr_t (*)(std::uintptr_t obj);
using CullSetterFn = void (*)(std::uintptr_t visual, std::uint16_t mask, bool set);
using DeleteFn = void (*)(std::uintptr_t obj, std::uint32_t flags);

bool g_die_ready = false;  // every function matched this build
float g_die_size = 0.12f;  // [dice] size_cm: face to face
bool g_die_shadow = true;  // [dice] shadow

struct HandIn {
    bool grip = false;
    std::optional<vr::RigidPose> pose;
    double time = 0.0;
};
std::mutex g_hand_mutex;
HandIn g_hand;

enum class DieState : int { None, InHand, Flying, Settling, Resting };
struct Die {
    std::uintptr_t obj = 0;
    DieState state = DieState::None;
    vr::Vec3 position{};
    vr::Vec3 velocity{};
    vr::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    vr::Vec3 spin{}; // axis * rad/s
    vr::Quat settle_from{0.0f, 0.0f, 0.0f, 1.0f}, settle_to{0.0f, 0.0f, 0.0f, 1.0f};
    float settle_t = 0.0f;
    double rest_since = 0.0;
    double resting_at = 0.0;
    double flying_since = 0.0;
    bool thrown = false;    // the throw pressed the game's Roll
    bool grip_seen = false; // the grip closed while the die was in the hand (opening it then throws)
    bool skip_sent = false;
    std::uint64_t serial = 0;
    std::uint64_t frames = 0; // placements of this die
    int face = 0;
    std::array<std::pair<double, vr::Vec3>, 8> trail{}; // the hand's recent positions, for the throw velocity
    int trail_n = 0;
};
Die g_die;                         // game thread (paperdoll-manager post-hook)
std::atomic<int> g_die_state{0};   // DieState, for the presenting thread
std::atomic<bool> g_forget_die{false};
std::atomic<std::uint64_t> g_dice_made{0}, g_dice_thrown{0}, g_dice_landed{0}, g_dice_failed{0};
std::chrono::steady_clock::time_point g_die_last{};

std::uintptr_t client_world() {
    std::uintptr_t holder = 0, world = 0;
    if (mem::try_copy(&holder, g_module.base + kWorldHolderRva, sizeof(holder)) && holder) {
        mem::try_copy(&world, holder + 0x1A8, sizeof(world));
    }
    return world;
}

std::uint64_t level_scene() {
    std::uintptr_t holder = 0, level = 0, scene_ptr = 0;
    std::uint64_t scene = 0;
    if (mem::try_copy(&holder, g_module.base + kLevelHolderRva, sizeof(holder)) && holder &&
        mem::try_copy(&level, holder + 0x90, sizeof(level)) && level &&
        mem::try_copy(&scene_ptr, level + 0x28, sizeof(scene_ptr)) && scene_ptr) {
        mem::try_copy(&scene, scene_ptr, sizeof(scene));
    }
    return scene;
}

int create_die_raw(std::uintptr_t& out, std::uint64_t scene) {
    __try {
        const std::uintptr_t allocator = reinterpret_cast<AllocatorFn>(g_module.base + kAllocatorRva)();
        std::uint64_t tmp = 0;
        const std::uintptr_t obj = reinterpret_cast<AllocFn>(g_module.base + kAllocRva)(allocator + 0xE8, 0x3A0, 0x10, &tmp);
        if (!obj) {
            return 1;
        }
        reinterpret_cast<ObjFn>(g_module.base + kCloneCtorRva)(obj);
        reinterpret_cast<CloneSceneFn>(g_module.base + kCloneSceneRva)(obj, scene);
        const std::uintptr_t table = *reinterpret_cast<const std::uintptr_t*>(g_module.base + kStringTableRva) + 0xC600;
        std::uint32_t id = 0xFFFFFFFFu;
        const StringRef text{kDieVisual, static_cast<std::uint32_t>(std::strlen(kDieVisual))};
        reinterpret_cast<FixedStringFn>(g_module.base + kFixedStringRva)(table, &id, &text);
        reinterpret_cast<CloneVisualFn>(g_module.base + kCloneVisualRva)(obj, &id, false, false); // synchronous
        reinterpret_cast<FixedStringReleaseFn>(g_module.base + kFixedStringReleaseRva)(table, &id); // it took its own ref
        out = obj;
        return 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 2;
    }
}

int destroy_die_raw(std::uintptr_t obj) {
    __try {
        reinterpret_cast<DeleteFn>(g_module.base + kCloneDeleteRva)(obj, 1);
        return 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 1;
    }
}

int place_die_raw(std::uintptr_t obj, const float* pos, const float* quat, float scale) {
    __try {
        const std::uintptr_t world = client_world();
        if (!world) {
            return 1;
        }
        const std::uintptr_t components = *reinterpret_cast<const std::uintptr_t*>(world + 0x30);
        const std::uint32_t type = *reinterpret_cast<const std::uint32_t*>(g_module.base + kTransformTypeRva);
        const std::uintptr_t sys = *reinterpret_cast<const std::uintptr_t*>(components + static_cast<std::uintptr_t>(type) * 0xF8);
        if (!sys) {
            return 2;
        }
        // The entity reference the transform system takes (AR 2.4): handle, world, no Transform*, "look it up".
        alignas(16) std::uint8_t ref[0x38]{};
        const std::uint64_t handle = *reinterpret_cast<const std::uint64_t*>(obj + 0x08);
        std::memcpy(ref + 0x00, &handle, sizeof(handle));
        std::memcpy(ref + 0x08, &world, sizeof(world));
        ref[0x30] = 1;
        const float scale3[3] = {scale, scale, scale};
        reinterpret_cast<TransformFn>(g_module.base + kTransformRva)(sys, ref, pos, quat, scale3, true, true);
        return 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 3;
    }
}

std::uintptr_t die_visual_raw(std::uintptr_t obj) {
    __try {
        return reinterpret_cast<VisualPtrFn>(g_module.base + kCloneVisualPtrRva)(obj);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
}

// AR 4.2 / 4.3: the visual's flags and its world transform (SE layout: +0x08 quat, +0x18 pos, +0x24 scale), to see
// the clone load and follow the writes.
void log_visual(std::uintptr_t obj, const char* when, const float* pos, float scale) {
    const std::uintptr_t v = die_visual_raw(obj);
    std::uint32_t flags[2]{};
    float t[8]{};
    if (v) {
        mem::try_copy(flags, v + 0xF0, sizeof(flags));
        mem::try_copy(t, v + 0x08, sizeof(t));
    }
    float bounds[6]{};
    if (v) {
        mem::try_copy(bounds, v + 0x40, sizeof(bounds));
    }
    log::info("dice: {}: clone 0x{:X} visual 0x{:X} flags 0x{:X} cull 0x{:X}; wrote pos ({:.2f}, {:.2f}, {:.2f}) scale {:.3f}; "
              "visual quat ({:.3f}, {:.3f}, {:.3f}, {:.3f}) pos ({:.2f}, {:.2f}, {:.2f}) scale {:.3f}; bounds ({:.2f}, {:.2f}, "
              "{:.2f})..({:.2f}, {:.2f}, {:.2f})",
              when, obj, v, flags[0], flags[1], pos[0], pos[1], pos[2], scale, t[0], t[1], t[2], t[3], t[4], t[5], t[6],
              t[7], bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5]);
}

int die_shadow_raw(std::uintptr_t obj, bool on) {
    __try {
        const std::uintptr_t visual = reinterpret_cast<VisualPtrFn>(g_module.base + kCloneVisualPtrRva)(obj);
        if (visual && !on) {
            reinterpret_cast<CullSetterFn>(g_module.base + kCullSetterRva)(visual, 0x803E, false);
        }
        return visual ? 0 : 1;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 2;
    }
}

float die_scale() {
    return g_die_size / (2.0f * kInradius);
}

vr::Quat normalized(const vr::Quat& q) {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    return n > 0.0f ? vr::Quat{q[0] / n, q[1] / n, q[2] / n, q[3] / n} : vr::Quat{0.0f, 0.0f, 0.0f, 1.0f};
}

vr::Quat from_to(const vr::Vec3& a, const vr::Vec3& b) {
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (d < -0.9999f) {
        return {1.0f, 0.0f, 0.0f, 0.0f}; // opposite: half a turn about x
    }
    const vr::Vec3 c{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    return normalized({c[0], c[1], c[2], 1.0f + d});
}

vr::Quat slerp(const vr::Quat& a, vr::Quat b, float t) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) {
        b = {-b[0], -b[1], -b[2], -b[3]};
        d = -d;
    }
    if (d > 0.9995f) {
        return normalized({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t});
    }
    const float th = std::acos(std::clamp(d, -1.0f, 1.0f));
    const float s0 = std::sin((1.0f - t) * th) / std::sin(th), s1 = std::sin(t * th) / std::sin(th);
    return {a[0] * s0 + b[0] * s1, a[1] * s0 + b[1] * s1, a[2] * s0 + b[2] * s1, a[3] * s0 + b[3] * s1};
}

// The rest orientation showing `face` on top, keeping the die's current heading.
vr::Quat face_up(int face, const vr::Quat& current) {
    const float* n = kFaces[std::clamp(face, 1, 20) - 1];
    const vr::Quat up = from_to({n[0], n[1], n[2]}, {0.0f, 1.0f, 0.0f});
    const vr::Vec3 x = vr::rotate(current, {1.0f, 0.0f, 0.0f});
    const vr::Vec3 x0 = vr::rotate(up, {1.0f, 0.0f, 0.0f});
    const float yaw = std::atan2(x[2], x[0]) - std::atan2(x0[2], x0[0]);
    const vr::Quat turn{0.0f, std::sin(-yaw * 0.5f), 0.0f, std::cos(-yaw * 0.5f)};
    return normalized(vr::multiply(turn, up));
}

void destroy_die(const char* why) {
    if (g_die.obj) {
        const int faulted = destroy_die_raw(g_die.obj);
        log::info("dice: the physical die removed ({}){}", why, faulted ? " (FAULTED)" : "");
    }
    g_die = Die{};
    g_die_state.store(0, std::memory_order_relaxed);
}

vr::Vec3 hand_velocity(const Die& d) {
    vr::Vec3 v{};
    const int n = std::min(d.trail_n, 8);
    if (n < 2) {
        return v;
    }
    const auto& newest = d.trail[static_cast<std::size_t>((d.trail_n - 1) % 8)];
    const auto& oldest = d.trail[static_cast<std::size_t>((d.trail_n - n) % 8)];
    const double span = newest.first - oldest.first;
    if (span < 1e-3) {
        return v;
    }
    for (std::size_t k = 0; k < 3; ++k) {
        v[k] = static_cast<float>((newest.second[k] - oldest.second[k]) / span);
    }
    return v;
}

void die_frame() {
    if (g_forget_die.exchange(false, std::memory_order_relaxed)) {
        g_die = Die{}; // the level is going: don't touch the clone
        g_die_state.store(0, std::memory_order_relaxed);
    }
    const auto now_tp = std::chrono::steady_clock::now();
    const float dt = g_die_last.time_since_epoch().count() == 0
                         ? 0.0f
                         : std::min(0.05f, std::chrono::duration<float>(now_tp - g_die_last).count());
    g_die_last = now_tp;
    const double now = std::chrono::duration<double>(now_tp.time_since_epoch()).count();
    const std::optional<Roll> roll = current();
    HandIn hand;
    {
        std::scoped_lock lock(g_hand_mutex);
        hand = g_hand;
    }
    const bool wanted = g_enabled.load(std::memory_order_relaxed) && g_die_ready && roll && roll->phase >= 1;
    if (!wanted) {
        if (g_die.obj) {
            destroy_die(!roll ? "the roll screen closed" : "physical dice off");
        }
        return;
    }
    // A roll screen waiting for the roll: a die in the hand.
    if (!g_die.obj && roll->phase == 1 && hand.pose) {
        const std::uint64_t scene = level_scene();
        std::uintptr_t obj = 0;
        const int failed = scene ? create_die_raw(obj, scene) : 9;
        if (failed) {
            g_dice_failed.fetch_add(1, std::memory_order_relaxed);
            static int logged = 0;
            if (logged++ < 5) {
                log::warn("dice: couldn't make the physical die (step {}, scene 0x{:X}); roll with the UI", failed, scene);
            }
            return;
        }
        g_die = Die{};
        g_die.obj = obj;
        g_die.state = DieState::InHand;
        g_die.serial = roll->serial;
        g_die.rotation = normalized({0.3f, 0.5f, 0.1f, 0.8f});
        const int shadow = die_shadow_raw(obj, g_die_shadow);
        g_dice_made.fetch_add(1, std::memory_order_relaxed);
        log::info("dice: the physical die is in your hand (clone 0x{:X}, {:.0f} cm, scale {:.3f}, visual {}); squeeze and throw, "
                  "or roll with the UI",
                  obj, g_die_size * 100.0f, die_scale(), shadow == 0 ? "loaded" : "NOT loaded");
    }
    if (!g_die.obj) {
        return;
    }
    Die& d = g_die;
    switch (d.state) {
    case DieState::InHand: {
        if (hand.pose) {
            const vr::Vec3 ahead = vr::rotate(hand.pose->orientation, {0.0f, -0.02f, 0.07f});
            d.position = {hand.pose->position[0] + ahead[0], hand.pose->position[1] + ahead[1], hand.pose->position[2] + ahead[2]};
            d.trail[static_cast<std::size_t>(d.trail_n % 8)] = {hand.time, d.position};
            ++d.trail_n;
        }
        d.grip_seen = d.grip_seen || hand.grip;
        // Thrown: the grip opened after closing on it. Or the game rolled anyway (the UI's Roll): it falls from the hand.
        const bool released = d.grip_seen && !hand.grip;
        if (released || roll->phase >= 3) {
            d.velocity = hand_velocity(d);
            static std::mt19937 rng{std::random_device{}()};
            std::uniform_real_distribution<float> u(-1.0f, 1.0f);
            const float speed = std::sqrt(d.velocity[0] * d.velocity[0] + d.velocity[1] * d.velocity[1] + d.velocity[2] * d.velocity[2]);
            const float spin = 6.0f + speed * 6.0f;
            d.spin = {u(rng) * spin, u(rng) * spin, u(rng) * spin};
            d.state = DieState::Flying;
            d.flying_since = now;
            if (roll->phase == 1) {
                request_roll();
                d.thrown = true;
            }
            g_dice_thrown.fetch_add(1, std::memory_order_relaxed);
            log::info("dice: the die {} at {:.1f} m/s", d.thrown ? "was thrown: rolling" : "falls (the game already rolled)", speed);
        }
        break;
    }
    case DieState::Flying: {
        d.velocity[1] -= kGravity * dt;
        const vr::Vec3 from = d.position;
        vr::Vec3 to{from[0] + d.velocity[0] * dt, from[1] + d.velocity[1] * dt, from[2] + d.velocity[2] * dt};
        const float r = g_die_size * 0.5f;
        // The first solid thing between here and the next position, a radius below (the player's own body passed).
        const vr::Vec3 probe_to{to[0], to[1] - r, to[2]};
        if (const auto hit = rayprobe::raycast_past_self(from, probe_to, 5, 0xFFFFFFFF, 0x8)) {
            const vr::Vec3 nrm = hit->normal;
            to = {hit->position[0] + nrm[0] * r, hit->position[1] + nrm[1] * r, hit->position[2] + nrm[2] * r};
            const float vn = d.velocity[0] * nrm[0] + d.velocity[1] * nrm[1] + d.velocity[2] * nrm[2];
            if (vn < 0.0f) {
                for (std::size_t k = 0; k < 3; ++k) {
                    const float normal_part = vn * nrm[k];
                    const float tangent = d.velocity[k] - normal_part;
                    d.velocity[k] = tangent * 0.7f - normal_part * 0.35f; // friction, restitution
                }
                for (auto& w : d.spin) {
                    w *= 0.7f;
                }
            }
            const float speed = std::sqrt(d.velocity[0] * d.velocity[0] + d.velocity[1] * d.velocity[1] + d.velocity[2] * d.velocity[2]);
            if (speed < 0.35f) {
                d.rest_since = d.rest_since == 0.0 ? now : d.rest_since;
            } else {
                d.rest_since = 0.0;
            }
        }
        d.position = to;
        const float w = std::sqrt(d.spin[0] * d.spin[0] + d.spin[1] * d.spin[1] + d.spin[2] * d.spin[2]);
        if (w > 1e-4f) {
            const float half = w * dt * 0.5f, s = std::sin(half) / w;
            d.rotation = normalized(vr::multiply({d.spin[0] * s, d.spin[1] * s, d.spin[2] * s, std::cos(half)}, d.rotation));
        }
        // On the ground and slow, with the game's result in: roll onto that face. Without a result yet it keeps
        // rocking in place. Something wrong (fell through the world, no result for long): it goes.
        if (d.rest_since > 0.0 && now - d.rest_since > 0.15 && roll->has_result) {
            d.face = roll->face;
            d.settle_from = d.rotation;
            d.settle_to = face_up(d.face, d.rotation);
            d.settle_t = 0.0f;
            d.velocity = {};
            d.state = DieState::Settling;
            log::info("dice: the die comes to rest on {} (dice {} / {}, total {}, {})", d.face, roll->dice[0], roll->dice[1],
                      roll->total, roll->success ? "success" : "failure");
        } else if (now - d.flying_since > 8.0) {
            if (roll->phase == 3 && !d.skip_sent) {
                request_skip();
            }
            destroy_die("never came to rest");
            return;
        }
        break;
    }
    case DieState::Settling: {
        d.settle_t = std::min(1.0f, d.settle_t + dt / 0.4f);
        const float e = 1.0f - (1.0f - d.settle_t) * (1.0f - d.settle_t);
        d.rotation = slerp(d.settle_from, d.settle_to, e);
        if (d.settle_t >= 1.0f) {
            d.state = DieState::Resting;
            d.resting_at = now;
            g_dice_landed.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
    case DieState::Resting: {
        // A moment to read it, then the game's own result (its skip of the UI animation), then the die goes.
        if (!d.skip_sent && now - d.resting_at > 0.8) {
            request_skip();
            d.skip_sent = true;
        }
        if (now - d.resting_at > 4.0 || (roll->phase >= 5 && now - d.resting_at > 2.5)) {
            destroy_die("rested");
            return;
        }
        break;
    }
    case DieState::None:
        break;
    }
    const float pos[3] = {d.position[0], d.position[1], d.position[2]};
    const float quat[4] = {d.rotation[0], d.rotation[1], d.rotation[2], d.rotation[3]};
    if (const int failed = place_die_raw(d.obj, pos, quat, die_scale())) {
        static int logged = 0;
        if (logged++ < 5) {
            log::warn("dice: placing the die failed (step {})", failed);
        }
    }
    ++d.frames;
    // The first dice: the visual over its first frames and then now and then (does it load, follow, scale?).
    if (g_dice_made.load(std::memory_order_relaxed) <= 3 && (d.frames <= 5 || d.frames % 90 == 0)) {
        log_visual(d.obj, std::format("frame {} {}", d.frames, static_cast<int>(d.state)).c_str(), pos, die_scale());
    }
    g_die_state.store(static_cast<int>(d.state), std::memory_order_relaxed);
}

} // namespace

void configure(const Config& config) {
    g_enabled.store(config.get_bool("dice", "physical", true), std::memory_order_relaxed);
    const std::string size = config.get("dice", "size_cm");
    g_die_size = size.empty() ? 0.12f : std::clamp(std::strtof(size.c_str(), nullptr), 4.0f, 40.0f) / 100.0f;
    g_die_shadow = config.get_bool("dice", "shadow", true);
}

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    configure(config);
    const bool update_ok = bytes_at(module.base + kUpdateRva, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55});
    const bool command_ok = bytes_at(module.base + kNextPhaseRva, {0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x48, 0x8D, 0x68, 0xA1});
    if (!update_ok || !command_ok) {
        health::set("physical dice", health::State::Unavailable, "the roll screen functions differ in this build");
        log::warn("dice: the roll screen's update / Roll command don't match this build; no physical dice");
        return;
    }
    if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(module.base + kUpdateRva),
                                                reinterpret_cast<void*>(&on_update))) {
        g_update_hook = std::move(*h);
    }
    g_die_ready =
        bytes_at(module.base + kAllocRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}) &&
        bytes_at(module.base + kAllocatorRva, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05}) &&
        bytes_at(module.base + kCloneCtorRva, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55}) &&
        bytes_at(module.base + kCloneSceneRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA}) &&
        bytes_at(module.base + kCloneVisualRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57}) &&
        bytes_at(module.base + kCloneDeleteRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA}) &&
        bytes_at(module.base + kFixedStringRva, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57}) &&
        bytes_at(module.base + kFixedStringReleaseRva, {0x40, 0x53, 0x55, 0x56, 0x57}) &&
        bytes_at(module.base + kTransformRva, {0x40, 0x53, 0x57, 0x41, 0x55, 0x41, 0x56}) &&
        bytes_at(module.base + kCloneVisualPtrRva, {0x48, 0x83, 0xEC, 0x68, 0x4C, 0x8D, 0x41, 0x08});
    health::set("physical dice",
                !g_update_hook ? health::State::Unavailable : g_enabled.load() ? health::State::Ready : health::State::Off,
                g_die_ready ? "[dice] physical" : "the die's functions differ: rolls follow the UI only");
    log::info("dice: following the roll screen (update exe+0x{:X}, Roll command exe+0x{:X}); physical dice {}{}", kUpdateRva,
              kNextPhaseRva, g_enabled.load() ? "on" : "off",
              g_die_ready ? std::format(" ({:.0f} cm die)", g_die_size * 100.0f) : std::string(" (die functions differ: no die)"));
}

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed) && static_cast<bool>(g_update_hook);
}

std::optional<Roll> current() {
    std::scoped_lock lock(g_mutex);
    // The screen's update stops when it closes: a roll not updated for half a second is gone.
    if (g_roll && std::chrono::steady_clock::now() - g_last_update > std::chrono::milliseconds(500)) {
        g_roll.reset();
        g_dc = 0;
    }
    return g_roll;
}

void request_roll() {
    g_roll_request.store(true, std::memory_order_relaxed);
}

void request_skip() {
    g_skip_request.store(true, std::memory_order_relaxed);
}

void set_hand(bool grip, std::optional<vr::RigidPose> grip_pose, double time_s) {
    std::scoped_lock lock(g_hand_mutex);
    g_hand = {grip, grip_pose, time_s};
}

bool wants_grip() {
    const int s = g_die_state.load(std::memory_order_relaxed);
    return s == static_cast<int>(DieState::InHand);
}

bool hide_ui() {
    const int s = g_die_state.load(std::memory_order_relaxed);
    return s == static_cast<int>(DieState::Flying) || s == static_cast<int>(DieState::Settling) ||
           (s == static_cast<int>(DieState::Resting) && g_skips_sent.load(std::memory_order_relaxed) == 0);
}

void on_manager_update() {
    if (g_die_ready && static_cast<bool>(g_update_hook)) {
        die_frame();
    }
}

void forget() {
    g_forget_die.store(true, std::memory_order_relaxed);
}

void report() {
    log::info("dice: {} roll screens, {} rolls and {} skips sent by the physical die; dice made {}, thrown {}, landed {}, "
              "failed {}",
              g_screens.load(), g_rolls_sent.load(), g_skips_sent.load(), g_dice_made.load(), g_dice_thrown.load(),
              g_dice_landed.load(), g_dice_failed.load());
}

} // namespace bg3vr::dice
