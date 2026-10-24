#include "bg3vr/dice.hpp"

#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <mutex>

namespace bg3vr::dice {

namespace {

// 4.1.1.7631656, agent AQ.
constexpr std::uintptr_t kUpdateRva = 0x1548FC0;   // gui::DCActiveRoll update (dc, ctx)
constexpr std::uintptr_t kNextPhaseRva = 0x15475D0; // UserTriggeredNextPhaseCommand handler (dc, 0, BaseComponent*)
constexpr std::size_t kPhase = 0x7A8;               // u32
constexpr std::size_t kRolled1 = 0x838, kRolled2 = 0x850, kFinal = 0x880, kTarget = 0x898, kSuccess = 0x8E0; // u8
constexpr std::size_t kCanRespond = 0x928;           // u8
constexpr std::size_t kTotal = 0xD58;                // u8
constexpr std::size_t kRollEntity = 0xCC8;           // u64

using UpdateFn = void (*)(std::uintptr_t dc, std::uintptr_t ctx);
using NextPhaseFn = void (*)(std::uintptr_t dc, std::uintptr_t zero, std::uintptr_t component);

ModuleInfo g_module;
safetyhook::InlineHook g_update_hook;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_roll_request{false};
std::atomic<bool> g_skip_request{false};
std::atomic<std::uint64_t> g_rolls_sent{0}, g_skips_sent{0}, g_screens{0};

std::mutex g_mutex;
std::optional<Roll> g_roll;          // under g_mutex
std::uintptr_t g_dc = 0;             // the view-model seen last (UI update thread)
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
            log::info("dice: roll screen 0x{:X} (roll entity 0x{:016X}) opened, phase {}, needs {} on the d20, thread {}", dc,
                      entity, phase, r.target, GetCurrentThreadId());
        } else if (before) {
            r.serial = before->serial;
        }
        g_roll = r;
        g_last_update = std::chrono::steady_clock::now();
    }
    if (before && before->phase != r.phase && before->serial == r.serial) {
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

} // namespace

void configure(const Config& config) {
    g_enabled.store(config.get_bool("dice", "physical", true), std::memory_order_relaxed);
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
    health::set("physical dice", g_update_hook ? (g_enabled.load() ? health::State::Ready : health::State::Off)
                                               : health::State::Unavailable,
                "[dice] physical");
    log::info("dice: following the roll screen (update exe+0x{:X}, Roll command exe+0x{:X}); physical dice {}", kUpdateRva,
              kNextPhaseRva, g_enabled.load() ? "on" : "off");
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

void report() {
    log::info("dice: {} roll screens, {} rolls and {} skips sent by the physical die", g_screens.load(), g_rolls_sent.load(),
              g_skips_sent.load());
}

} // namespace bg3vr::dice
