#include "bg3vr/events.hpp"

#include "bg3vr/gpu_trace.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/views.hpp"

#include <safetyhook.hpp>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <iterator>
#include <mutex>

namespace bg3vr::events {

namespace {

ModuleInfo g_module;
std::mutex g_install_mutex;
std::atomic<bool> g_armed{false};
std::atomic<std::uint64_t> g_start_frame{0};
std::atomic<int> g_frames{0};
std::atomic<std::int64_t> g_budget{0};
// The render thread (the one that begins recording): job submits and waits are logged only there, or the
// game thread's own jobs use up the budget.
std::atomic<DWORD> g_render_thread{0};

bool on_render_thread() {
    return GetCurrentThreadId() == g_render_thread.load(std::memory_order_relaxed);
}

struct Site {
    const char* signature;
    safetyhook::MidHook hook;
};

// Returns the frame number (since arming) if this event should be logged.
bool take(std::uint64_t& frame) {
    if (!g_armed.load(std::memory_order_relaxed)) {
        return false;
    }
    frame = render::frame_count() - g_start_frame.load(std::memory_order_relaxed);
    if (frame > static_cast<std::uint64_t>(g_frames.load(std::memory_order_relaxed)) ||
        g_budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        if (g_armed.exchange(false)) {
            log::info("events: done (frame {}, budget left {})", frame, g_budget.load());
        }
        return false;
    }
    return true;
}

std::uintptr_t return_address(const safetyhook::Context& ctx) {
    std::uintptr_t caller = 0;
    mem::try_copy(&caller, ctx.rsp, sizeof(caller));
    return caller;
}

// render_view_begin / render_view_end entries: rcx = view.
void on_begin(safetyhook::Context& ctx) {
    g_render_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    gpu_trace::marker(std::format("begin-recording view 0x{:X} ({})", ctx.rcx, views::view_role(ctx.rcx)));
    std::uint64_t frame;
    if (take(frame)) {
        log::info("events: f{} t{} begin-recording view 0x{:X} ({}) from {}", frame, GetCurrentThreadId(), ctx.rcx,
                  views::view_role(ctx.rcx), mem::describe(g_module, return_address(ctx)));
    }
}
void on_end(safetyhook::Context& ctx) {
    gpu_trace::marker(std::format("end-recording view 0x{:X} ({})", ctx.rcx, views::view_role(ctx.rcx)));
    std::uint64_t frame;
    if (take(frame)) {
        log::info("events: f{} t{} end-recording view 0x{:X} ({}) from {}", frame, GetCurrentThreadId(), ctx.rcx,
                  views::view_role(ctx.rcx), mem::describe(g_module, return_address(ctx)));
    }
}
// 0x407B540 entry: rcx = view; arms and releases its job context.
void on_release(safetyhook::Context& ctx) {
    std::uint64_t frame;
    if (take(frame)) {
        log::info("events: f{} t{} release jobs of view 0x{:X} ({})", frame, GetCurrentThreadId(), ctx.rcx,
                  views::view_role(ctx.rcx));
    }
}
// 0x3CC7050 in end-recording: rbx = the stage slot about to have its slot 8 (wait) called.
void on_stage_wait(safetyhook::Context& ctx) {
    std::uintptr_t stage = 0, view = 0, counter = 0, vtable = 0, wait = 0;
    std::uint8_t async = 0;
    mem::try_copy(&stage, ctx.rbx, sizeof(stage));
    mem::try_copy(&view, stage + 0x08, sizeof(view));
    gpu_trace::marker(std::format("stage 0x{:02X} of view 0x{:X} ({}) waits", views::stage_id_of(stage), view, views::view_role(view)));
    std::uint64_t frame;
    if (!take(frame)) {
        return;
    }
    mem::try_copy(&counter, stage + 0x2138, sizeof(counter));
    mem::try_copy(&async, stage + 0x18, 1);
    mem::try_copy(&vtable, stage, sizeof(vtable));
    mem::try_copy(&wait, vtable + 8 * 8, sizeof(wait));
    log::info("events: f{} t{} stage 0x{:02X} 0x{:X} of view 0x{:X} ({}) wait {} async {} counter [{}] {}", frame,
              GetCurrentThreadId(), views::stage_id_of(stage), stage, view, views::view_role(view),
              mem::describe(g_module, wait), async, views::counter_name(counter), views::describe_counter(counter));
}
// 0x410E5D0 entry: rcx = counter, rdx = job.
void on_submit(safetyhook::Context& ctx) {
    std::uint64_t frame;
    if (!on_render_thread() || !take(frame)) {
        return;
    }
    std::uintptr_t job_vtable = 0;
    mem::try_copy(&job_vtable, ctx.rdx, sizeof(job_vtable));
    log::info("events: f{} t{} submit job 0x{:X} (vtable {}) from {} to [{}] {}", frame, GetCurrentThreadId(), ctx.rdx,
              mem::describe(g_module, job_vtable), mem::describe(g_module, return_address(ctx)),
              views::counter_name(ctx.rcx), views::describe_counter(ctx.rcx));
}
// 0x410E880 entry: rcx = counter, dl = help run jobs while waiting.
void on_wait(safetyhook::Context& ctx) {
    std::uint64_t frame;
    if (!on_render_thread() || !take(frame)) {
        return;
    }
    log::info("events: f{} t{} wait (help {}) from {} on [{}] {}", frame, GetCurrentThreadId(), ctx.rdx & 0xFF,
              mem::describe(g_module, return_address(ctx)), views::counter_name(ctx.rcx),
              views::describe_counter(ctx.rcx));
}

Site g_sites[] = {{"render_view_begin"}, {"render_view_end"}, {"view_release_jobs"},
                  {"stage_wait_call"},   {"job_submit"},      {"job_wait"}};
constexpr safetyhook::MidHookFn kCallbacks[] = {&on_begin, &on_end, &on_release, &on_stage_wait, &on_submit, &on_wait};

} // namespace

void arm(const ModuleInfo& module, int frames, std::size_t max_events) {
    {
        std::scoped_lock install(g_install_mutex);
        g_module = module;
        for (std::size_t i = 0; i < std::size(g_sites); ++i) {
            Site& site = g_sites[i];
            if (site.hook) {
                continue;
            }
            const auto address = sig::lookup(site.signature);
            if (!address) {
                log::warn("events: signature {} not resolved", site.signature);
                continue;
            }
            auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(*address), kCallbacks[i]);
            if (!hook) {
                log::error("events: hook {} at exe+0x{:X} failed (error type {})", site.signature, *address - module.base,
                           static_cast<int>(hook.error().type));
                continue;
            }
            site.hook = std::move(*hook);
            log::info("events: hooked {} at exe+0x{:X}", site.signature, *address - module.base);
        }
    }
    g_start_frame.store(render::frame_count());
    g_frames.store(frames);
    g_budget.store(static_cast<std::int64_t>(max_events));
    g_armed.store(true);
    log::info("events: logging the next {} frame(s), at most {} events", frames, max_events);
}

std::atomic<bool> g_auto{false};

void set_auto(bool on) {
    g_auto.store(on);
}

bool auto_on() {
    return g_auto.load();
}

void arm_auto(const ModuleInfo& module, int frames, std::size_t max_events) {
    if (g_auto.load()) {
        arm(module, frames, max_events);
    }
}

void disarm() {
    g_armed.store(false);
}

} // namespace bg3vr::events
