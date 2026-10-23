#include "bg3vr/shadows.hpp"

#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/stereo.hpp"

#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <mutex>

namespace bg3vr::shadows {

namespace {

// Stage 0x02 (sun cascades, vtable 0x58C4E18), 4.1.1.7631656 (agent T 2.1): prepare (slot 5) and execute (slot 6)
// both take only the stage; [stage+0x08] = its view, [stage+0x18] = run this frame (the execute returns at once
// when 0).
constexpr std::uintptr_t kPrepareRva = 0x3E1A410;
constexpr std::uintptr_t kExecuteRva = 0x3E1B0C0;
constexpr std::size_t kView = 0x08;
constexpr std::size_t kRun = 0x18;
// The stage's CPU-side results the consumers (0x03 resolve, clustered constants, 0x1B, 0x00, 0x41) read.
constexpr std::size_t kCacheFlag = 0x21D8;   // byte
constexpr std::size_t kCacheData = 0x21E0;   // .. +0x240F: distant-cache records (world space)
constexpr std::size_t kCacheDataSize = 0x230;
constexpr std::size_t kCacheOn = 0x241B;     // byte
constexpr std::size_t kSplits = 0x2470;      // .. +0x24AF: split distances, cascade params, 2 x radius
constexpr std::size_t kSplitsSize = 0x40;
constexpr std::size_t kMatrices = 0x24B0;    // 4 x 16 floats, row-major; translation row relative to the view origin
constexpr std::size_t kCount = 0x2688;       // u32 cascades in use
constexpr std::size_t kControllerTranslateSlot = 9;
constexpr std::size_t kViewController = 0x21D8;

constexpr int kValidationFrames = 60;
constexpr float kTolerance = 2e-3f;

enum class State { Validating, Sharing, Rejected };

safetyhook::InlineHook g_prepare_hook;
safetyhook::InlineHook g_execute_hook;
std::atomic<bool> g_enabled{true};
std::atomic<State> g_state{State::Validating};
std::atomic<bool> g_eye_wanted{false}; // the right eye's prepare would have run the stage this frame

// The left eye's results, taken right after its execute (render thread; the right eye's execute follows on the
// same thread, so this needs no lock against it, only against report()).
struct Snapshot {
    std::uint64_t serial = 0;
    bool ran = false;
    float origin[3]{};
    std::uint8_t cache_flag = 0;
    std::array<std::uint8_t, kCacheDataSize> cache{};
    std::uint8_t cache_on = 0;
    std::array<float, kSplitsSize / 4> splits{};
    std::array<float, 64> matrices{};
    std::uint32_t count = 0;
};
std::mutex g_mutex; // the snapshot and the check state (the eyes' recording may run on different job threads)
Snapshot g_main;
std::uint64_t g_used_serial = 0;
std::uintptr_t g_eye_view_seen = 0;

int g_validated = 0;
float g_max_error = 0.0f;
std::atomic<std::uint64_t> g_shared{0};
std::atomic<std::uint64_t> g_skipped{0};
std::atomic<std::uint64_t> g_stale{0};
std::atomic<std::uint64_t> g_no_main{0};

bool view_origin(std::uintptr_t view, float (&out)[3]) {
    std::uintptr_t controller = 0, vtable = 0, slot = 0;
    if (!mem::try_copy(&controller, view + kViewController, sizeof(controller)) || !controller ||
        !mem::try_copy(&vtable, controller, sizeof(vtable)) ||
        !mem::try_copy(&slot, vtable + kControllerTranslateSlot * sizeof(void*), sizeof(slot)) || !slot) {
        return false;
    }
    using TranslateFn = const float* (*)(std::uintptr_t);
    const float* p = reinterpret_cast<TranslateFn>(slot)(controller);
    return p && mem::try_copy(out, reinterpret_cast<std::uintptr_t>(p), sizeof(out));
}

void take_snapshot(std::uintptr_t stage, std::uintptr_t view, bool ran) {
    Snapshot s;
    s.serial = g_main.serial + 1;
    s.ran = ran && view_origin(view, s.origin);
    const auto* base = reinterpret_cast<const std::uint8_t*>(stage);
    if (s.ran) {
        s.cache_flag = base[kCacheFlag];
        std::memcpy(s.cache.data(), base + kCacheData, kCacheDataSize);
        s.cache_on = base[kCacheOn];
        std::memcpy(s.splits.data(), base + kSplits, kSplitsSize);
        std::memcpy(s.matrices.data(), base + kMatrices, sizeof(s.matrices));
        std::memcpy(&s.count, base + kCount, sizeof(s.count));
    }
    g_main = s;
}

// The left eye's cascade matrices with their translation row moved to `origin` (agent T 2.4:
// r3 += d.x r0 + d.y r1 + d.z r2, d = origin - the left eye's origin).
std::array<float, 64> rebased(const float (&origin)[3]) {
    std::array<float, 64> m = g_main.matrices;
    const float d[3] = {origin[0] - g_main.origin[0], origin[1] - g_main.origin[1], origin[2] - g_main.origin[2]};
    for (int c = 0; c < 4; ++c) {
        float* r = m.data() + c * 16;
        for (int k = 0; k < 4; ++k) {
            r[12 + k] += d[0] * r[k] + d[1] * r[4 + k] + d[2] * r[8 + k];
        }
    }
    return m;
}

void validate(std::uintptr_t stage, std::uintptr_t view) {
    const auto* base = reinterpret_cast<const std::uint8_t*>(stage);
    if (!g_main.ran || g_main.serial == g_used_serial || base[kRun] == 0) {
        return;
    }
    g_used_serial = g_main.serial;
    float origin[3];
    if (!view_origin(view, origin)) {
        return;
    }
    const std::array<float, 64> expect = rebased(origin);
    float own[64];
    float splits[kSplitsSize / 4];
    std::uint32_t count = 0;
    std::memcpy(own, base + kMatrices, sizeof(own));
    std::memcpy(splits, base + kSplits, sizeof(splits));
    std::memcpy(&count, base + kCount, sizeof(count));
    float error = 0.0f;
    const std::uint32_t used = std::min<std::uint32_t>(count, 4);
    for (std::uint32_t i = 0; i < used * 16; ++i) {
        error = std::max(error, std::abs(own[i] - expect[i]) / std::max(1.0f, std::abs(own[i])));
    }
    for (std::size_t i = 0; i < std::size(splits); ++i) {
        error = std::max(error, std::abs(splits[i] - g_main.splits[i]) / std::max(1.0f, std::abs(splits[i])));
    }
    if (count != g_main.count) {
        error = std::max(error, 1.0f);
    }
    g_max_error = std::max(g_max_error, error);
    ++g_validated;
    if (g_validated <= 3 || error > kTolerance) {
        log::info("shadows: check {}: {} cascades (left eye {}), largest difference between the right eye's own cascades "
                  "and the left eye's re-based ones {:.2e}; origins left ({:.3f}, {:.3f}, {:.3f}) right ({:.3f}, {:.3f}, "
                  "{:.3f}); right eye matrix 0 rows 0/3 ({:.4f} {:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} {:.4f} {:.4f}) re-based "
                  "row 3 ({:.4f} {:.4f} {:.4f} {:.4f})",
                  g_validated, count, g_main.count, error, g_main.origin[0], g_main.origin[1], g_main.origin[2], origin[0],
                  origin[1], origin[2], own[0], own[1], own[2], own[3], own[12], own[13], own[14], own[15], expect[12],
                  expect[13], expect[14], expect[15]);
    }
    if (g_validated >= kValidationFrames) {
        const bool ok = g_max_error <= kTolerance;
        g_state = ok ? State::Sharing : State::Rejected;
        log::info("shadows: {} frames checked, largest difference {:.2e}: {}", g_validated, g_max_error,
                  ok ? "the right eye now reuses the left eye's sun shadows (its own cascade pass is skipped)"
                     : "the eyes' cascades differ more than expected; each eye keeps rendering its own");
    }
}

// The right eye gets the left eye's results instead of running the stage.
void share(std::uintptr_t stage, std::uintptr_t view) {
    if (!g_main.ran) {
        g_no_main.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_main.serial == g_used_serial) {
        g_stale.fetch_add(1, std::memory_order_relaxed); // the left eye didn't run again since: copy what it has
    }
    g_used_serial = g_main.serial;
    float origin[3];
    if (!view_origin(view, origin)) {
        return;
    }
    const std::array<float, 64> m = rebased(origin);
    auto* base = reinterpret_cast<std::uint8_t*>(stage);
    base[kCacheFlag] = g_main.cache_flag;
    std::memcpy(base + kCacheData, g_main.cache.data(), kCacheDataSize);
    base[kCacheOn] = g_main.cache_on;
    std::memcpy(base + kSplits, g_main.splits.data(), kSplitsSize);
    std::memcpy(base + kMatrices, m.data(), sizeof(m));
    std::memcpy(base + kCount, &g_main.count, sizeof(g_main.count));
    g_shared.fetch_add(1, std::memory_order_relaxed);
}

void on_prepare(std::uintptr_t stage) {
    g_prepare_hook.call<void>(stage);
    std::uintptr_t view = 0;
    mem::try_copy(&view, stage + kView, sizeof(view));
    if (stereo::eye_of(view) != 1) {
        return;
    }
    auto* run = reinterpret_cast<std::uint8_t*>(stage + kRun);
    const bool wanted = *run != 0;
    g_eye_wanted.store(wanted, std::memory_order_relaxed);
    if (wanted && g_enabled.load(std::memory_order_relaxed) && g_state.load(std::memory_order_relaxed) == State::Sharing) {
        *run = 0; // the game's own "no sun" path for this view: no culls, cascade renders, cache update or skinning
        g_skipped.fetch_add(1, std::memory_order_relaxed);
    }
}

void on_execute(std::uintptr_t stage) {
    std::uintptr_t view = 0;
    mem::try_copy(&view, stage + kView, sizeof(view));
    const int eye = stereo::eye_of(view);
    if (eye == 0) {
        const bool ran = *reinterpret_cast<const std::uint8_t*>(stage + kRun) != 0;
        g_execute_hook.call<void>(stage);
        std::scoped_lock lock(g_mutex);
        take_snapshot(stage, view, ran);
        return;
    }
    if (eye != 1) {
        g_execute_hook.call<void>(stage);
        return;
    }
    std::unique_lock lock(g_mutex);
    if (view != g_eye_view_seen) {
        // A new right-eye view (stereo turned on again): check again before sharing.
        g_eye_view_seen = view;
        g_validated = 0;
        g_max_error = 0.0f;
        g_state = State::Validating;
    }
    const State state = g_state.load(std::memory_order_relaxed);
    if (state == State::Sharing && g_enabled.load(std::memory_order_relaxed)) {
        if (g_eye_wanted.load(std::memory_order_relaxed)) {
            share(stage, view);
        }
        return; // the run flag is off: the original would return at once
    }
    lock.unlock();
    g_execute_hook.call<void>(stage);
    if (state == State::Validating && g_enabled.load(std::memory_order_relaxed)) {
        lock.lock();
        validate(stage, view);
    }
}

bool bytes_at(std::uintptr_t address, std::initializer_list<std::uint8_t> expected) {
    std::size_t i = 0;
    for (const std::uint8_t b : expected) {
        std::uint8_t actual = 0;
        if (!mem::try_copy(&actual, address + i++, 1) || actual != b) {
            return false;
        }
    }
    return true;
}

} // namespace

void configure(const Config& config) {
    const bool on = config.get_bool("views", "share_shadows", true);
    if (g_enabled.exchange(on) != on) {
        log::info("shadows: sharing the sun shadows between the eyes {}", on ? "on" : "off");
    }
}

void install(const Config& config, const ModuleInfo& module) {
    configure(config);
    const std::uintptr_t prepare = module.base + kPrepareRva;
    const std::uintptr_t execute = module.base + kExecuteRva;
    if (!bytes_at(prepare, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9}) ||
        !bytes_at(execute, {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0x60, 0x02, 0x00, 0x00, 0x80, 0x79, 0x18, 0x00})) {
        health::set("shared sun shadows", health::State::Unavailable, "stage 0x02 functions differ in this build");
        log::warn("shadows: the sun shadow stage's prepare / execute don't match this build; each eye renders its own");
        return;
    }
    auto p = safetyhook::InlineHook::create(reinterpret_cast<void*>(prepare), reinterpret_cast<void*>(&on_prepare));
    auto e = safetyhook::InlineHook::create(reinterpret_cast<void*>(execute), reinterpret_cast<void*>(&on_execute));
    if (!p || !e) {
        health::set("shared sun shadows", health::State::Unavailable, "hook failed");
        log::warn("shadows: hooking the sun shadow stage failed; each eye renders its own");
        return;
    }
    g_prepare_hook = std::move(*p);
    g_execute_hook = std::move(*e);
    health::set("shared sun shadows", g_enabled.load() ? health::State::Ready : health::State::Off, "[views] share_shadows");
    log::info("shadows: sun shadow stage hooked (prepare exe+0x{:X}, execute exe+0x{:X}); {}", kPrepareRva, kExecuteRva,
              g_enabled.load() ? "the right eye will reuse the left eye's cascades after the first 60 frames check out"
                               : "off ([views] share_shadows)");
}

void report() {
    const State state = g_state.load();
    log::info("shadows: {}; {} right-eye cascade passes skipped, {} shared copies ({} from a repeated left-eye frame, {} "
              "with no left-eye cascades)",
              !g_enabled.load()                ? "off"
              : state == State::Sharing        ? "sharing"
              : state == State::Rejected       ? "rejected after the check"
                                               : "checking",
              g_skipped.load(), g_shared.load(), g_stale.load(), g_no_main.load());
}

} // namespace bg3vr::shadows
