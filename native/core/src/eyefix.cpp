#include "bg3vr/eyefix.hpp"

#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/views.hpp"

#include <safetyhook.hpp>

#include <array>
#include <atomic>
#include <format>
#include <initializer_list>
#include <string>

namespace bg3vr::eyefix {

namespace {

ModuleInfo g_module;
bool g_serialize = true;
bool g_shared_exposure = true;

// Job graph (docs/re-notes/stereo.md; 0x3AC5169..0x3AC51A2 calls it the same way).
constexpr std::uintptr_t kAddEdgeRva = 0x41074A0;       // (sched, A, B): A waits for B
constexpr std::uintptr_t kSchedulerRva = 0x614FBC0;     // sched*
constexpr std::size_t kViewContext = 0x40;              // [view+0x40] = 17 counters
constexpr std::size_t kCounterStage47 = 0x58;           // the stage 0x47 counter (job dumps: stage 0 id 0x47)
constexpr std::size_t kCounterEndOfRecording = 0x78;    // what the special view's context waits on per view
constexpr std::size_t kCounterPredecessors = 0x268;     // 3 slots
constexpr std::size_t kExposureJobView = 0x18;          // view = [job - 0x18] (0x3EDE1A5)
using AddEdgeFn = void (*)(std::uintptr_t sched, std::uintptr_t waiter, std::uintptr_t waited_for);
using JobFn = void (*)(std::uintptr_t job, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c);

// The eye view's own per-object data slot (agent W 2.3-2.4): stage 0x47's execute 0x3E53DE0 stores the view's split
// index into stage+0x219C (`mov [rcx+0x219C], edx` at 0x3E53E01), and the writer (job 0x3E53FA0) and all five readers
// index the per-object slot with it. Both eyes render with index 0, so they wrote the same slot: the flicker the
// serialisation fixed by making the eye record only after the main view had finished (which kept the two views from
// recording in parallel). The eye now stores 1, the game's own second split-screen player slot.
constexpr std::uintptr_t kSlotStoreRva = 0x3E53E01;
constexpr std::size_t kStageView = 0x08;
safetyhook::MidHook g_slot_hook;
bool g_eye_slot = false; // installed
std::atomic<std::uint64_t> g_slot_stores{0};

// Stage 0x3E (texture / mesh streaming queries around the camera entity, agent W 2.7): the eye repeats the main view's
// queries; its execute 0x3E51D10 returns without submitting them.
constexpr std::uintptr_t kStreamingExecuteRva = 0x3E51D10;
safetyhook::InlineHook g_streaming_hook;
std::atomic<std::uint64_t> g_streaming_skipped{0};

safetyhook::InlineHook g_exposure_hook;
std::atomic<std::uint64_t> g_exposure_runs{0};
std::atomic<std::uint64_t> g_exposure_skipped{0};
std::atomic<std::uintptr_t> g_linked_eye{0};

std::uintptr_t peek(std::uintptr_t address) {
    std::uintptr_t value = 0;
    mem::try_copy(&value, address, sizeof(value));
    return value;
}

void exposure_job(std::uintptr_t job, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    g_exposure_runs.fetch_add(1, std::memory_order_relaxed);
    if (g_shared_exposure) {
        const std::uintptr_t view = peek(job - kExposureJobView);
        const auto eyes = views::eye_views();
        if (eyes && view && view == (*eyes)[1]) {
            g_exposure_skipped.fetch_add(1, std::memory_order_relaxed);
            return; // the second eye uses the exposure the main eye computed this frame
        }
    }
    g_exposure_hook.call<void>(job, a, b, c);
}

void on_slot_store(safetyhook::Context& ctx) {
    const std::uintptr_t view = peek(ctx.rcx + kStageView);
    const auto eyes = views::eye_views();
    if (eyes && view && view == (*eyes)[1]) {
        ctx.rdx = 1;
        g_slot_stores.fetch_add(1, std::memory_order_relaxed);
    }
}

void streaming_execute(std::uintptr_t stage, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    const std::uintptr_t view = peek(stage + kStageView);
    const auto eyes = views::eye_views();
    if (eyes && view && view == (*eyes)[1]) {
        g_streaming_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_streaming_hook.call<void>(stage, a, b, c);
}

bool bytes_at(std::uintptr_t address, std::initializer_list<std::uint8_t> expected) {
    std::size_t i = 0;
    for (const std::uint8_t byte : expected) {
        std::uint8_t actual = 0;
        if (!mem::try_copy(&actual, address + i++, 1) || actual != byte) {
            return false;
        }
    }
    return true;
}

std::string predecessors(std::uintptr_t counter) {
    std::string out;
    for (int i = 0; i < 3; ++i) {
        out += std::format("{}0x{:X}", i ? " " : "", peek(counter + kCounterPredecessors + 8 * i));
    }
    return out;
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    g_shared_exposure = config.get_bool("views", "shared_exposure", true);
    if (config.get_bool("views", "eye_slot", true)) {
        const std::uintptr_t site = module.base + kSlotStoreRva;
        // movsx edx, byte [rax+0x2268] ; mov [rcx+0x219C], edx
        if (bytes_at(site - 7, {0x0F, 0xBE, 0x90, 0x68, 0x22, 0x00, 0x00, 0x89, 0x91, 0x9C, 0x21, 0x00, 0x00})) {
            if (auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(site), &on_slot_store)) {
                g_slot_hook = std::move(*hook);
                g_eye_slot = true;
                log::info("eyes: the eye view's per-object data slot is 1 (stage 0x47, exe+0x{:X}); both eye views record "
                          "in parallel",
                          kSlotStoreRva);
            }
        }
        if (!g_eye_slot) {
            log::warn("eyes: the stage 0x47 slot store doesn't match this build; serialising the eye views instead");
        }
    }
    // With its own slot the eye needs no serialisation (the cost: it recorded only after the main view finished).
    g_serialize = config.get_bool("views", "serialize_eyes", !g_eye_slot);
    if (config.get_bool("views", "skip_eye_streaming", true)) {
        const std::uintptr_t site = module.base + kStreamingExecuteRva;
        if (bytes_at(site, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20})) {
            if (auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(site),
                                                           reinterpret_cast<void*>(&streaming_execute))) {
                g_streaming_hook = std::move(*hook);
                log::info("eyes: the eye view skips its streaming queries (stage 0x3E, exe+0x{:X}); the main view's cover "
                          "both",
                          kStreamingExecuteRva);
            }
        } else {
            log::warn("eyes: stage 0x3E's execute doesn't match this build; both views run their streaming queries");
        }
    }
    if (g_shared_exposure) {
        const auto site = sig::lookup("exposure_step_job");
        if (!site) {
            log::warn("eyes: signature exposure_step_job not resolved; each eye keeps its own exposure pass");
            health::set("shared exposure", health::State::Unavailable, "signature exposure_step_job");
        } else if (auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(*site),
                                                               reinterpret_cast<void*>(&exposure_job))) {
            g_exposure_hook = std::move(*hook);
            health::set("shared exposure", health::State::Ready);
            log::info("eyes: exposure step job hooked at exe+0x{:X}; the second eye uses the main eye's exposure",
                      *site - module.base);
        } else {
            health::set("shared exposure", health::State::Unavailable, "hook failed");
            log::error("eyes: exposure hook failed (error type {})", static_cast<int>(hook.error().type));
        }
    } else {
        health::set("shared exposure", health::State::Off, "[views] shared_exposure");
    }
    health::set("serialized eye views", g_serialize ? health::State::Ready : health::State::Off,
                g_serialize ? "" : "[views] serialize_eyes");
}

void link_eyes(std::uintptr_t main_view, std::uintptr_t eye_view) {
    if (!g_serialize || !main_view || !eye_view || g_linked_eye.load() == eye_view) {
        return;
    }
    const std::uintptr_t main_ctx = peek(main_view + kViewContext);
    const std::uintptr_t eye_ctx = peek(eye_view + kViewContext);
    const std::uintptr_t waiter = eye_ctx ? peek(eye_ctx + kCounterStage47) : 0;
    const std::uintptr_t waited_for = main_ctx ? peek(main_ctx + kCounterEndOfRecording) : 0;
    const std::uintptr_t sched = peek(g_module.base + kSchedulerRva);
    if (!waiter || !waited_for || !sched || main_ctx == eye_ctx) {
        log::warn("eyes: can't serialise the eye views (main ctx 0x{:X}, eye ctx 0x{:X}, counters 0x{:X} / 0x{:X}, "
                  "sched 0x{:X})",
                  main_ctx, eye_ctx, waiter, waited_for, sched);
        return;
    }
    // Room for a predecessor, and not there already (0x41074A0 silently drops a fourth).
    bool free_slot = false;
    for (int i = 0; i < 3; ++i) {
        const std::uintptr_t p = peek(waiter + kCounterPredecessors + 8 * i);
        if (p == waited_for) {
            log::info("eyes: the eye view's stage 0x47 already waits for the main view's recording");
            g_linked_eye.store(eye_view);
            return;
        }
        free_slot = free_slot || p == 0;
    }
    if (!free_slot) {
        log::warn("eyes: the eye view's stage 0x47 counter 0x{:X} has no free predecessor slot ({}); not serialised",
                  waiter, predecessors(waiter));
        return;
    }
    const std::string before = predecessors(waiter);
    reinterpret_cast<AddEdgeFn>(g_module.base + kAddEdgeRva)(sched, waiter, waited_for);
    g_linked_eye.store(eye_view);
    log::info("eyes: eye view 0x{:X} stage 0x47 counter 0x{:X} now waits for main view 0x{:X} end of recording 0x{:X} "
              "(predecessors {} -> {})",
              eye_view, waiter, main_view, waited_for, before, predecessors(waiter));
}

void report() {
    log::info("eyes: exposure jobs {} ({} skipped for the second eye), eye views serialised {}, eye slot {} ({} "
              "stores), eye streaming queries skipped {}",
              g_exposure_runs.load(std::memory_order_relaxed), g_exposure_skipped.load(std::memory_order_relaxed),
              g_linked_eye.load() ? "yes" : "no", g_eye_slot ? "1" : "shared", g_slot_stores.load(),
              g_streaming_skipped.load());
}

} // namespace bg3vr::eyefix
