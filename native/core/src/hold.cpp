#include "bg3vr/hold.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/pattern.hpp"
#include "bg3vr/rayprobe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <span>
#include <string>

namespace bg3vr::hold {

namespace {

ModuleInfo g_module;
bool g_probe = false;
bool g_hold = false;
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
};
std::mutex g_mutex;
std::array<HandInput, 2> g_input{};

// Game thread.
struct Held {
    std::uintptr_t item = 0;
    std::uint64_t entity = 0;
    vr::RigidPose offset; // item relative to the hand
    std::uint64_t frames = 0;
};
std::array<bool, 2> g_was_down{};
std::array<std::optional<Held>, 2> g_held{};
std::uint64_t g_grabs = 0;
std::atomic<bool> g_forget{false}; // set by forget() on any thread, handled on the game thread
std::uint64_t g_misses = 0;

vr::Quat conjugate(const vr::Quat& q) {
    return {-q[0], -q[1], -q[2], q[3]};
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

bool still_item(const Held& held) {
    std::uint64_t handle = 0;
    return mem::try_copy(&handle, held.item + kItemHandle, sizeof(handle)) && handle == held.entity;
}

void try_grab(std::size_t h, const HandInput& input) {
    if (!input.aim_pose || !input.grip_pose) {
        return;
    }
    const vr::Vec3 dir = vr::rotate(input.aim_pose->orientation, {0.0f, 0.0f, 1.0f});
    const vr::Vec3 from = input.aim_pose->position;
    const vr::Vec3 to{from[0] + dir[0] * g_reach, from[1] + dir[1] * g_reach, from[2] + dir[2] * g_reach};
    const auto hit = rayprobe::raycast(from, to);
    const char* hand = h == 0 ? "left" : "right";
    if (!hit) {
        ++g_misses;
        log::info("grab: {} hand: nothing within {:.1f} m", hand, g_reach);
        return;
    }
    std::uintptr_t helper = 0;
    mem::try_copy(&helper, g_module.base + kEntityHelper, sizeof(helper));
    const std::uintptr_t item = hit->entity != rayprobe::kNullEntity && helper
                                    ? fn<EntityToItemFn>(kEntityToItem)(helper, hit->entity)
                                    : 0;
    std::uint64_t item_handle = 0;
    const bool is_item = item && mem::try_copy(&item_handle, item + kItemHandle, sizeof(item_handle)) &&
                         item_handle == hit->entity;
    const auto pose = is_item ? item_pose(item) : std::nullopt;
    log::info("grab: {} hand hit at {:.2f} m ({:.2f}, {:.2f}, {:.2f}), actor type {}, wrapper 0x{:X} {}, container 0x{:X}, "
              "entity 0x{:016X} -> item 0x{:X}{}{}",
              hand, hit->distance, hit->position[0], hit->position[1], hit->position[2], hit->actor_type, hit->wrapper,
              mem::describe(g_module, [&] {
                  std::uintptr_t vtable = 0;
                  mem::try_copy(&vtable, hit->wrapper, sizeof(vtable));
                  return vtable;
              }()),
              hit->container, hit->entity, item, is_item ? "" : " (not a client item)",
              pose ? std::format(" at ({:.2f}, {:.2f}, {:.2f})", pose->position[0], pose->position[1], pose->position[2])
                   : std::string());
    if (!g_hold || !is_item || !pose) {
        return;
    }
    fn<ItemFn>(kSuspend)(item);
    const vr::Quat inverse = conjugate(input.grip_pose->orientation);
    const vr::Vec3 rel{pose->position[0] - input.grip_pose->position[0], pose->position[1] - input.grip_pose->position[1],
                       pose->position[2] - input.grip_pose->position[2]};
    Held held;
    held.item = item;
    held.entity = hit->entity;
    held.offset = {vr::rotate(inverse, rel), vr::multiply(inverse, pose->orientation)};
    g_held[h] = held;
    ++g_grabs;
    log::info("grab: {} hand holds item 0x{:X} (offset ({:.2f}, {:.2f}, {:.2f}))", hand, item, held.offset.position[0],
              held.offset.position[1], held.offset.position[2]);
}

void hold_frame(std::size_t h, const HandInput& input) {
    Held& held = *g_held[h];
    if (!still_item(held)) {
        log::warn("grab: the held item 0x{:X} is gone; dropped without touching it", held.item);
        g_held[h].reset();
        return;
    }
    if (!input.grip_pose) {
        return; // tracking lost: keep it where it is
    }
    const vr::RigidPose& hand = *input.grip_pose;
    const vr::Vec3 offset = vr::rotate(hand.orientation, held.offset.position);
    const float position[3] = {hand.position[0] + offset[0], hand.position[1] + offset[1], hand.position[2] + offset[2]};
    const vr::Quat rotation = vr::multiply(hand.orientation, held.offset.orientation);
    const float quat[4] = {rotation[0], rotation[1], rotation[2], rotation[3]};
    fn<SetVecFn>(kSetTranslate)(held.item, position);
    fn<SetVecFn>(kSetRotate)(held.item, quat);
    ++held.frames;
}

void release(std::size_t h) {
    Held& held = *g_held[h];
    if (still_item(held)) {
        fn<ItemFn>(kResume)(held.item);
        fn<SetFlagsFn>(kSetFlags)(held.item, kReportTransform);
        const auto pose = item_pose(held.item);
        log::info("grab: {} hand released item 0x{:X} after {} frames{}", h == 0 ? "left" : "right", held.item,
                  held.frames,
                  pose ? std::format(" at ({:.2f}, {:.2f}, {:.2f}); reported to the server", pose->position[0],
                                     pose->position[1], pose->position[2])
                       : std::string());
    } else {
        log::warn("grab: the released item 0x{:X} is gone", held.item);
    }
    g_held[h].reset();
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    g_probe = config.get_bool("grab", "probe", false);
    g_hold = config.get_bool("grab", "hold", false);
    const std::string reach = config.get("grab", "reach");
    if (!reach.empty()) {
        g_reach = std::clamp(std::strtof(reach.c_str(), nullptr), 0.2f, 10.0f);
    }
    if (!g_probe && !g_hold) {
        log::info("grab: off ([grab] probe / hold)");
        return;
    }
    g_ready = check(kEntityToItem) && check(kSetTranslate) && check(kSetRotate) && check(kSuspend) && check(kResume) &&
              check(kSetFlags);
    log::info("grab: {} (reach {:.1f} m){}", g_hold ? "picking items up with the grips" : "probe only (logs what a grip hits)",
              g_reach, g_ready ? "" : "; UNAVAILABLE: a function didn't match");
}

bool enabled() {
    return g_ready && (g_probe || g_hold);
}

void set_hand(std::size_t hand, bool grip, std::optional<vr::RigidPose> grip_pose, std::optional<vr::RigidPose> aim_pose) {
    if (hand > 1) {
        return;
    }
    std::scoped_lock lock(g_mutex);
    g_input[hand] = {grip, grip_pose, aim_pose};
}

void on_frame() {
    if (!enabled()) {
        return;
    }
    if (g_forget.exchange(false, std::memory_order_relaxed)) {
        for (auto& held : g_held) {
            held.reset();
        }
        g_was_down = {};
    }
    std::array<HandInput, 2> input;
    {
        std::scoped_lock lock(g_mutex);
        input = g_input;
    }
    for (std::size_t h = 0; h < 2; ++h) {
        const bool down = input[h].grip;
        if (down && !g_was_down[h] && !g_held[h]) {
            try_grab(h, input[h]);
        } else if (down && g_held[h]) {
            hold_frame(h, input[h]);
        } else if (!down && g_held[h]) {
            release(h);
        }
        g_was_down[h] = down;
    }
}

void forget() {
    g_forget.store(true, std::memory_order_relaxed);
}

void report() {
    log::info("grab: {}, {} grabs, {} misses", enabled() ? (g_hold ? "hold" : "probe") : "off", g_grabs, g_misses);
}

} // namespace bg3vr::hold
