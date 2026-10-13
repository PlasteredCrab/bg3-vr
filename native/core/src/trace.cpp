#include "bg3vr/trace.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/signatures.hpp"

#include <safetyhook.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <mutex>

namespace bg3vr::trace {

namespace {

struct Call {
    std::uintptr_t caller = 0;
    std::uintptr_t rcx = 0;
    std::uintptr_t rdx = 0;
    std::uint64_t count = 0;
};

struct Target {
    const char* signature;
    safetyhook::MidHook hook;
    std::atomic<std::uint64_t> calls{0};
    std::mutex mutex;
    std::array<Call, 24> seen{};
    std::size_t seen_count = 0;
    std::uint64_t dropped = 0; // calls whose combination didn't fit in `seen`
};

Target g_targets[] = {{"render_view_begin"}, {"render_view_end"}, {"game_pipeline_setup"}, {"render_pipeline_setup"}};
std::mutex g_install_mutex;
ModuleInfo g_module;

void record(Target& target, safetyhook::Context& ctx) {
    target.calls.fetch_add(1, std::memory_order_relaxed);
    std::uintptr_t caller = 0;
    mem::try_copy(&caller, ctx.rsp, sizeof(caller)); // at function entry: the return address
    std::scoped_lock lock(target.mutex);
    for (std::size_t i = 0; i < target.seen_count; ++i) {
        Call& call = target.seen[i];
        if (call.caller == caller && call.rcx == ctx.rcx && call.rdx == ctx.rdx) {
            ++call.count;
            return;
        }
    }
    if (target.seen_count < target.seen.size()) {
        target.seen[target.seen_count++] = {caller, ctx.rcx, ctx.rdx, 1};
    } else {
        ++target.dropped;
    }
}

// One callback per target (mid-hook callbacks carry no user data).
template <std::size_t I>
void on_call(safetyhook::Context& ctx) {
    record(g_targets[I], ctx);
}

constexpr safetyhook::MidHookFn kCallbacks[] = {&on_call<0>, &on_call<1>, &on_call<2>, &on_call<3>};

} // namespace

void start(const ModuleInfo& module) {
    std::scoped_lock install(g_install_mutex);
    g_module = module;
    for (std::size_t i = 0; i < std::size(g_targets); ++i) {
        Target& target = g_targets[i];
        {
            std::scoped_lock lock(target.mutex);
            target.seen_count = 0;
            target.dropped = 0;
        }
        target.calls.store(0, std::memory_order_relaxed);
        if (target.hook) {
            continue;
        }
        const auto site = sig::lookup(target.signature);
        if (!site) {
            log::warn("trace: signature {} not resolved", target.signature);
            continue;
        }
        auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(*site), kCallbacks[i]);
        if (!hook) {
            log::error("trace: hook {} at exe+0x{:X} failed (error type {})", target.signature, *site - module.base,
                       static_cast<int>(hook.error().type));
            continue;
        }
        target.hook = std::move(*hook);
        log::info("trace: tracing {} at exe+0x{:X}", target.signature, *site - module.base);
    }
}

void stop() {
    std::scoped_lock install(g_install_mutex);
    for (Target& target : g_targets) {
        target.hook = {};
    }
    log::info("trace: hooks removed");
}

void report() {
    for (Target& target : g_targets) {
        std::scoped_lock lock(target.mutex);
        log::info("trace: {}: {} call(s), {} distinct caller/rcx/rdx{}", target.signature,
                  target.calls.load(std::memory_order_relaxed), target.seen_count,
                  target.dropped ? std::format(" ({} calls didn't fit)", target.dropped) : std::string());
        for (std::size_t i = 0; i < target.seen_count; ++i) {
            const Call& call = target.seen[i];
            log::info("trace:   {}x from {} rcx 0x{:X} rdx 0x{:X}", call.count, mem::describe(g_module, call.caller),
                      call.rcx, call.rdx);
        }
    }
}

} // namespace bg3vr::trace
