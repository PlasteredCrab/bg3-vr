#include "bg3vr/sharing.hpp"

#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/stereo.hpp"
#include "bg3vr/views.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace bg3vr::sharing {

namespace {

constexpr std::uintptr_t kBlendShapesRva = 0x3E15C50;  // (stage 0x41, sets)
constexpr std::uintptr_t kCullFeedbackRva = 0x3F7A510; // job run: (job) -> 0x3F7A700([job+0x10], job+0x20, [job+0x18])
constexpr std::uint8_t kBlendShapesBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18,
                                              0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55};
constexpr std::uint8_t kCullFeedbackBytes[] = {0x4C, 0x8B, 0x41, 0x18, 0x48, 0x8D, 0x51, 0x20, 0x48, 0x8B, 0x49, 0x10, 0xE9};

ModuleInfo g_module;
safetyhook::InlineHook g_blend_hook, g_feedback_hook;
std::atomic<bool> g_share_blend{true};
std::atomic<bool> g_skip_feedback{false};
std::atomic<std::uint64_t> g_blend_left{0}, g_blend_right{0}, g_blend_skipped{0};

// Cull feedback timing per (eye 0 / 1 / other, stage id).
std::mutex g_mutex;
struct Timing {
    double ms = 0.0;
    std::uint64_t calls = 0, skipped = 0;
};
std::array<std::array<Timing, 0x101>, 3> g_feedback{};
double g_qpc_ms = 0.0;

std::int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

std::uintptr_t view_of_stage(std::uintptr_t stage) {
    std::uintptr_t view = 0;
    if (stage) {
        mem::try_copy(&view, stage + 0x08, sizeof(view));
    }
    return view;
}

void on_blend_shapes(std::uintptr_t stage, std::uintptr_t sets) {
    const int eye = stereo::eye_of(view_of_stage(stage));
    if (eye == 1 && g_share_blend.load(std::memory_order_relaxed)) {
        g_blend_skipped.fetch_add(1, std::memory_order_relaxed);
        return; // the left eye's dispatches already filled these meshes' buffers this frame
    }
    (eye == 0 ? g_blend_left : g_blend_right).fetch_add(eye >= 0 ? 1 : 0, std::memory_order_relaxed);
    g_blend_hook.call<void>(stage, sets);
}

void on_cull_feedback(std::uintptr_t job) {
    std::uintptr_t stage = 0;
    mem::try_copy(&stage, job + 0x10, sizeof(stage));
    const int eye = stereo::eye_of(view_of_stage(stage));
    const int id = stage ? views::stage_id_of(stage) : -1;
    const std::size_t slot = eye == 0 ? 0 : eye == 1 ? 1 : 2;
    const std::size_t key = id >= 0 && id <= 0xFF ? static_cast<std::size_t>(id) : 0x100;
    if (eye == 1 && g_skip_feedback.load(std::memory_order_relaxed)) {
        std::scoped_lock lock(g_mutex);
        ++g_feedback[slot][key].skipped;
        return;
    }
    const std::int64_t t0 = qpc();
    g_feedback_hook.call<void>(job);
    const double ms = static_cast<double>(qpc() - t0) * g_qpc_ms;
    std::scoped_lock lock(g_mutex);
    Timing& t = g_feedback[slot][key];
    t.ms += ms;
    ++t.calls;
}

bool bytes_match(std::uintptr_t address, const std::uint8_t* expected, std::size_t size) {
    std::array<std::uint8_t, 32> actual{};
    return size <= actual.size() && mem::try_copy(actual.data(), address, size) && std::memcmp(actual.data(), expected, size) == 0;
}

} // namespace

void configure(const Config& config) {
    g_share_blend.store(config.get_bool("views", "share_blendshapes", true));
    g_skip_feedback.store(config.get_bool("views", "skip_eye_cull_feedback", false));
    log::info("sharing: right eye's blend shapes {}, right eye's cull feedback {}",
              g_share_blend.load() ? "taken from the left eye" : "its own", g_skip_feedback.load() ? "SKIPPED" : "run (timed)");
}

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_ms = 1000.0 / static_cast<double>(f.QuadPart);
    configure(config);
    std::string detail;
    if (bytes_match(module.base + kBlendShapesRva, kBlendShapesBytes, sizeof(kBlendShapesBytes))) {
        if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(module.base + kBlendShapesRva),
                                                    reinterpret_cast<void*>(&on_blend_shapes))) {
            g_blend_hook = std::move(*h);
        }
    }
    if (bytes_match(module.base + kCullFeedbackRva, kCullFeedbackBytes, sizeof(kCullFeedbackBytes))) {
        if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(module.base + kCullFeedbackRva),
                                                    reinterpret_cast<void*>(&on_cull_feedback))) {
            g_feedback_hook = std::move(*h);
        }
    }
    health::set("Eye sharing", g_blend_hook && g_feedback_hook ? health::State::Ready : health::State::Unavailable,
                std::format("blend shapes {}, cull feedback {}", g_blend_hook ? "hooked" : "differs", g_feedback_hook ? "hooked" : "differs"));
    log::info("sharing: blend shapes exe+0x{:X} {}, cull feedback exe+0x{:X} {}", kBlendShapesRva,
              g_blend_hook ? "hooked" : "NOT hooked (bytes differ)", kCullFeedbackRva,
              g_feedback_hook ? "hooked" : "NOT hooked (bytes differ)");
}

void report() {
    std::string text;
    {
        std::scoped_lock lock(g_mutex);
        constexpr const char* kNames[3] = {"left", "right", "other"};
        for (std::size_t s = 0; s < 3; ++s) {
            for (std::size_t k = 0; k < 0x101; ++k) {
                const Timing& t = g_feedback[s][k];
                if (t.calls || t.skipped) {
                    text += std::format("\n    {} stage {}: {} calls, {:.3f} ms each, {} skipped", kNames[s],
                                        k == 0x100 ? std::string("?") : std::format("0x{:02X}", k), t.calls,
                                        t.calls ? t.ms / static_cast<double>(t.calls) : 0.0, t.skipped);
                }
            }
        }
        g_feedback = {};
    }
    log::info("sharing: blend shapes run left {} / right {}, right skipped {}; cull feedback jobs (worker time):{}",
              g_blend_left.exchange(0), g_blend_right.exchange(0), g_blend_skipped.exchange(0), text.empty() ? " none" : text);
}

} // namespace bg3vr::sharing
