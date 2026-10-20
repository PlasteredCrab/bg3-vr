#include "bg3vr/cull.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>
#include <optional>

namespace bg3vr::cull {

namespace {

struct Target {
    std::uintptr_t object = 0;
    std::uintptr_t vtable = 0;
};

std::mutex g_mutex;
std::vector<Target> g_targets; // hidden every check
std::vector<Target> g_restore; // shown again once, then dropped

std::atomic<std::uint64_t> g_checks{0};
std::atomic<std::uint64_t> g_calls{0};
std::atomic<bool> g_pending{false};       // a hide/show request is waiting
std::atomic<std::uint64_t> g_frames{0};   // view updates seen, for throttling
std::atomic<std::uint64_t> g_ticks{0};    // QPC ticks spent checking
constexpr std::uint64_t kCheckInterval = 32;

ModuleInfo g_module;

using ToggleCullFn = void (*)(std::uintptr_t, bool);

// Guarded reads, not VirtualQuery: in the game, mem::readable took ~2.6 ms per call (5.3 ms per
// check with two objects, measured 2026-10-12), which caused a stutter every few frames.
bool still_valid(const Target& target, std::uint16_t& flags) {
    std::uintptr_t vtable = 0;
    return mem::try_copy(&vtable, target.object, sizeof(vtable)) && vtable == target.vtable &&
           mem::try_copy(&flags, target.object + layout::kCullFlags, sizeof(flags));
}

void set_main_view(const Target& target, bool visible) {
    const auto toggle = mem::read<ToggleCullFn>(target.vtable + layout::kToggleCullFlag600Slot * sizeof(void*));
    toggle(target.object, visible);
    g_calls.fetch_add(1, std::memory_order_relaxed);
}

std::optional<Target> check(std::uintptr_t object) {
    if (!mem::readable(object, 0x40)) {
        log::warn("hide: 0x{:X} is not readable", object);
        return std::nullopt;
    }
    const auto vtable = mem::read<std::uintptr_t>(object);
    if (!mem::in_section(g_module, ".rdata", vtable, (layout::kToggleCullFlag600Slot + 1) * sizeof(void*))) {
        log::warn("hide: 0x{:X} has vtable 0x{:X} outside .rdata", object, vtable);
        return std::nullopt;
    }
    const auto toggle = mem::read<std::uintptr_t>(vtable + layout::kToggleCullFlag600Slot * sizeof(void*));
    const auto get_flags = mem::read<std::uintptr_t>(vtable + layout::kGetCullFlagsSlot * sizeof(void*));
    if (!mem::in_section(g_module, ".text", toggle, 64)) {
        log::warn("hide: slot {} of 0x{:X} is outside .text", layout::kToggleCullFlag600Slot, object);
        return std::nullopt;
    }
    const auto code = mem::read<std::array<std::uint8_t, 64>>(toggle);
    bool has_constant = false;
    for (std::size_t i = 0; i + 1 < code.size(); ++i) {
        has_constant = has_constant || (code[i] == 0x00 && code[i + 1] == 0x06) || (code[i] == 0xFF && code[i + 1] == 0xF9);
    }
    log::info("hide: object 0x{:X} vtable {} ToggleCullFlag600 {} GetCullFlags {} flags 0x{:X} check {}", object,
              mem::describe(g_module, vtable), mem::describe(g_module, toggle), mem::describe(g_module, get_flags),
              mem::read<std::uint16_t>(object + layout::kCullFlags), has_constant ? "passed" : "FAILED");
    if (!has_constant) {
        return std::nullopt;
    }
    return Target{object, vtable};
}

} // namespace

void init(const ModuleInfo& module) {
    g_module = module;
}

void set_hidden(const std::vector<std::uintptr_t>& objects) {
    std::vector<Target> validated;
    for (const std::uintptr_t object : objects) {
        if (const auto target = check(object)) {
            validated.push_back(*target);
        }
    }
    std::scoped_lock lock(g_mutex);
    g_restore.insert(g_restore.end(), g_targets.begin(), g_targets.end());
    g_targets = std::move(validated);
    g_pending.store(true, std::memory_order_release);
    log::info("hide: {} object(s) hidden from the main view, {} to restore", g_targets.size(), g_restore.size());
}

void forget() {
    std::scoped_lock lock(g_mutex);
    const std::size_t dropped = g_targets.size() + g_restore.size();
    g_targets.clear();
    g_restore.clear();
    log::info("hide: forgot {} object(s) without touching them", dropped);
}

void on_frame() {
    const bool due = g_frames.fetch_add(1, std::memory_order_relaxed) % kCheckInterval == 0;
    if (!due && !g_pending.load(std::memory_order_acquire)) {
        return;
    }
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    std::scoped_lock lock(g_mutex);
    g_pending.store(false, std::memory_order_release);
    std::uint16_t flags = 0;
    for (const Target& target : g_restore) {
        if (still_valid(target, flags)) {
            set_main_view(target, true);
        }
    }
    g_restore.clear();
    std::erase_if(g_targets, [&flags](const Target& target) {
        if (!still_valid(target, flags)) {
            return true; // destroyed or replaced: drop it
        }
        if (flags & layout::kMainViewBits) {
            set_main_view(target, false);
        }
        return false;
    });
    if (!g_targets.empty()) {
        g_checks.fetch_add(1, std::memory_order_relaxed);
    }
    QueryPerformanceCounter(&end);
    g_ticks.fetch_add(static_cast<std::uint64_t>(end.QuadPart - start.QuadPart), std::memory_order_relaxed);
}

void report() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const std::uint64_t checks = g_checks.load(std::memory_order_relaxed);
    const double total_us =
        1e6 * static_cast<double>(g_ticks.load(std::memory_order_relaxed)) / static_cast<double>(frequency.QuadPart);
    log::info("hide: ToggleCullFlag600 called {} times so far (once per hide/show, plus each time the game restored "
              "the flags); {} checks, {:.1f} us total, {:.2f} us each",
              g_calls.load(std::memory_order_relaxed), checks, total_us,
              checks ? total_us / static_cast<double>(checks) : 0.0);
}

} // namespace bg3vr::cull
