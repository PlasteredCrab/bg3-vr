#include "bg3vr/weaponset.hpp"

#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/pattern.hpp"

#include <safetyhook.hpp>

#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <vector>

namespace bg3vr::weaponset {

namespace {

// 4.1.1.7631656, agent L (L_weapon_sets.md 1.3); prologues from bg3vr_disasm 2026-10-20.
struct Function {
    const char* name;
    std::uintptr_t rva;
    const char* prologue;
};
constexpr Function kUpdate{"esv::WeaponSetSystem update", 0x2B89930,
                           "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 D0 F8 FF FF 48 81 EC 30 08 00 00 48 8B 05 ?? ?? ?? ??"};
// Array<16-byte entry>::push (also called by the 0xEC handler at 0x306BBB0 on system+0x10).
constexpr Function kPush{"16-byte array push", 0x5BC780,
                         "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 8B 49 08 8B 43 0C 3B C8 7F ?? FF C0 8D 14 09 3B D0 0F 4C D0 3B CA 7D ?? 48 8B CB E8"};
constexpr std::size_t kRequests = 0x10;

struct Request {
    std::uint64_t entity;
    std::uint8_t set;
    std::uint8_t toggle;
    std::uint8_t pad[6];
};
static_assert(sizeof(Request) == 16);

using UpdateFn = void (*)(std::uintptr_t system, std::uintptr_t context);
using PushFn = void* (*)(std::uintptr_t array, const Request* entry);

ModuleInfo g_module;
safetyhook::InlineHook g_hook;
bool g_ready = false;
std::mutex g_mutex;
std::vector<Request> g_pending;
std::atomic<std::uint64_t> g_requested{0};
std::atomic<std::uint64_t> g_handed{0};
std::atomic<std::uint64_t> g_updates{0};

bool check(const Function& f) {
    const auto pattern = parse_pattern(f.prologue);
    std::array<std::uint8_t, 64> bytes{};
    if (!pattern || pattern->size() > bytes.size() ||
        !mem::try_copy(bytes.data(), g_module.base + f.rva, pattern->size())) {
        return false;
    }
    const auto at = find_first(std::span<const std::uint8_t>(bytes.data(), pattern->size()), *pattern);
    if (!at || *at != 0) {
        log::error("weapon set: {} at exe+0x{:X} doesn't have the expected prologue (game updated?)", f.name, f.rva);
        return false;
    }
    return true;
}

void update(std::uintptr_t system, std::uintptr_t context) {
    g_updates.fetch_add(1, std::memory_order_relaxed);
    std::vector<Request> pending;
    {
        std::scoped_lock lock(g_mutex);
        pending.swap(g_pending);
    }
    if (!pending.empty()) {
        const auto push = reinterpret_cast<PushFn>(g_module.base + kPush.rva);
        for (const Request& r : pending) {
            push(system + kRequests, &r);
            g_handed.fetch_add(1, std::memory_order_relaxed);
            log::info("weapon set: entity 0x{:016X} -> {} (handed to the server's weapon set system 0x{:X})", r.entity,
                      r.set ? "ranged" : "melee", system);
        }
    }
    g_hook.call<void>(system, context);
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    if (!config.get_bool("holster", "weapon_sets", true)) {
        health::set("weapon sets", health::State::Off, "[holster] weapon_sets");
        return;
    }
    if (!check(kUpdate) || !check(kPush)) {
        health::set("weapon sets", health::State::Unavailable, "a function didn't match");
        return;
    }
    auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(g_module.base + kUpdate.rva),
                                               reinterpret_cast<void*>(&update));
    if (!hook) {
        log::error("weapon set: couldn't hook the server's weapon set system (exe+0x{:X})", kUpdate.rva);
        health::set("weapon sets", health::State::Unavailable, "hook failed");
        return;
    }
    g_hook = std::move(*hook);
    g_ready = true;
    health::set("weapon sets", health::State::Ready);
    log::info("weapon set: hooked the server's weapon set system (exe+0x{:X}); holsters can pick melee or ranged", kUpdate.rva);
}

bool available() {
    return g_ready;
}

void request(std::uint64_t entity, std::uint8_t set) {
    if (!g_ready || !entity || set > 1) {
        log::warn("weapon set: request for 0x{:016X} set {} ignored ({})", entity, set, g_ready ? "bad arguments" : "unavailable");
        return;
    }
    {
        std::scoped_lock lock(g_mutex);
        g_pending.push_back({entity, set, 0, {}});
    }
    g_requested.fetch_add(1, std::memory_order_relaxed);
    log::info("weapon set: entity 0x{:016X} -> {} requested", entity, set ? "ranged" : "melee");
}

void report() {
    log::info("weapon set: {} ({} requested, {} handed over, {} server updates seen)", g_ready ? "ready" : "unavailable",
              g_requested.load(), g_handed.load(), g_updates.load());
}

} // namespace bg3vr::weaponset
