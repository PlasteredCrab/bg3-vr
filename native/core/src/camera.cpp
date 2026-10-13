#include "bg3vr/camera.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/signatures.hpp"

#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace bg3vr::camera {

namespace {

// State shared between the bridge thread and the game's camera update.
std::mutex g_mutex;
std::uintptr_t g_controller = 0;
std::optional<Pose> g_pose;
std::atomic<FrameCallback> g_frame_callback{nullptr};

std::atomic<std::uint64_t> g_view_calls{0};
std::uint64_t g_last_reported_calls = 0;
std::atomic<std::uint64_t> g_transform_writes{0};
std::atomic<std::uint64_t> g_view_checks{0};
std::atomic<float> g_view_max_error{0.0f};
bool g_force_view = false;

safetyhook::MidHook g_view_update_hook;
safetyhook::MidHook g_set_view_hook;

ModuleInfo g_module;

// Every controller that passes through the view setter, with call counts (fixed table, no locks).
constexpr std::size_t kTrackedControllers = 16;
std::array<std::atomic<std::uintptr_t>, kTrackedControllers> g_seen_controllers{};
std::array<std::atomic<std::uint64_t>, kTrackedControllers> g_seen_counts{};

void track_controller(std::uintptr_t controller) {
    for (std::size_t i = 0; i < kTrackedControllers; ++i) {
        std::uintptr_t current = g_seen_controllers[i].load(std::memory_order_acquire);
        if (current == 0) {
            std::uintptr_t expected = 0;
            if (g_seen_controllers[i].compare_exchange_strong(expected, controller)) {
                current = controller;
            } else {
                current = expected;
            }
        }
        if (current == controller) {
            g_seen_counts[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

std::optional<Pose> pose_for(std::uintptr_t controller) {
    std::scoped_lock lock(g_mutex);
    if (!g_pose || controller != g_controller) {
        return std::nullopt;
    }
    return g_pose;
}

// Entry of the view update: rcx = controller. Write the pose into the camera's Transform.
void on_view_update(safetyhook::Context& ctx) {
    if (const FrameCallback callback = g_frame_callback.load(std::memory_order_acquire)) {
        callback();
    }
    const auto pose = pose_for(ctx.rcx);
    if (!pose) {
        return;
    }
    using GetTransformFn = float* (*)(std::uintptr_t);
    const auto vtable = mem::read<std::uintptr_t>(ctx.rcx);
    const auto get_transform = mem::read<GetTransformFn>(vtable + layout::kGetWorldTransformSlot * sizeof(void*));
    float* transform = get_transform(ctx.rcx);
    if (!transform) {
        return;
    }
    const auto q = pose_quaternion(*pose);
    std::memcpy(transform, q.data(), sizeof(q));
    const float translate[3] = {pose->x, pose->y, pose->z};
    std::memcpy(reinterpret_cast<std::uint8_t*>(transform) + layout::kTransformTranslate, translate, sizeof(translate));
    g_transform_writes.fetch_add(1, std::memory_order_relaxed);
}

// At the call to the matrix setter: rbx = controller, rdx = the view matrix the game just built.
void on_set_view(safetyhook::Context& ctx) {
    g_view_calls.fetch_add(1, std::memory_order_relaxed);
    track_controller(ctx.rbx);
    const auto pose = pose_for(ctx.rbx);
    if (!pose) {
        return;
    }
    Mat4 world, expected;
    pose_matrices(*pose, world, expected);
    float* built = reinterpret_cast<float*>(ctx.rdx);
    float error = 0.0f;
    for (std::size_t i = 0; i < 16; ++i) {
        error = std::max(error, std::abs(built[i] - expected[i]));
    }
    g_view_checks.fetch_add(1, std::memory_order_relaxed);
    if (error > g_view_max_error.load(std::memory_order_relaxed)) {
        g_view_max_error.store(error, std::memory_order_relaxed);
    }
    if (g_force_view) {
        std::memcpy(built, expected.data(), sizeof(expected));
    }
}

bool install_mid_hook(const char* signature, safetyhook::MidHookFn callback, safetyhook::MidHook& out) {
    const auto site = sig::lookup(signature);
    if (!site) {
        log::error("camera: signature {} not resolved", signature);
        return false;
    }
    auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(*site), callback);
    if (!hook) {
        log::error("camera: mid-hook {} at exe+0x{:X} failed (error type {})", signature, *site - g_module.base,
                   static_cast<int>(hook.error().type));
        return false;
    }
    out = std::move(*hook);
    log::info("camera: hook {} installed at exe+0x{:X}", signature, *site - g_module.base);
    return true;
}

bool looks_like_controller(std::uintptr_t controller) {
    if (!mem::readable(controller, layout::kSize)) {
        log::warn("camera: 0x{:X} is not readable memory", controller);
        return false;
    }
    const auto vtable = mem::read<std::uintptr_t>(controller);
    if (!mem::in_section(g_module, ".rdata", vtable, layout::kVtableSlots * sizeof(void*))) {
        log::warn("camera: 0x{:X} has vtable 0x{:X} outside .rdata; not a controller", controller, vtable);
        return false;
    }
    const auto get_transform = mem::read<std::uintptr_t>(vtable + layout::kGetWorldTransformSlot * sizeof(void*));
    if (!mem::in_section(g_module, ".text", get_transform)) {
        log::warn("camera: vtable slot {} = 0x{:X} is outside .text", layout::kGetWorldTransformSlot, get_transform);
        return false;
    }

    const float fov = mem::read<float>(controller + layout::kFov);
    const float near_plane = mem::read<float>(controller + layout::kNearPlane);
    const float aspect = mem::read<float>(controller + layout::kAspectRatio);
    const auto inv_view = mem::read<Mat4>(controller + layout::kInvViewMatrix);
    log::info("camera: controller 0x{:X} vtable {} GetWorldTransform {}", controller, mem::describe(g_module, vtable),
              mem::describe(g_module, get_transform));
    log::info("camera: fov {:.4f} rad ({:.1f} deg) near {:.3f} aspect {:.3f} pos ({:.3f}, {:.3f}, {:.3f})", fov,
              fov * 57.29578f, near_plane, aspect, inv_view[12], inv_view[13], inv_view[14]);

    if (!(fov > 0.1f && fov < 3.2f) || !(near_plane > 0.0f && near_plane < 10.0f) || !(aspect > 0.5f && aspect < 5.0f)) {
        log::warn("camera: field values don't match the expected layout; ignoring this controller");
        return false;
    }
    return true;
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    g_force_view = config.get_bool("camera", "force_view", false);
    install_mid_hook("camera_view_update", &on_view_update, g_view_update_hook);
    install_mid_hook("camera_set_view_call", &on_set_view, g_set_view_hook);
}

bool attach(std::uintptr_t controller) {
    const bool valid = looks_like_controller(controller);
    std::scoped_lock lock(g_mutex);
    g_controller = valid ? controller : 0;
    return valid;
}

std::uintptr_t controller() {
    std::scoped_lock lock(g_mutex);
    return g_controller;
}

void set_pose(std::optional<Pose> pose) {
    std::scoped_lock lock(g_mutex);
    g_pose = pose;
    if (!pose) {
        log::info("camera: pose override off");
    } else {
        log::info("camera: pose override ({:.2f}, {:.2f}, {:.2f}) yaw {:.1f} pitch {:.1f}{}", pose->x, pose->y,
                  pose->z, pose->yaw_deg, pose->pitch_deg, g_controller ? "" : " (no controller attached yet)");
    }
}

void set_frame_callback(FrameCallback callback) {
    g_frame_callback.store(callback, std::memory_order_release);
}

void report() {
    const std::uint64_t calls = g_view_calls.load(std::memory_order_relaxed);
    log::info("camera: view set {} times since the last report; transform written {} times; built view checked {} "
              "times, max error {:.6f}{}",
              calls - g_last_reported_calls, g_transform_writes.load(std::memory_order_relaxed),
              g_view_checks.load(std::memory_order_relaxed), g_view_max_error.load(std::memory_order_relaxed),
              g_force_view ? " (force_view on)" : "");
    g_view_max_error.store(0.0f, std::memory_order_relaxed);
    g_last_reported_calls = calls;

    const std::uintptr_t driven = controller();
    for (std::size_t i = 0; i < kTrackedControllers; ++i) {
        const std::uintptr_t seen = g_seen_controllers[i].load(std::memory_order_acquire);
        if (seen == 0) {
            break;
        }
        if (!mem::readable(seen, layout::kSize)) {
            log::info("camera:   controller 0x{:X} (gone) views set {}", seen,
                      g_seen_counts[i].load(std::memory_order_relaxed));
            continue;
        }
        const auto vtable = mem::read<std::uintptr_t>(seen);
        const auto inv_view = mem::read<Mat4>(seen + layout::kInvViewMatrix);
        log::info("camera:   controller 0x{:X} vtable {} views set {} ortho {} fov {:.3f} near {:.3f} pos "
                  "({:.2f}, {:.2f}, {:.2f}) fwd ({:.2f}, {:.2f}, {:.2f}){}",
                  seen, mem::describe(g_module, vtable), g_seen_counts[i].load(std::memory_order_relaxed),
                  mem::read<std::uint8_t>(seen + layout::kIsOrthographic), mem::read<float>(seen + layout::kFov),
                  mem::read<float>(seen + layout::kNearPlane), inv_view[12], inv_view[13], inv_view[14], inv_view[8],
                  inv_view[9], inv_view[10], seen == driven ? "  <- driven" : "");
    }
}

} // namespace bg3vr::camera
