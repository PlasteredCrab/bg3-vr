#include "bg3vr/fading.hpp"

#include "bg3vr/camera.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/stereo.hpp"
#include "bg3vr/vrplay.hpp"

#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <initializer_list>

namespace bg3vr::fading {

namespace {

// Agent AJ (AJ_roof_hiding.md), 4.1.1.7631656:
//   1. Obstruction fading: ecl::obstruction::RaycastSystem's update 0x18CF450 casts from the camera toward the
//      character and fades (to nothing, rounded) the roofs, ceilings, walls and FadeTrigger groups in between. It runs
//      only while GlobalSwitches+0x1CE ("Fading") is non-zero, and with it 0 it asks every active obstruction fade to
//      fade back in and starts no new ones. Only 0x18CF450 reads the byte, so it is set to 0 just around that call while
//      first person is on (the user's config is never touched). Picking then hits ceilings again too.
//   2. The see-through capsule (main view only): 0x17F2B80 builds the focus constants (b7) the SeeThrough materials cut
//      a hole with; with the view's two "active" bytes (+0x2CF8 / +0x2D30) at 0 the capsule shrinks to nothing.
// Third person (the view behind the character) keeps the game's fading: there the camera is outside again.
constexpr std::uintptr_t kGlobalSwitchesRva = 0x604B1B8;
constexpr std::size_t kFadingSwitch = 0x1CE;
constexpr std::uintptr_t kObstructionRva = 0x18CF450, kSeeThroughRva = 0x17F2B80;
constexpr std::size_t kSeeThroughActiveA = 0x2CF8, kSeeThroughActiveB = 0x2D30;

// Agent AW (AW_black_areas.md): the room black-out, stage 0x33's job 0x1816520. Every room-trigger volume whose room is
// not active (no party member inside, no open door joining it to one) goes into a screen mask, and EoCRoomTriggerBlur
// lerps the lit image to (0,0,0,1) under it, except at the far plane. With the camera at the eyes, a room above you
// blacks out everything seen through it (the 2026-10-23 15:09 helm ceiling). Part 2 (rooms): at 0x18167A5 the job
// tests the room count [rax+0x21EC]; pointing rax at a zero makes it take its own skip path 0x1817451, the one it
// takes in every dialogue. Part 1 (regions): at 0x18165F0 r13b = [view+0x2BFC]; 0 skips the region pass that writes
// black outside the player's region volume. Registers only, render worker thread, per job call.
constexpr std::uintptr_t kRoomCountTestRva = 0x18167A5, kRegionFlagRva = 0x18165F0;
constexpr std::size_t kRoomCount = 0x21EC;
const std::int32_t g_zero_rooms = 0;
safetyhook::MidHook g_rooms_hook, g_region_hook;
std::atomic<bool> g_rooms_visible{true};
std::atomic<std::uint64_t> g_room_jobs{0}, g_rooms_skipped{0}, g_region_jobs{0}, g_regions_skipped{0};
std::atomic<int> g_room_logs{0};

ModuleInfo g_module;
safetyhook::InlineHook g_obstruction_hook, g_see_through_hook;
std::atomic<bool> g_enabled{true};
std::atomic<std::uint64_t> g_suppressed{0}, g_holes_closed{0};
std::atomic<bool> g_logged_on{false};

bool in_first_person() {
    return vrplay::active() && !vrplay::third_person() && !camera::chase();
}

bool first_person() {
    return g_enabled.load(std::memory_order_relaxed) && in_first_person();
}

void on_room_count(safetyhook::Context& ctx) {
    std::int32_t rooms = 0;
    mem::try_copy(&rooms, ctx.rax + kRoomCount, sizeof(rooms));
    g_room_jobs.fetch_add(1, std::memory_order_relaxed);
    const bool skip = rooms > 0 && g_rooms_visible.load(std::memory_order_relaxed) && in_first_person();
    if (skip) {
        ctx.rax = reinterpret_cast<std::uintptr_t>(&g_zero_rooms) - kRoomCount;
        g_rooms_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    const int n = g_room_logs.load(std::memory_order_relaxed);
    if (rooms > 0 && n < 40 && g_room_logs.compare_exchange_strong(const_cast<int&>(n), n + 1)) {
        std::uintptr_t view = 0;
        mem::try_copy(&view, ctx.rsi - 0x18, sizeof(view));
        log::info("fading: room black-out: {} room volume(s) in view of 0x{:X} (eye {}){}", rooms, view, stereo::eye_of(view),
                  skip ? ": skipped in first person, rooms stay lit" : "");
    }
}

void on_region_flag(safetyhook::Context& ctx) {
    g_region_jobs.fetch_add(1, std::memory_order_relaxed);
    if ((ctx.r13 & 0xFF) != 0 && g_rooms_visible.load(std::memory_order_relaxed) && in_first_person()) {
        ctx.r13 &= ~std::uintptr_t{0xFF};
        if (g_regions_skipped.fetch_add(1, std::memory_order_relaxed) < 5) {
            log::info("fading: region black-out (outside the player's region) skipped in first person");
        }
    }
}

void on_obstruction(std::uintptr_t system, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    std::uintptr_t switches = 0;
    if (!first_person() || !mem::try_copy(&switches, g_module.base + kGlobalSwitchesRva, sizeof(switches)) || !switches) {
        g_logged_on.store(false, std::memory_order_relaxed);
        g_obstruction_hook.call<void>(system, a, b, c);
        return;
    }
    auto* fading = reinterpret_cast<std::uint8_t*>(switches + kFadingSwitch);
    const std::uint8_t saved = *fading;
    *fading = 0;
    g_obstruction_hook.call<void>(system, a, b, c);
    *fading = saved;
    g_suppressed.fetch_add(1, std::memory_order_relaxed);
    if (!g_logged_on.exchange(true, std::memory_order_relaxed)) {
        log::info("fading: first person: obstruction fading off (roofs, ceilings and walls fade back in; the game's "
                  "Fading switch was {})", saved);
    }
}

void on_see_through(std::uintptr_t view, std::uintptr_t recorder, std::uintptr_t a, std::uintptr_t b) {
    if (first_person() && view) {
        std::uint8_t active[2]{};
        mem::try_copy(&active[0], view + kSeeThroughActiveA, 1);
        mem::try_copy(&active[1], view + kSeeThroughActiveB, 1);
        if (active[0] || active[1]) {
            *reinterpret_cast<std::uint8_t*>(view + kSeeThroughActiveA) = 0;
            *reinterpret_cast<std::uint8_t*>(view + kSeeThroughActiveB) = 0;
            const std::uint64_t n = g_holes_closed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 3 || n % 10000 == 0) {
                log::info("fading: first person: the see-through hole of view 0x{:X} closed ({} times)", view, n);
            }
        }
    }
    g_see_through_hook.call<void>(view, recorder, a, b);
}

bool bytes_at(std::uintptr_t address, std::initializer_list<std::uint8_t> expected) {
    std::array<std::uint8_t, 32> actual{};
    if (expected.size() > actual.size() || !mem::try_copy(actual.data(), address, expected.size())) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), actual.begin());
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    g_enabled.store(config.get_bool("vr", "first_person_no_fading", true));
    const bool obstruction_ok = bytes_at(module.base + kObstructionRva, {0x40, 0x53, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20,
                                                                         0x48, 0x8B, 0xE9});
    const bool see_through_ok = bytes_at(module.base + kSeeThroughRva, {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x41, 0x56, 0x48, 0x8D,
                                                                        0x68, 0x98});
    if (obstruction_ok) {
        if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(module.base + kObstructionRva),
                                                    reinterpret_cast<void*>(&on_obstruction))) {
            g_obstruction_hook = std::move(*h);
        }
    }
    if (see_through_ok) {
        if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(module.base + kSeeThroughRva),
                                                    reinterpret_cast<void*>(&on_see_through))) {
            g_see_through_hook = std::move(*h);
        }
    }
    g_rooms_visible.store(config.get_bool("vr", "first_person_rooms_visible", true));
    if (bytes_at(module.base + kRoomCountTestRva, {0x83, 0xB8, 0xEC, 0x21, 0x00, 0x00, 0x00, 0x0F, 0x8E, 0x9F, 0x0C, 0x00, 0x00}) &&
        bytes_at(module.base + kRegionFlagRva, {0x44, 0x88, 0x6C, 0x24, 0x50, 0x45, 0x33, 0xFF, 0x45, 0x84, 0xED})) {
        if (auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(module.base + kRoomCountTestRva), &on_room_count)) {
            g_rooms_hook = std::move(*h);
        }
        if (auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(module.base + kRegionFlagRva), &on_region_flag)) {
            g_region_hook = std::move(*h);
        }
    }
    health::set("rooms lit in first person", g_rooms_hook && g_region_hook
                                                 ? (g_rooms_visible.load() ? health::State::Ready : health::State::Off)
                                                 : health::State::Unavailable,
                "[vr] first_person_rooms_visible");
    log::info("fading: unentered rooms {} in first person (room black-out exe+0x{:X} {}, region black-out exe+0x{:X} {})",
              g_rooms_visible.load() ? "stay lit" : "black out as the game does", kRoomCountTestRva,
              g_rooms_hook ? "hooked" : "NOT hooked", kRegionFlagRva, g_region_hook ? "hooked" : "NOT hooked");
    health::set("ceilings in first person", g_obstruction_hook ? (g_enabled.load() ? health::State::Ready : health::State::Off)
                                                               : health::State::Unavailable,
                "[vr] first_person_no_fading");
    log::info("fading: roofs and ceilings stay in first person ({}; obstruction update {}, see-through {})",
              g_enabled.load() ? "on" : "off", g_obstruction_hook ? "hooked" : "NOT hooked (build differs)",
              g_see_through_hook ? "hooked" : "NOT hooked (build differs)");
}

void configure(const Config& config) {
    g_enabled.store(config.get_bool("vr", "first_person_no_fading", true));
    g_rooms_visible.store(config.get_bool("vr", "first_person_rooms_visible", true));
}

void report() {
    log::info("fading: obstruction fading kept off {} times, see-through holes closed {} times; room black-out jobs {} "
              "({} skipped), region passes {} ({} skipped)",
              g_suppressed.load(), g_holes_closed.load(), g_room_jobs.load(), g_rooms_skipped.load(), g_region_jobs.load(),
              g_regions_skipped.load());
}

} // namespace bg3vr::fading
