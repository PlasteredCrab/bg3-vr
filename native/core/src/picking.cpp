#include "bg3vr/picking.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/rayprobe.hpp"
#include "bg3vr/signatures.hpp"

#include <safetyhook.hpp>

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace bg3vr::picking {

namespace {

ModuleInfo g_module;
safetyhook::InlineHook g_hook;
std::uintptr_t g_return1 = 0;
std::uintptr_t g_return2 = 0;
std::atomic<bool> g_hand{false};
std::atomic<std::uint64_t> g_calls{0};       // picking calls seen
std::atomic<std::uint64_t> g_replaced{0};
std::atomic<std::uint64_t> g_no_aim{0};      // hand on but no tracked aim ray
std::atomic<int> g_logged{0};
constexpr int kLogFirst = 3;

using RayFn = void* (*)(std::uintptr_t camera, float u, float v, float* out_a, float* out_b, bool cached);

void* hooked_ray(std::uintptr_t camera, float u, float v, float* out_a, float* out_b, bool cached) {
    void* result = g_hook.call<void*>(camera, u, v, out_a, out_b, cached);
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (caller != g_return1 && caller != g_return2) {
        return result;
    }
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const bool hand = g_hand.load(std::memory_order_relaxed);
    const auto aim = hand ? rayprobe::hand_aim() : std::nullopt;
    if (g_logged.load(std::memory_order_relaxed) < kLogFirst) {
        g_logged.fetch_add(1, std::memory_order_relaxed);
        log::info("pick: call from exe+0x{:X}, u {:.3f} v {:.3f} cached {}: game ray A ({:.3f}, {:.3f}, {:.3f}) B ({:.3f}, "
                  "{:.3f}, {:.3f}){}",
                  caller - g_module.base, u, v, cached, out_a[0], out_a[1], out_a[2], out_b[0], out_b[1], out_b[2],
                  std::isfinite(out_b[0]) && std::isfinite(out_b[1]) && std::isfinite(out_b[2]) ? "" : " (B not finite)");
    }
    if (!hand) {
        return result;
    }
    if (!aim) {
        g_no_aim.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const vr::Vec3 dir = vr::rotate(aim->orientation, {0.0f, 0.0f, 1.0f});
    for (int i = 0; i < 3; ++i) {
        out_a[i] = aim->position[static_cast<std::size_t>(i)];
        out_b[i] = aim->position[static_cast<std::size_t>(i)] + dir[static_cast<std::size_t>(i)] * layout::kRayLength;
    }
    g_replaced.fetch_add(1, std::memory_order_relaxed);
    return result;
}

// The byte before `return_rva` must end a `call rel32` to `target`.
bool calls_target(std::uintptr_t return_rva, std::uintptr_t target) {
    std::uint8_t opcode = 0;
    std::int32_t rel = 0;
    const std::uintptr_t ret = g_module.base + return_rva;
    if (!mem::try_copy(&opcode, ret - 5, 1) || !mem::try_copy(&rel, ret - 4, 4) || opcode != 0xE8) {
        return false;
    }
    return ret + static_cast<std::intptr_t>(rel) == target;
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    const auto site = sig::lookup("camera_screen_to_world_ray");
    if (!site) {
        log::warn("pick: signature camera_screen_to_world_ray not resolved; hand pointing unavailable");
        return;
    }
    if (!calls_target(layout::kPickReturn1, *site) || !calls_target(layout::kPickReturn2, *site)) {
        log::warn("pick: the picking call sites (exe+0x{:X}, exe+0x{:X}) don't call exe+0x{:X}; hand pointing unavailable",
                  layout::kPickReturn1, layout::kPickReturn2, *site - module.base);
        return;
    }
    g_return1 = module.base + layout::kPickReturn1;
    g_return2 = module.base + layout::kPickReturn2;
    auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(*site), reinterpret_cast<void*>(&hooked_ray));
    if (!hook) {
        log::error("pick: inline hook failed (error type {})", static_cast<int>(hook.error().type));
        return;
    }
    g_hook = std::move(*hook);
    set_hand(config.get_bool("pick", "hand", false));
    log::info("pick: hook at exe+0x{:X} (picking rays from exe+0x{:X} / exe+0x{:X})", *site - module.base,
              layout::kPickReturn1, layout::kPickReturn2);
}

void set_hand(bool on) {
    g_hand.store(on, std::memory_order_relaxed);
    log::info("pick: hand pointing {}", on ? "on (the right hand's aim ray replaces the mouse ray)" : "off (mouse)");
}

void report() {
    log::info("pick: {} picking rays, {} from the hand, {} with no tracked aim", g_calls.load(std::memory_order_relaxed),
              g_replaced.load(std::memory_order_relaxed), g_no_aim.load(std::memory_order_relaxed));
}

} // namespace bg3vr::picking
