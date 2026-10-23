#include "bg3vr/stereo.hpp"

#include "bg3vr/camera.hpp"
#include "bg3vr/dolls.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/text.hpp"

#include <safetyhook.hpp>

#include <algorithm>
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
float g_ipd_override_m = 0.0f; // 0 = the runtime's IPD

safetyhook::MidHook g_hook;
ModuleInfo g_module;

// Where each eye view's camera-relative origin is ([views] eye_origin; agent S, S_decals.md):
//   Proxy: real stereo. Every draw places its vertices relative to the view's ReadState controller's
//          GetWorldTranslate (vtable slot 9) and the vertex shaders use projection x view ROTATION only, so with the
//          one shared controller both eyes rasterised from the head (no parallax at all), while decal / surface /
//          lighting shaders add PerCamera +0xD0 (the eye position the rewrite put there) back: decals and pools at the
//          wrong depth. Each eye view gets its own copy of the controller whose slot 9 returns that eye's position and
//          whose view / inverse view are the eye's; the rewrite then only replaces the projection.
//   Head:  no parallax, but consistent: the rewrite keeps the head position (Fix A).
enum class Origin { Proxy, Head };
std::atomic<Origin> g_origin{Origin::Proxy};

// The gameplay controller class (agent G: vtable 0x58B6750, 0x330 bytes; slot 9 = GetWorldTranslate 0x3CF0740).
constexpr std::uintptr_t kControllerVtableRva = 0x58B6750;
constexpr std::size_t kControllerSize = 0x330;
constexpr std::size_t kTranslateSlot = 9;
constexpr std::size_t kProxyVtableSlots = 96;
constexpr std::size_t kProxyRing = 8; // the render side may still read proxies from the last few frames
constexpr std::size_t kTransformSlot = 11; // 0x3CF0760: the camera entity's Transform (quat +0x00, translate +0x10)
struct alignas(16) Proxy {
    std::uint8_t object[kControllerSize]; // first: `this` of a virtual call is the Proxy
    alignas(16) float translate[4]{};
    camera::EyeFov fov{};
    alignas(16) float transform[16]{}; // slot 11's Transform with the head roll taken out (render thread)
};
struct ProxyRing {
    std::array<Proxy, kProxyRing> slots{};
    std::size_t next = 0;
};
std::array<ProxyRing, 2> g_proxies; // render thread only (begin-recording hook and the camera hook after it)
// The UI dolls' eye views (dolls.cpp) get proxies too, from small rings of their own: a doll frame is recorded and
// finished within one pass of the render thread (agent V 1.1), so a ring can go to another view once unused.
struct DollRing {
    std::uintptr_t view = 0;
    std::uint64_t used = 0;
    ProxyRing ring;
};
std::array<DollRing, 8> g_doll_rings;
std::uint64_t g_doll_tick = 0;
std::atomic<std::uint64_t> g_doll_proxied{0};

ProxyRing& doll_ring(std::uintptr_t view) {
    DollRing* oldest = &g_doll_rings[0];
    for (DollRing& r : g_doll_rings) {
        if (r.view == view) {
            r.used = ++g_doll_tick;
            return r.ring;
        }
        if (r.used < oldest->used) {
            oldest = &r;
        }
    }
    oldest->view = view;
    oldest->used = ++g_doll_tick;
    return oldest->ring;
}

// The doll proxy `controller` is, if it is one.
const Proxy* doll_proxy(std::uintptr_t controller) {
    for (const DollRing& r : g_doll_rings) {
        for (const Proxy& p : r.ring.slots) {
            if (reinterpret_cast<std::uintptr_t>(&p) == controller) {
                return &p;
            }
        }
    }
    return nullptr;
}
std::array<std::uintptr_t, kProxyVtableSlots> g_proxy_vtable{};
constexpr std::size_t kLastSeenController = 0x2B00; // the controller 0x3CC6DC0 compared +0x21D8 with last frame
std::atomic<bool> g_keep_history{true};

// Agent AE 2: the sky, the aerial perspective, the fog in-scatter, clouds and forward smoke are multiplied by the
// exposure record 0x3CCCB20(view) returns (the view's own stage 0x21 exposure step); lit surfaces aren't. Each eye
// adapted its own exposure, and in the 13:20 run everything exposure-scaled (sky, haze, clouds) was darker in the right
// eye while lit surfaces matched. The right eye gets the left eye's record: its previous-frame buffer, which the left
// eye's job doesn't write this frame (it writes the other one). [views] eye_exposure_from_left.
// Agent AH (AH_surface_cells.md): blood pools and other surfaces are drawn only inside 16x16-pixel tiles that the view's
// compute pass (stage 0x3D, job 0x18126A0, EoCSurfaceMeshGenerate) marks, rebuilding each pixel's ray from the
// controller's projection (+0x110: the head's union field of view, centred). The draw places its tile grid with the
// controller's inverse projection (+0x150), then the eye's off-centre projection scales and shifts it. On uneven ground
// the two errors don't cancel and tiles drop out, differently in each eye (the square gaps in blood on rocks). Fixed
// together (either half alone is worse): the pass's ray constants from the view's own eye projection (mid-hook at
// 0x1812942), and each eye's proxy carries the eye projection and its inverse. [views] eye_surface_mesh.
constexpr std::uintptr_t kSurfaceRaysRva = 0x1812942;
constexpr std::size_t kControllerProjection = 0x110, kControllerInverseProjection = 0x150, kControllerOrthographic = 0x12;
safetyhook::MidHook g_surface_hook;
std::atomic<bool> g_surface_fix{true};
std::atomic<std::uint64_t> g_surface_fixed{0};

void on_surface_rays(safetyhook::Context& ctx) {
    if (!g_surface_fix.load(std::memory_order_relaxed)) {
        return;
    }
    const std::uintptr_t view = ctx.rbx;
    if (stereo::eye_of(view) < 0) {
        return;
    }
    std::uintptr_t controller = 0;
    std::uint8_t orthographic = 1;
    std::int32_t size[2]{};
    float p[16]{};
    if (!mem::try_copy(&controller, view + layout::kReadStateController, sizeof(controller)) || !controller ||
        !mem::try_copy(&orthographic, controller + kControllerOrthographic, 1) || orthographic ||
        !mem::try_copy(size, ctx.rbp + 0x08, sizeof(size)) || size[0] <= 0 || size[1] <= 0 ||
        !mem::try_copy(p, view + layout::kPerCamera0 + layout::kProjection, sizeof(p)) || p[0] == 0.0f || p[5] == 0.0f) {
        return;
    }
    const float a = p[0], c = p[5], b = p[8], e = p[9];
    const float constants[4] = {2.0f / (static_cast<float>(size[0]) * a), -2.0f / (static_cast<float>(size[1]) * c),
                                (-1.0f - b) / a, (1.0f - e) / c};
    float before[4]{};
    mem::try_copy(before, ctx.rcx + 0x10, sizeof(before));
    std::memcpy(reinterpret_cast<void*>(ctx.rcx + 0x10), constants, sizeof(constants));
    const std::uint64_t n = g_surface_fixed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4 || n % 20000 == 0) {
        log::info("stereo: surface tile rays of {} eye view 0x{:X} ({}x{}): ({:.6f}, {:.6f}, {:.4f}, {:.4f}) -> ({:.6f}, "
                  "{:.6f}, {:.4f}, {:.4f}) ({} times)",
                  stereo::eye_of(view) == 0 ? "left" : "right", view, size[0], size[1], before[0], before[1], before[2],
                  before[3], constants[0], constants[1], constants[2], constants[3], n);
    }
}

// Agent AL 3: SSAO (stage 0x37 job 0x3E4F490) rebuilds view positions with cb0[1] = (2/(w P0), -2/(h P5), (-1-P2)/P0,
// (1+P6)/P5): P2/P6 are 0 in this layout, so each eye's off-centre projection (P8/P9) was ignored and its AO was computed
// on a sheared copy of the scene (the same bug class as the surface tiles). At the join 0x3E4FD36 (view [rbp-0x60],
// controller rsi, the offsets at [rbp+0xA8] / [rbp+0xAC]) both eyes get (-1-P8)/P0 and (1-P9)/P5 from their own
// unjittered PerCamera block [0]. [views] eye_ssao.
constexpr std::uintptr_t kSsaoRaysRva = 0x3E4FD36;
safetyhook::MidHook g_ssao_hook;
std::atomic<bool> g_ssao_fix{true};
std::atomic<std::uint64_t> g_ssao_fixed{0};

void on_ssao_rays(safetyhook::Context& ctx) {
    if (!g_ssao_fix.load(std::memory_order_relaxed)) {
        return;
    }
    std::uintptr_t view = 0;
    std::uint8_t orthographic = 1;
    float p[16]{};
    if (!mem::try_copy(&view, ctx.rbp - 0x60, sizeof(view)) || stereo::eye_of(view) < 0 ||
        !mem::try_copy(&orthographic, ctx.rsi + kControllerOrthographic, 1) || orthographic ||
        !mem::try_copy(p, view + layout::kPerCamera0 + layout::kProjection, sizeof(p)) || p[0] == 0.0f || p[5] == 0.0f) {
        return;
    }
    const float x = (-1.0f - p[8]) / p[0], y = (1.0f - p[9]) / p[5];
    float before[2]{};
    mem::try_copy(before, ctx.rbp + 0xA8, sizeof(before));
    *reinterpret_cast<float*>(ctx.rbp + 0xA8) = x;
    *reinterpret_cast<float*>(ctx.rbp + 0xAC) = y;
    const std::uint64_t n = g_ssao_fixed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4 || n % 40000 == 0) {
        log::info("stereo: SSAO rays of {} eye view 0x{:X}: ({:.4f}, {:.4f}) -> ({:.4f}, {:.4f}) ({} times)",
                  stereo::eye_of(view) == 0 ? "left" : "right", view, before[0], before[1], x, y, n);
    }
}

constexpr std::uintptr_t kExposureRva = 0x3CCCB20;
safetyhook::InlineHook g_exposure_hook;
std::atomic<bool> g_exposure_from_left{true};
std::atomic<std::uint64_t> g_exposure_redirects{0};

// Agent AI 1: the fog lighting job (0x3E3CCE0) keeps 93% of last frame's in-scatter rescaled by t26 / t1 (this frame's
// exposure / last frame's). t26 comes from 0x3CCCB20 (redirected above), but t1 is picked inline from the view's own
// exposure step (0x3E3E266..0x3E3E337), so the right eye's history was scaled by main's exposure over its own every
// frame and its glow toward the sun and the far haze drained away. At the join 0x3E3E337 (rsi = view, r8 = the t1
// record) the right eye gets main's record too, the one t26 got: the ratio is 1 again.
constexpr std::uintptr_t kFogHistoryExposureRva = 0x3E3E337;
safetyhook::MidHook g_fog_history_hook;
std::atomic<std::uint64_t> g_fog_history_fixed{0};

std::uintptr_t on_exposure(std::uintptr_t view);

void on_fog_history_exposure(safetyhook::Context& ctx) {
    if (!g_exposure_from_left.load(std::memory_order_relaxed) || !g_exposure_hook) {
        return;
    }
    std::uintptr_t main = 0;
    {
        std::scoped_lock lock(g_mutex);
        if (ctx.rsi && ctx.rsi == g_views[1]) {
            main = g_views[0];
        }
    }
    if (!main) {
        return;
    }
    const std::uintptr_t before = ctx.r8;
    const std::uintptr_t left = g_exposure_hook.call<std::uintptr_t>(main);
    if (left) {
        ctx.r8 = left;
    }
    const std::uint64_t n = g_fog_history_fixed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4 || n % 20000 == 0) {
        log::info("stereo: right eye's fog history exposure (t1) 0x{:X} replaced by the left eye's 0x{:X} ({} times)", before,
                  left, n);
    }
}

std::uintptr_t on_exposure(std::uintptr_t view) {
    std::uintptr_t main = 0;
    if (g_exposure_from_left.load(std::memory_order_relaxed)) {
        std::scoped_lock lock(g_mutex);
        if (view && view == g_views[1]) {
            main = g_views[0];
        }
    }
    if (!main) {
        return g_exposure_hook.call<std::uintptr_t>(view);
    }
    const std::uintptr_t own = g_exposure_hook.call<std::uintptr_t>(view);
    const std::uintptr_t left = g_exposure_hook.call<std::uintptr_t>(main);
    const std::uint64_t n = g_exposure_redirects.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4 || n % 20000 == 0) {
        log::info("stereo: right eye's exposure record 0x{:X} replaced by the left eye's 0x{:X} ({} times)", own, left, n);
    }
    return left ? left : own;
}
bool g_proxy_ready = false;
safetyhook::MidHook g_begin_hook;
std::array<std::atomic<std::uint64_t>, 2> g_proxied{};
std::atomic<std::uint64_t> g_proxy_wrong_class{0};
std::atomic<std::uint64_t> g_proxy_no_head{0};

const float* proxy_translate(const Proxy* self) {
    return self->translate;
}

// Slot 11 (agent Z 3b): the CPU-built particles (stage 0x48 fills 0x3F59B10 / 0x3F5D2E0) take their facing from the
// camera entity's Transform. camera.cpp writes the head's full rotation into it, roll included, so camera-facing
// particles rolled in the world when the head tilted. The proxy returns a copy with the roll removed: the same forward
// axis (local +Z), the local X axis made level. Everything else in the Transform is the game's.
using TransformFn = const float* (*)(const void*);
std::uintptr_t g_real_transform = 0;
std::atomic<bool> g_roll_free{true}; // [views] particles_level
std::atomic<std::uint64_t> g_roll_free_calls{0};

const float* proxy_transform(Proxy* self) {
    const float* real = reinterpret_cast<TransformFn>(g_real_transform)(self);
    if (!real || !g_roll_free.load(std::memory_order_relaxed) || !mem::try_copy(self->transform, reinterpret_cast<std::uintptr_t>(real), sizeof(self->transform))) {
        return real;
    }
    const vr::Quat q{self->transform[0], self->transform[1], self->transform[2], self->transform[3]};
    const vr::Vec3 f = vr::rotate(q, {0.0f, 0.0f, 1.0f});
    const float yaw = std::atan2(f[0], f[2]);
    const float pitch = std::atan2(-f[1], std::sqrt(f[0] * f[0] + f[2] * f[2]));
    const vr::Quat qy{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    const vr::Quat qx{std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
    const vr::Quat level = vr::multiply(qy, qx);
    std::memcpy(self->transform, level.data(), sizeof(level));
    g_roll_free_calls.fetch_add(1, std::memory_order_relaxed);
    return self->transform;
}

// The eye's offset in the head and its field of view, for the head view `head` (or the latest located eyes when the
// view isn't in the camera module's history). False if neither is known.
bool eye_offset(int eye, const std::optional<camera::HeadView>& head, const std::optional<Eyes>& eyes, float scale,
                float ipd_override, vr::RigidPose& offset, camera::EyeFov& fov) {
    const std::array<vr::RigidPose, 2>* both = nullptr;
    if (head) {
        offset = eye_in_head(head->pose, head->eye_pose[eye], scale);
        fov = head->eye_fov[eye];
        both = &head->eye_pose;
    } else if (eyes) {
        offset = eye_in_head(eyes->centre_xr, eyes->eye_xr[eye], scale);
        fov = eyes->fov[eye];
        both = &eyes->eye_xr;
    } else {
        return false;
    }
    if (ipd_override > 0.0f && both) {
        // A chosen IPD: the eye offset stretched so the eyes sit that far apart (the runtime's direction kept).
        const auto& a = (*both)[0].position;
        const auto& b = (*both)[1].position;
        const float runtime = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        if (runtime > 0.01f) {
            const float factor = ipd_override / runtime;
            for (float& axis : offset.position) {
                axis *= factor;
            }
        }
    }
    return true;
}

std::array<std::atomic<std::uint64_t>, 2> g_rewrites{};
std::atomic<std::uint64_t> g_without_eyes{0};
std::atomic<std::uint64_t> g_without_camera{0};
std::atomic<std::uint64_t> g_orthographic{0};
std::atomic<std::uint64_t> g_single_rewrites{0};
std::atomic<std::uint64_t> g_unmatched{0};

// Views whose camera blocks this frame came from the hook (the eye views, or the one view showing the driven camera),
// for the later stage hooks: stage 0x1B's reprojection and stage 0x40's velocity / TAA blocks read the controller
// directly, after the hook (agent N 1.5, 1.6).
std::array<std::atomic<std::uintptr_t>, 4> g_rewritten{};
void mark_rewritten(std::uintptr_t view) {
    for (auto& slot : g_rewritten) {
        if (slot.load(std::memory_order_relaxed) == view) {
            return;
        }
    }
    static std::atomic<std::size_t> next{0};
    g_rewritten[next.fetch_add(1, std::memory_order_relaxed) % g_rewritten.size()].store(view, std::memory_order_relaxed);
}
bool was_rewritten(std::uintptr_t view) {
    for (const auto& slot : g_rewritten) {
        if (view && slot.load(std::memory_order_relaxed) == view) {
            return true;
        }
    }
    return false;
}

// The room filter of outline draws (agent R): 0x17E3320(level object, view, position) -> bool; counted per view.
safetyhook::InlineHook g_room_hook;
std::array<std::atomic<std::uintptr_t>, 2> g_room_view{};
std::array<std::atomic<std::uint64_t>, 2> g_room_calls{}, g_room_false{};
std::atomic<std::uint64_t> g_room_copies{0};
std::atomic<std::int32_t> g_room_main{-1}, g_room_eye{-1};

bool on_room_filter(std::uintptr_t level_object, std::uintptr_t view, std::uintptr_t position) {
    const bool visible = g_room_hook.call<bool>(level_object, view, position);
    for (std::size_t i = 0; i < 2; ++i) {
        if (view && g_room_view[i].load(std::memory_order_relaxed) == view) {
            g_room_calls[i].fetch_add(1, std::memory_order_relaxed);
            if (!visible) {
                g_room_false[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return visible;
}

safetyhook::MidHook g_reprojection_hook; // stage 0x1B, 0x3E32C44
safetyhook::MidHook g_velocity_hook;     // stage 0x40, 0x3E12730
std::atomic<std::uint64_t> g_reprojections{0};
std::atomic<std::uint64_t> g_velocity_rewrites{0};
std::atomic<int> g_logged_reprojection{0};
std::atomic<std::uint8_t> g_outline_main{0xFF}, g_outline_eye{0xFF};
std::atomic<std::uint64_t> g_outline_copies{0};
constexpr std::uintptr_t kRenderSettings = 0x604AA10; // DAT_14604AA10: +0x80 / +0x77 antialiasing / upscaler modes

void rewrite_views_of(std::uintptr_t view) {
    auto* base = reinterpret_cast<std::uint8_t*>(view);
    rewrite_view_block(base + layout::kPerView0, base + layout::kPerCamera0);
    rewrite_view_block(base + layout::kPerView1, base + layout::kPerCamera1);
    mark_rewritten(view);
}

// Stage 0x1B (clustered lighting) execute, after it computed P x V of the head into 16 doubles at [rsp+0x20] for its
// temporal reprojection: the eye's jittered block [1] instead (agent N fix 2). rbx = the view, rdi = the stage.
void on_reprojection(safetyhook::Context& ctx) {
    const std::uintptr_t view = ctx.rbx;
    if (!was_rewritten(view)) {
        return;
    }
    camera::Mat4 v{}, p{};
    std::memcpy(v.data(), reinterpret_cast<const void*>(view + layout::kPerCamera1 + layout::kView), sizeof(v));
    std::memcpy(p.data(), reinterpret_cast<const void*>(view + layout::kPerCamera1 + layout::kProjection), sizeof(p));
    double out[16];
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += static_cast<double>(p[k * 4 + row]) * static_cast<double>(v[column * 4 + k]);
            }
            out[column * 4 + row] = sum;
        }
    }
    auto* game = reinterpret_cast<double*>(ctx.rsp + 0x20);
    if (g_logged_reprojection.fetch_add(1, std::memory_order_relaxed) < 4) {
        log::info("stereo: stage 0x1B view 0x{:X}: game P*V [{:.4f} {:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} {:.4f} {:.4f} | "
                  "{:.4f} {:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} {:.4f} {:.4f}] -> eye [{:.4f} {:.4f} {:.4f} {:.4f} | "
                  "{:.4f} {:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} {:.4f} {:.4f}]",
                  view, game[0], game[1], game[2], game[3], game[4], game[5], game[6], game[7], game[8], game[9],
                  game[10], game[11], game[12], game[13], game[14], game[15], out[0], out[1], out[2], out[3], out[4],
                  out[5], out[6], out[7], out[8], out[9], out[10], out[11], out[12], out[13], out[14], out[15]);
    }
    std::memcpy(game, out, sizeof(out));
    g_reprojections.fetch_add(1, std::memory_order_relaxed);
}

// Stage 0x40 (camera velocity, TAA) execute, after it built its current camera block from the controller: the eye's
// block instead ([1] in TAA / upscaler modes, else [0]); a stage seen for the first time gets it as its previous
// block too (agent N fix 3). rbx = the stage, its view at [stage+8].
void on_velocity(safetyhook::Context& ctx) {
    const std::uintptr_t stage = ctx.rbx;
    const auto view = mem::read<std::uintptr_t>(stage + 8);
    if (!view || !was_rewritten(view)) {
        return;
    }
    const auto settings = mem::read<std::uintptr_t>(g_module.base + kRenderSettings);
    const std::uint8_t aa = settings ? mem::read<std::uint8_t>(settings + 0x80) : 0;
    const std::uint8_t upscaler = settings ? mem::read<std::uint8_t>(settings + 0x77) : 0;
    const bool jittered = (aa >= 2 && aa <= 4) || upscaler == 2 || upscaler == 3 || upscaler == 5;
    const auto* block = reinterpret_cast<const std::uint8_t*>(view + (jittered ? layout::kPerCamera1 : layout::kPerCamera0));
    auto* current = reinterpret_cast<std::uint8_t*>(stage + 0x2230);
    std::memcpy(current + 0x00, block + layout::kView, 64);
    std::memcpy(current + 0x40, block + layout::kProjection, 64);
    std::memcpy(current + 0x80, block + layout::kViewProjection, 64);
    std::memcpy(current + 0xC0, block + layout::kPosition, 12);
    static std::array<std::atomic<std::uintptr_t>, 8> seen{};
    bool known = false;
    for (const auto& s : seen) {
        known = known || s.load(std::memory_order_relaxed) == stage;
    }
    if (!known) {
        static std::atomic<std::size_t> next{0};
        seen[next.fetch_add(1, std::memory_order_relaxed) % seen.size()].store(stage, std::memory_order_relaxed);
        auto* previous = reinterpret_cast<std::uint8_t*>(stage + 0x2150);
        std::memcpy(previous, current, 0xCC);
        log::info("stereo: stage 0x40 0x{:X} of view 0x{:X}: velocity / TAA camera from the eye's block [{}] (aa {}, "
                  "upscaler {})", stage, view, jittered ? 1 : 0, aa, upscaler);
    }
    g_velocity_rewrites.fetch_add(1, std::memory_order_relaxed);
}

bool bytes_at(std::uintptr_t address, std::initializer_list<std::uint8_t> expected) {
    std::array<std::uint8_t, 32> got{};
    if (expected.size() > got.size() || !mem::try_copy(got.data(), address, expected.size())) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), got.begin());
}

bool matrix_matches(const float* candidate, const camera::Mat4& expected) {
    for (std::size_t i = 0; i < 16; ++i) {
        if (std::abs(candidate[i] - expected[i]) > 1e-3f * (std::max)(1.0f, std::abs(expected[i]))) {
            return false;
        }
    }
    return true;
}

// The head view the camera module applied for the view matrix this block was built from (the render
// thread works a frame or two behind the game thread, so it's looked up, not assumed).
std::optional<camera::HeadView> applied_head(const std::uint8_t* block) {
    std::array<camera::AppliedView, camera::kAppliedHistory> applied;
    const std::size_t count = camera::applied_views(applied);
    const auto* view_matrix = reinterpret_cast<const float*>(block + layout::kView);
    for (std::size_t i = 0; i < count; ++i) {
        if (matrix_matches(view_matrix, applied[i].view_matrix)) {
            return applied[i].head;
        }
    }
    return std::nullopt;
}

// Right after a render view built its PerCamera blocks in begin-recording, before the upload:
// rdi = the view.
void on_camera_built(safetyhook::Context& ctx) {
    const std::uintptr_t view = ctx.rdi;
    int eye = -1;
    std::optional<Eyes> eyes;
    float scale = 1.0f;
    float ipd_override = 0.0f;
    {
        std::scoped_lock lock(g_mutex);
        ipd_override = g_ipd_override_m;
        if (view == g_views[0]) {
            eye = 0;
        } else if (view == g_views[1]) {
            eye = 1;
        }
        eyes = g_eyes;
        scale = g_world_scale;
    }
    // The builder skips both blocks for a view without a camera; an orthographic camera's projection
    // isn't the perspective form rewritten here. Unassigned views are only touched when they show the
    // camera this mod drives.
    const auto controller = mem::read<std::uintptr_t>(view + layout::kReadStateController);
    if (eye < 0 && controller) {
        if (const Proxy* proxy = doll_proxy(controller)) {
            // A UI doll's eye view, built from its proxy: the view and position are that eye's already; the window's
            // off-axis projection goes in.
            rewrite_block(reinterpret_cast<std::uint8_t*>(view + layout::kPerCamera0), vr::RigidPose{}, proxy->fov, false);
            rewrite_block(reinterpret_cast<std::uint8_t*>(view + layout::kPerCamera1), vr::RigidPose{}, proxy->fov, true);
            rewrite_views_of(view);
            return;
        }
    }
    if (eye < 0 && (!controller || controller != camera::controller())) {
        return;
    }
    if (!controller) {
        g_without_camera.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (mem::read<std::uint8_t>(controller + layout::kControllerIsOrthographic) != 0) {
        g_orthographic.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto* block0 = reinterpret_cast<std::uint8_t*>(view + layout::kPerCamera0);
    auto* block1 = reinterpret_cast<std::uint8_t*>(view + layout::kPerCamera1);
    const std::optional<camera::HeadView> head = applied_head(block0);
    if (eye < 0) {
        // Mono or alternate eye (one view): the game draws block [1] with a symmetric projection from the
        // controller's own FOV and aspect, so the head view's field of view goes in here.
        if (!head) {
            g_unmatched.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        rewrite_block(block0, vr::RigidPose{}, head->fov, false);
        rewrite_block(block1, vr::RigidPose{}, head->fov, true);
        rewrite_views_of(view);
        g_single_rewrites.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Same-frame stereo: this view draws one eye of the head view it was built from (or, if that isn't
    // found, of the latest located eyes).
    vr::RigidPose offset;
    camera::EyeFov fov;
    const auto& ring = g_proxies[static_cast<std::size_t>(eye)].slots;
    const auto proxy = std::find_if(ring.begin(), ring.end(), [&](const Proxy& p) {
        return reinterpret_cast<std::uintptr_t>(&p) == controller;
    });
    if (proxy != ring.end()) {
        // The view was built from this eye's proxy: the view and position are the eye's already; only the eye's
        // projection goes in.
        fov = proxy->fov;
        g_proxied[static_cast<std::size_t>(eye)].fetch_add(1, std::memory_order_relaxed);
    } else {
        if (!eye_offset(eye, head, eyes, scale, ipd_override, offset, fov)) {
            g_without_eyes.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!head) {
            g_unmatched.fetch_add(1, std::memory_order_relaxed);
        }
        // Without a proxy the draws are relative to the head: keep the head's position so the shaders that add
        // +0xD0 back agree with the geometry (agent S Fix A); the eye's rotation and projection still go in.
        offset.position = {0.0f, 0.0f, 0.0f};
    }
    rewrite_block(block0, offset, fov, false);
    rewrite_block(block1, offset, fov, true);
    rewrite_views_of(view);
    // The outline combine runs only where [view+0x2961] is 0; the created eye view never gets the game's per-player
    // updates of that byte (agent N 4): it follows the main view's.
    std::uintptr_t main_view = 0, eye_view = 0;
    {
        std::scoped_lock lock(g_mutex);
        main_view = g_views[0];
        eye_view = g_views[1];
    }
    if (main_view && eye_view) {
        const auto main_gate = mem::read<std::uint8_t>(main_view + layout::kOutlineGate);
        const auto eye_gate = mem::read<std::uint8_t>(eye_view + layout::kOutlineGate);
        if (main_gate != g_outline_main.load(std::memory_order_relaxed) || eye_gate != g_outline_eye.load(std::memory_order_relaxed)) {
            log::info("stereo: outline gate [view+0x2961] main 0x{:X} = {}, eye 0x{:X} = {}{}", main_view, main_gate,
                      eye_view, eye_gate, main_gate != eye_gate ? " (copying the main view's)" : "");
            g_outline_main.store(main_gate, std::memory_order_relaxed);
            g_outline_eye.store(main_gate, std::memory_order_relaxed);
        }
        if (eye_gate != main_gate) {
            *reinterpret_cast<std::uint8_t*>(eye_view + layout::kOutlineGate) = main_gate;
            g_outline_copies.fetch_add(1, std::memory_order_relaxed);
        }
        // The player's room (agent R): outline draws are filtered by [view+0x2BF8], which the game writes only into
        // views registered with the player manager; the eye view would stay in room 0 and lose every indoor outline.
        // Also the see-through (focus) source +0x2CC8..+0x2D87 and the focus constants +0x2C00..+0x2C3F built from it
        // (zero in the eye otherwise: wall / roof cut-away could differ per eye).
        g_room_view[0].store(main_view, std::memory_order_relaxed);
        g_room_view[1].store(eye_view, std::memory_order_relaxed);
        const auto main_room = mem::read<std::int32_t>(main_view + 0x2BF8);
        const auto eye_room = mem::read<std::int32_t>(eye_view + 0x2BF8);
        if (main_room != g_room_main.load(std::memory_order_relaxed) || eye_room != g_room_eye.load(std::memory_order_relaxed)) {
            log::info("stereo: player room [view+0x2BF8] main {} eye {} (+0x2BFC {} / {}, +0x2BFD {} / {}, +0x2AEA {} / {}); "
                      "copying the main view's room, player and focus data to the eye",
                      main_room, eye_room, mem::read<std::uint8_t>(main_view + 0x2BFC), mem::read<std::uint8_t>(eye_view + 0x2BFC),
                      mem::read<std::uint8_t>(main_view + 0x2BFD), mem::read<std::uint8_t>(eye_view + 0x2BFD),
                      mem::read<std::uint8_t>(main_view + 0x2AEA), mem::read<std::uint8_t>(eye_view + 0x2AEA));
            g_room_main.store(main_room, std::memory_order_relaxed);
            g_room_eye.store(main_room, std::memory_order_relaxed);
        }
        std::memcpy(reinterpret_cast<void*>(eye_view + 0x2BE8), reinterpret_cast<const void*>(main_view + 0x2BE8), 0x18);
        *reinterpret_cast<std::uint8_t*>(eye_view + 0x2AEA) = mem::read<std::uint8_t>(main_view + 0x2AEA);
        *reinterpret_cast<std::uint8_t*>(eye_view + 0x2258) = mem::read<std::uint8_t>(main_view + 0x2258);
        std::memcpy(reinterpret_cast<void*>(eye_view + 0x2CC8), reinterpret_cast<const void*>(main_view + 0x2CC8), 0xC0);
        std::memcpy(reinterpret_cast<void*>(eye_view + 0x2C00), reinterpret_cast<const void*>(main_view + 0x2C00), 0x40);
        g_room_copies.fetch_add(1, std::memory_order_relaxed);
    }
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

void rewrite_view_block(std::uint8_t* per_view, const std::uint8_t* per_camera) {
    camera::Mat4 v{}, p{};
    float far_z = 0.0f;
    std::memcpy(v.data(), per_camera + layout::kView, sizeof(v));
    std::memcpy(p.data(), per_camera + layout::kProjection, sizeof(p));
    std::memcpy(&far_z, per_camera + layout::kFar, sizeof(far_z));
    if (p[0] == 0.0f || p[5] == 0.0f) {
        return;
    }
    // Inverse of the rigid view: the transposed rotation and the camera position.
    camera::Mat4 inverse{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            inverse[c * 4 + r] = v[r * 4 + c];
        }
    }
    const vr::Vec3 position = camera_position(v);
    inverse[12] = position[0];
    inverse[13] = position[1];
    inverse[14] = position[2];
    inverse[15] = 1.0f;
    std::memcpy(per_view + layout::kViewInverse, inverse.data(), sizeof(inverse));
    const float R[3] = {v[0], v[4], v[8]};
    const float U[3] = {v[1], v[5], v[9]};
    const float F[3] = {v[2], v[6], v[10]};
    const float corners[3][2] = {{-1.0f, 1.0f}, {3.0f, 1.0f}, {-1.0f, -3.0f}};
    for (int i = 0; i < 3; ++i) {
        const float a = (corners[i][0] - p[8]) / p[0];
        const float b = (corners[i][1] - p[9]) / p[5];
        const float ray[4] = {far_z * (F[0] + R[0] * a + U[0] * b), far_z * (F[1] + R[1] * a + U[1] * b),
                              far_z * (F[2] + R[2] * a + U[2] * b), 0.0f};
        std::memcpy(per_view + layout::kViewCorners + 16 * i, ray, sizeof(ray));
    }
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

// Inverse of a rigid view (world to camera): camera to world.
camera::Mat4 rigid_inverse(const camera::Mat4& v) {
    camera::Mat4 inverse{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            inverse[c * 4 + r] = v[r * 4 + c];
        }
    }
    const vr::Vec3 position = camera_position(v);
    inverse[12] = position[0];
    inverse[13] = position[1];
    inverse[14] = position[2];
    inverse[15] = 1.0f;
    return inverse;
}

// GameRenderView begin-recording entry (0x17F21E0, render thread, per view; the frame sync 0x4072CB0 has just
// copied the real controller into ReadState +0x21D8): an eye view gets its eye's proxy controller for this frame.
// A UI doll's eye view: its proxy sits at that eye, looking as the doll camera looks (dolls.cpp: the window).
void doll_view_begin(std::uintptr_t view) {
    std::uintptr_t controller = 0;
    if (!mem::try_copy(&controller, view + layout::kReadStateController, sizeof(controller)) || !controller) {
        return;
    }
    std::uintptr_t vtable = 0;
    mem::try_copy(&vtable, controller, sizeof(vtable));
    if (vtable != g_module.base + kControllerVtableRva) {
        return;
    }
    const auto doll = dolls::eye_camera(view, controller);
    if (!doll) {
        return;
    }
    camera::Mat4 camera_view{};
    std::memcpy(camera_view.data(), reinterpret_cast<const void*>(controller + camera::layout::kViewMatrix), sizeof(camera_view));
    const vr::RigidPose offset = vr::from_openxr(vr::RigidPose{doll->offset, vr::Quat{0.0f, 0.0f, 0.0f, 1.0f}});
    const camera::Mat4 view_matrix = eye_view(camera_view, offset);
    const camera::Mat4 inverse = rigid_inverse(view_matrix);
    const vr::Vec3 position = camera_position(view_matrix);
    ProxyRing& ring = doll_ring(view);
    Proxy& proxy = ring.slots[ring.next];
    ring.next = (ring.next + 1) % kProxyRing;
    std::memcpy(proxy.object, reinterpret_cast<const void*>(controller), kControllerSize);
    const std::uintptr_t proxy_vtable = reinterpret_cast<std::uintptr_t>(g_proxy_vtable.data());
    std::memcpy(proxy.object, &proxy_vtable, sizeof(proxy_vtable));
    std::memcpy(proxy.object + camera::layout::kViewMatrix, view_matrix.data(), sizeof(view_matrix));
    std::memcpy(proxy.object + camera::layout::kInvViewMatrix, inverse.data(), sizeof(inverse));
    proxy.translate[0] = position[0];
    proxy.translate[1] = position[1];
    proxy.translate[2] = position[2];
    proxy.fov = doll->fov;
    *reinterpret_cast<std::uintptr_t*>(view + layout::kReadStateController) = reinterpret_cast<std::uintptr_t>(&proxy);
    g_doll_proxied.fetch_add(1, std::memory_order_relaxed);
}

void on_view_begin(safetyhook::Context& ctx) {
    if (!g_proxy_ready) {
        return;
    }
    const std::uintptr_t view = ctx.rcx;
    if (stereo::eye_of(view) < 0) {
        if (dolls::enabled()) {
            doll_view_begin(view);
        }
        return;
    }
    if (g_origin.load(std::memory_order_relaxed) != Origin::Proxy) {
        return;
    }
    int eye = -1;
    std::optional<Eyes> eyes;
    float scale = 1.0f;
    float ipd_override = 0.0f;
    {
        std::scoped_lock lock(g_mutex);
        if (view == g_views[0]) {
            eye = 0;
        } else if (view == g_views[1]) {
            eye = 1;
        }
        eyes = g_eyes;
        scale = g_world_scale;
        ipd_override = g_ipd_override_m;
    }
    if (eye < 0) {
        return;
    }
    std::uintptr_t controller = 0;
    if (!mem::try_copy(&controller, view + layout::kReadStateController, sizeof(controller)) || !controller) {
        return;
    }
    std::uintptr_t vtable = 0;
    mem::try_copy(&vtable, controller, sizeof(vtable));
    if (vtable != g_module.base + kControllerVtableRva) {
        // Not the gameplay controller class (or already a proxy): leave it.
        if (vtable != reinterpret_cast<std::uintptr_t>(g_proxy_vtable.data()) &&
            g_proxy_wrong_class.fetch_add(1, std::memory_order_relaxed) < 5) {
            log::warn("stereo: eye view 0x{:X}'s controller 0x{:X} has vtable {}, not the gameplay controller's; no "
                      "per-eye origin for it", view, controller, mem::describe(g_module, vtable));
        }
        return;
    }
    // The head view this frame was built from: the controller's view matrix, looked up in the camera module's history.
    camera::Mat4 head_view{};
    std::memcpy(head_view.data(), reinterpret_cast<const void*>(controller + camera::layout::kViewMatrix), sizeof(head_view));
    std::optional<camera::HeadView> head;
    {
        std::array<camera::AppliedView, camera::kAppliedHistory> applied;
        const std::size_t count = camera::applied_views(applied);
        for (std::size_t i = 0; i < count; ++i) {
            if (matrix_matches(head_view.data(), applied[i].view_matrix)) {
                head = applied[i].head;
                break;
            }
        }
    }
    vr::RigidPose offset;
    camera::EyeFov fov;
    if (!eye_offset(eye, head, eyes, scale, ipd_override, offset, fov)) {
        g_proxy_no_head.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const camera::Mat4 view_matrix = eye_view(head_view, offset);
    const camera::Mat4 inverse = rigid_inverse(view_matrix);
    const vr::Vec3 position = camera_position(view_matrix);
    ProxyRing& ring = g_proxies[static_cast<std::size_t>(eye)];
    Proxy& proxy = ring.slots[ring.next];
    ring.next = (ring.next + 1) % kProxyRing;
    std::memcpy(proxy.object, reinterpret_cast<const void*>(controller), kControllerSize);
    const std::uintptr_t proxy_vtable = reinterpret_cast<std::uintptr_t>(g_proxy_vtable.data());
    std::memcpy(proxy.object, &proxy_vtable, sizeof(proxy_vtable));
    std::memcpy(proxy.object + camera::layout::kViewMatrix, view_matrix.data(), sizeof(view_matrix));
    std::memcpy(proxy.object + camera::layout::kInvViewMatrix, inverse.data(), sizeof(inverse));
    proxy.translate[0] = position[0];
    proxy.translate[1] = position[1];
    proxy.translate[2] = position[2];
    proxy.fov = fov;
    // The eye's own projection and its inverse in the proxy (agent AH fix 2; with the tile-ray hook).
    if (g_surface_fix.load(std::memory_order_relaxed) && !proxy.object[kControllerOrthographic]) {
        camera::Mat4 game{};
        std::memcpy(game.data(), proxy.object + kControllerProjection, sizeof(game));
        const camera::Mat4 p = eye_projection(game, fov, 0.0f, 0.0f);
        // Column-major [a,0,0,0, 0,c,0,0, b,e,zs,1, 0,0,zo,0]: inverse [1/a,0,0,0, 0,1/c,0,0, 0,0,0,1/zo,
        // -b/a,-e/c,1,-zs/zo]. Only that shape (a perspective projection as the game builds it).
        if (p[11] == 1.0f && p[15] == 0.0f && p[0] != 0.0f && p[5] != 0.0f && p[14] != 0.0f) {
            const camera::Mat4 inverse_p{1.0f / p[0], 0.0f, 0.0f, 0.0f,  0.0f, 1.0f / p[5], 0.0f, 0.0f,
                                       0.0f, 0.0f, 0.0f, 1.0f / p[14], -p[8] / p[0], -p[9] / p[5], 1.0f, -p[10] / p[14]};
            std::memcpy(proxy.object + kControllerProjection, p.data(), sizeof(p));
            std::memcpy(proxy.object + kControllerInverseProjection, inverse_p.data(), sizeof(inverse_p));
        }
    }
    *reinterpret_cast<std::uintptr_t*>(view + layout::kReadStateController) = reinterpret_cast<std::uintptr_t>(&proxy);
    // Agent AE 3: begin-recording step 0x3CC6DC0 sets the view's "camera changed" flag (+0x2AED: fog history reset,
    // exposure jumps instead of adapting) whenever +0x21D8 differs from the controller it saw last (+0x2B00). The
    // proxy comes from a ring, so that was every frame, in both eyes. The last-seen field gets this frame's proxy too,
    // unless the real controller changed (a real camera cut keeps its reset). [views] keep_view_history.
    if (g_keep_history.load(std::memory_order_relaxed)) {
        static std::array<std::uintptr_t, 2> last_real{};
        if (last_real[static_cast<std::size_t>(eye)] == controller) {
            *reinterpret_cast<std::uintptr_t*>(view + kLastSeenController) = reinterpret_cast<std::uintptr_t>(&proxy);
        }
        last_real[static_cast<std::size_t>(eye)] = controller;
    }
    const std::uint64_t n = g_proxied[static_cast<std::size_t>(eye)].load(std::memory_order_relaxed);
    if (n < 4 || n % 600 == 0) {
        // The real controller's origin (its slot 9) next to the eye's: they should differ by half the IPD along the
        // head's right axis.
        using TranslateFn = const float* (*)(std::uintptr_t);
        std::uintptr_t real_slot = 0;
        mem::try_copy(&real_slot, vtable + kTranslateSlot * sizeof(void*), sizeof(real_slot));
        const float* real = real_slot ? reinterpret_cast<TranslateFn>(real_slot)(controller) : nullptr;
        log::info("stereo: {} eye view 0x{:X}: proxy 0x{:X} for controller 0x{:X}; origin ({:.3f}, {:.3f}, {:.3f}), the "
                  "controller's ({:.3f}, {:.3f}, {:.3f}); eye offset ({:.4f}, {:.4f}, {:.4f}){}",
                  eye == 0 ? "left" : "right", view, reinterpret_cast<std::uintptr_t>(&proxy), controller, position[0],
                  position[1], position[2], real ? real[0] : 0.0f, real ? real[1] : 0.0f, real ? real[2] : 0.0f,
                  offset.position[0], offset.position[1], offset.position[2], head ? "" : " (latest eyes, view not matched)");
    }
}

void configure(const Config& config) {
    const std::string origin = text::to_lower(config.get("views", "eye_origin", "proxy"));
    const Origin wanted = origin == "head" ? Origin::Head : Origin::Proxy;
    if (g_origin.exchange(wanted) != wanted) {
        log::info("stereo: eye origin {}", wanted == Origin::Proxy ? "per eye (stereo parallax)" : "the head (no parallax)");
    }
    const std::string scale = config.get("xr", "world_scale");
    const std::string ipd = config.get("xr", "ipd_mm");
    std::scoped_lock lock(g_mutex);
    g_world_scale = scale.empty() ? 1.0f : std::clamp(std::strtof(scale.c_str(), nullptr), 0.25f, 4.0f);
    const float mm = ipd.empty() ? 0.0f : std::strtof(ipd.c_str(), nullptr);
    g_ipd_override_m = mm >= 40.0f && mm <= 90.0f ? mm / 1000.0f : 0.0f;
    log::info("stereo: world scale {:.2f}, IPD {}", g_world_scale,
              g_ipd_override_m > 0.0f ? std::format("{:.0f} mm (set)", mm) : std::string("from the headset"));
}

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    configure(config);
    const auto site = sig::lookup("render_view_camera_built");
    if (!site) {
        health::set("same-frame stereo", health::State::Unavailable, "signature render_view_camera_built");
        log::error("stereo: signature render_view_camera_built not resolved; same-frame stereo unavailable");
        return;
    }
    auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(*site), &on_camera_built);
    if (!hook) {
        health::set("same-frame stereo", health::State::Unavailable, "hook failed");
        log::error("stereo: mid-hook at exe+0x{:X} failed (error type {})", *site - module.base,
                   static_cast<int>(hook.error().type));
        return;
    }
    g_hook = std::move(*hook);
    health::set("same-frame stereo", health::State::Ready);
    log::info("stereo: hook render_view_camera_built installed at exe+0x{:X}", *site - module.base);

    // Per-eye origins: the proxy controllers' vtable is the gameplay controller's with slot 9 replaced.
    const auto begin = sig::lookup("render_view_begin");
    const std::uintptr_t real_vtable = module.base + kControllerVtableRva;
    std::uintptr_t real_translate = 0;
    if (begin && *begin - module.base == 0x17F21E0 &&
        mem::try_copy(g_proxy_vtable.data(), real_vtable, sizeof(g_proxy_vtable)) &&
        mem::try_copy(&real_translate, real_vtable + kTranslateSlot * sizeof(void*), sizeof(real_translate)) &&
        real_translate == module.base + 0x3CF0740) {
        g_proxy_vtable[kTranslateSlot] = reinterpret_cast<std::uintptr_t>(&proxy_translate);
        // Slot 11 only when it is the expected Transform getter (mov rdx,[rcx+318h]; mov rcx,[rcx+320h]; jmp).
        std::uintptr_t real_transform = g_proxy_vtable[kTransformSlot];
        std::array<std::uint8_t, 14> code{};
        constexpr std::array<std::uint8_t, 14> expected{0x48, 0x8B, 0x91, 0x18, 0x03, 0x00, 0x00,
                                                        0x48, 0x8B, 0x89, 0x20, 0x03, 0x00, 0x00};
        if (real_transform == module.base + 0x3CF0760 && mem::try_copy(code.data(), real_transform, code.size()) &&
            code == expected) {
            g_real_transform = real_transform;
            g_roll_free.store(config.get_bool("views", "particles_level", true), std::memory_order_relaxed);
            g_keep_history.store(config.get_bool("views", "keep_view_history", true), std::memory_order_relaxed);
            g_proxy_vtable[kTransformSlot] = reinterpret_cast<std::uintptr_t>(&proxy_transform);
            log::info("stereo: proxy slot 11 (camera Transform, exe+0x3CF0760) replaced: camera-facing particles {}",
                      g_roll_free.load() ? "stay level when the head rolls" : "roll with the head ([views] particles_level)");
        } else {
            log::warn("stereo: controller slot 11 isn't the expected Transform getter (0x{:X}); particles roll with the head",
                      real_transform);
        }
        g_exposure_from_left.store(config.get_bool("views", "eye_exposure_from_left", true), std::memory_order_relaxed);
        g_surface_fix.store(config.get_bool("views", "eye_surface_mesh", true), std::memory_order_relaxed);
        g_ssao_fix.store(config.get_bool("views", "eye_ssao", true), std::memory_order_relaxed);
        // movss [rbp+0A0h], xmm0 ; mov edx, 40h
        if (bytes_at(module.base + kSsaoRaysRva, {0xF3, 0x0F, 0x11, 0x85, 0xA0, 0x00, 0x00, 0x00, 0xBA, 0x40, 0x00, 0x00, 0x00})) {
            if (auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(module.base + kSsaoRaysRva), &on_ssao_rays)) {
                g_ssao_hook = std::move(*h);
                log::info("stereo: SSAO uses each eye's own projection (exe+0x{:X})", kSsaoRaysRva);
            }
        } else {
            log::warn("stereo: the SSAO ray constants exe+0x{:X} don't match this build", kSsaoRaysRva);
        }
        // mov [rcx+20h], edi ; mov edx, 57h
        if (bytes_at(module.base + kSurfaceRaysRva, {0x89, 0x79, 0x20, 0xBA, 0x57, 0x00, 0x00, 0x00})) {
            if (auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(module.base + kSurfaceRaysRva), &on_surface_rays)) {
                g_surface_hook = std::move(*h);
                log::info("stereo: surface tiles (blood pools) use each eye's own projection (exe+0x{:X}){}", kSurfaceRaysRva,
                          g_surface_fix.load() ? "" : " - off ([views] eye_surface_mesh)");
            }
        } else {
            log::warn("stereo: the surface tile pass exe+0x{:X} doesn't match this build; blood pools may have gaps per eye",
                      kSurfaceRaysRva);
            g_surface_fix.store(false);
        }
        if (bytes_at(module.base + kExposureRva, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xB2, 0x21})) {
            if (auto e = safetyhook::InlineHook::create(reinterpret_cast<void*>(module.base + kExposureRva),
                                                        reinterpret_cast<void*>(&on_exposure))) {
                g_exposure_hook = std::move(*e);
                // movsd xmm0, [rip+..] ; lea rdx, [rbp+120h]
                if (bytes_at(module.base + kFogHistoryExposureRva, {0xF2, 0x0F, 0x10, 0x05}) &&
                    bytes_at(module.base + kFogHistoryExposureRva + 8, {0x48, 0x8D, 0x95, 0x20, 0x01, 0x00, 0x00})) {
                    if (auto f = safetyhook::MidHook::create(reinterpret_cast<void*>(module.base + kFogHistoryExposureRva),
                                                             &on_fog_history_exposure)) {
                        g_fog_history_hook = std::move(*f);
                        log::info("stereo: the right eye's fog history uses the left eye's exposure too (exe+0x{:X})",
                                  kFogHistoryExposureRva);
                    }
                } else {
                    log::warn("stereo: the fog history exposure read exe+0x{:X} doesn't match this build", kFogHistoryExposureRva);
                }
                log::info("stereo: the right eye uses the left eye's exposure (exe+0x{:X}){}", kExposureRva,
                          g_exposure_from_left.load() ? "" : " - off ([views] eye_exposure_from_left)");
            }
        } else {
            log::warn("stereo: the exposure lookup exe+0x{:X} doesn't match this build; each eye adapts its own", kExposureRva);
        }
        if (auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(*begin), &on_view_begin)) {
            g_begin_hook = std::move(*h);
            g_proxy_ready = true;
            health::set("stereo parallax", g_origin.load() == Origin::Proxy ? health::State::Ready : health::State::Off,
                        "[views] eye_origin");
            log::info("stereo: per-eye origin ready (begin-recording exe+0x17F21E0; proxy controllers of class "
                      "exe+0x{:X}, slot 9 exe+0x3CF0740 replaced); [views] eye_origin = {}",
                      kControllerVtableRva, g_origin.load() == Origin::Proxy ? "proxy" : "head");
        }
    }
    if (!g_proxy_ready) {
        health::set("stereo parallax", health::State::Unavailable, "controller class / begin-recording didn't match");
        log::warn("stereo: per-eye origin unavailable (begin-recording or the controller class doesn't match this build); "
                  "both eyes render from the head");
    }

    // The two stages that rebuild the camera from the controller after the hook (agent N 1.5 / 1.6), 4.1.1.7631656:
    // checked byte for byte before hooking.
    const std::uintptr_t reprojection = module.base + 0x3E32C44; // cmp byte [rdi+2411h],0 ; lea rcx,[rdi+24F8h]
    if (config.get_bool("views", "eye_reprojection", true) &&
        bytes_at(reprojection, {0x80, 0xBF, 0x11, 0x24, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x8F, 0xF8, 0x24, 0x00, 0x00})) {
        if (auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(reprojection), &on_reprojection)) {
            g_reprojection_hook = std::move(*h);
            log::info("stereo: stage 0x1B reprojection hook at exe+0x3E32C44 (lighting history follows each eye)");
        }
    } else {
        log::warn("stereo: stage 0x1B reprojection site doesn't match (or [views] eye_reprojection off); not hooked");
    }
    // Outline room filter (agent R), for counting what each eye drops.
    const std::uintptr_t room = module.base + 0x17E3320;
    if (bytes_at(room, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0x05})) {
        if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(room), reinterpret_cast<void*>(&on_room_filter))) {
            g_room_hook = std::move(*h);
            log::info("stereo: outline room filter hooked at exe+0x17E3320 (counting per eye)");
        }
    }
    const std::uintptr_t velocity = module.base + 0x3E12730; // cmp byte [rbx+18h],0 ; jz ; mov rcx,[rbx+2138h]
    if (config.get_bool("views", "eye_velocity", true) &&
        bytes_at(velocity, {0x80, 0x7B, 0x18, 0x00, 0x74, 0x21, 0x48, 0x8B, 0x8B, 0x38, 0x21, 0x00, 0x00})) {
        if (auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(velocity), &on_velocity)) {
            g_velocity_hook = std::move(*h);
            log::info("stereo: stage 0x40 velocity / TAA hook at exe+0x3E12730 (motion vectors follow each eye)");
        }
    } else {
        log::warn("stereo: stage 0x40 velocity site doesn't match (or [views] eye_velocity off); not hooked");
    }
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
    for (auto& slot : g_rewritten) {
        slot.store(0, std::memory_order_relaxed); // the stage hooks stop following views that may be gone
    }
    for (auto& v : g_room_view) {
        v.store(0, std::memory_order_relaxed);
    }
    log::info("stereo: off");
}

int eye_of(std::uintptr_t view) {
    std::scoped_lock lock(g_mutex);
    return !view ? -1 : view == g_views[0] ? 0 : view == g_views[1] ? 1 : -1;
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
    log::info("stereo: views left 0x{:X} right 0x{:X}; rewrites left {} right {}, single view {}; head view not "
              "matched {}; skipped: no eyes {}, no camera {}, orthographic {}",
              views[0], views[1], g_rewrites[0].load(std::memory_order_relaxed),
              g_rewrites[1].load(std::memory_order_relaxed), g_single_rewrites.load(std::memory_order_relaxed),
              g_unmatched.load(std::memory_order_relaxed), g_without_eyes.load(std::memory_order_relaxed),
              g_without_camera.load(std::memory_order_relaxed), g_orthographic.load(std::memory_order_relaxed));
    log::info("stereo: per-view lighting blocks rewritten with the camera; stage 0x1B reprojections {}, stage 0x40 "
              "velocity blocks {}; outline gate copies {} (main {}, eye {})",
              g_reprojections.load(), g_velocity_rewrites.load(), g_outline_copies.load(), g_outline_main.load(),
              g_outline_eye.load());
    log::info("stereo: outline room filter: left eye {} calls / {} dropped, right eye {} / {}; room data copied {} times "
              "(room main {}, eye {})",
              g_room_calls[0].load(), g_room_false[0].load(), g_room_calls[1].load(), g_room_false[1].load(),
              g_room_copies.load(), g_room_main.load(), g_room_eye.load());
    log::info("stereo: per-eye origin {}: proxied left {} right {}, wrong controller class {}, no head view {}; UI doll "
              "eye views proxied {}",
              !g_proxy_ready ? "unavailable" : g_origin.load() == Origin::Proxy ? "on" : "off (head)",
              g_proxied[0].load(), g_proxied[1].load(), g_proxy_wrong_class.load(), g_proxy_no_head.load(),
              g_doll_proxied.load());
}

} // namespace bg3vr::stereo
