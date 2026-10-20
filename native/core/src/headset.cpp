#include "bg3vr/headset.hpp"

#include "bg3vr/blit.hpp"
#include "bg3vr/bodyik.hpp"
#include "bg3vr/camera.hpp"
#include "bg3vr/gpu_trace.hpp"
#include "bg3vr/hold.hpp"
#include "bg3vr/holster.hpp"
#include "bg3vr/melee.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/bridge.hpp"
#include "bg3vr/locomotion.hpp"
#include "bg3vr/rayprobe.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/resolution.hpp"
#include "bg3vr/stereo.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/views.hpp"
#include "bg3vr/vrmenu.hpp"
#include "bg3vr/vrplay.hpp"
#include "bg3vr/win_util.hpp"
#include "bg3vr/xr.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace bg3vr::headset {

namespace {

enum class Mode { Screen, Mono, Alternate };

const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::Mono: return "mono";
    case Mode::Alternate: return "alternate";
    default: return "screen";
    }
}

// Settings (written by install() before the callback is registered, or atomically).
Mode g_mode = Mode::Screen;
float g_screen_width_m = 3.0f;
float g_screen_distance_m = 2.5f;
std::atomic<int> g_lag{-1}; // -1: find it in the RenderFrame
bool g_desktop_vsync = false; // false: Present doesn't wait for the monitor while the headset runs

// Everything below runs on the game's presenting thread, except install(), set_lag() and report().
bool g_failed = false;
bool g_ready = false;
xr::System g_system;
std::unique_ptr<xr::Session> g_session;
UINT g_image_width = 0;  // the back buffer the blitter was made for (it follows resizes)
UINT g_eye_width = 0;    // the eye images (the headset's recommended size when the blitter scales)
UINT g_eye_height = 0;
UINT g_quad_width = 0;   // the flat screen image
UINT g_quad_height = 0;
DXGI_FORMAT g_back_format = DXGI_FORMAT_UNKNOWN;
// The back buffer goes into the images through the blitter (any format, sRGB-decoded, sub-rectangles);
// a plain copy only when it can't be set up and the formats allow.
blit::Blitter g_blitter;
bool g_linearize = false;
UINT g_image_height = 0;

// Head tracking state: the view each eye image was last rendered with, whether the eye has an image
// yet, the eye the next head view is for (alternate), and how long the camera has gone without
// applying a new head view (no pose held: show the screen instead).
std::vector<XrView> g_rendered;
std::array<bool, 2> g_has_image{};
int g_next_eye = 0;
std::uint64_t g_last_serial = 0;
int g_stale_presents = 0;
constexpr int kStaleLimit = 10;

// Finding the rendered frame's view matrix inside the RenderFrame (lag -1).
constexpr std::size_t kSearchBytes = 0x8000;
constexpr int kSearchEvery = 30;
constexpr int kSearchAttempts = 300;
std::optional<std::size_t> g_frame_offset;
std::size_t g_frame_source = 0; // 0: the RenderFrame, then each render view
constexpr std::size_t kRenderViewReadState = 0x2158; // see views.cpp
int g_searches = 0;
int g_offset_misses = 0;
std::atomic<int> g_measured_lag{-1};
std::atomic<bool> g_restart_search{false};

// Motion controllers. Sampled and logged on the presenting thread; read anywhere through hands().
bool g_input_wanted = true;   // [xr] input
bool g_input_haptics = true;  // [xr] input_haptics: a short pulse on each grip press (proves haptics work)
bool g_input = false;         // the action set was created
std::mutex g_hands_mutex;
std::array<HandState, 2> g_hands{};
std::array<HandState, 2> g_logged{}; // the last state the change log compared against
std::uint64_t g_hand_samples = 0;
std::uint64_t g_input_unfocused = 0; // frames xrSyncActions gave no input (session not focused)
struct HapticRequest {
    std::atomic<bool> pending{false};
    std::atomic<float> amplitude{0.0f};
    std::atomic<float> seconds{0.0f};
};
std::array<HapticRequest, 2> g_haptics;
std::atomic<bool> g_hands_report{false};
std::atomic<bool> g_hands_buzz{false};
constexpr std::uint64_t kHandLogEvery = 900; // samples between periodic pose lines (~10 s at 90 Hz)
std::atomic<bool> g_recenter_request{true}; // the first tracked frame recenters
std::atomic<bool> g_ui_place_request{true}; // place the UI panel in front of the head on the next frame
std::atomic<bool> g_ui_wrist{false};         // [ui] placement = wrist (toggled by the "panel" binding)
std::atomic<bool> g_menu_toggle_request{false}; // the "vrmenu" binding was pressed
// [vr] dominant_hand: the hand that points (laser, poke, menu, picking, melee) and gets the "right_*" bindings; the
// other hand carries the wrist UI and walks with its stick. Right by default.
std::atomic<std::size_t> g_dominant{kRightHand};
std::size_t pointer_hand() {
    return g_dominant.load(std::memory_order_relaxed);
}
std::size_t wrist_hand() {
    return 1 - g_dominant.load(std::memory_order_relaxed);
}
std::atomic<bool> g_menu_is_open{false};        // the VR menu is up: the controllers drive it
HWND g_window = nullptr;                    // the game window (the laser moves the mouse cursor in it)
bool g_was_driven = false;

// Same-frame stereo (stereo.hpp), switched at runtime; needs a head-tracked mode.
std::atomic<bool> g_stereo{false};
std::atomic<std::uint64_t> g_stereo_frames{0};

std::atomic<std::uint64_t> g_submitted{0};
std::atomic<std::uint64_t> g_skipped{0};
std::atomic<std::uint64_t> g_screen_frames{0};
std::atomic<std::uint64_t> g_matched{0};
std::atomic<std::uint64_t> g_guessed{0};
std::atomic<std::uint64_t> g_unmatched{0};

constexpr float kDegrees = 180.0f / std::numbers::pi_v<float>;

// openxr_loader.dll is delay-loaded and sits next to the core (bin\bg3vr), which isn't on the
// normal DLL search path; load it by full path first so the delay-load finds it.
bool load_loader() {
    HMODULE core = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&load_loader), &core);
    wchar_t path[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(core, path, static_cast<DWORD>(std::size(path)));
    const auto loader = std::filesystem::path(std::wstring(path, length)).parent_path() / L"openxr_loader.dll";
    if (!LoadLibraryExW(loader.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
        log::error("xr: can't load {} (error {})", to_utf8(loader.wstring()), GetLastError());
        return false;
    }
    return true;
}

bool same_adapter(ID3D11Device* device, const LUID& wanted, std::string& name) {
    IDXGIDevice* dxgi = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) {
        return false;
    }
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC desc{};
    const bool ok = SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc));
    if (adapter) {
        adapter->Release();
    }
    dxgi->Release();
    name = to_utf8(desc.Description);
    return ok && std::memcmp(&desc.AdapterLuid, &wanted, sizeof(LUID)) == 0;
}

// CopySubresourceRegion needs both textures in the same format group: identical, or the sRGB and
// linear forms of the same 8-bit layout.
bool copy_compatible(DXGI_FORMAT a, DXGI_FORMAT b) {
    const auto group = [](DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        default: return format;
        }
    };
    return group(a) == group(b);
}

vr::RigidPose rigid_pose(const XrPosef& pose) {
    return {{pose.position.x, pose.position.y, pose.position.z},
            {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}};
}

camera::EyeFov eye_fov(const XrFovf& fov) {
    return {fov.angleLeft * kDegrees, fov.angleRight * kDegrees, fov.angleUp * kDegrees, fov.angleDown * kDegrees};
}

XrView xr_view(const vr::RigidPose& pose, const camera::EyeFov& fov) {
    XrView view{XR_TYPE_VIEW};
    view.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
    view.pose.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
    view.fov = {fov.left_deg / kDegrees, fov.right_deg / kDegrees, fov.up_deg / kDegrees, fov.down_deg / kDegrees};
    return view;
}

// `view` as the camera's head view; `views` (both eyes, located with it) are kept alongside.
camera::HeadView head_view(const XrView& view, int eye, const std::vector<XrView>& views) {
    camera::HeadView head;
    head.pose = rigid_pose(view.pose);
    head.fov = eye_fov(view.fov);
    head.eye = eye;
    for (std::size_t i = 0; i < 2 && i < views.size(); ++i) {
        head.eye_pose[i] = rigid_pose(views[i].pose);
        head.eye_fov[i] = eye_fov(views[i].fov);
    }
    return head;
}

XrView xr_view(const camera::HeadView& head) {
    return xr_view(head.pose, head.fov);
}

// Mono and same-frame stereo: one view between the eyes that covers both eyes' fields of view.
camera::HeadView centre_view(const std::vector<XrView>& views) {
    XrView centre = views[0];
    centre.pose.position = {(views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
                            (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
                            (views[0].pose.position.z + views[1].pose.position.z) * 0.5f};
    centre.fov.angleLeft = (std::min)(views[0].fov.angleLeft, views[1].fov.angleLeft);
    centre.fov.angleRight = (std::max)(views[0].fov.angleRight, views[1].fov.angleRight);
    centre.fov.angleUp = (std::max)(views[0].fov.angleUp, views[1].fov.angleUp);
    centre.fov.angleDown = (std::min)(views[0].fov.angleDown, views[1].fov.angleDown);
    return head_view(centre, 0, views);
}

bool matrix_matches(const float* candidate, const camera::Mat4& expected) {
    for (std::size_t i = 0; i < 16; ++i) {
        if (std::abs(candidate[i] - expected[i]) > 1e-3f * (std::max)(1.0f, std::abs(expected[i]))) {
            return false;
        }
    }
    return true;
}

// Which applied view the frame being presented was built from: look for one of their view matrices
// in the presenting RenderFrame or, once !vr_views has found them, in the render views (at the
// place found earlier, or by searching now and then). The RenderFrame looks like the per-window
// output object, so the render views are the likelier place.
std::optional<std::size_t> match_in_frame(std::uintptr_t render_frame,
                                          const std::array<camera::AppliedView, camera::kAppliedHistory>& applied,
                                          std::size_t count) {
    if (count == 0) {
        return std::nullopt;
    }
    std::vector<std::uintptr_t> sources;
    if (render_frame) {
        sources.push_back(render_frame);
    }
    for (const std::uintptr_t read_state : views::found()) {
        sources.push_back(read_state - kRenderViewReadState);
    }
    if (g_restart_search.exchange(false, std::memory_order_relaxed)) {
        g_frame_offset.reset();
        g_searches = 0;
    }
    if (g_frame_offset) {
        float matrix[16];
        if (g_frame_source < sources.size() &&
            mem::try_copy(matrix, sources[g_frame_source] + *g_frame_offset, sizeof(matrix))) {
            for (std::size_t i = 0; i < count; ++i) {
                if (matrix_matches(matrix, applied[i].view_matrix)) {
                    g_offset_misses = 0;
                    return i;
                }
            }
        }
        if (++g_offset_misses > 120) {
            log::warn("xr: the view matrix at RenderFrame+0x{:X} stopped matching; searching again", *g_frame_offset);
            g_frame_offset.reset();
            g_searches = 0;
        }
        return std::nullopt;
    }
    if (g_searches >= kSearchAttempts || (g_searches++ % kSearchEvery) != 0) {
        return std::nullopt;
    }
    static std::vector<float> buffer(kSearchBytes / sizeof(float));
    for (std::size_t source = 0; source < sources.size(); ++source) {
        std::size_t readable = kSearchBytes;
        while (readable >= 0x1000 && !mem::try_copy(buffer.data(), sources[source], readable)) {
            readable /= 2;
        }
        if (readable < 0x1000) {
            continue;
        }
        // Every copy, with how many camera updates old it is; the freshest one wins (views hold
        // several copies, e.g. this frame's and the previous frame's).
        const std::size_t floats = readable / sizeof(float);
        std::optional<std::pair<std::size_t, std::size_t>> best; // offset, lag
        for (std::size_t at = 0; at + 16 <= floats; ++at) {
            for (std::size_t i = 0; i < count; ++i) {
                if (matrix_matches(&buffer[at], applied[i].view_matrix)) {
                    log::info("xr:   view matrix applied {} update(s) ago at {}+0x{:X}", i,
                              source == 0 && render_frame ? "RenderFrame" : "render view", at * sizeof(float));
                    if (!best || i < best->second) {
                        best = {{at * sizeof(float), i}};
                    }
                    break;
                }
            }
        }
        if (best) {
            g_frame_source = source;
            g_frame_offset = best->first;
            log::info("xr: using the view matrix at {}+0x{:X} (0x{:X}): applied {} camera update(s) before this Present",
                      source == 0 && render_frame ? "RenderFrame" : "render view", best->first, sources[source],
                      best->second);
            return best->second;
        }
    }
    if (g_searches >= kSearchAttempts) {
        log::warn("xr: no applied view matrix found in the RenderFrame or the {} known render view(s); assuming a lag "
                  "of 1 (adjust with !vr_xr_lag; !vr_views find, then !vr_xr_lag auto to search again)",
                  sources.size() - (render_frame ? 1 : 0));
    }
    return std::nullopt;
}

void start(IDXGISwapChain* swapchain) {
    ID3D11Device* device = render::device();
    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    g_window = desc.OutputWindow;
    if (!device || !load_loader()) {
        health::set("headset", health::State::Unavailable, "see the xr: lines above");
        g_failed = true;
        return;
    }

    std::string error;
    const auto status = xr::open(g_system, error);
    if (!g_system.runtime.empty()) {
        log::info("xr: runtime {}", g_system.runtime);
    }
    if (status != xr::Status::Ok) {
        log::error("xr: {}", error);
        health::set("headset", health::State::Unavailable, "see the xr: lines above");
        g_failed = true;
        return;
    }
    log::info("xr: headset {}, {} views, recommended {}x{}", g_system.headset, g_system.views.size(),
              g_system.views.empty() ? 0 : g_system.views[0].recommendedImageRectWidth,
              g_system.views.empty() ? 0 : g_system.views[0].recommendedImageRectHeight);
    if (g_system.views.size() != 2) {
        log::error("xr: expected 2 views (stereo), the runtime has {}", g_system.views.size());
        health::set("headset", health::State::Unavailable, "see the xr: lines above");
        g_failed = true;
        return;
    }

    std::string adapter;
    if (!same_adapter(device, g_system.adapter, adapter)) {
        log::error("xr: the game renders on '{}', but the headset needs a different GPU; set the game and SteamVR "
                   "to the same GPU",
                   adapter);
        health::set("headset", health::State::Unavailable, "see the xr: lines above");
        g_failed = true;
        return;
    }

    // The screen image is the back buffer's size. The eye images are the headset's recommended size when the blitter
    // scales (each eye's half of the back buffer, at whatever resolution the game renders, is drawn into it), else
    // the back buffer's for a plain copy.
    const auto width = static_cast<int32_t>(desc.BufferDesc.Width);
    const auto height = static_cast<int32_t>(desc.BufferDesc.Height);
    g_back_format = desc.BufferDesc.Format;
    std::string blit_error;
    const bool blitting = g_blitter.create(device, desc.BufferDesc.Width, desc.BufferDesc.Height,
                                           desc.BufferDesc.Format, blit_error);
    if (!blitting) {
        log::warn("xr: no shader copy ({}); trying a plain copy", blit_error);
    }
    g_session = std::make_unique<xr::Session>();
    // With the blitter the images can be 8-bit sRGB whatever the back buffer is.
    const DXGI_FORMAT wanted = blitting ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : desc.BufferDesc.Format;
    const auto eye_width = blitting ? static_cast<int32_t>(g_system.views[0].recommendedImageRectWidth) : width;
    const auto eye_height = blitting ? static_cast<int32_t>(g_system.views[0].recommendedImageRectHeight) : height;
    if (!g_session->create(g_system, device, error, wanted, eye_width, eye_height) ||
        !g_session->create_quad(width, height, error)) {
        log::error("xr: {}", error);
        g_session.reset();
        health::set("headset", health::State::Unavailable, "see the xr: lines above");
        g_failed = true;
        return;
    }
    g_linearize = blitting && blit::is_srgb(g_session->format()) && !blit::is_srgb(desc.BufferDesc.Format);
    if (!blitting && !copy_compatible(g_session->format(), desc.BufferDesc.Format)) {
        log::error("xr: the runtime offers no swapchain format the game's back buffer (format {}) can be copied into "
                   "(runtime picked {})",
                   static_cast<int>(desc.BufferDesc.Format), static_cast<int>(g_session->format()));
        g_session.reset();
        health::set("headset", health::State::Unavailable, "see the xr: lines above");
        g_failed = true;
        return;
    }
    g_image_width = desc.BufferDesc.Width;
    g_image_height = desc.BufferDesc.Height;
    g_eye_width = static_cast<UINT>(eye_width);
    g_eye_height = static_cast<UINT>(eye_height);
    g_quad_width = static_cast<UINT>(width);
    g_quad_height = static_cast<UINT>(height);
    // The render resolution follows the headset (resolution.cpp): both eyes side by side in the back buffer.
    resolution::set_window(desc.OutputWindow);
    resolution::set_headset_eye(g_system.views[0].recommendedImageRectWidth, g_system.views[0].recommendedImageRectHeight);
    g_rendered.assign(2, {XR_TYPE_VIEW});
    if (g_input_wanted) {
        std::string input_error;
        g_input = g_session->create_input(input_error);
        if (g_input) {
            log::info("xr input: controller actions attached (Touch, Index, Vive and simple profiles suggested); "
                      "changes are logged as 'xr input:' lines, poses every {} frames; hand tracking {}",
                      kHandLogEvery,
                      g_session->has_hand_tracking() ? "on (a hand without a controller is tracked: point and close "
                                                       "it to click, close an open hand to grab, palm to face + pinch "
                                                       "on the other hand for the VR menu)"
                                                     : "not offered by this runtime / headset (XR_EXT_hand_tracking)");
            health::set("hand tracking", g_session->has_hand_tracking() ? health::State::Ready : health::State::Unavailable,
                        g_session->has_hand_tracking() ? "" : "the runtime has no XR_EXT_hand_tracking");
        } else {
            log::warn("xr input: no controllers ({}); the headset still works", input_error);
        }
    } else {
        log::info("xr input: off ([xr] input = false)");
    }
    health::set("headset", health::State::Ready, std::string("mode ") + mode_name(g_mode));
    log::info("xr: session open on '{}', mode {}: images {}x{} format {} (game back buffer format {}, {}{}); screen "
              "{:.1f} m wide at {:.1f} m",
              adapter, mode_name(g_mode), width, height, static_cast<int>(g_session->format()),
              static_cast<int>(desc.BufferDesc.Format), blitting ? "shader copy" : "plain copy",
              g_linearize ? ", sRGB-decoded" : "", g_screen_width_m, g_screen_distance_m);
    if (g_mode != Mode::Screen) {
        log::info("xr: head tracking starts when a pose is held (!vr_cam_set here); until then, the screen");
    }
    if (!g_desktop_vsync) {
        render::set_sync_interval_override(0); // the headset paces the game, not the monitor
    }
    g_ready = true;
}

// The comfort vignette for this frame: eased toward max(stick speed, smooth turn rate) times [vr] comfort_vignette.
float g_vignette = 0.0f;
float comfort_vignette_now(double dt) {
    const float strength = vrplay::comfort_vignette();
    const float want = vrplay::active() && strength > 0.0f ? strength * std::max(locomotion::speed(), vrplay::turn_rate())
                                                           : 0.0f;
    const float rate = static_cast<float>(dt) / 0.15f; // about 0.15 s in and out
    g_vignette += std::clamp(want - g_vignette, -rate, rate);
    return g_vignette;
}
float g_frame_vignette = 0.0f; // set once per headset frame

// The aiming beam ([pointer], the VR menu's View page): from the pointing hand to where its ray hits the ground or a
// character / item (rayprobe), drawn into each eye's copy with that eye's rendered pose and field of view.
std::atomic<int> g_beam_style{3};              // 0 off, 1 solid, 2 dashed, 3 flowing dots, 4 pulse
std::array<std::atomic<float>, 3> g_beam_colour{}; // linear rgb
std::atomic<float> g_beam_opacity{0.55f};
std::atomic<float> g_beam_width{0.006f};       // uv of the image height at the hand
std::atomic<float> g_beam_range{12.0f};        // metres when nothing is hit
float g_beam_time = 0.0f;
std::uint64_t g_beam_frames = 0;

// A LOCAL-space point in an eye image's uv, for the pose and field of view the image was rendered with.
std::optional<std::array<float, 2>> eye_uv(const vr::Vec3& point, const vr::RigidPose& eye, const camera::EyeFov& fov) {
    const vr::Quat inverse{-eye.orientation[0], -eye.orientation[1], -eye.orientation[2], eye.orientation[3]};
    const vr::Vec3 p = vr::rotate(inverse, {point[0] - eye.position[0], point[1] - eye.position[1], point[2] - eye.position[2]});
    if (p[2] > -0.02f) {
        return std::nullopt; // behind the eye
    }
    const float x = p[0] / -p[2], y = p[1] / -p[2];
    const float l = std::tan(fov.left_deg / kDegrees), r = std::tan(fov.right_deg / kDegrees);
    const float u = std::tan(fov.up_deg / kDegrees), d = std::tan(fov.down_deg / kDegrees);
    return std::array<float, 2>{(x - l) / (r - l), (u - y) / (u - d)};
}

blit::Blitter::Overlay beam_overlay(const vr::RigidPose& eye, const camera::EyeFov& fov);
std::optional<std::array<float, 2>> g_screen_cursor; // third person: the laser dot on the flat screen (uv)

// `rect` (back buffer pixels; width 0 = all of it) into the whole image.
// The back buffer changed size (the VR resolution, a window resize): the blitter's copy is remade to match.
void follow_back_buffer(IDXGISwapChain* swapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (!g_blitter.ready() || FAILED(swapchain->GetDesc(&desc)) ||
        (desc.BufferDesc.Width == g_image_width && desc.BufferDesc.Height == g_image_height)) {
        return;
    }
    std::string error;
    g_blitter.destroy();
    if (g_blitter.create(render::device(), desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format, error)) {
        log::info("xr: the back buffer is now {}x{} (was {}x{}); each eye gets {}x{} into a {}x{} image",
                  desc.BufferDesc.Width, desc.BufferDesc.Height, g_image_width, g_image_height,
                  desc.BufferDesc.Width / 2, desc.BufferDesc.Height, g_eye_width, g_eye_height);
        g_image_width = desc.BufferDesc.Width;
        g_image_height = desc.BufferDesc.Height;
        g_back_format = desc.BufferDesc.Format;
    } else {
        log::error("xr: couldn't remake the copy for a {}x{} back buffer: {}", desc.BufferDesc.Width,
                   desc.BufferDesc.Height, error);
    }
}

// `rect` (back buffer pixels; width 0 = all of it) into the whole `target_width` x `target_height` image.
void copy_back_buffer(IDXGISwapChain* swapchain, const xr::EyeImage& target, UINT target_width, UINT target_height,
                      const blit::Blitter::Rect& rect = {}, const blit::Blitter::Overlay* overlay_in = nullptr) {
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) {
        return;
    }
    if (g_blitter.ready()) {
        D3D11_TEXTURE2D_DESC source{};
        back_buffer->GetDesc(&source);
        if (source.Width == g_image_width && source.Height == g_image_height) {
            blit::Blitter::Overlay overlay;
            overlay.vignette = g_frame_vignette;
            g_blitter.draw(back_buffer, rect, target.view, target_width, target_height, g_linearize,
                           overlay_in ? *overlay_in : overlay);
        }
        back_buffer->Release();
        return;
    }
    // The window can be resized after the images were made; copy the overlapping part (no scaling
    // without the blitter: a sub-rectangle lands unscaled in the image's corner).
    D3D11_TEXTURE2D_DESC source{};
    back_buffer->GetDesc(&source);
    const UINT left = rect.width ? rect.x : 0, top = rect.width ? rect.y : 0;
    const UINT right = rect.width ? rect.x + rect.width : source.Width;
    const UINT bottom = rect.width ? rect.y + rect.height : source.Height;
    const D3D11_BOX box{left, top, 0, (std::min)({right, source.Width, left + g_image_width}),
                        (std::min)({bottom, source.Height, top + g_image_height}), 1};
    ID3D11DeviceContext* context = nullptr;
    render::device()->GetImmediateContext(&context);
    context->CopySubresourceRegion(target.texture, 0, 0, 0, 0, back_buffer, 0, &box);
    context->Release();
    back_buffer->Release();
}

void submit_screen(IDXGISwapChain* swapchain, const xr::Frame& frame) {
    if (frame.state.shouldRender) {
        const xr::EyeImage image = g_session->acquire_quad();
        blit::Blitter::Overlay overlay; // the flat screen: no vignette; the laser dot in third person
        if (g_screen_cursor) {
            overlay.cursor_u = (*g_screen_cursor)[0];
            overlay.cursor_v = (*g_screen_cursor)[1];
            overlay.cursor_radius = 0.006f;
        }
        copy_back_buffer(swapchain, image, g_quad_width, g_quad_height, {}, &overlay);
        g_session->release_quad();
    }
    g_session->end_frame_quad(frame, g_screen_width_m, g_screen_distance_m);
    g_screen_frames.fetch_add(1, std::memory_order_relaxed);
}

// Head-tracked modes: put the presented image into the eye(s) it was rendered for, submit both eyes
// with the poses they were rendered with, and hand the camera the view for a later frame.
void submit_tracked(IDXGISwapChain* swapchain, std::uintptr_t render_frame, const xr::Frame& frame) {
    std::array<camera::AppliedView, camera::kAppliedHistory> applied;
    const std::size_t count = camera::applied_views(applied);
    if (count > 0 && applied[0].serial != g_last_serial) {
        g_last_serial = applied[0].serial;
        g_stale_presents = 0;
    } else if (g_stale_presents < kStaleLimit) {
        ++g_stale_presents;
    }
    const bool driven = count > 0 && g_stale_presents < kStaleLimit;
    if (driven && !g_was_driven) {
        g_recenter_request.store(true, std::memory_order_relaxed); // the camera was just taken over
    }
    g_was_driven = driven;

    // Same-frame stereo once it's switched on and both eye views are assigned.
    const bool stereo = g_stereo.load(std::memory_order_relaxed) && stereo::active();

    // Which applied view the presented image came from.
    const camera::AppliedView* rendered = nullptr;
    const int lag = g_lag.load(std::memory_order_relaxed);
    if (driven) {
        if (lag < 0) {
            if (const auto index = match_in_frame(render_frame, applied, count)) {
                rendered = &applied[*index];
                g_measured_lag.store(static_cast<int>(*index), std::memory_order_relaxed);
                g_matched.fetch_add(1, std::memory_order_relaxed);
            } else if (g_frame_offset) {
                g_unmatched.fetch_add(1, std::memory_order_relaxed); // known offset, no match: skip the image
            }
        }
        if (!rendered && !(lag < 0 && g_frame_offset)) {
            const int guess = lag < 0 ? 1 : lag;
            if (static_cast<std::size_t>(guess) < count) {
                rendered = &applied[guess];
                g_guessed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (!driven) {
        submit_screen(swapchain, frame);
    } else {
        if (rendered && frame.state.shouldRender && stereo) {
            // Same-frame stereo: the left eye's view drew the back buffer's left half, the right eye's the
            // right half (views::set_stereo_layout), each from its eye of the same located head.
            const UINT half = g_image_width / 2;
            for (int eye = 0; eye < 2; ++eye) {
                const blit::Blitter::Rect rect{eye == 0 ? 0u : half, 0u, eye == 0 ? half : g_image_width - half,
                                               g_image_height};
                const xr::EyeImage image = g_session->acquire(static_cast<std::size_t>(eye));
                const blit::Blitter::Overlay overlay = beam_overlay(rendered->head.eye_pose[eye], rendered->head.eye_fov[eye]);
                copy_back_buffer(swapchain, image, g_eye_width, g_eye_height, rect, &overlay);
                g_session->release(static_cast<std::size_t>(eye));
                g_rendered[eye] = xr_view(rendered->head.eye_pose[eye], rendered->head.eye_fov[eye]);
                g_has_image[eye] = true;
            }
            g_stereo_frames.fetch_add(1, std::memory_order_relaxed);
        } else if (rendered && frame.state.shouldRender) {
            const int first = g_mode == Mode::Mono ? 0 : rendered->head.eye;
            const int last = g_mode == Mode::Mono ? 1 : rendered->head.eye;
            for (int eye = first; eye <= last; ++eye) {
                const xr::EyeImage image = g_session->acquire(static_cast<std::size_t>(eye));
                const blit::Blitter::Overlay overlay = beam_overlay(rendered->head.pose, rendered->head.fov);
                copy_back_buffer(swapchain, image, g_eye_width, g_eye_height, {}, &overlay);
                g_session->release(static_cast<std::size_t>(eye));
                g_rendered[eye] = xr_view(rendered->head);
                g_has_image[eye] = true;
            }
        }
        static const std::vector<XrView> kNone;
        g_session->end_frame(frame, g_has_image[0] && g_has_image[1] ? g_rendered : kNone);
    }

    // The view for the frame the game renders next, predicted for when that frame will be shown.
    const int measured = g_measured_lag.load(std::memory_order_relaxed);
    const int ahead = 1 + (lag >= 0 ? lag : (measured >= 0 ? measured : 1));
    std::vector<XrView> views;
    const XrTime when = frame.state.predictedDisplayTime + frame.state.predictedDisplayPeriod * ahead;
    if (g_session->locate(when, views) && views.size() == 2) {
        if (g_recenter_request.exchange(false, std::memory_order_relaxed)) {
            camera::set_recenter(centre_view(views).pose);
            g_ui_place_request.store(true, std::memory_order_relaxed); // the UI panel moves in front again
            bodyik::recalibrate();                                      // and the hands take their orientation again
        }
        if (stereo) {
            // The game camera at the head centre covering both eyes (culling sees both); the stereo hook
            // moves each eye's view to its eye.
            const camera::HeadView centre = centre_view(views);
            camera::set_head(centre);
            stereo::set_eyes(stereo::Eyes{centre.pose, centre.eye_pose, centre.eye_fov});
        } else if (g_mode == Mode::Mono) {
            camera::set_head(centre_view(views));
        } else {
            camera::set_head(head_view(views[g_next_eye], g_next_eye, views));
            g_next_eye ^= 1;
        }
    }
}

const char* hand_name(std::size_t hand) {
    return hand == kLeftHand ? "left" : "right";
}

std::string pose_text(const vr::RigidPose& pose) {
    return std::format("pos ({:.3f}, {:.3f}, {:.3f}) rot ({:.3f}, {:.3f}, {:.3f}, {:.3f})", pose.position[0],
                       pose.position[1], pose.position[2], pose.orientation[0], pose.orientation[1],
                       pose.orientation[2], pose.orientation[3]);
}

std::string hand_text(const HandState& hand) {
    return std::format("{}{} grip {} | aim {}{} | vel ({:.2f}, {:.2f}, {:.2f}) ang ({:.2f}, {:.2f}, {:.2f}){} | trigger "
                       "{:.2f} squeeze {:.2f} stick ({:.2f}, {:.2f}){}{}{}{}",
                       hand.active ? "active" : "inactive", hand.tracked ? " tracked" : " untracked",
                       pose_text(hand.grip), pose_text(hand.aim), hand.aim_valid ? "" : " (invalid)",
                       hand.velocity[0], hand.velocity[1], hand.velocity[2], hand.angular_velocity[0],
                       hand.angular_velocity[1], hand.angular_velocity[2], hand.velocity_valid ? "" : " (invalid)",
                       hand.trigger, hand.squeeze, hand.stick_x, hand.stick_y, hand.stick_click ? " stick-click" : "",
                       hand.primary ? " primary" : "", hand.secondary ? " secondary" : "", hand.menu ? " menu" : "") +
           (hand.world_valid ? " | world " + pose_text(hand.world_grip) : " | world (no held pose)");
}

void log_profiles() {
    for (std::size_t h = 0; h < 2; ++h) {
        const std::string profile = g_session->interaction_profile(h);
        log::info("xr input: {} hand profile {}", hand_name(h), profile.empty() ? "(none)" : profile);
    }
}

// Logs what changed on one hand since the last sample: presence, tracking, buttons, analog presses.
void log_hand_changes(std::size_t h, const HandState& before, const HandState& now) {
    const char* name = hand_name(h);
    if (before.active != now.active) {
        log::info("xr input: {} controller {}", name, now.active ? "connected" : "gone");
    }
    if (before.tracked != now.tracked) {
        log::info("xr input: {} grip {} ({})", name, now.tracked ? "tracked" : "lost", pose_text(now.grip));
    }
    const auto edge = [&](const char* what, bool was, bool is) {
        if (was != is) {
            log::info("xr input: {} {} {} | {}", name, what, is ? "down" : "up", hand_text(now));
        }
    };
    edge("trigger", before.trigger > 0.5f, now.trigger > 0.5f);
    edge("squeeze", before.squeeze > 0.5f, now.squeeze > 0.5f);
    edge("stick-click", before.stick_click, now.stick_click);
    edge("primary", before.primary, now.primary);
    edge("secondary", before.secondary, now.secondary);
    edge("menu", before.menu, now.menu);
    const auto pushed = [](const HandState& hand) {
        return hand.stick_x * hand.stick_x + hand.stick_y * hand.stick_y > 0.25f;
    };
    if (pushed(before) != pushed(now)) {
        log::info("xr input: {} stick {} ({:.2f}, {:.2f})", name, pushed(now) ? "pushed" : "centred", now.stick_x,
                  now.stick_y);
    }
}

// Hand tracking: what a closing hand means is decided as it closes. After pointing (index straight, the other fingers
// curled) it is a click; from an open hand it is a grab. It stays that until the hand opens again.
struct HandIntent {
    bool closed = false;
    bool click = false;
    bool pointing = false; // the last pose before closing was pointing
};
std::array<HandIntent, 2> g_intent{};
std::array<std::uint64_t, 2> g_hand_clicks{}, g_hand_grabs{};

HandState hand_state(const xr::Hand& hand, std::uint64_t sample, std::size_t index) {
    HandState state;
    state.active = hand.active;
    state.tracked = hand.grip_valid;
    state.grip = rigid_pose(hand.grip);
    state.aim = rigid_pose(hand.aim);
    state.aim_valid = hand.aim_valid;
    state.velocity = {hand.linear_velocity.x, hand.linear_velocity.y, hand.linear_velocity.z};
    state.angular_velocity = {hand.angular_velocity.x, hand.angular_velocity.y, hand.angular_velocity.z};
    state.velocity_valid = hand.velocity_valid;
    state.trigger = hand.trigger;
    state.squeeze = hand.squeeze;
    state.stick_x = hand.thumbstick.x;
    state.stick_y = hand.thumbstick.y;
    state.stick_click = hand.thumbstick_click;
    state.primary = hand.primary;
    state.secondary = hand.secondary;
    state.menu = hand.menu;
    if (hand.hand_tracked) {
        state.hand_tracked = true;
        state.tip = {hand.index_tip.x, hand.index_tip.y, hand.index_tip.z};
        state.palm_normal = {hand.palm_normal.x, hand.palm_normal.y, hand.palm_normal.z};
        state.pinch = hand.pinch;
        HandIntent& in = g_intent[index];
        const bool closed = in.closed ? hand.fist > 0.45f && !hand.index_extended
                                      : hand.fist > 0.7f && !hand.index_extended;
        if (closed && !in.closed) {
            in.click = in.pointing;
            ++(in.click ? g_hand_clicks : g_hand_grabs)[index];
        } else if (!closed) {
            if (hand.index_extended && hand.fist > 0.6f) {
                in.pointing = true;
            } else if (hand.fist < 0.3f) {
                in.pointing = false;
            }
        }
        in.closed = closed;
        state.trigger = closed && in.click ? 1.0f : 0.0f;
        state.squeeze = closed && !in.click ? 1.0f : 0.0f;
    }
    if (state.tracked) {
        if (const auto world = camera::tracked_to_world(state.grip)) {
            state.world_grip = *world;
            state.world_valid = true;
        }
    }
    state.sample = sample;
    return state;
}

// Once per submitted frame: sample the controllers, publish them, log changes, run queued haptics.
// First <-> third person: the Lua side switches the camera, then tells the core (bridge view=).
void request_view_toggle() {
    const bool to_third = !vrplay::third_person();
    bridge::push_request(to_third ? "view_third" : "view_first");
    log::info("vr: asking the Lua side for {} person", to_third ? "third" : "first");
}

// The game UI panel between the world and the left wrist; returns true when it is now on the wrist.
bool toggle_ui_wrist() {
    const bool wrist = !g_ui_wrist.load(std::memory_order_relaxed);
    g_ui_wrist.store(wrist, std::memory_order_relaxed);
    if (!wrist) {
        g_ui_place_request.store(true, std::memory_order_relaxed); // back in front of the head
    }
    log::info("ui: panel {}", wrist ? "on the left wrist" : "in front of you");
    return wrist;
}

void update_input(const xr::Frame& frame) {
    if (!g_input) {
        return;
    }
    if (g_session->take_profile_changed()) {
        log::info("xr input: the runtime changed the controller bindings");
        log_profiles();
    }
    xr::Hand raw[2];
    if (!g_session->sync_input(frame.state.predictedDisplayTime, raw)) {
        if (g_input_unfocused++ % kHandLogEvery == 0) {
            log::info("xr input: no input (session not focused, e.g. the SteamVR dashboard is open), {} frame(s) so far",
                      g_input_unfocused);
        }
    }
    ++g_hand_samples;
    const std::array<HandState, 2> now{hand_state(raw[0], g_hand_samples, 0), hand_state(raw[1], g_hand_samples, 1)};
    {
        std::lock_guard lock(g_hands_mutex);
        g_hands = now;
    }
    for (std::size_t h = 0; h < 2; ++h) {
        log_hand_changes(h, g_logged[h], now[h]);
        if (g_input_haptics && g_logged[h].squeeze <= 0.5f && now[h].squeeze > 0.5f) {
            g_session->haptic(h, 0.3f, 0.03f);
        }
    }
    g_logged = now;
    // The right hand's pointing ray in the world, for the raycast probe (and later the grab candidates).
    std::optional<vr::RigidPose> aim;
    if (now[pointer_hand()].aim_valid) {
        aim = camera::tracked_to_world(now[pointer_hand()].aim);
    }
    rayprobe::set_hand_aim(aim);
    // Holsters on the body (first person, VR play on, menu closed): a grip closing over a shoulder, at a hip or behind
    // the back belongs to the holster, not to picking up world items.
    holster::Output holsters;
    if (vrplay::active() && !vrplay::third_person() && !g_menu_is_open.load(std::memory_order_relaxed) &&
        frame.views.size() == 2) {
        holster::Hand body_hands[2];
        for (std::size_t h = 0; h < 2; ++h) {
            body_hands[h] = {now[h].tracked, now[h].grip.position, now[h].active && now[h].squeeze > 0.5f};
        }
        holsters = holster::on_frame(centre_view(frame.views).pose, body_hands);
        for (std::size_t h = 0; h < 2; ++h) {
            if (holsters.haptic[h] > 0.0f) {
                g_session->haptic(h, holsters.haptic[h], 0.02f);
            }
        }
    }
    // Grips and aims for picking things up (world, game convention).
    for (std::size_t h = 0; h < 2; ++h) {
        std::optional<vr::RigidPose> aim_world;
        if (now[h].aim_valid) {
            aim_world = camera::tracked_to_world(now[h].aim);
        }
        hold::set_hand(h, now[h].active && now[h].squeeze > 0.5f && !g_menu_is_open.load(std::memory_order_relaxed) &&
                              !holsters.grip_taken[h],
                       now[h].world_valid ? std::optional<vr::RigidPose>(now[h].world_grip) : std::nullopt, aim_world,
                       static_cast<double>(frame.state.predictedDisplayTime) * 1e-9);
    }
    // The right hand's swing for physical melee.
    melee::set_hand(now[pointer_hand()].aim_valid && !g_menu_is_open.load(std::memory_order_relaxed)
                        ? camera::tracked_to_world(now[pointer_hand()].aim)
                        : std::nullopt,
                    static_cast<double>(frame.state.predictedDisplayTime) * 1e-9);
    // The grips for the arm IK (world, game convention).
    bodyik::set_targets(now[kLeftHand].world_valid ? std::optional<vr::RigidPose>(now[kLeftHand].world_grip) : std::nullopt,
                        now[kRightHand].world_valid ? std::optional<vr::RigidPose>(now[kRightHand].world_grip) : std::nullopt);
    // The left stick drives locomotion (when it is on); zero while the hand isn't active.
    const std::size_t walk = wrist_hand();
    const bool stick = now[walk].active && !g_menu_is_open.load(std::memory_order_relaxed);
    locomotion::set_stick(stick ? now[walk].stick_x : 0.0f, stick ? now[walk].stick_y : 0.0f);
    // Buttons, turning and recentring while VR play is on. vrplay's "right" hand is the dominant one (left-handed: the
    // controllers swap roles).
    // Tracked hands: the trigger is a click, the grab is no right-click (only grabbing and holsters use it), and the
    // other hand's palm turned to the face with a pinch is its menu button.
    vrplay::Hand play[2];
    for (std::size_t slot = 0; slot < 2; ++slot) {
        const std::size_t h = slot == 1 ? pointer_hand() : wrist_hand();
        const HandState& hs = now[h];
        bool menu = hs.menu;
        if (hs.hand_tracked && slot == 0 && frame.views.size() == 2) {
            const vr::Vec3 eye = centre_view(frame.views).pose.position;
            const vr::Vec3 to_eye{eye[0] - hs.grip.position[0], eye[1] - hs.grip.position[1], eye[2] - hs.grip.position[2]};
            const float len = std::sqrt(to_eye[0] * to_eye[0] + to_eye[1] * to_eye[1] + to_eye[2] * to_eye[2]);
            const float facing = len > 1e-3f ? (hs.palm_normal[0] * to_eye[0] + hs.palm_normal[1] * to_eye[1] +
                                                hs.palm_normal[2] * to_eye[2]) / len
                                             : 0.0f;
            static bool pinched = false;
            pinched = pinched ? hs.pinch > 0.5f : hs.pinch > 0.85f;
            menu = facing > 0.6f && pinched;
        }
        play[slot] = {hs.active || hs.hand_tracked, hs.trigger, hs.hand_tracked ? 0.0f : hs.squeeze, hs.stick_x, hs.stick_y,
                      hs.stick_click, hs.primary, hs.secondary, menu, holsters.grip_taken[h]};
    }
    const vrplay::Feedback feedback =
        vrplay::on_controllers(play, static_cast<double>(frame.state.predictedDisplayTime) * 1e-9);
    if (feedback.recenter) {
        g_recenter_request.store(true, std::memory_order_relaxed);
    }
    if (feedback.toggle_view) {
        request_view_toggle();
    }
    if (feedback.toggle_panel) {
        toggle_ui_wrist();
    }
    if (feedback.toggle_menu) {
        g_menu_toggle_request.store(true, std::memory_order_relaxed);
    }
    for (std::size_t slot = 0; slot < 2; ++slot) {
        if (feedback.haptic[slot] > 0.0f) {
            g_session->haptic(slot == 1 ? pointer_hand() : wrist_hand(), feedback.haptic[slot], 0.02f);
        }
    }
    if (g_hand_samples % kHandLogEvery == 1) {
        for (std::size_t h = 0; h < 2; ++h) {
            log::info("xr input: {} {}", hand_name(h), hand_text(now[h]));
        }
    }
    const bool buzz = g_hands_buzz.exchange(false, std::memory_order_relaxed);
    if (g_hands_report.exchange(false, std::memory_order_relaxed)) {
        log::info("xr input: {} samples, {} without input", g_hand_samples, g_input_unfocused);
        log_profiles();
        for (std::size_t h = 0; h < 2; ++h) {
            log::info("xr input: {} {}", hand_name(h), hand_text(now[h]));
        }
    }
    for (std::size_t h = 0; h < 2; ++h) {
        if (buzz) {
            g_session->haptic(h, 0.5f, 0.1f);
        }
        if (g_haptics[h].pending.exchange(false, std::memory_order_acquire)) {
            g_session->haptic(h, g_haptics[h].amplitude.load(std::memory_order_relaxed),
                              g_haptics[h].seconds.load(std::memory_order_relaxed));
        }
    }
}

// The headset state for the Lua side (bridge::write_status, read by Client/VRAuto.lua): written on changes only.
int g_status_written = -1; // -1 unknown, 0 off, 1 ready
bool g_autostart = true;   // [vr] autostart
void publish_status(bool ready) {
    if (g_status_written == static_cast<int>(ready)) {
        return;
    }
    g_status_written = ready;
    bridge::set_status("mode", mode_name(g_mode));
    bridge::set_status("autostart", g_autostart ? "1" : "0");
    bridge::set_status("xr", ready ? "ready" : "off");
    log::info("xr: status for Lua: {}", ready ? "ready" : "off");
}


// The game's UI on a VR panel (agent F, F_ui.md; docs/design/vr-ui.md). The UI is drawn once per frame into its own
// texture, which the final composite (stage 0x07) binds as pixel-shader resource 1 over the back buffer. While VR play
// is on: that texture is found by gpu_trace's UI watch, copied (with alpha) into the XR panel layer, placed about 1 m in
// front of the player (re-placed on recentre), and taken out of the eye images by clearing GlobalSwitches+0x0D, which
// the composite tests before binding it. The right hand's aim ray drives a laser dot on the panel and moves the real
// mouse cursor there, so hover and clicks reach the UI the game's own way.
bool g_ui_enabled = true;          // [ui] panel
bool g_ui_remove_from_eyes = true; // [ui] remove_from_eyes
bool g_ui_laser = true;            // [ui] laser
std::atomic<float> g_ui_wrist_width{0.3f}; // [ui] wrist_width (m)
float g_ui_distance = 1.0f;        // [ui] distance (m)
float g_ui_width = 1.1f;           // [ui] width (m)
float g_ui_drop = 0.15f;           // [ui] below_eyes (m)
constexpr UINT kUiMaxWidth = 2560;
blit::Blitter g_ui_blitter;
ID3D11Texture2D* g_ui_source_seen = nullptr; // identity of the texture the blitter was made for (not held)
UINT g_ui_panel_width = 0;
UINT g_ui_panel_height = 0;
bool g_ui_linearize = false;
bool g_ui_failed = false;
std::optional<xr::PanelPlacement> g_ui_place;
bool g_ui_shown = false;
int g_ui_flag_saved = -1; // GlobalSwitches+0x0D before it was cleared (-1: not cleared)
bool g_ui_cursor_on_panel = false;
bool g_ui_touch_down = false; // the controller tip pressed the panel: the left mouse button is held
std::uint64_t g_ui_touches = 0;
std::uint64_t g_ui_frames = 0;
std::uint64_t g_ui_laser_hits = 0;

std::uint8_t* ui_composite_flag() {
    static const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t switches = 0;
    if (!mem::try_copy(&switches, base + 0x604B1B8, sizeof(switches)) || !switches) {
        return nullptr;
    }
    return reinterpret_cast<std::uint8_t*>(switches + 0x0D);
}

// Whether the composite draws the UI into the eye images (restores the game's own value when shown again).
void ui_set_in_eyes(bool show) {
    std::uint8_t* flag = ui_composite_flag();
    if (!flag) {
        return;
    }
    std::uint8_t current = 0;
    if (!mem::try_copy(&current, reinterpret_cast<std::uintptr_t>(flag), 1)) {
        return;
    }
    if (!show) {
        if (g_ui_flag_saved < 0) {
            g_ui_flag_saved = current;
            log::info("ui: UI taken out of the eye images (GlobalSwitches+0x0D was {})", current);
        }
        if (current != 0) {
            *flag = 0; // the game may set it again (e.g. a UI toggle): re-applied every frame
        }
    } else if (g_ui_flag_saved >= 0) {
        *flag = static_cast<std::uint8_t>(g_ui_flag_saved);
        log::info("ui: UI back in the eye images (GlobalSwitches+0x0D = {})", g_ui_flag_saved);
        g_ui_flag_saved = -1;
    }
}

// A world panel in front of the head (yaw only, so it stands upright), `distance` ahead and `drop` below the eyes.
xr::PanelPlacement place_in_front(const vr::RigidPose& head) {
    const vr::Vec3 forward = vr::rotate(head.orientation, {0.0f, 0.0f, -1.0f});
    const float yaw = std::atan2(-forward[0], -forward[2]);
    xr::PanelPlacement place;
    place.anchor = xr::PanelPlacement::Anchor::Local;
    place.pose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    place.pose.position = {head.position[0] - std::sin(yaw) * g_ui_distance, head.position[1] - g_ui_drop,
                           head.position[2] - std::cos(yaw) * g_ui_distance};
    place.width_m = g_ui_width;
    return place;
}

// The panel on the back of the left wrist, facing the eyes when the forearm is raised like reading a watch (left grip
// space: -Z along the index finger, +Y up).
xr::PanelPlacement wrist_place() {
    xr::PanelPlacement place;
    place.anchor = wrist_hand() == kLeftHand ? xr::PanelPlacement::Anchor::LeftGrip : xr::PanelPlacement::Anchor::RightGrip;
    place.pose.position = {0.0f, 0.06f, 0.10f};
    place.pose.orientation = {-0.5f, 0.0f, 0.0f, 0.8660254f}; // 60 degrees up about X
    place.width_m = g_ui_wrist_width;
    return place;
}

// A grip-anchored placement as a LOCAL-space one, from the hand's current grip pose (OpenXR convention).
xr::PanelPlacement in_local(const xr::PanelPlacement& place, const vr::RigidPose& grip) {
    const vr::Quat q{place.pose.orientation.x, place.pose.orientation.y, place.pose.orientation.z, place.pose.orientation.w};
    const vr::Vec3 offset = vr::rotate(grip.orientation, {place.pose.position.x, place.pose.position.y, place.pose.position.z});
    const vr::Quat r = vr::multiply(grip.orientation, q);
    xr::PanelPlacement local = place;
    local.anchor = xr::PanelPlacement::Anchor::Local;
    local.pose.position = {grip.position[0] + offset[0], grip.position[1] + offset[1], grip.position[2] + offset[2]};
    local.pose.orientation = {r[0], r[1], r[2], r[3]};
    return local;
}

// Where the aim ray meets the panel's front face, in panel uv (0..1, v down), or nullopt.
std::optional<std::array<float, 2>> panel_uv(const vr::RigidPose& aim, const xr::PanelPlacement& place, float height_m) {
    const vr::Quat q{place.pose.orientation.x, place.pose.orientation.y, place.pose.orientation.z, place.pose.orientation.w};
    const vr::Vec3 c{place.pose.position.x, place.pose.position.y, place.pose.position.z};
    const vr::Vec3 n = vr::rotate(q, {0.0f, 0.0f, 1.0f});
    const vr::Vec3 right = vr::rotate(q, {1.0f, 0.0f, 0.0f});
    const vr::Vec3 up = vr::rotate(q, {0.0f, 1.0f, 0.0f});
    const vr::Vec3 d = vr::rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
    const auto dot = [](const vr::Vec3& a, const vr::Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float facing = dot(d, n);
    if (facing > -1e-3f) {
        return std::nullopt; // pointing away from (or along) the panel's front
    }
    const vr::Vec3 to_centre{c[0] - aim.position[0], c[1] - aim.position[1], c[2] - aim.position[2]};
    const float t = dot(to_centre, n) / facing;
    if (t <= 0.0f || t > 20.0f) {
        return std::nullopt;
    }
    const vr::Vec3 local{aim.position[0] + d[0] * t - c[0], aim.position[1] + d[1] * t - c[1],
                         aim.position[2] + d[2] * t - c[2]};
    const float u = dot(local, right) / place.width_m + 0.5f;
    const float v = 0.5f - dot(local, up) / height_m;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return std::nullopt;
    }
    return std::array<float, 2>{u, v};
}

// Moves the real mouse cursor to (u, v) of the game window's client area (only while the game has the focus).
void move_cursor(float u, float v) {
    if (!g_window || GetForegroundWindow() != g_window) {
        return;
    }
    RECT client{};
    if (!GetClientRect(g_window, &client)) {
        return;
    }
    POINT point{static_cast<LONG>(u * static_cast<float>(client.right)), static_cast<LONG>(v * static_cast<float>(client.bottom))};
    ClientToScreen(g_window, &point);
    SetCursorPos(point.x, point.y);
}

// Where `position` is on the panel: uv and the distance in front of its face (metres; negative = behind).
std::optional<std::array<float, 3>> panel_point(const vr::Vec3& position, const xr::PanelPlacement& place, float height_m) {
    const vr::Quat q{place.pose.orientation.x, place.pose.orientation.y, place.pose.orientation.z, place.pose.orientation.w};
    const vr::Vec3 local{position[0] - place.pose.position.x, position[1] - place.pose.position.y,
                         position[2] - place.pose.position.z};
    const auto dot = [](const vr::Vec3& a, const vr::Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float u = dot(local, vr::rotate(q, {1.0f, 0.0f, 0.0f})) / place.width_m + 0.5f;
    const float v = 0.5f - dot(local, vr::rotate(q, {0.0f, 1.0f, 0.0f})) / height_m;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return std::nullopt;
    }
    return std::array<float, 3>{u, v, dot(local, vr::rotate(q, {0.0f, 0.0f, 1.0f}))};
}

// Third person: the laser points at the flat screen quad (LOCAL, g_screen_distance_m ahead, g_screen_width_m wide).
void update_screen_laser() {
    g_screen_cursor.reset();
    if (!vrplay::active() || !vrplay::third_person() || !g_ui_laser || !g_image_width ||
        g_menu_is_open.load(std::memory_order_relaxed)) {
        return;
    }
    xr::PanelPlacement screen;
    screen.anchor = xr::PanelPlacement::Anchor::Local;
    screen.pose.position = {0.0f, 0.0f, -g_screen_distance_m};
    screen.width_m = g_screen_width_m;
    const float height = g_screen_width_m * static_cast<float>(g_image_height) / static_cast<float>(g_image_width);
    const HandState right = hands()[pointer_hand()];
    const auto uv = right.aim_valid ? panel_uv(right.aim, screen, height) : std::nullopt;
    if (uv) {
        g_screen_cursor = *uv;
        move_cursor((*uv)[0], (*uv)[1]);
    }
}

void update_ui_panel(IDXGISwapChain* swapchain, const xr::Frame& frame) {
    update_screen_laser();
    const bool want = g_ui_enabled && !g_ui_failed && vrplay::active() && !vrplay::third_person() && g_mode != Mode::Screen;
    gpu_trace::watch_ui(want);
    if (!want) {
        if (g_ui_shown) {
            g_session->set_panel(std::nullopt);
            ui_set_in_eyes(true);
            g_ui_shown = false;
            g_ui_place.reset();
            g_ui_place_request.store(true, std::memory_order_relaxed);
            log::info("ui: panel hidden");
        }
        return;
    }
    ID3D11Texture2D* back_buffer = nullptr;
    D3D11_TEXTURE2D_DESC back_desc{};
    if (SUCCEEDED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) {
        gpu_trace::set_back_buffer(reinterpret_cast<std::uintptr_t>(back_buffer));
        back_buffer->GetDesc(&back_desc);
        back_buffer->Release();
    }
    ID3D11Texture2D* ui = gpu_trace::ui_texture();
    if (!ui) {
        ui_set_in_eyes(true); // the composite must bind the UI once for the watch to find it
        return;
    }
    // A resized window gets a new UI target, which the watch only sees while the composite binds it: after a back
    // buffer size change, let the composite bind the UI for a few frames.
    static UINT last_width = 0, last_height = 0;
    static int redetect = 0;
    if (back_desc.Width && (back_desc.Width != last_width || back_desc.Height != last_height)) {
        if (last_width) {
            log::info("ui: back buffer now {}x{}; looking for the UI texture again", back_desc.Width, back_desc.Height);
            redetect = 5;
        }
        last_width = back_desc.Width;
        last_height = back_desc.Height;
    }
    if (redetect > 0) {
        --redetect;
        ui_set_in_eyes(true);
        ui->Release();
        return;
    }
    if (ui != g_ui_source_seen) {
        D3D11_TEXTURE2D_DESC desc{};
        ui->GetDesc(&desc);
        std::string error;
        if (!g_ui_blitter.create(render::device(), desc.Width, desc.Height, desc.Format, error)) {
            log::error("ui: can't copy the UI texture ({}x{} format {}): {}; panel off", desc.Width, desc.Height,
                       static_cast<int>(desc.Format), error);
            health::set("UI panel", health::State::Unavailable, "see the ui: lines");
            g_ui_failed = true;
            ui->Release();
            return;
        }
        // A UI texture of another shape (the render resolution changed): the panel is made again to match.
        if (g_session->has_panel() && g_ui_panel_width &&
            std::abs(static_cast<float>(desc.Width) / static_cast<float>(std::max<UINT>(desc.Height, 1)) -
                     static_cast<float>(g_ui_panel_width) / static_cast<float>(std::max<UINT>(g_ui_panel_height, 1))) > 0.01f) {
            log::info("ui: the UI texture is now {}x{}; making the panel again", desc.Width, desc.Height);
            g_session->set_panel(std::nullopt);
            g_session->destroy_panel();
        }
        if (!g_session->has_panel()) {
            g_ui_panel_width = std::min<UINT>(desc.Width, kUiMaxWidth);
            g_ui_panel_height = desc.Height * g_ui_panel_width / std::max<UINT>(desc.Width, 1);
            if (!g_session->create_panel(static_cast<int32_t>(g_ui_panel_width), static_cast<int32_t>(g_ui_panel_height),
                                         error)) {
                log::error("ui: can't create the panel layer: {}; panel off", error);
                health::set("UI panel", health::State::Unavailable, "see the ui: lines");
                g_ui_failed = true;
                ui->Release();
                return;
            }
        }
        g_ui_linearize = blit::is_srgb(g_session->format()) && !blit::is_srgb(desc.Format);
        g_ui_source_seen = ui;
        log::info("ui: panel {}x{} from the UI texture {}x{} format {}{}; {:.2f} m wide, {:.2f} m away", g_ui_panel_width,
                  g_ui_panel_height, desc.Width, desc.Height, static_cast<int>(desc.Format),
                  g_ui_linearize ? " (sRGB-decoded)" : "", g_ui_width, g_ui_distance);
    }
    if ((g_ui_place_request.exchange(false, std::memory_order_relaxed) || !g_ui_place) && frame.views.size() == 2) {
        g_ui_place = place_in_front(centre_view(frame.views).pose);
    }
    if (!g_ui_place) {
        ui->Release();
        return;
    }
    // Where it is this frame: the world panel, or on the left wrist.
    const std::array<HandState, 2> both = hands();
    const bool wrist = g_ui_wrist.load(std::memory_order_relaxed);
    // A tracked hand has no grip action space for the runtime to anchor the panel to: place it in LOCAL from the
    // hand's grip every frame instead.
    const bool wrist_hand_tracked = wrist && both[wrist_hand()].hand_tracked && both[wrist_hand()].tracked;
    const xr::PanelPlacement place = !wrist                ? *g_ui_place
                                     : wrist_hand_tracked ? in_local(wrist_place(), both[wrist_hand()].grip)
                                                          : wrist_place();
    // The laser.
    blit::Blitter::Overlay overlay;
    overlay.keep_alpha = true;
    const float height_m =
        place.width_m * static_cast<float>(g_ui_panel_height) / static_cast<float>(std::max<UINT>(g_ui_panel_width, 1));
    if (g_ui_laser && !g_menu_is_open.load(std::memory_order_relaxed)) {
        const HandState& right = both[pointer_hand()];
        std::optional<xr::PanelPlacement> target;
        if (!wrist || wrist_hand_tracked) {
            target = place;
        } else if (both[wrist_hand()].tracked) {
            target = in_local(place, both[wrist_hand()].grip);
        }
        // Touch: the right controller's tip within 3 cm of the face (or up to 3 cm through it) points there; pushing in
        // past 5 mm holds the left mouse button until the tip comes back out past 12 mm or leaves the panel.
        std::optional<std::array<float, 2>> touch;
        if (right.aim_valid && target) {
            const auto p = panel_point(right.hand_tracked ? right.tip : right.aim.position, *target, height_m);
            if (p && (*p)[2] < 0.03f && (*p)[2] > -0.03f) {
                touch = std::array<float, 2>{(*p)[0], (*p)[1]};
                const bool in = (*p)[2] < (g_ui_touch_down ? 0.012f : 0.005f);
                if (in != g_ui_touch_down) {
                    move_cursor((*touch)[0], (*touch)[1]); // the press lands where the tip is
                    vrplay::mouse_left(in);
                    g_ui_touch_down = in;
                    if (in) {
                        ++g_ui_touches;
                        g_session->haptic(pointer_hand(), 0.3f, 0.015f);
                    }
                }
            }
        }
        if (!touch && g_ui_touch_down) {
            vrplay::mouse_left(false);
            g_ui_touch_down = false;
        }
        const auto uv = touch ? touch : right.aim_valid && target ? panel_uv(right.aim, *target, height_m) : std::nullopt;
        if (uv) {
            overlay.cursor_u = (*uv)[0];
            overlay.cursor_v = (*uv)[1];
            overlay.cursor_radius = 0.008f;
            move_cursor((*uv)[0], (*uv)[1]);
            ++g_ui_laser_hits;
            g_ui_cursor_on_panel = true;
        } else if (g_ui_cursor_on_panel) {
            move_cursor(0.5f, 0.5f); // off the panel: park the cursor where the UI doesn't block world clicks
            g_ui_cursor_on_panel = false;
        }
    }
    if (frame.state.shouldRender) {
        const xr::EyeImage image = g_session->acquire_panel();
        g_ui_blitter.draw(ui, {}, image.view, g_ui_panel_width, g_ui_panel_height, g_ui_linearize, overlay);
        g_session->release_panel();
    }
    ui->Release();
    g_session->set_panel(place);
    ui_set_in_eyes(!g_ui_remove_from_eyes);
    if (!g_ui_shown) {
        log::info("ui: panel shown");
        health::set("UI panel", health::State::Ready);
    }
    g_ui_shown = true;
    ++g_ui_frames;
}

// The VR menu (vrmenu.hpp) on panel 1: world-locked in front of the head where it was opened, drawn by its renderer
// thread, uploaded when a new image is ready. The right hand points (laser + trigger) or touches it.
bool g_menu_enabled = true;    // [menu] enabled
float g_menu_width = 0.62f;    // [ui] menu_width (m)
float g_menu_distance = 0.7f;  // [ui] menu_distance (m)
vrmenu::Menu g_menu;
std::unique_ptr<vrmenu::Renderer> g_menu_renderer;
bool g_menu_open = false;
bool g_menu_failed = false;
bool g_menu_image = false; // an image has been uploaded
std::optional<xr::PanelPlacement> g_menu_place;
std::uint64_t g_menu_submitted = 0; // the Menu revision last handed to the renderer
ID3D11Texture2D* g_menu_texture = nullptr;
blit::Blitter g_menu_blitter;
std::vector<std::uint32_t> g_menu_pixels;
bool g_menu_trigger = false;
bool g_menu_touching = false;
std::uint64_t g_menu_clicks = 0;
std::uint64_t g_menu_frames = 0;

bool create_menu() {
    std::string error;
    g_menu_renderer = std::make_unique<vrmenu::Renderer>();
    if (!g_menu_renderer->start()) {
        error = "the menu renderer didn't start (see above)";
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = vrmenu::kWidth;
    desc.Height = vrmenu::kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (error.empty() && FAILED(render::device()->CreateTexture2D(&desc, nullptr, &g_menu_texture))) {
        error = "CreateTexture2D failed";
    }
    if (error.empty()) {
        g_menu_blitter.create(render::device(), desc.Width, desc.Height, desc.Format, error);
    }
    if (error.empty()) {
        g_session->create_panel(vrmenu::kWidth, vrmenu::kHeight, error, 1);
    }
    if (!error.empty()) {
        log::error("menu: can't show the VR menu: {}", error);
        health::set("VR menu", health::State::Unavailable, error);
        return false;
    }
    log::info("menu: ready ({}x{} image, {:.2f} m wide, {:.2f} m away)", vrmenu::kWidth, vrmenu::kHeight, g_menu_width,
              g_menu_distance);
    return true;
}

void set_menu_open(bool open) {
    g_menu_open = open;
    g_menu_is_open.store(open, std::memory_order_relaxed);
    vrplay::set_menu_open(open);
    g_menu_place.reset(); // placed in front of the head each time it opens
    g_menu_trigger = true; // the press that opened it must not click
    g_menu_touching = false;
    if (!open) {
        g_session->set_panel(std::nullopt, 1);
    }
}

void update_menu(const xr::Frame& frame) {
    if (g_menu_toggle_request.exchange(false, std::memory_order_relaxed)) {
        if (!g_menu_open && g_menu_enabled && !g_menu_failed && !g_menu_renderer) {
            g_menu_failed = !create_menu();
        }
        if (g_menu_open || (g_menu_enabled && !g_menu_failed)) {
            set_menu_open(!g_menu_open);
        } else {
            log::info("menu: not available ({})", g_menu_enabled ? "it failed to start" : "[menu] enabled = false");
        }
    }
    if (g_menu_open && !vrplay::active()) {
        set_menu_open(false);
    }
    if (!g_menu_open) {
        return;
    }
    if (!g_menu_place && frame.views.size() == 2) {
        const vr::RigidPose head = centre_view(frame.views).pose;
        const vr::Vec3 forward = vr::rotate(head.orientation, {0.0f, 0.0f, -1.0f});
        const float yaw = std::atan2(-forward[0], -forward[2]);
        xr::PanelPlacement place;
        place.anchor = xr::PanelPlacement::Anchor::Local;
        place.pose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
        place.pose.position = {head.position[0] - std::sin(yaw) * g_menu_distance, head.position[1] - 0.12f,
                               head.position[2] - std::cos(yaw) * g_menu_distance};
        place.width_m = g_menu_width;
        g_menu_place = place;
    }
    if (!g_menu_place) {
        return;
    }
    const float height_m = g_menu_width * static_cast<float>(vrmenu::kHeight) / static_cast<float>(vrmenu::kWidth);
    const HandState right = hands()[pointer_hand()];
    // Touch: the controller's tip within 3 cm in front of the face (or 2 cm through it); pressing in past 5 mm clicks.
    std::optional<std::array<float, 2>> pointer;
    bool click = false;
    if (right.aim_valid) {
        if (const auto p = panel_point(right.hand_tracked ? right.tip : right.aim.position, *g_menu_place, height_m);
            p && (*p)[2] < 0.03f && (*p)[2] > -0.02f) {
            pointer = std::array<float, 2>{(*p)[0], (*p)[1]};
            const bool in = (*p)[2] < 0.005f;
            click = in && !g_menu_touching;
            g_menu_touching = in;
        } else {
            g_menu_touching = false;
        }
        if (!pointer) {
            pointer = panel_uv(right.aim, *g_menu_place, height_m);
        }
    }
    const bool trigger = right.active && right.trigger > (g_menu_trigger ? 0.4f : 0.6f);
    click = click || (trigger && !g_menu_trigger && pointer);
    g_menu_trigger = trigger;
    // Which controllers are in use (the Controls page and the Guide name their buttons).
    std::string profile = "other";
    {
        const std::string bound = g_session->interaction_profile(pointer_hand());
        if (bound.find("oculus/touch") != std::string::npos) {
            profile = "touch";
        } else if (bound.find("valve/index") != std::string::npos) {
            profile = "index";
        } else if (bound.find("htc/vive") != std::string::npos) {
            profile = "vive";
        } else if (bound.empty() && !right.active) {
            profile = "hands";
        }
    }
    {
        static XrTime last = 0;
        const XrTime now_time = frame.state.predictedDisplayTime;
        g_menu.tick(last ? std::clamp(static_cast<float>(static_cast<double>(now_time - last) * 1e-9), 0.0f, 0.1f) : 0.0f);
        last = now_time;
    }
    const vrmenu::Actions actions = g_menu.update(pointer, click, right.active ? right.stick_x : 0.0f,
                                                  {vrplay::third_person(), g_ui_wrist.load(std::memory_order_relaxed), profile});
    if (click) {
        ++g_menu_clicks;
    }
    if (!actions.settings.empty()) {
        std::string lines;
        for (const auto& [name, value] : actions.settings) {
            lines += std::format("set={} {}\n", name, value);
        }
        bridge::post(std::move(lines));
    }
    if (actions.toggle_view) {
        request_view_toggle();
    }
    if (actions.toggle_ui) {
        bridge::post(std::format("set=ui.placement {}\n", toggle_ui_wrist() ? "wrist" : "world"));
    }
    if (actions.recenter) {
        g_recenter_request.store(true, std::memory_order_relaxed);
        g_menu_place.reset(); // and the menu comes along
        log::info("menu: recentre");
    }
    if (actions.recalibrate) {
        vrplay::recalibrate();
        log::info("menu: calibrating the eye height again");
    }
    if (actions.haptic > 0.0f) {
        g_session->haptic(pointer_hand(), actions.haptic, 0.015f);
    }
    if (actions.close) {
        set_menu_open(false);
        return;
    }
    if (g_menu.revision() != g_menu_submitted) {
        g_menu_submitted = g_menu.revision();
        g_menu_renderer->submit(g_menu.snapshot());
    }
    if (g_menu_renderer->take(g_menu_pixels)) {
        ID3D11DeviceContext* context = nullptr;
        render::device()->GetImmediateContext(&context);
        if (context) {
            context->UpdateSubresource(g_menu_texture, 0, nullptr, g_menu_pixels.data(), vrmenu::kWidth * 4, 0);
            context->Release();
            g_menu_image = true;
        }
    }
    if (!g_menu_image || !g_menu_place) {
        return;
    }
    if (frame.state.shouldRender) {
        blit::Blitter::Overlay overlay;
        overlay.keep_alpha = true;
        if (pointer) {
            overlay.cursor_u = (*pointer)[0];
            overlay.cursor_v = (*pointer)[1];
            overlay.cursor_radius = g_menu_touching ? 0.004f : 0.006f;
        }
        const xr::EyeImage image = g_session->acquire_panel(1);
        g_menu_blitter.draw(g_menu_texture, {}, image.view, vrmenu::kWidth, vrmenu::kHeight,
                            blit::is_srgb(g_session->format()), overlay);
        g_session->release_panel(1);
    }
    g_session->set_panel(g_menu_place, 1);
    ++g_menu_frames;
}

blit::Blitter::Overlay beam_overlay(const vr::RigidPose& eye, const camera::EyeFov& fov) {
    blit::Blitter::Overlay overlay;
    overlay.vignette = g_frame_vignette;
    const int style = g_beam_style.load(std::memory_order_relaxed);
    if (style <= 0 || !vrplay::active() || vrplay::third_person() || g_menu_is_open.load(std::memory_order_relaxed)) {
        return overlay;
    }
    const HandState hand = hands()[pointer_hand()];
    if (!hand.aim_valid) {
        return overlay;
    }
    const vr::Vec3 dir = vr::rotate(hand.aim.orientation, {0.0f, 0.0f, -1.0f});
    const float scale = std::max(camera::world_scale(), 0.01f);
    const float length = rayprobe::beam_distance().value_or(g_beam_range.load(std::memory_order_relaxed)) / scale;
    const vr::Vec3 start = hand.hand_tracked ? hand.tip : hand.aim.position;
    const vr::Vec3 end{start[0] + dir[0] * length, start[1] + dir[1] * length, start[2] + dir[2] * length};
    const auto a = eye_uv(start, eye, fov);
    const auto b = eye_uv(end, eye, fov);
    if (!a || !b) {
        return overlay;
    }
    overlay.beam_u0 = (*a)[0];
    overlay.beam_v0 = (*a)[1];
    overlay.beam_u1 = (*b)[0];
    overlay.beam_v1 = (*b)[1];
    for (int i = 0; i < 3; ++i) {
        overlay.beam_rgba[i] = g_beam_colour[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
    }
    overlay.beam_rgba[3] = g_beam_opacity.load(std::memory_order_relaxed);
    overlay.beam_width = g_beam_width.load(std::memory_order_relaxed);
    overlay.beam_style = style;
    overlay.beam_time = g_beam_time;
    overlay.beam_end = rayprobe::beam_distance() ? 0.012f : 0.0f;
    ++g_beam_frames;
    return overlay;
}

// Just before the game's Present.
void on_present(IDXGISwapChain* swapchain, std::uintptr_t render_frame) {
    if (g_failed) {
        return;
    }
    if (!g_ready) {
        start(swapchain);
        if (!g_ready) {
            return;
        }
    }
    if (!g_session->poll()) {
        log::info("xr: the runtime ended the session");
        publish_status(false);
        camera::set_head(std::nullopt);
        render::set_sync_interval_override(-1);
        g_session.reset();
        g_blitter.destroy();
        health::set("headset", health::State::Unavailable, "see the xr: lines above");
        g_failed = true;
        return;
    }
    xr::Frame frame;
    if (!g_session->running() || !g_session->begin_frame(frame)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        publish_status(false);
        return;
    }
    publish_status(true);
    follow_back_buffer(swapchain);
    if (frame.views.size() == 2) {
        // The interpupillary distance the runtime reports (the headset's IPD setting), applied 1:1 at world scale 1.
        static float logged_ipd = -1.0f;
        const auto& a = frame.views[0].pose.position;
        const auto& b = frame.views[1].pose.position;
        const float ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        if (std::abs(ipd - logged_ipd) > 0.0005f) {
            log::info("xr: IPD {:.1f} mm from the runtime's eye poses (used as is: world scale 1 game unit = 1 m)", ipd * 1000.0f);
            logged_ipd = ipd;
        }
    }
    update_input(frame);
    {
        static XrTime last = 0;
        const double dt = last ? static_cast<double>(frame.state.predictedDisplayTime - last) * 1e-9 : 0.0;
        last = frame.state.predictedDisplayTime;
        g_frame_vignette = comfort_vignette_now(std::clamp(dt, 0.0, 0.1));
        g_beam_time += static_cast<float>(std::clamp(dt, 0.0, 0.1));
    }
    update_menu(frame);
    update_ui_panel(swapchain, frame);
    if (g_mode == Mode::Screen) {
        submit_screen(swapchain, frame);
    } else {
        submit_tracked(swapchain, render_frame, frame);
    }
    g_submitted.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void install(const Config& config) {
    if (!config.get_bool("xr", "enabled", false)) {
        health::set("headset", health::State::Off, "[xr] enabled = false");
        log::info("xr: disabled ([xr] enabled = false in bg3vr.ini)");
        return;
    }
    const std::string mode = text::to_lower(config.get("xr", "mode", "screen"));
    if (mode == "mono") {
        g_mode = Mode::Mono;
    } else if (mode == "alternate") {
        g_mode = Mode::Alternate;
    } else if (mode != "screen") {
        log::warn("xr: unknown mode '{}' (screen, mono or alternate); using screen", mode);
    }
    const std::string width = config.get("xr", "screen_width_m");
    const std::string distance = config.get("xr", "screen_distance_m");
    if (!width.empty()) {
        g_screen_width_m = std::strtof(width.c_str(), nullptr);
    }
    if (!distance.empty()) {
        g_screen_distance_m = std::strtof(distance.c_str(), nullptr);
    }
    g_desktop_vsync = config.get_bool("xr", "desktop_vsync", false);
    g_input_wanted = config.get_bool("xr", "input", true);
    g_input_haptics = config.get_bool("xr", "input_haptics", true);
    g_autostart = config.get_bool("vr", "autostart", true);
    g_ui_enabled = config.get_bool("ui", "panel", true);
    g_ui_remove_from_eyes = config.get_bool("ui", "remove_from_eyes", true);
    g_ui_laser = config.get_bool("ui", "laser", true);
    const auto ui_number = [&](const char* key, float fallback, float lo, float hi) {
        const std::string v = config.get("ui", key);
        return v.empty() ? fallback : std::clamp(std::strtof(v.c_str(), nullptr), lo, hi);
    };
    g_ui_distance = ui_number("distance", 1.0f, 0.3f, 5.0f);
    g_ui_width = ui_number("width", 1.1f, 0.2f, 5.0f);
    g_ui_drop = ui_number("below_eyes", 0.15f, -1.0f, 1.0f);
    g_ui_wrist_width = ui_number("wrist_width", 0.3f, 0.1f, 1.0f);
    g_ui_wrist.store(text::to_lower(config.get("ui", "placement", "wrist")) == "wrist", std::memory_order_relaxed);
    g_menu_enabled = config.get_bool("menu", "enabled", true);
    g_menu_width = ui_number("menu_width", 0.62f, 0.2f, 2.0f);
    g_menu_distance = ui_number("menu_distance", 0.7f, 0.3f, 3.0f);
    g_menu.load(config);
    configure_ui(config);
    const std::string lag = text::to_lower(config.get("xr", "lag", "auto"));
    if (lag != "auto") {
        set_lag(static_cast<int>(std::strtol(lag.c_str(), nullptr, 10)));
    }
    render::set_present_callback(&on_present);
    log::info("xr: enabled, mode {}; the session opens on the first presented frame", mode_name(g_mode));
}

void configure_ui(const Config& config) {
    const std::size_t dominant = text::to_lower(config.get("vr", "dominant_hand", "right")) == "left" ? kLeftHand : kRightHand;
    if (g_dominant.exchange(dominant) != dominant) {
        log::info("vr: dominant hand {} (points and gets the right_* bindings); the wrist UI is on the {} wrist",
                  dominant == kLeftHand ? "left" : "right", dominant == kLeftHand ? "right" : "left");
    }
    {
        // The aiming beam.
        const std::string style = text::to_lower(config.get("pointer", "style", "dots"));
        const int s = style == "off" ? 0 : style == "solid" ? 1 : style == "dashed" ? 2 : style == "pulse" ? 4 : 3;
        // Named colours, sRGB values given as linear (the eye images are sRGB).
        struct Named {
            const char* name;
            float r, g, b;
        };
        constexpr Named kColours[] = {{"gold", 0.85f, 0.55f, 0.15f}, {"white", 0.9f, 0.9f, 0.9f}, {"blue", 0.15f, 0.45f, 1.0f},
                                      {"green", 0.2f, 0.85f, 0.3f},  {"red", 0.95f, 0.15f, 0.1f},  {"purple", 0.6f, 0.25f, 0.95f}};
        const std::string colour = text::to_lower(config.get("pointer", "colour", "gold"));
        Named chosen = kColours[0];
        for (const Named& n : kColours) {
            if (colour == n.name) {
                chosen = n;
            }
        }
        const auto number = [&](const char* key, float fallback, float lo, float hi) {
            const std::string v = config.get("pointer", key);
            return v.empty() ? fallback : std::clamp(std::strtof(v.c_str(), nullptr), lo, hi);
        };
        g_beam_style = s;
        g_beam_colour[0] = chosen.r;
        g_beam_colour[1] = chosen.g;
        g_beam_colour[2] = chosen.b;
        g_beam_opacity = number("opacity", 0.55f, 0.05f, 1.0f);
        g_beam_width = number("width", 0.006f, 0.001f, 0.03f);
        g_beam_range = number("range", 12.0f, 1.0f, 60.0f);
        rayprobe::set_beam(s > 0, g_beam_range.load());
        log::info("pointer: aiming beam {} ({}, opacity {:.2f}, width {:.3f}, range {:.0f} m)",
                  s == 0 ? "off" : style, chosen.name, g_beam_opacity.load(), g_beam_width.load(), g_beam_range.load());
    }
    const std::string v = config.get("ui", "wrist_width");
    const float width = v.empty() ? 0.3f : std::clamp(std::strtof(v.c_str(), nullptr), 0.1f, 1.0f);
    if (g_ui_wrist_width.exchange(width) != width) {
        log::info("ui: wrist panel {:.2f} m wide", width);
    }
}

void recenter() {
    g_recenter_request.store(true, std::memory_order_relaxed);
    log::info("xr: recentering on the next frame");
}

void set_lag(int frames) {
    g_lag.store(std::clamp(frames, -1, static_cast<int>(camera::kAppliedHistory) - 1), std::memory_order_relaxed);
    if (frames < 0) {
        g_restart_search.store(true, std::memory_order_relaxed);
    }
    if (frames < 0) {
        log::info("xr: lag auto (find the rendered view in the RenderFrame)");
    } else {
        log::info("xr: lag {} camera update(s) between a frame's pose and its Present", frames);
    }
}

bool set_stereo(bool on) {
    if (on && g_mode == Mode::Screen) {
        log::warn("xr: same-frame stereo needs a head-tracked mode ([xr] mode = mono or alternate)");
        return false;
    }
    g_stereo.store(on, std::memory_order_relaxed);
    if (!on) {
        stereo::set_eyes(std::nullopt);
    }
    log::info("xr: same-frame stereo {}", on ? "on" : "off");
    return true;
}

void report() {
    log::info("xr: {} frames submitted ({} as the screen, {} same-frame stereo), {} skipped (session not running); "
              "rendered view matched {} / unmatched {} / assumed {}; measured lag {}",
              g_submitted.load(std::memory_order_relaxed), g_screen_frames.load(std::memory_order_relaxed),
              g_stereo_frames.load(std::memory_order_relaxed),
              g_skipped.load(std::memory_order_relaxed), g_matched.load(std::memory_order_relaxed),
              g_unmatched.load(std::memory_order_relaxed), g_guessed.load(std::memory_order_relaxed),
              g_measured_lag.load(std::memory_order_relaxed));
    log::info("ui: panel {} ({}), {} frames, laser on it {} frames, {} touch click(s); VR menu {} ({} frames, {} click(s)){}",
              g_ui_shown ? "shown" : "hidden", g_ui_wrist.load(std::memory_order_relaxed) ? "wrist" : "world", g_ui_frames,
              g_ui_laser_hits, g_ui_touches, g_menu_is_open.load(std::memory_order_relaxed) ? "open" : "closed",
              g_menu_frames, g_menu_clicks, g_menu_failed ? ", FAILED to start" : "");
    log::info("pointer: aiming beam drawn in {} eye copies; last hit {}", g_beam_frames,
              rayprobe::beam_distance() ? std::format("{:.2f} m", *rayprobe::beam_distance()) : std::string("none"));
    log::info("xr input: hand tracking {}; tracked-hand clicks left {} right {}, grabs left {} right {}",
              g_session && g_session->has_hand_tracking() ? "on" : "off", g_hand_clicks[0], g_hand_clicks[1],
              g_hand_grabs[0], g_hand_grabs[1]);
}

std::array<HandState, 2> hands() {
    std::lock_guard lock(g_hands_mutex);
    return g_hands;
}

void haptic(std::size_t hand, float amplitude, float seconds) {
    if (hand > 1) {
        return;
    }
    g_haptics[hand].amplitude.store(std::clamp(amplitude, 0.0f, 1.0f), std::memory_order_relaxed);
    g_haptics[hand].seconds.store(std::clamp(seconds, 0.0f, 2.0f), std::memory_order_relaxed);
    g_haptics[hand].pending.store(true, std::memory_order_release);
}

void report_hands(bool buzz) {
    if (buzz) {
        g_hands_buzz.store(true, std::memory_order_relaxed);
    }
    g_hands_report.store(true, std::memory_order_relaxed);
    log::info("xr input: hands report requested{} (logged on the next headset frame{})", buzz ? " with a buzz" : "",
              g_input ? "" : "; input isn't running yet");
}

} // namespace bg3vr::headset
