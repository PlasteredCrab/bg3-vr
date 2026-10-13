#include "bg3vr/camera.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/vr_math.hpp"

#include <safetyhook.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <numbers>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

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

// Mouse look (a desktop stand-in for head rotation): while a pose is held, dragging with the
// middle mouse button turns it. State touched only on the game thread, under g_mutex.
bool g_mouselook = true;
float g_mouselook_sensitivity = 0.15f; // degrees per pixel
bool g_dragging = false;
POINT g_last_cursor{};

// Automatic look-around: the view sweeps around the pose it started from (g_sweep_base), as
// yaw = base + A*sin(2*pi*t/T), pitch = base + B*sin(4*pi*t/T), so a cycle pans left-right once and
// nods twice. Computed from the base every frame, so it never drifts. Under g_mutex.
std::optional<Sweep> g_sweep;
Pose g_sweep_base;
std::chrono::steady_clock::time_point g_sweep_start;

// Per-eye projection override. The game rebuilds the projection only when ProjectionDirty is set,
// so a change sets g_projection_dirty and the next view update of the driven camera marks the
// controller dirty (on the game thread). Under g_mutex.
std::optional<EyeFov> g_eye_fov;
bool g_projection_dirty = false;
std::atomic<std::uint64_t> g_projection_overrides{0};

// Head tracking: the latest located view (set every frame by the headset), the pose last written into
// the Transform (what the setter check compares against), and the history of applied head views.
// Under g_mutex.
std::optional<HeadView> g_head;
float g_world_scale = 1.0f;
// Recentering origin in game convention: head views are taken relative to it (yaw only).
vr::Vec3 g_recenter_position{0.0f, 0.0f, 0.0f};
float g_recenter_yaw_deg = 0.0f;
std::optional<vr::RigidPose> g_written;
std::array<AppliedView, kAppliedHistory> g_applied{};
std::uint64_t g_applied_serial = 0;
std::atomic<std::uint64_t> g_head_writes{0};

// Following the character (under g_mutex, except the counters).
std::optional<Follow> g_follow;
bool g_follow_has_last = false;
std::array<float, 3> g_follow_last{};
std::atomic<std::uint64_t> g_follow_reads{0};
std::atomic<std::uint64_t> g_follow_rejects{0};
constexpr float kFollowMaxJump = 10.0f;

// Moves the held pose to the followed character, if the read looks sane. Under g_mutex.
void update_follow() {
    float translate[3];
    if (!mem::try_copy(translate, g_follow->transform + layout::kTransformTranslate, sizeof(translate)) ||
        !std::isfinite(translate[0]) || !std::isfinite(translate[1]) || !std::isfinite(translate[2])) {
        g_follow_rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_follow_has_last) {
        const float dx = translate[0] - g_follow_last[0], dy = translate[1] - g_follow_last[1],
                    dz = translate[2] - g_follow_last[2];
        if (dx * dx + dy * dy + dz * dz > kFollowMaxJump * kFollowMaxJump) {
            g_follow_rejects.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_follow_last = {translate[0], translate[1], translate[2]};
    g_follow_has_last = true;
    g_pose->x = translate[0];
    g_pose->y = translate[1] + g_follow->height;
    g_pose->z = translate[2];
    if (g_sweep) {
        g_sweep_base.x = g_pose->x;
        g_sweep_base.y = g_pose->y;
        g_sweep_base.z = g_pose->z;
    }
    g_follow_reads.fetch_add(1, std::memory_order_relaxed);
}
bool same_fov(const EyeFov& a, const EyeFov& b) {
    return a.left_deg == b.left_deg && a.right_deg == b.right_deg && a.up_deg == b.up_deg && a.down_deg == b.down_deg;
}

safetyhook::MidHook g_view_update_hook;
safetyhook::MidHook g_set_view_hook;
safetyhook::MidHook g_set_projection_hook;

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


// Called once per view update of the driven camera, under g_mutex: middle-drag turns the held pose.
void update_mouselook(Pose& pose) {
    const bool held = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    POINT cursor{};
    GetCursorPos(&cursor);
    if (held && g_dragging) {
        pose.yaw_deg += static_cast<float>(cursor.x - g_last_cursor.x) * g_mouselook_sensitivity;
        pose.pitch_deg = std::clamp(
            pose.pitch_deg - static_cast<float>(cursor.y - g_last_cursor.y) * g_mouselook_sensitivity, -89.0f, 89.0f);
    }
    g_dragging = held;
    g_last_cursor = cursor;
}

// Entry of the view update: rcx = controller. Write the pose into the camera's Transform.
void on_view_update(safetyhook::Context& ctx) {
    if (const FrameCallback callback = g_frame_callback.load(std::memory_order_acquire)) {
        callback();
    }
    std::optional<Pose> pose;
    std::optional<HeadView> head;
    {
        std::scoped_lock lock(g_mutex);
        if (ctx.rcx != g_controller) {
            return;
        }
        // The head view's field of view goes into the projection built later in this same update.
        if (g_pose && g_head && (!g_eye_fov || !same_fov(*g_eye_fov, g_head->fov))) {
            g_eye_fov = g_head->fov;
            g_projection_dirty = true;
        }
        if (g_projection_dirty) {
            *reinterpret_cast<std::uint8_t*>(ctx.rcx + layout::kProjectionDirty) = 1;
            g_projection_dirty = false;
        }
        if (!g_pose) {
            return;
        }
        if (g_follow) {
            update_follow();
        }
        if (g_sweep) {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_sweep_start).count();
            const double phase = 2.0 * std::numbers::pi * t / std::max(0.5f, g_sweep->period_s);
            g_pose->yaw_deg = g_sweep_base.yaw_deg + g_sweep->yaw_deg * static_cast<float>(std::sin(phase));
            g_pose->pitch_deg = std::clamp(
                g_sweep_base.pitch_deg + g_sweep->pitch_deg * static_cast<float>(std::sin(2.0 * phase)), -89.0f, 89.0f);
        } else if (g_mouselook) {
            update_mouselook(*g_pose);
        }
        pose = g_pose;
        head = g_head;
    }
    using GetTransformFn = float* (*)(std::uintptr_t);
    const auto vtable = mem::read<std::uintptr_t>(ctx.rcx);
    const auto get_transform = mem::read<GetTransformFn>(vtable + layout::kGetWorldTransformSlot * sizeof(void*));
    float* transform = get_transform(ctx.rcx);
    if (!transform) {
        return;
    }
    vr::RigidPose world{{pose->x, pose->y, pose->z}, pose_quaternion(*pose)};
    if (head) {
        vr::RigidPose local = vr::from_openxr(head->pose);
        vr::Vec3 position;
        float recenter_yaw;
        {
            std::scoped_lock lock(g_mutex);
            position = g_recenter_position;
            recenter_yaw = g_recenter_yaw_deg;
        }
        local = vr::recentered(local, position, recenter_yaw);

        for (float& axis : local.position) {
            axis *= g_world_scale;
        }
        world = vr::anchor(*pose, local);
    }
    std::memcpy(transform, world.orientation.data(), sizeof(world.orientation));
    std::memcpy(reinterpret_cast<std::uint8_t*>(transform) + layout::kTransformTranslate, world.position.data(),
                sizeof(world.position));
    g_transform_writes.fetch_add(1, std::memory_order_relaxed);

    std::scoped_lock lock(g_mutex);
    g_written = world;
    if (head) {
        std::move_backward(g_applied.begin(), g_applied.end() - 1, g_applied.end());
        AppliedView& applied = g_applied.front();
        Mat4 camera_to_world;
        vr::camera_matrices(world, camera_to_world, applied.view_matrix);
        applied.head = *head;
        applied.serial = ++g_applied_serial;
        g_head_writes.fetch_add(1, std::memory_order_relaxed);
    }
}

// At the call to the matrix setter: rbx = controller, rdx = the view matrix the game just built.
void on_set_view(safetyhook::Context& ctx) {
    g_view_calls.fetch_add(1, std::memory_order_relaxed);
    track_controller(ctx.rbx);
    std::optional<vr::RigidPose> written;
    {
        std::scoped_lock lock(g_mutex);
        if (ctx.rbx != g_controller || !g_pose) {
            return;
        }
        written = g_written;
    }
    if (!written) {
        return;
    }
    Mat4 world, expected;
    vr::camera_matrices(*written, world, expected);
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

// At the call to the projection setter: rbx = controller, rdx = the projection the game just built.
void on_set_projection(safetyhook::Context& ctx) {
    std::optional<EyeFov> fov;
    {
        std::scoped_lock lock(g_mutex);
        if (!g_eye_fov || ctx.rbx != g_controller) {
            return;
        }
        fov = g_eye_fov;
    }
    constexpr float kRadians = std::numbers::pi_v<float> / 180.0f;
    const float near_plane = mem::read<float>(ctx.rbx + layout::kNearPlane);
    const float far_plane = mem::read<float>(ctx.rbx + layout::kFarPlane);
    const Mat4 projection = vr::projection(fov->left_deg * kRadians, fov->right_deg * kRadians, fov->up_deg * kRadians,
                                           fov->down_deg * kRadians, near_plane, far_plane);
    std::memcpy(reinterpret_cast<void*>(ctx.rdx), projection.data(), sizeof(projection));
    g_projection_overrides.fetch_add(1, std::memory_order_relaxed);
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
    g_mouselook = config.get_bool("camera", "mouselook", true);
    const std::string sensitivity = config.get("camera", "mouselook_sensitivity");
    if (!sensitivity.empty()) {
        g_mouselook_sensitivity = std::strtof(sensitivity.c_str(), nullptr);
    }
    const std::string scale = config.get("xr", "world_scale");
    if (!scale.empty()) {
        g_world_scale = std::strtof(scale.c_str(), nullptr);
    }
    install_mid_hook("camera_view_update", &on_view_update, g_view_update_hook);
    install_mid_hook("camera_set_view_call", &on_set_view, g_set_view_hook);
    install_mid_hook("camera_set_projection_call", &on_set_projection, g_set_projection_hook);
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
    g_written.reset();
    if (pose) {
        g_sweep_base = *pose; // a running sweep continues around the new pose
    }
    if (!pose) {
        log::info("camera: pose override off");
    } else {
        log::info("camera: pose override ({:.2f}, {:.2f}, {:.2f}) yaw {:.1f} pitch {:.1f}{}", pose->x, pose->y,
                  pose->z, pose->yaw_deg, pose->pitch_deg, g_controller ? "" : " (no controller attached yet)");
    }
}

void set_eye_fov(std::optional<EyeFov> fov) {
    std::scoped_lock lock(g_mutex);
    g_eye_fov = fov;
    g_projection_dirty = true;
    if (fov) {
        log::info("camera: eye projection left {:.1f} right {:.1f} up {:.1f} down {:.1f} deg{}", fov->left_deg,
                  fov->right_deg, fov->up_deg, fov->down_deg, g_controller ? "" : " (no controller attached yet)");
    } else {
        log::info("camera: eye projection off (game projection)");
    }
}

void set_sweep(std::optional<Sweep> sweep) {
    std::scoped_lock lock(g_mutex);
    if (sweep) {
        // Start from the held pose, or keep the original base if a sweep is already running.
        if (!g_sweep && g_pose) {
            g_sweep_base = *g_pose;
        }
        g_sweep_start = std::chrono::steady_clock::now();
        log::info("camera: sweep on (yaw +/-{:.0f} deg, pitch +/-{:.0f} deg, {:.1f} s per cycle){}", sweep->yaw_deg,
                  sweep->pitch_deg, sweep->period_s, g_pose ? "" : " (starts when a pose is held)");
    } else if (g_sweep && g_pose) {
        *g_pose = g_sweep_base; // stop facing where the sweep started
        log::info("camera: sweep off");
    }
    g_sweep = sweep;
}

void set_mouselook(bool enabled) {
    std::scoped_lock lock(g_mutex);
    g_mouselook = enabled;
    g_dragging = false;
    log::info("camera: mouse look {} (hold the middle mouse button and drag; {:.2f} deg per pixel)",
              enabled ? "on" : "off", g_mouselook_sensitivity);
}

void set_follow(std::optional<Follow> follow) {
    std::scoped_lock lock(g_mutex);
    const bool changed = follow.has_value() != g_follow.has_value() ||
                         (follow && (follow->transform != g_follow->transform || follow->height != g_follow->height));
    if (!follow || !g_follow || follow->transform != g_follow->transform) {
        g_follow_has_last = false; // a new component: don't compare against the old one's position
    }
    g_follow = follow;
    if (changed && follow) {
        log::info("camera: following the transform at 0x{:X}, {:.2f} above it{}", follow->transform, follow->height,
                  g_pose ? "" : " (takes effect when a pose is held)");
    } else if (changed) {
        log::info("camera: follow off");
    }
}
void set_head(std::optional<HeadView> view) {
    std::scoped_lock lock(g_mutex);
    if (view.has_value() != g_head.has_value()) {
        log::info("camera: head tracking {}{}", view ? "on" : "off",
                  view && !g_pose ? " (takes effect when a pose is held: !vr_cam_set here)" : "");
        if (!view) {
            g_eye_fov.reset(); // back to the game's projection
            g_projection_dirty = true;
        }
    }
    g_head = view;
}

void set_recenter(const vr::RigidPose& origin) {
    const vr::RigidPose game = vr::from_openxr(origin);
    const float yaw = vr::facing_yaw(game.orientation);

    std::scoped_lock lock(g_mutex);
    g_recenter_position = game.position;
    g_recenter_yaw_deg = yaw;
    log::info("camera: recentered on the head at ({:.2f}, {:.2f}, {:.2f}) facing yaw {:.1f}", game.position[0],
              game.position[1], game.position[2], yaw);
}
std::size_t applied_views(std::array<AppliedView, kAppliedHistory>& out) {
    std::scoped_lock lock(g_mutex);
    out = g_applied;
    return static_cast<std::size_t>(std::min<std::uint64_t>(g_applied_serial, kAppliedHistory));
}

void set_frame_callback(FrameCallback callback) {
    g_frame_callback.store(callback, std::memory_order_release);
}

void report() {
    const std::uint64_t calls = g_view_calls.load(std::memory_order_relaxed);
    log::info("camera: view set {} times since the last report; transform written {} times; built view checked {} "
              "times, max error {:.6f}{}; projection overridden {} times",
              calls - g_last_reported_calls, g_transform_writes.load(std::memory_order_relaxed),
              g_view_checks.load(std::memory_order_relaxed), g_view_max_error.load(std::memory_order_relaxed),
              g_force_view ? " (force_view on)" : "", g_projection_overrides.load(std::memory_order_relaxed));
    log::info("camera: head-tracked writes {} (world scale {:.2f})", g_head_writes.load(std::memory_order_relaxed),
              g_world_scale);
    log::info("camera: follow reads {}, rejected {}", g_follow_reads.load(std::memory_order_relaxed),
              g_follow_rejects.load(std::memory_order_relaxed));
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
