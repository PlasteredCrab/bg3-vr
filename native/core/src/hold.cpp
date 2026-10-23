#include "bg3vr/hold.hpp"

#include "bg3vr/bridge.hpp"
#include "bg3vr/corpse.hpp"
#include "bg3vr/grab_math.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/pattern.hpp"
#include "bg3vr/rayprobe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace bg3vr::hold {

namespace {

ModuleInfo g_module;
bool g_probe = false;
std::atomic<bool> g_hold{false};
bool g_ready = false; // every function checked
float g_reach = 1.5f;

// 4.1.1.7631656, agent E (E_items_interaction.md 1, 2.2, 3); prologues re-disassembled 2026-10-20.
struct Function {
    const char* name;
    std::uintptr_t rva;
    const char* prologue;
};
constexpr Function kEntityToItem{"ecl entity -> ecl::Item", 0x704BD0,
                                 "48 89 5C 24 18 56 48 81 EC 90 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 80 00 00 00 48 8B C2 48 8B DA 48 C1 E8 36"};
constexpr Function kSetTranslate{"ecl::Item::SetTranslate", 0x1C77050,
                                 "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 8D 59 20"};
constexpr Function kSetRotate{"ecl::Item::SetRotate", 0x1C77130,
                              "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 8D 59 20"};
constexpr Function kSuspend{"client physics suspend", 0x1C7DD00, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B D9 80 79 6A 00"};
constexpr Function kResume{"client physics resume", 0x1C7DE60, "48 89 5C 24 08 57 48 83 EC 50 48 8B D9 0F B6 41 6A"};
constexpr Function kSetFlags{"ecl::Item::SetFlags", 0x1C76D50, "40 53 56 57 48 83 EC 30 48 8B 59 18"};
constexpr std::uintptr_t kEntityHelper = 0x6007F68; // ecl entity helper*
constexpr std::size_t kItemHandle = 0x10;
constexpr std::size_t kItemGetTransformSlot = 0xD0 / 8; // returns {quat +0x00, translate +0x10}
constexpr std::uint64_t kReportTransform = 0x40000;

using EntityToItemFn = std::uintptr_t (*)(std::uintptr_t helper, std::uint64_t handle);
using SetVecFn = void (*)(std::uintptr_t item, const float* value);
using ItemFn = void (*)(std::uintptr_t item);
using SetFlagsFn = void (*)(std::uintptr_t item, std::uint64_t mask);
using GetTransformFn = const float* (*)(std::uintptr_t item);

template <class F>
F fn(const Function& f) {
    return reinterpret_cast<F>(g_module.base + f.rva);
}

bool check(const Function& f) {
    const auto pattern = parse_pattern(f.prologue);
    std::array<std::uint8_t, 64> bytes{};
    if (!pattern || pattern->size() > bytes.size() ||
        !mem::try_copy(bytes.data(), g_module.base + f.rva, pattern->size())) {
        return false;
    }
    const auto at = find_first(std::span<const std::uint8_t>(bytes.data(), pattern->size()), *pattern);
    if (!at || *at != 0) {
        log::error("grab: {} at exe+0x{:X} doesn't have the expected prologue (game updated?)", f.name, f.rva);
        return false;
    }
    return true;
}

// Hands, from the headset.
struct HandInput {
    bool grip = false;
    std::optional<vr::RigidPose> grip_pose;
    std::optional<vr::RigidPose> aim_pose;
    double time = 0.0; // sample time, seconds
    bool in_pack = false; // over a shoulder or behind the back
};
std::mutex g_mutex;
std::array<HandInput, 2> g_input{};

// Throwing and pulling ([grab]).
bool g_throw = true;
float g_throw_min_speed = 1.5f;
bool g_pull = true;
float g_pull_range = 6.0f;
float g_catch_radius = 0.35f;
constexpr float kGravity = 9.81f;
constexpr double kMaxFlight = 4.0;

// Game thread.
struct Held {
    std::uintptr_t item = 0;
    std::uint64_t entity = 0;
    vr::RigidPose offset; // item relative to the hand
    std::uint64_t frames = 0;
};
struct Flight {
    std::uintptr_t item = 0;
    std::uint64_t entity = 0;
    vr::Vec3 position{};
    vr::Vec3 velocity{};
    vr::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    double age = 0.0;
    int to_hand = -1; // a pull toward this hand (caught there), or -1 for a throw
};
struct Target {
    std::uintptr_t item = 0;
    std::uint64_t entity = 0;
};
std::array<bool, 2> g_was_down{};
std::array<std::optional<Held>, 2> g_held{};
std::array<std::optional<Target>, 2> g_pull_target{};
std::array<grab::ThrowTracker, 2> g_trackers{};
std::array<double, 2> g_tracked_time{};
std::vector<Flight> g_flights;
std::uint64_t g_grabs = 0;
std::uint64_t g_throws = 0;
std::uint64_t g_pulls = 0;
std::uint64_t g_catches = 0;
std::atomic<bool> g_forget{false}; // set by forget() on any thread, handled on the game thread

// Gloves: the item each hand's aim highlights (game thread), published for the laser and the grip binding.
struct Highlight {
    std::uintptr_t item = 0;
    std::uint64_t entity = 0;
    float distance = 0.0f;
    double seen = 0.0; // steady-clock seconds of the last ray that found it
};
std::array<std::optional<Highlight>, 2> g_highlight{};
std::array<std::atomic<int>, 2> g_aim_state{};
std::array<std::atomic<bool>, 2> g_holding{};
std::array<std::atomic<bool>, 2> g_incoming{}; // an item is flying to this hand
std::array<int, 2> g_cone_step{};
std::uint64_t g_scan_frame = 0;
std::uint64_t g_highlights = 0;
std::uint64_t g_locks = 0;
std::uint64_t g_stows = 0;
constexpr double kHighlightHold = 0.25;   // seconds a highlight survives without a ray finding it (no flicker)
constexpr float kConeDegrees = 4.0f;      // aim assist: the rays around the centre one
constexpr int kConeRays = 6;
constexpr float kAimSkip = 0.25f;         // metres ahead of the hand before the ray starts (the player's own body)
constexpr float kHomingRange = 1.2f;      // a pulled item steers into the hand inside this distance
struct HapticSlot {
    std::atomic<bool> pending{false};
    std::atomic<float> amplitude{0.0f};
    std::atomic<float> seconds{0.0f};
};
std::array<HapticSlot, 2> g_haptic;

double seconds_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void buzz(std::size_t h, float amplitude, float seconds) {
    g_haptic[h].amplitude.store(amplitude, std::memory_order_relaxed);
    g_haptic[h].seconds.store(seconds, std::memory_order_relaxed);
    g_haptic[h].pending.store(true, std::memory_order_release);
}
std::uint64_t g_misses = 0;
std::chrono::steady_clock::time_point g_last_frame{};

const char* hand_name(std::size_t h) {
    return h == 0 ? "left" : "right";
}

vr::Quat conjugate(const vr::Quat& q) {
    return {-q[0], -q[1], -q[2], q[3]};
}

float length(const vr::Vec3& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

vr::Vec3 minus(const vr::Vec3& a, const vr::Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

std::optional<vr::RigidPose> item_pose(std::uintptr_t item) {
    std::uintptr_t vtable = 0;
    std::uintptr_t slot = 0;
    if (!mem::try_copy(&vtable, item, sizeof(vtable)) ||
        !mem::try_copy(&slot, vtable + kItemGetTransformSlot * sizeof(void*), sizeof(slot)) ||
        !mem::in_section(g_module, ".text", slot)) {
        return std::nullopt;
    }
    const float* t = reinterpret_cast<GetTransformFn>(slot)(item);
    float values[7];
    if (!t || !mem::try_copy(values, reinterpret_cast<std::uintptr_t>(t), sizeof(float) * 4) ||
        !mem::try_copy(values + 4, reinterpret_cast<std::uintptr_t>(t) + 0x10, sizeof(float) * 3)) {
        return std::nullopt;
    }
    return vr::RigidPose{{values[4], values[5], values[6]}, {values[0], values[1], values[2], values[3]}};
}

bool still_item(std::uintptr_t item, std::uint64_t entity) {
    std::uint64_t handle = 0;
    return mem::try_copy(&handle, item + kItemHandle, sizeof(handle)) && handle == entity;
}

void place(std::uintptr_t item, const vr::Vec3& position, const vr::Quat& rotation) {
    const float p[3] = {position[0], position[1], position[2]};
    const float q[4] = {rotation[0], rotation[1], rotation[2], rotation[3]};
    fn<SetVecFn>(kSetTranslate)(item, p);
    fn<SetVecFn>(kSetRotate)(item, q);
}

// Physics back on and the final pose to the server (the end of every hold, throw and pull).
void settle(std::uintptr_t item, const char* why) {
    fn<ItemFn>(kResume)(item);
    fn<SetFlagsFn>(kSetFlags)(item, kReportTransform);
    const auto pose = item_pose(item);
    log::info("grab: item 0x{:X} {}{}", item, why,
              pose ? std::format(" at ({:.2f}, {:.2f}, {:.2f}); reported to the server", pose->position[0],
                                 pose->position[1], pose->position[2])
                   : std::string());
}

struct Resolved {
    std::uintptr_t item = 0;
    std::uint64_t entity = 0;
    vr::RigidPose pose;
    float distance = 0.0f;
};

// The client item an entity handle from a ray hit belongs to (0 when it isn't one).
std::uintptr_t item_of(std::uint64_t entity) {
    if (entity == rayprobe::kNullEntity) {
        return 0;
    }
    std::uintptr_t helper = 0;
    mem::try_copy(&helper, g_module.base + kEntityHelper, sizeof(helper));
    const std::uintptr_t item = helper ? fn<EntityToItemFn>(kEntityToItem)(helper, entity) : 0;
    return item && still_item(item, entity) ? item : 0;
}

bool busy_item(std::uint64_t entity) {
    for (const auto& held : g_held) {
        if (held && held->entity == entity) {
            return true;
        }
    }
    for (const Flight& f : g_flights) {
        if (f.entity == entity) {
            return true;
        }
    }
    return false;
}

// One aim ray of the gloves: the item it hits first within the pull range, if any.
std::optional<Highlight> aim_ray(const vr::RigidPose& aim, const vr::Vec3& local_dir, float range) {
    const vr::Vec3 dir = vr::rotate(aim.orientation, local_dir);
    const vr::Vec3 from{aim.position[0] + dir[0] * kAimSkip, aim.position[1] + dir[1] * kAimSkip,
                        aim.position[2] + dir[2] * kAimSkip};
    const vr::Vec3 to{aim.position[0] + dir[0] * range, aim.position[1] + dir[1] * range, aim.position[2] + dir[2] * range};
    const auto hit = rayprobe::raycast(from, to);
    if (!hit) {
        return std::nullopt;
    }
    const std::uintptr_t item = item_of(hit->entity);
    if (!item || busy_item(hit->entity)) {
        return std::nullopt;
    }
    return Highlight{item, hit->entity, hit->distance + kAimSkip, 0.0};
}

// Gloves, one hand per frame: the centre ray and one of the cone rays around it (all six every 6 scans of a hand).
void scan(std::size_t h, const HandInput& input) {
    const double now = seconds_now();
    std::optional<Highlight> found = aim_ray(*input.aim_pose, {0.0f, 0.0f, 1.0f}, g_pull_range);
    if (!found) {
        const float a = kConeDegrees * 3.14159265f / 180.0f;
        const float phi = 2.0f * 3.14159265f * static_cast<float>(g_cone_step[h]) / static_cast<float>(kConeRays);
        g_cone_step[h] = (g_cone_step[h] + 1) % kConeRays;
        found = aim_ray(*input.aim_pose, {std::sin(a) * std::cos(phi), std::sin(a) * std::sin(phi), std::cos(a)}, g_pull_range);
    }
    std::optional<Highlight>& current = g_highlight[h];
    if (found) {
        found->seen = now;
        const bool is_new = !current || current->entity != found->entity;
        current = found;
        if (is_new) {
            ++g_highlights;
            buzz(h, 0.12f, 0.015f);
            const auto pose = item_pose(found->item);
            log::info("gloves: {} hand highlights item 0x{:X} (entity 0x{:016X}) {:.2f} m away{}", hand_name(h), found->item,
                      found->entity, found->distance,
                      pose ? std::format(" at ({:.2f}, {:.2f}, {:.2f})", pose->position[0], pose->position[1], pose->position[2])
                           : std::string());
        }
    } else if (current && (now - current->seen > kHighlightHold || !still_item(current->item, current->entity))) {
        log::info("gloves: {} hand no longer highlights item 0x{:X}", hand_name(h), current->item);
        current.reset();
    }
}

// The client item the hand's aim ray hits within `range`, logged.
std::optional<Resolved> aim_at_item(std::size_t h, const HandInput& input, float range) {
    const vr::Vec3 dir = vr::rotate(input.aim_pose->orientation, {0.0f, 0.0f, 1.0f});
    const vr::Vec3 from = input.aim_pose->position;
    const vr::Vec3 to{from[0] + dir[0] * range, from[1] + dir[1] * range, from[2] + dir[2] * range};
    const auto hit = rayprobe::raycast(from, to);
    if (!hit) {
        ++g_misses;
        log::info("grab: {} hand: nothing within {:.1f} m", hand_name(h), range);
        return std::nullopt;
    }
    std::uintptr_t helper = 0;
    mem::try_copy(&helper, g_module.base + kEntityHelper, sizeof(helper));
    const std::uintptr_t item = hit->entity != rayprobe::kNullEntity && helper
                                    ? fn<EntityToItemFn>(kEntityToItem)(helper, hit->entity)
                                    : 0;
    const bool is_item = item && still_item(item, hit->entity);
    const auto pose = is_item ? item_pose(item) : std::nullopt;
    log::info("grab: {} hand hit at {:.2f} m ({:.2f}, {:.2f}, {:.2f}), actor type {}, wrapper 0x{:X} {}, container 0x{:X}, "
              "entity 0x{:016X} -> item 0x{:X}{}{}",
              hand_name(h), hit->distance, hit->position[0], hit->position[1], hit->position[2], hit->actor_type,
              hit->wrapper,
              mem::describe(g_module, [&] {
                  std::uintptr_t vtable = 0;
                  mem::try_copy(&vtable, hit->wrapper, sizeof(vtable));
                  return vtable;
              }()),
              hit->container, hit->entity, item, is_item ? "" : " (not a client item)",
              pose ? std::format(" at ({:.2f}, {:.2f}, {:.2f})", pose->position[0], pose->position[1], pose->position[2])
                   : std::string());
    if (!is_item || !pose) {
        return std::nullopt;
    }
    return Resolved{item, hit->entity, *pose, hit->distance};
}

void take(std::size_t h, std::uintptr_t item, std::uint64_t entity, const vr::RigidPose& pose, const vr::RigidPose& hand,
          bool already_suspended) {
    if (!already_suspended) {
        fn<ItemFn>(kSuspend)(item);
    }
    const vr::Quat inverse = conjugate(hand.orientation);
    Held held;
    held.item = item;
    held.entity = entity;
    held.offset = {vr::rotate(inverse, minus(pose.position, hand.position)), vr::multiply(inverse, pose.orientation)};
    g_held[h] = held;
    ++g_grabs;
    log::info("grab: {} hand holds item 0x{:X} (offset ({:.2f}, {:.2f}, {:.2f}))", hand_name(h), item,
              held.offset.position[0], held.offset.position[1], held.offset.position[2]);
}

void press(std::size_t h, const HandInput& input) {
    if (!input.aim_pose || !input.grip_pose) {
        return;
    }
    // Closing the hand on an item flying to it: caught.
    for (std::size_t i = 0; g_hold && i < g_flights.size(); ++i) {
        Flight& f = g_flights[i];
        if (f.to_hand == static_cast<int>(h) && length(minus(f.position, input.grip_pose->position)) < g_catch_radius * 1.6f &&
            still_item(f.item, f.entity)) {
            take(h, f.item, f.entity, {f.position, f.rotation}, *input.grip_pose, true);
            ++g_catches;
            buzz(h, 0.8f, 0.08f);
            log::info("gloves: {} hand caught item 0x{:X} by closing on it", hand_name(h), f.item);
            g_flights.erase(g_flights.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
    // The highlighted item.
    if (g_hold && g_highlight[h] && still_item(g_highlight[h]->item, g_highlight[h]->entity)) {
        const Highlight target = *g_highlight[h];
        g_highlight[h].reset();
        const auto pose = item_pose(target.item);
        if (!pose) {
            log::warn("gloves: {} hand: the highlighted item 0x{:X} has no transform", hand_name(h), target.item);
            return;
        }
        const float distance = length(minus(pose->position, input.grip_pose->position));
        if (distance <= g_reach) {
            take(h, target.item, target.entity, *pose, *input.grip_pose, false);
            buzz(h, 0.5f, 0.04f);
        } else if (g_pull) {
            g_pull_target[h] = Target{target.item, target.entity};
            ++g_locks;
            buzz(h, 0.35f, 0.04f);
            log::info("gloves: {} hand locks on item 0x{:X} {:.1f} m away; flick the hand back to pull it", hand_name(h),
                      target.item, distance);
        }
        return;
    }
    if (g_hold) {
        return; // nothing highlighted: the grip keeps its binding (vrplay asks wants_grip())
    }
    const float range = g_reach;
    const auto target = aim_at_item(h, input, range);
    if (!g_hold || !target) {
        return;
    }
    if (target->distance <= g_reach) {
        take(h, target->item, target->entity, target->pose, *input.grip_pose, false);
    } else if (g_pull) {
        g_pull_target[h] = Target{target->item, target->entity};
        log::info("grab: {} hand aims at item 0x{:X} {:.1f} m away; flick the hand back to pull it", hand_name(h),
                  target->item, target->distance);
    }
}

void hold_frame(std::size_t h, const HandInput& input) {
    Held& held = *g_held[h];
    if (!still_item(held.item, held.entity)) {
        log::warn("grab: the held item 0x{:X} is gone; dropped without touching it", held.item);
        g_held[h].reset();
        return;
    }
    if (!input.grip_pose) {
        return; // tracking lost: keep it where it is
    }
    const vr::RigidPose& hand = *input.grip_pose;
    const vr::Vec3 offset = vr::rotate(hand.orientation, held.offset.position);
    place(held.item, {hand.position[0] + offset[0], hand.position[1] + offset[1], hand.position[2] + offset[2]},
          vr::multiply(hand.orientation, held.offset.orientation));
    ++held.frames;
}

void release(std::size_t h, bool in_pack) {
    Held held = *g_held[h];
    g_held[h].reset();
    if (!still_item(held.item, held.entity)) {
        log::warn("grab: the released item 0x{:X} is gone", held.item);
        return;
    }
    if (in_pack) {
        // Over a shoulder or behind the back: into the inventory. The item settles where it is first (physics back on,
        // pose to the server), then Lua asks the server to move it (Client/Holster.lua, Server/Holster.lua).
        settle(held.item, std::format("let go over the shoulder by the {} hand: stowing", hand_name(h)).c_str());
        bridge::push_request(std::format("stow 0x{:016X} {}", held.entity, hand_name(h)));
        ++g_stows;
        buzz(h, 0.5f, 0.05f);
        log::info("gloves: {} hand stows item 0x{:X} (entity 0x{:016X}) in the inventory", hand_name(h), held.item,
                  held.entity);
        return;
    }
    const auto velocity = g_trackers[h].velocity(0.1);
    const float speed = velocity ? length(velocity->linear) : 0.0f;
    const auto pose = item_pose(held.item);
    if (g_throw && speed >= g_throw_min_speed && pose) {
        g_flights.push_back({held.item, held.entity, pose->position, velocity->linear, pose->orientation, 0.0, -1});
        ++g_throws;
        log::info("grab: {} hand throws item 0x{:X} at {:.1f} m/s (after {} frames held)", hand_name(h), held.item, speed,
                  held.frames);
        return;
    }
    settle(held.item, std::format("released by the {} hand after {} frames", hand_name(h), held.frames).c_str());
}

// A flick back toward the player while aiming at a far item: it flies on an arc into the hand.
void pull_frame(std::size_t h, const HandInput& input) {
    const Target target = *g_pull_target[h];
    if (!still_item(target.item, target.entity) || !input.grip_pose) {
        return;
    }
    const auto velocity = g_trackers[h].velocity(0.08);
    const auto pose = item_pose(target.item);
    if (!velocity || !pose || !grab::is_flick(input.grip_pose->position, velocity->linear, pose->position)) {
        return;
    }
    fn<ItemFn>(kSuspend)(target.item);
    buzz(h, 0.6f, 0.06f);
    const vr::Vec3 hand = input.grip_pose->position;
    const grab::Launch launch = grab::launch_velocity(pose->position, {hand[0], hand[1] + 0.1f, hand[2]}, kGravity, 0.3f);
    g_flights.push_back({target.item, target.entity, pose->position, launch.velocity, pose->orientation, 0.0,
                         static_cast<int>(h)});
    g_pull_target[h].reset();
    ++g_pulls;
    log::info("grab: {} hand pulls item 0x{:X} (flight {:.2f} s)", hand_name(h), target.item, launch.time);
}

// Moves every item in flight; lands it on the first solid thing in its path, or in a hand that is pulling it.
void flights_frame(double dt, const std::array<HandInput, 2>& input) {
    for (std::size_t i = 0; i < g_flights.size();) {
        Flight& f = g_flights[i];
        bool done = false;
        if (!still_item(f.item, f.entity)) {
            log::warn("grab: an item in flight (0x{:X}) is gone", f.item);
            g_flights.erase(g_flights.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        f.age += dt;
        // A pulled item steers into its hand over the last stretch (the hand moved since the launch), keeping its speed
        // (at least 3 m/s) so it arrives instead of hanging in front of the hand.
        if (f.to_hand >= 0) {
            const HandInput& hand = input[static_cast<std::size_t>(f.to_hand)];
            if (hand.grip_pose) {
                const vr::Vec3 to_hand = minus(hand.grip_pose->position, f.position);
                const float d = length(to_hand);
                if (d < kHomingRange && d > 1e-3f) {
                    const float speed = std::max(3.0f, length(f.velocity));
                    const float blend = std::clamp(1.0f - d / kHomingRange, 0.25f, 0.9f);
                    for (std::size_t k = 0; k < 3; ++k) {
                        f.velocity[k] = f.velocity[k] * (1.0f - blend) + to_hand[k] / d * speed * blend;
                    }
                }
            }
        }
        const vr::Vec3 from = f.position;
        vr::Vec3 to{from[0] + f.velocity[0] * static_cast<float>(dt), from[1] + f.velocity[1] * static_cast<float>(dt),
                    from[2] + f.velocity[2] * static_cast<float>(dt)};
        f.velocity[1] -= kGravity * static_cast<float>(dt);

        // Caught by the pulling hand (grip still held, item close).
        if (f.to_hand >= 0) {
            const HandInput& hand = input[static_cast<std::size_t>(f.to_hand)];
            if (hand.grip && hand.grip_pose && !g_held[static_cast<std::size_t>(f.to_hand)] &&
                length(minus(to, hand.grip_pose->position)) < g_catch_radius) {
                take(static_cast<std::size_t>(f.to_hand), f.item, f.entity, {to, f.rotation}, *hand.grip_pose, true);
                ++g_catches;
                buzz(static_cast<std::size_t>(f.to_hand), 0.8f, 0.08f);
                log::info("gloves: {} hand caught item 0x{:X} after {:.2f} s in flight", hand_name(static_cast<std::size_t>(f.to_hand)),
                          f.item, f.age);
                g_flights.erase(g_flights.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        // Landing: the first solid thing between here and the next position (skipping the item's own shape). A pulled
        // item near its hand doesn't land, so the player's own body can't stop it short of the catch.
        bool near_hand = false;
        if (f.to_hand >= 0) {
            const HandInput& hand = input[static_cast<std::size_t>(f.to_hand)];
            near_hand = hand.grip_pose && length(minus(from, hand.grip_pose->position)) < 1.0f;
        }
        vr::Vec3 start = from;
        for (int attempt = 0; attempt < 3 && !done && !near_hand; ++attempt) {
            // Exclude 0x8 only: the ragdoll group 0x4004 shares bit 0x4 with the old 0xC, which hid every corpse (agent Q).
            const auto hit = rayprobe::raycast(start, to, 5, 0xFFFFFFFF, 0x8);
            if (!hit) {
                break;
            }
            if (hit->entity == f.entity) {
                const vr::Vec3 d = minus(to, start);
                const float l = length(d);
                if (l < 1e-4f) {
                    break;
                }
                start = {hit->position[0] + d[0] / l * 0.05f, hit->position[1] + d[1] / l * 0.05f,
                         hit->position[2] + d[2] / l * 0.05f};
                continue;
            }
            to = {hit->position[0] + hit->normal[0] * 0.03f, hit->position[1] + hit->normal[1] * 0.03f,
                  hit->position[2] + hit->normal[2] * 0.03f};
            done = true;
        }
        f.position = to;
        place(f.item, f.position, f.rotation);
        if (done || f.age > kMaxFlight) {
            settle(f.item, done ? "landed" : "stopped (flight too long)");
            g_flights.erase(g_flights.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    g_probe = config.get_bool("grab", "probe", false);
    g_hold = config.get_bool("grab", "hold", true);
    const auto number = [&](const char* key, float fallback, float lo, float hi) {
        const std::string v = config.get("grab", key);
        return v.empty() ? fallback : std::clamp(std::strtof(v.c_str(), nullptr), lo, hi);
    };
    g_reach = number("reach", 1.5f, 0.2f, 10.0f);
    g_throw = config.get_bool("grab", "throw", true);
    g_throw_min_speed = number("throw_min_speed", 1.5f, 0.3f, 20.0f);
    g_pull = config.get_bool("grab", "pull", true);
    g_pull_range = number("pull_range", 6.0f, 1.0f, 30.0f);
    g_catch_radius = number("catch_radius", 0.35f, 0.1f, 1.5f);
    // The functions are checked even while grabbing is off, so the VR menu can turn it on later.
    g_ready = check(kEntityToItem) && check(kSetTranslate) && check(kSetRotate) && check(kSuspend) && check(kResume) &&
              check(kSetFlags);
    if (!g_probe && !g_hold) {
        health::set("grabbing", g_ready ? health::State::Off : health::State::Unavailable, g_ready ? "[grab] hold (or the VR menu)" : "a function didn't match");
        log::info("grab: off ([grab] probe / hold, or the VR menu){}", g_ready ? "" : "; UNAVAILABLE: a function didn't match");
        return;
    }
    health::set("grabbing", g_ready ? health::State::Ready : health::State::Unavailable, g_ready ? (g_hold ? "hold, throw, pull" : "probe") : "a function didn't match");
    log::info("grab: {} (reach {:.1f} m{}{}){}",
              g_hold ? "picking items up with the grips" : "probe only (logs what a grip hits)", g_reach,
              g_hold && g_throw ? std::format(", throws above {:.1f} m/s", g_throw_min_speed) : std::string(),
              g_hold && g_pull ? std::format(", pulls from {:.1f} m with a flick", g_pull_range) : std::string(),
              g_ready ? "" : "; UNAVAILABLE: a function didn't match");
}

bool enabled() {
    return g_ready && (g_probe || g_hold);
}

bool hold_on() {
    return g_ready && g_hold.load(std::memory_order_relaxed);
}

void set_hold(bool on) {
    if (g_hold.exchange(on) != on) {
        if (!on) {
            forget(); // let go of anything held
        }
        log::info("grab: picking things up {}{}", on ? "on" : "off", on && !g_ready ? " (UNAVAILABLE: a function didn't match)" : "");
    }
}

void set_hand(std::size_t hand, bool grip, std::optional<vr::RigidPose> grip_pose, std::optional<vr::RigidPose> aim_pose,
              double time_s, bool in_pack) {
    if (hand > 1) {
        return;
    }
    std::scoped_lock lock(g_mutex);
    g_input[hand] = {grip, grip_pose, aim_pose, time_s, in_pack};
}

Aim aim_state(std::size_t hand) {
    return hand > 1 ? Aim::None : static_cast<Aim>(g_aim_state[hand].load(std::memory_order_relaxed));
}

bool wants_grip(std::size_t hand) {
    return hand <= 1 && g_ready && g_hold.load(std::memory_order_relaxed) &&
           (aim_state(hand) != Aim::None || g_incoming[hand].load(std::memory_order_relaxed));
}

bool holding(std::size_t hand) {
    return hand <= 1 && g_holding[hand].load(std::memory_order_relaxed);
}

std::optional<Haptic> take_haptic(std::size_t hand) {
    if (hand > 1 || !g_haptic[hand].pending.exchange(false, std::memory_order_acquire)) {
        return std::nullopt;
    }
    return Haptic{g_haptic[hand].amplitude.load(std::memory_order_relaxed), g_haptic[hand].seconds.load(std::memory_order_relaxed)};
}

void on_frame() {
    if (!enabled()) {
        return;
    }
    if (g_forget.exchange(false, std::memory_order_relaxed)) {
        for (auto& held : g_held) {
            held.reset();
        }
        for (auto& target : g_pull_target) {
            target.reset();
        }
        for (auto& highlight : g_highlight) {
            highlight.reset();
        }
        g_flights.clear();
        g_was_down = {};
    }
    std::array<HandInput, 2> input;
    {
        std::scoped_lock lock(g_mutex);
        input = g_input;
    }
    const auto now = std::chrono::steady_clock::now();
    const double dt = g_last_frame.time_since_epoch().count() == 0
                          ? 0.0
                          : std::min(0.1, std::chrono::duration<double>(now - g_last_frame).count());
    g_last_frame = now;
    for (std::size_t h = 0; h < 2; ++h) {
        if (input[h].grip_pose && input[h].time > g_tracked_time[h]) {
            g_trackers[h].add(input[h].time, *input[h].grip_pose);
            g_tracked_time[h] = input[h].time;
        }
        // corpse::on_frame ran first this frame: a grip that took a body is that body's, not an item press.
        const bool down = input[h].grip && !corpse::holding(h);
        // Gloves: a free, open hand looks for items (one hand per frame; a hand that closed this frame uses what it
        // had highlighted).
        const bool free_hand = !g_held[h] && !g_pull_target[h] && !down && !corpse::holding(h);
        if (g_hold && g_pull && free_hand && input[h].aim_pose && g_scan_frame % 2 == h) {
            scan(h, input[h]);
        } else if (!free_hand && !down) {
            g_highlight[h].reset();
        }
        if (down && !g_was_down[h] && !g_held[h]) {
            press(h, input[h]);
        } else if (down && g_held[h]) {
            hold_frame(h, input[h]);
        } else if (down && g_pull_target[h]) {
            pull_frame(h, input[h]);
        } else if (!down) {
            if (g_held[h]) {
                release(h, input[h].in_pack);
            }
            if (g_pull_target[h]) {
                log::info("gloves: {} hand let go of the lock on item 0x{:X} without pulling", hand_name(h), g_pull_target[h]->item);
            }
            g_pull_target[h].reset();
        }
        g_was_down[h] = down;
    }
    ++g_scan_frame;
    if (dt > 0.0 && !g_flights.empty()) {
        flights_frame(dt, input);
    }
    // What the hands are doing, for the laser, the grip binding and the holsters (other threads).
    for (std::size_t h = 0; h < 2; ++h) {
        const Aim state = g_held[h] ? Aim::Holding : g_pull_target[h] ? Aim::Locked : g_highlight[h] ? Aim::Highlight : Aim::None;
        g_aim_state[h].store(static_cast<int>(state), std::memory_order_relaxed);
        g_holding[h].store(g_held[h].has_value(), std::memory_order_relaxed);
        bool incoming = false;
        for (const Flight& f : g_flights) {
            incoming = incoming || f.to_hand == static_cast<int>(h);
        }
        g_incoming[h].store(incoming, std::memory_order_relaxed);
    }
}

void forget() {
    g_forget.store(true, std::memory_order_relaxed);
    for (std::size_t h = 0; h < 2; ++h) {
        g_aim_state[h].store(static_cast<int>(Aim::None), std::memory_order_relaxed);
        g_holding[h].store(false, std::memory_order_relaxed);
        g_incoming[h].store(false, std::memory_order_relaxed);
    }
}

void report() {
    log::info("grab: {}, {} grabs, {} throws, {} pulls ({} caught), {} misses; gloves: {} highlights, {} locks, {} stowed",
              enabled() ? (g_hold ? "hold" : "probe") : "off", g_grabs, g_throws, g_pulls, g_catches, g_misses, g_highlights,
              g_locks, g_stows);
}

} // namespace bg3vr::hold
