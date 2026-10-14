#include "bg3vr/stereo.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/signatures.hpp"

#include <safetyhook.hpp>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numbers>
#include <string>

namespace bg3vr::stereo {

namespace {

constexpr float kRadians = std::numbers::pi_v<float> / 180.0f;

// Column-major (glm) product a * b.
camera::Mat4 multiply(const camera::Mat4& a, const camera::Mat4& b) {
    camera::Mat4 out{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[column * 4 + k];
            }
            out[column * 4 + row] = sum;
        }
    }
    return out;
}

vr::Quat conjugate(const vr::Quat& q) {
    return {-q[0], -q[1], -q[2], q[3]};
}

// Shared between the headset (set_eyes), the bridge (assign) and the render thread (the hook).
std::mutex g_mutex;
std::optional<Eyes> g_eyes;
std::array<std::uintptr_t, 2> g_views{}; // the view drawing each eye, 0 for none
float g_world_scale = 1.0f;

safetyhook::MidHook g_hook;
ModuleInfo g_module;

std::array<std::atomic<std::uint64_t>, 2> g_rewrites{};
std::atomic<std::uint64_t> g_without_eyes{0};
std::atomic<std::uint64_t> g_without_camera{0};
std::atomic<std::uint64_t> g_orthographic{0};

// Right after a render view built its PerCamera blocks in begin-recording, before the upload:
// rdi = the view.
void on_camera_built(safetyhook::Context& ctx) {
    const std::uintptr_t view = ctx.rdi;
    int eye = -1;
    std::optional<Eyes> eyes;
    float scale = 1.0f;
    {
        std::scoped_lock lock(g_mutex);
        if (view == g_views[0]) {
            eye = 0;
        } else if (view == g_views[1]) {
            eye = 1;
        } else {
            return;
        }
        eyes = g_eyes;
        scale = g_world_scale;
    }
    if (!eyes) {
        g_without_eyes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The builder skips both blocks for a view without a camera; an orthographic camera's projection
    // isn't the perspective form rewritten here.
    const auto controller = mem::read<std::uintptr_t>(view + layout::kReadStateController);
    if (!controller) {
        g_without_camera.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (mem::read<std::uint8_t>(controller + layout::kControllerIsOrthographic) != 0) {
        g_orthographic.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const vr::RigidPose offset = eye_in_head(eyes->centre_xr, eyes->eye_xr[eye], scale);
    rewrite_block(reinterpret_cast<std::uint8_t*>(view + layout::kPerCamera0), offset, eyes->fov[eye], false);
    rewrite_block(reinterpret_cast<std::uint8_t*>(view + layout::kPerCamera1), offset, eyes->fov[eye], true);
    g_rewrites[eye].fetch_add(1, std::memory_order_relaxed);
}

} // namespace

vr::RigidPose eye_in_head(const vr::RigidPose& centre_xr, const vr::RigidPose& eye_xr, float world_scale) {
    const vr::Quat unrotate = conjugate(centre_xr.orientation);
    const vr::RigidPose relative{
        vr::rotate(unrotate, {eye_xr.position[0] - centre_xr.position[0], eye_xr.position[1] - centre_xr.position[1],
                              eye_xr.position[2] - centre_xr.position[2]}),
        vr::multiply(unrotate, eye_xr.orientation)};
    vr::RigidPose game = vr::from_openxr(relative);
    for (float& axis : game.position) {
        axis *= world_scale;
    }
    return game;
}

camera::Mat4 eye_view(const camera::Mat4& head_view, const vr::RigidPose& eye_in_head) {
    camera::Mat4 eye_to_head, head_to_eye;
    vr::camera_matrices(eye_in_head, eye_to_head, head_to_eye);
    return multiply(head_to_eye, head_view);
}

camera::Mat4 eye_projection(const camera::Mat4& game, const camera::EyeFov& fov, float jitter_x, float jitter_y) {
    const float l = std::tan(fov.left_deg * kRadians), r = std::tan(fov.right_deg * kRadians);
    const float u = std::tan(fov.up_deg * kRadians), d = std::tan(fov.down_deg * kRadians);
    camera::Mat4 out = game;
    // Same x/y terms as vr::projection (and the game's own builder 0x3CCCFD0).
    out[0] = 2.0f / (r - l);
    out[5] = 2.0f / (u - d);
    out[8] = -(r + l) / (r - l) + jitter_x;
    out[9] = -(u + d) / (u - d) + jitter_y;
    return out;
}

camera::Mat4 rotation_view_projection(const camera::Mat4& projection, const camera::Mat4& view) {
    camera::Mat4 rotation = view;
    rotation[3] = rotation[7] = rotation[11] = 0.0f;
    rotation[12] = rotation[13] = rotation[14] = 0.0f;
    rotation[15] = 1.0f;
    return multiply(projection, rotation);
}

vr::Vec3 camera_position(const camera::Mat4& view) {
    // view = [R | t] (world to camera): the camera sits at -R^T t.
    vr::Vec3 position{};
    for (int i = 0; i < 3; ++i) {
        position[i] = -(view[i * 4 + 0] * view[12] + view[i * 4 + 1] * view[13] + view[i * 4 + 2] * view[14]);
    }
    return position;
}

void rewrite_block(std::uint8_t* block, const vr::RigidPose& eye_in_head, const camera::EyeFov& fov, bool jittered) {
    camera::Mat4 head_view, projection;
    std::memcpy(head_view.data(), block + layout::kView, sizeof(head_view));
    std::memcpy(projection.data(), block + layout::kProjection, sizeof(projection));
    const camera::Mat4 view = eye_view(head_view, eye_in_head);
    const camera::Mat4 eye = eye_projection(projection, fov, jittered ? projection[8] : 0.0f, jittered ? projection[9] : 0.0f);
    const camera::Mat4 view_projection = rotation_view_projection(eye, view);
    const vr::Vec3 position = camera_position(view);
    std::memcpy(block + layout::kView, view.data(), sizeof(view));
    std::memcpy(block + layout::kProjection, eye.data(), sizeof(eye));
    std::memcpy(block + layout::kViewProjection, view_projection.data(), sizeof(view_projection));
    std::memcpy(block + layout::kPosition, position.data(), sizeof(position));
}

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    const std::string scale = config.get("xr", "world_scale");
    if (!scale.empty()) {
        g_world_scale = std::strtof(scale.c_str(), nullptr);
    }
    const auto site = sig::lookup("render_view_camera_built");
    if (!site) {
        log::error("stereo: signature render_view_camera_built not resolved; same-frame stereo unavailable");
        return;
    }
    auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(*site), &on_camera_built);
    if (!hook) {
        log::error("stereo: mid-hook at exe+0x{:X} failed (error type {})", *site - module.base,
                   static_cast<int>(hook.error().type));
        return;
    }
    g_hook = std::move(*hook);
    log::info("stereo: hook render_view_camera_built installed at exe+0x{:X}", *site - module.base);
}

void set_eyes(std::optional<Eyes> eyes) {
    std::scoped_lock lock(g_mutex);
    g_eyes = eyes;
}

void assign(std::uintptr_t view, int eye) {
    std::scoped_lock lock(g_mutex);
    for (std::uintptr_t& assigned : g_views) {
        if (assigned == view) {
            assigned = 0;
        }
    }
    if (eye == 0 || eye == 1) {
        g_views[static_cast<std::size_t>(eye)] = view;
        log::info("stereo: view 0x{:X} draws the {} eye", view, eye == 0 ? "left" : "right");
    }
}

void clear() {
    std::scoped_lock lock(g_mutex);
    g_views = {};
    g_eyes.reset();
    log::info("stereo: off");
}

bool active() {
    std::scoped_lock lock(g_mutex);
    return g_views[0] && g_views[1];
}

void report() {
    std::array<std::uintptr_t, 2> views;
    {
        std::scoped_lock lock(g_mutex);
        views = g_views;
    }
    log::info("stereo: views left 0x{:X} right 0x{:X}; rewrites left {} right {}; skipped: no eyes {}, no camera {}, "
              "orthographic {}",
              views[0], views[1], g_rewrites[0].load(std::memory_order_relaxed),
              g_rewrites[1].load(std::memory_order_relaxed), g_without_eyes.load(std::memory_order_relaxed),
              g_without_camera.load(std::memory_order_relaxed), g_orthographic.load(std::memory_order_relaxed));
}

} // namespace bg3vr::stereo
