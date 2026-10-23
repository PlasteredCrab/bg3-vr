#include "bg3vr/camera.hpp"

#include "bg3vr/bodyik.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/skeleton.hpp"
#include "bg3vr/vr_math.hpp"

#include <safetyhook.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <numbers>
#include <cmath>
#include <format>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace bg3vr::camera {

namespace {

// State shared between the bridge thread and the game's camera update.
std::mutex g_mutex;
std::uintptr_t g_controller = 0;
ModuleInfo g_module;
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
// Third person in VR (set_chase): the view stays stereo and head-tracked, its origin behind and above the character
// along the turned yaw (stick turns, not the head and not the body's facing), so it moves only with the character's
// position, your head and your own turns.
std::atomic<bool> g_chase{false};
std::atomic<float> g_chase_back{2.5f};
std::atomic<float> g_chase_up{0.6f};
// Free camera (set_free): the view leaves the character where it is and the sticks fly it (move_free), for aiming
// spells out of the character's sight. Still stereo and head-tracked; moves only with the head, the sticks and turns.
std::atomic<bool> g_free{false};
// Agent Z 2: the CPU cull frustum (0x4086010) is built from the controller's game FOV +0x14 and aspect +0x20 (55 deg /
// 1.6), symmetric, at the head. Each eye's wider, lopsided field of view reached past it on a different side, so
// ground decals and blood tiles were culled in blocks, different per eye. With head tracking on these two fields
// get a symmetric frustum enclosing both eyes (2 degrees of margin for the eye offset); the game's values come back
// when head tracking stops. [views] cull_both_eyes.
bool g_cull_both_eyes = true;
std::optional<std::pair<float, float>> g_saved_fov; // the game's +0x14 / +0x20
std::uint64_t g_cull_fov_writes = 0;
bool g_projection_dirty = false;
std::atomic<std::uint64_t> g_projection_overrides{0};

// Head tracking: the latest located view (set every frame by the headset), the pose last written into
// the Transform (what the setter check compares against), and the history of applied head views.
// Under g_mutex.
std::optional<HeadView> g_head;
std::atomic<float> g_world_scale{1.0f};
// Recentering origin in game convention: head views are taken relative to it (yaw only).
vr::Vec3 g_recenter_position{0.0f, 0.0f, 0.0f};
float g_recenter_yaw_deg = 0.0f;
std::optional<vr::RigidPose> g_written;
// The held pose (after following and mouse look) at the last view update: where tracked poses are anchored.
std::optional<Pose> g_anchor;
std::array<AppliedView, kAppliedHistory> g_applied{};
std::uint64_t g_applied_serial = 0;
std::atomic<std::uint64_t> g_head_writes{0};

// Following the character (under g_mutex, except the counters).
std::optional<Follow> g_follow;
float g_follow_requested = std::numeric_limits<float>::quiet_NaN(); // the height the bridge last asked for
bool g_follow_has_last = false;
std::array<float, 3> g_follow_last{};
std::atomic<std::uint64_t> g_follow_reads{0};
std::atomic<std::uint64_t> g_follow_rejects{0};
std::atomic<std::uint64_t> g_follow_teleports{0};
// A jump this large between two reads is a teleport (cutscenes and dialogues place the player's body; the first
// headset run, 2026-10-20, showed 70 such jumps rejected by the old 10 m sanity limit, leaving the camera behind).
constexpr float kTeleportDistance = 2.0f;
constexpr float kWorldLimit = 100000.0f; // beyond this a read is garbage, not a position
bool g_teleport_turn = false;            // [vr] teleport_turn: face where the body faces after a teleport (off: only the head turns the view)
std::atomic<bool> g_anchor_head{false};  // [vr] camera_anchor = head / head_exact (default body: the view moves only with the head)
std::atomic<float> g_head_smoothing{0.15f}; // seconds: how slowly the animation's head bob and sway reach the camera
vr::Vec3 g_eyes_smoothed{};               // under g_mutex: the eyes' offset from the body (model space), smoothed
bool g_eyes_smoothed_valid = false;
std::chrono::steady_clock::time_point g_eyes_smoothed_at{};
float g_step_max = 0.0f;                  // the largest camera move between two updates (no teleport), metres
std::uint64_t g_step_updates = 0;
std::array<float, 3> g_step_last{};
bool g_step_have_last = false;
std::atomic<float> g_eye_offset{0.0f};   // [vr] eye_offset (head mode)
std::atomic<std::uint64_t> g_follow_from_visual{0};
std::atomic<std::uint64_t> g_follow_from_eyes{0};
bool g_follow_source_visual = false; // under g_mutex: the last accepted read came from the visual
bool g_follow_logged_source = false;

bool sane(const float (&p)[3]) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && std::abs(p[0]) < kWorldLimit &&
           std::abs(p[1]) < kWorldLimit && std::abs(p[2]) < kWorldLimit;
}

// Moves the held pose to the followed character, if the read looks sane. Under g_mutex.
void update_follow() {
    // The rendered body (the skeleton read this same view update, in the frame callback), else the transform.
    float translate[3]{};
    float q[4]{};
    std::optional<vr::Vec3> eyes;
    bool from_visual = false;
    // Read this frame or the last, whether or not its pose is current (agent AV F2: a scene's stand-in can stop
    // animating; its bones are then its last pose and its transform is still current. The character's transform from
    // Lua would put the camera at the hidden real body).
    std::uint32_t frame_now = 0;
    std::memcpy(&frame_now, reinterpret_cast<const void*>(g_module.base + skeleton::layout::kFrameCounter), sizeof(frame_now));
    if (const auto head = skeleton::head(); head && frame_now - head->frame <= 2) {
        translate[0] = head->visual_position[0];
        translate[1] = head->visual_position[1];
        translate[2] = head->visual_position[2];
        q[0] = head->visual_rotation[0];
        q[1] = head->visual_rotation[1];
        q[2] = head->visual_rotation[2];
        q[3] = head->visual_rotation[3];
        from_visual = sane(translate);
        if (from_visual && g_anchor_head.load(std::memory_order_relaxed)) {
            // The eyes as animated (before the body turn moves the bones), else as read; the head bone + 0.10 m when the
            // eye bones are missing or don't sit near the head.
            std::optional<vr::Vec3> model = bodyik::animated_eyes(skeleton::current());
            if (!model && head->has_eyes) {
                model = head->eyes_model;
            }
            if (!model || std::abs((*model)[1] - head->model[1]) > 0.3f) {
                model = vr::Vec3{head->model[0], head->model[1] + 0.10f, head->model[2]};
            }
            // The body's position is followed exactly (teleports, cutscene placement); the eyes' offset from it is
            // smoothed, so the walk cycle's bob and the idle sway don't shake the view.
            const auto now = std::chrono::steady_clock::now();
            const float smoothing = g_head_smoothing.load(std::memory_order_relaxed);
            if (!g_eyes_smoothed_valid || smoothing <= 0.0f ||
                std::abs((*model)[0] - g_eyes_smoothed[0]) + std::abs((*model)[1] - g_eyes_smoothed[1]) +
                        std::abs((*model)[2] - g_eyes_smoothed[2]) > 1.0f) {
                g_eyes_smoothed = *model; // first read, exact mode, or a big change (sitting down, a new skeleton)
            } else {
                const float dt = std::clamp(std::chrono::duration<float>(now - g_eyes_smoothed_at).count(), 0.0f, 0.1f);
                const float k = 1.0f - std::exp(-dt / smoothing);
                for (int i = 0; i < 3; ++i) {
                    g_eyes_smoothed[i] += ((*model)[i] - g_eyes_smoothed[i]) * k;
                }
            }
            g_eyes_smoothed_valid = true;
            g_eyes_smoothed_at = now;
            const float s = head->visual_scale;
            const vr::Vec3 offset = vr::rotate(head->visual_rotation,
                                               {g_eyes_smoothed[0] * s, g_eyes_smoothed[1] * s, g_eyes_smoothed[2] * s});
            eyes = vr::Vec3{translate[0] + offset[0], translate[1] + offset[1], translate[2] + offset[2]};
        }
    }
    if (!from_visual &&
        (!mem::try_copy(translate, g_follow->transform + layout::kTransformTranslate, sizeof(translate)) || !sane(translate))) {
        g_follow_rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (from_visual != g_follow_source_visual || !g_follow_logged_source) {
        log::info("camera: following {}", from_visual ? (eyes ? "the rendered body's eyes (skeleton)" : "the rendered body (skeleton)")
                                                      : "the character's transform (no skeleton yet)");
        g_follow_logged_source = true;
        g_follow_has_last = g_follow_has_last && from_visual == g_follow_source_visual; // a new source isn't a teleport
        g_follow_source_visual = from_visual;
    }
    if (g_follow_has_last) {
        const float dx = translate[0] - g_follow_last[0], dy = translate[1] - g_follow_last[1],
                    dz = translate[2] - g_follow_last[2];
        const float jump = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (jump > kTeleportDistance) {
            // The body was placed somewhere else (cutscene, dialogue, teleport): come along, and optionally turn to
            // where the body now faces (same yaw as the Lua follow start: 2 atan2(q.y, q.w) + 180).
            g_follow_teleports.fetch_add(1, std::memory_order_relaxed);
            const bool have_rotation = (from_visual || mem::try_copy(q, g_follow->transform, sizeof(q))) &&
                                       std::isfinite(q[1]) && std::isfinite(q[3]) && (q[1] != 0.0f || q[3] != 0.0f);
            const float yaw = have_rotation ? std::atan2(q[1], q[3]) * 2.0f * 57.2957795f + 180.0f : g_pose->yaw_deg;
            if (g_teleport_turn && have_rotation) {
                g_pose->yaw_deg = std::fmod(yaw + 540.0f, 360.0f) - 180.0f;
            }
            log::info("camera: the followed body moved {:.1f} m in one update (teleport / cutscene); following{}", jump,
                      g_teleport_turn && have_rotation ? std::format(", now facing yaw {:.1f}", g_pose->yaw_deg)
                                                       : std::string());
        }
    }
    g_follow_last = {translate[0], translate[1], translate[2]};
    g_follow_has_last = true;
    if (eyes && !g_chase.load(std::memory_order_relaxed)) {
        g_pose->x = (*eyes)[0];
        g_pose->y = (*eyes)[1] + g_eye_offset.load(std::memory_order_relaxed);
        g_pose->z = (*eyes)[2];
        g_follow_from_eyes.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_pose->x = translate[0];
        g_pose->y = translate[1] + g_follow->height;
        g_pose->z = translate[2];
    }
    if (g_chase.load(std::memory_order_relaxed)) {
        // Behind along the turned yaw (pitch and head left out), and up.
        Pose level = *g_pose;
        level.pitch_deg = 0.0f;
        const auto turned = pose_quaternion(level);
        const vr::Vec3 forward = vr::rotate(vr::Quat{turned[0], turned[1], turned[2], turned[3]}, {0.0f, 0.0f, 1.0f});
        const float back = g_chase_back.load(std::memory_order_relaxed);
        g_pose->x -= forward[0] * back;
        g_pose->z -= forward[2] * back;
        g_pose->y += g_chase_up.load(std::memory_order_relaxed);
    }
    if (from_visual) {
        g_follow_from_visual.fetch_add(1, std::memory_order_relaxed);
    }
    // The largest step the anchor took between two updates (teleports excluded), logged now and then.
    if (g_step_have_last) {
        const float dx = g_pose->x - g_step_last[0], dy = g_pose->y - g_step_last[1], dz = g_pose->z - g_step_last[2];
        const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (step < kTeleportDistance) {
            g_step_max = std::max(g_step_max, step);
        }
    }
    if (++g_step_updates % 1500 == 0) {
        log::info("camera: largest step between two updates over the last 1500: {:.3f} m (anchor {}, smoothing {:.2f} s)",
                  g_step_max, eyes ? "eyes" : "body", g_head_smoothing.load());
        g_step_max = 0.0f;
    }
    g_step_last = {g_pose->x, g_pose->y, g_pose->z};
    g_step_have_last = true;
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

// The per-frame callback (agent AV F1): 0x4085150 has 39 callers, among them the cull frustum, picking and
// render-side stage code on the render thread and workers, so the callback ran several times a frame on several
// threads. It now runs once per game frame ([exe+0x614FB88]) on the game thread, learned from the calls that update the
// driven controller; if no call on that thread came for 30 frames (a scene camera has the view), any thread may.
std::atomic<std::uint32_t> g_game_thread{0};
std::uint32_t g_callback_frame = 0xFFFFFFFFu; // the frame the callback last ran in (only touched by the callback's caller)
std::mutex g_callback_mutex;
std::atomic<std::uint64_t> g_callback_runs{0}, g_callback_skips{0}, g_callback_fallbacks{0};
std::atomic<int> g_logged_calls{0};
struct ThreadCalls {
    std::uint32_t thread = 0;
    std::uint64_t calls = 0, driven = 0;
};
std::array<ThreadCalls, 8> g_thread_calls{}; // under g_callback_mutex

void run_frame_callback(const safetyhook::Context& ctx, bool driven) {
    const FrameCallback callback = g_frame_callback.load(std::memory_order_acquire);
    std::uint32_t frame = 0;
    std::memcpy(&frame, reinterpret_cast<const void*>(g_module.base + skeleton::layout::kFrameCounter), sizeof(frame));
    const std::uint32_t thread = GetCurrentThreadId();
    if (driven) {
        g_game_thread.store(thread, std::memory_order_relaxed);
    }
    if (g_logged_calls.load(std::memory_order_relaxed) < 200 && g_logged_calls.fetch_add(1, std::memory_order_relaxed) < 200) {
        std::uintptr_t ret = 0;
        mem::try_copy(&ret, ctx.rsp, sizeof(ret));
        log::info("camera: view update f{} t{} from exe+0x{:X} controller 0x{:X}{}", frame, thread, ret - g_module.base, ctx.rcx,
                  driven ? " (the driven camera)" : "");
    }
    bool run = false;
    {
        std::scoped_lock lock(g_callback_mutex);
        for (ThreadCalls& t : g_thread_calls) {
            if (t.thread == thread || t.thread == 0) {
                t.thread = thread;
                ++t.calls;
                t.driven += driven ? 1 : 0;
                break;
            }
        }
        if (frame != g_callback_frame) {
            const std::uint32_t game = g_game_thread.load(std::memory_order_relaxed);
            const bool stalled = g_callback_frame != 0xFFFFFFFFu && frame - g_callback_frame > 30;
            run = game == 0 || thread == game || stalled;
            if (run) {
                if (stalled && game != 0 && thread != game) {
                    g_callback_fallbacks.fetch_add(1, std::memory_order_relaxed);
                }
                g_callback_frame = frame;
            }
        }
    }
    if (!run) {
        g_callback_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_callback_runs.fetch_add(1, std::memory_order_relaxed);
    if (callback) {
        callback();
    }
}

// Entry of the view update: rcx = controller. Write the pose into the camera's Transform.
void on_view_update(safetyhook::Context& ctx) {
    bool driven = false;
    {
        std::scoped_lock lock(g_mutex);
        driven = ctx.rcx == g_controller && g_controller != 0;
    }
    run_frame_callback(ctx, driven);
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
        if (g_cull_both_eyes && g_pose && g_head) {
            constexpr float kRadians = std::numbers::pi_v<float> / 180.0f;
            constexpr float kMarginDeg = 2.0f;
            float up = 0.0f, down = 0.0f, left = 0.0f, right = 0.0f;
            for (const EyeFov& f : {g_head->fov, g_head->eye_fov[0], g_head->eye_fov[1]}) {
                up = std::max(up, std::tan(std::clamp(f.up_deg + kMarginDeg, 1.0f, 85.0f) * kRadians));
                down = std::max(down, std::tan(std::clamp(-f.down_deg + kMarginDeg, 1.0f, 85.0f) * kRadians));
                right = std::max(right, std::tan(std::clamp(f.right_deg + kMarginDeg, 1.0f, 85.0f) * kRadians));
                left = std::max(left, std::tan(std::clamp(-f.left_deg + kMarginDeg, 1.0f, 85.0f) * kRadians));
            }
            const float vertical = std::max(up, down);
            const float fov = 2.0f * std::atan(vertical);
            const float aspect = std::max(left, right) / vertical;
            float* fields = reinterpret_cast<float*>(ctx.rcx + layout::kFov);
            float* aspect_field = reinterpret_cast<float*>(ctx.rcx + layout::kAspectRatio);
            if (!g_saved_fov) {
                g_saved_fov = std::pair{*fields, *aspect_field};
                log::info("camera: cull frustum widened to enclose both eyes: fov {:.1f} deg, aspect {:.2f} (the game's "
                          "{:.1f} deg / {:.2f} come back when head tracking stops)",
                          fov / kRadians, aspect, *fields / kRadians, *aspect_field);
            }
            if (std::abs(*fields - fov) > 1e-4f || std::abs(*aspect_field - aspect) > 1e-4f) {
                *fields = fov;
                *aspect_field = aspect;
                g_projection_dirty = true;
                if (++g_cull_fov_writes <= 5 || g_cull_fov_writes % 500 == 0) {
                    log::info("camera: cull fov / aspect set to {:.1f} deg / {:.3f} ({} writes; the game or a new field of "
                              "view changed them)", fov / kRadians, aspect, g_cull_fov_writes);
                }
            }
        } else if (g_saved_fov && !g_head) {
            *reinterpret_cast<float*>(ctx.rcx + layout::kFov) = g_saved_fov->first;
            *reinterpret_cast<float*>(ctx.rcx + layout::kAspectRatio) = g_saved_fov->second;
            g_projection_dirty = true;
            log::info("camera: the game's fov / aspect restored ({:.3f} rad / {:.2f})", g_saved_fov->first, g_saved_fov->second);
            g_saved_fov.reset();
        }
        if (g_projection_dirty) {
            *reinterpret_cast<std::uint8_t*>(ctx.rcx + layout::kProjectionDirty) = 1;
            g_projection_dirty = false;
        }
        if (!g_pose) {
            return;
        }
        if (g_follow && !g_free.load(std::memory_order_relaxed)) {
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
        vr::Vec3 position;
        float recenter_yaw;
        {
            std::scoped_lock lock(g_mutex);
            position = g_recenter_position;
            recenter_yaw = g_recenter_yaw_deg;
        }
        world = vr::tracked_to_world(head->pose, position, recenter_yaw, g_world_scale, *pose);
    }
    std::memcpy(transform, world.orientation.data(), sizeof(world.orientation));
    std::memcpy(reinterpret_cast<std::uint8_t*>(transform) + layout::kTransformTranslate, world.position.data(),
                sizeof(world.position));
    g_transform_writes.fetch_add(1, std::memory_order_relaxed);

    std::scoped_lock lock(g_mutex);
    g_written = world;
    g_anchor = *pose;
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
        g_world_scale = std::clamp(std::strtof(scale.c_str(), nullptr), 0.25f, 4.0f);
    }
    g_teleport_turn = config.get_bool("vr", "teleport_turn", false);
    g_cull_both_eyes = config.get_bool("views", "cull_both_eyes", true);
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

bool pose_held() {
    std::scoped_lock lock(g_mutex);
    return g_pose.has_value();
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

void set_world_scale(float scale) {
    const float clamped = std::clamp(scale, 0.25f, 4.0f);
    if (g_world_scale.exchange(clamped) != clamped) {
        log::info("camera: world scale {:.2f}", clamped);
    }
}

float world_scale() {
    return g_world_scale.load(std::memory_order_relaxed);
}

void set_chase(bool on, float back_m, float up_m) {
    const bool was = g_chase.exchange(on);
    g_chase_back.store(std::clamp(back_m, 0.5f, 8.0f));
    g_chase_up.store(std::clamp(up_m, -1.0f, 4.0f));
    if (was != on) {
        log::info("camera: {}", on ? std::format("third person in VR: {:.1f} m behind and {:.1f} m above the character's eyes",
                                                 g_chase_back.load(), g_chase_up.load())
                                   : std::string("first person"));
    }
}

bool chase() {
    return g_chase.load(std::memory_order_relaxed);
}

void set_free(bool on) {
    if (g_free.exchange(on) != on) {
        std::scoped_lock lock(g_mutex);
        log::info("camera: {}", on ? std::format("free camera at ({:.2f}, {:.2f}, {:.2f}): the sticks fly it", g_pose ? g_pose->x : 0.0f,
                                                 g_pose ? g_pose->y : 0.0f, g_pose ? g_pose->z : 0.0f)
                                   : std::string("free camera off: following the character again"));
    }
}

bool free_camera() {
    return g_free.load(std::memory_order_relaxed);
}

void move_free(float right, float forward, float up, float seconds) {
    if (!g_free.load(std::memory_order_relaxed) || seconds <= 0.0f) {
        return;
    }
    std::scoped_lock lock(g_mutex);
    if (!g_pose || !g_written) {
        return;
    }
    // Along where the head looks, kept level (the left stick flies horizontally; the right stick's y is height).
    const vr::Vec3 look = vr::rotate(g_written->orientation, {0.0f, 0.0f, 1.0f});
    float fx = look[0], fz = look[2];
    const float l = std::sqrt(fx * fx + fz * fz);
    if (l < 1e-3f) {
        return;
    }
    fx /= l;
    fz /= l;
    // Right: the head's own right axis (+X of the view, as converted from the headset), kept level.
    const vr::Vec3 side = vr::rotate(g_written->orientation, {1.0f, 0.0f, 0.0f});
    float rx = side[0], rz = side[2];
    const float rl = std::sqrt(rx * rx + rz * rz);
    if (rl < 1e-3f) {
        return;
    }
    rx /= rl;
    rz /= rl;
    const float speed = 4.0f * seconds;
    g_pose->x += (fx * forward + rx * right) * speed;
    g_pose->z += (fz * forward + rz * right) * speed;
    g_pose->y += up * 2.5f * seconds;
}

void set_anchor(bool head, float eye_offset, float smoothing_s) {
    const bool was = g_anchor_head.exchange(head);
    g_eye_offset.store(eye_offset);
    g_head_smoothing.store(std::clamp(smoothing_s, 0.0f, 1.0f));
    if (was != head) {
        log::info("camera: anchored at {}", head ? "the character's eyes" : "the character's body (steady)");
    }
}

void set_follow_height(float height) {
    std::scoped_lock lock(g_mutex);
    if (g_follow && std::isfinite(height)) {
        g_follow->height = height;
    }
}

std::optional<vr::Vec3> follow_position() {
    std::scoped_lock lock(g_mutex);
    if (!g_follow || !g_follow_has_last) {
        return std::nullopt;
    }
    return vr::Vec3{g_follow_last[0], g_follow_last[1], g_follow_last[2]};
}

std::optional<float> follow_height() {
    std::scoped_lock lock(g_mutex);
    if (!g_follow) {
        return std::nullopt;
    }
    return g_follow->height;
}

void add_yaw(float degrees) {
    std::scoped_lock lock(g_mutex);
    if (!g_pose || !std::isfinite(degrees)) {
        return;
    }
    g_pose->yaw_deg = std::fmod(g_pose->yaw_deg + degrees + 540.0f, 360.0f) - 180.0f;
    if (g_sweep) {
        g_sweep_base.yaw_deg = g_pose->yaw_deg;
    }
}

void set_follow(std::optional<Follow> follow) {
    std::scoped_lock lock(g_mutex);
    // The Lua follow tick re-sends its start height (1.7) whenever the character's transform moves (cutscenes,
    // conversations, equipment). A re-send of the same height keeps the one calibrated from the eyes
    // (set_follow_height); a different height is a new request and applies.
    if (follow && g_follow && follow->height == g_follow_requested) {
        follow->height = g_follow->height;
    } else if (follow) {
        g_follow_requested = follow->height;
    }
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

std::optional<vr::RigidPose> tracked_to_world(const vr::RigidPose& xr_pose) {
    std::scoped_lock lock(g_mutex);
    if (!g_anchor || !g_pose) {
        return std::nullopt;
    }
    return vr::tracked_to_world(xr_pose, g_recenter_position, g_recenter_yaw_deg, g_world_scale, *g_anchor);
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
    {
        std::string threads;
        std::scoped_lock lock(g_callback_mutex);
        for (const ThreadCalls& t : g_thread_calls) {
            if (t.thread) {
                threads += std::format(" t{} {} calls ({} driven);", t.thread, t.calls, t.driven);
            }
        }
        log::info("camera: frame callback ran {} times, {} view updates skipped (other threads / same frame), {} fallbacks off "
                  "the game thread t{};{}",
                  g_callback_runs.load(), g_callback_skips.load(), g_callback_fallbacks.load(), g_game_thread.load(), threads);
    }
    const std::uint64_t calls = g_view_calls.load(std::memory_order_relaxed);
    log::info("camera: view set {} times since the last report; transform written {} times; built view checked {} "
              "times, max error {:.6f}{}; projection overridden {} times",
              calls - g_last_reported_calls, g_transform_writes.load(std::memory_order_relaxed),
              g_view_checks.load(std::memory_order_relaxed), g_view_max_error.load(std::memory_order_relaxed),
              g_force_view ? " (force_view on)" : "", g_projection_overrides.load(std::memory_order_relaxed));
    log::info("camera: head-tracked writes {} (world scale {:.2f})", g_head_writes.load(std::memory_order_relaxed),
              g_world_scale.load());
    log::info("camera: follow reads {} ({} from the rendered body, {} at its eyes), rejected {}, teleports followed {}",
              g_follow_reads.load(std::memory_order_relaxed), g_follow_from_visual.load(std::memory_order_relaxed),
              g_follow_from_eyes.load(std::memory_order_relaxed), g_follow_rejects.load(std::memory_order_relaxed),
              g_follow_teleports.load(std::memory_order_relaxed));
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
