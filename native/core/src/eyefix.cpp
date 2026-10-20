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
    g_serialize = config.get_bool("views", "serialize_eyes", true);
    g_shared_exposure = config.get_bool("views", "shared_exposure", true);
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
    log::info("eyes: exposure jobs {} ({} skipped for the second eye), eye views serialised {}",
              g_exposure_runs.load(std::memory_order_relaxed), g_exposure_skipped.load(std::memory_order_relaxed),
              g_linked_eye.load() ? "yes" : "no");
}

} // namespace bg3vr::eyefix
