#include "bg3vr/headset.hpp"

#include "bg3vr/gameui.hpp"

#include "bg3vr/blit.hpp"
#include "bg3vr/bodyik.hpp"
#include "bg3vr/camera.hpp"
#include "bg3vr/combat.hpp"
#include "bg3vr/dolls.hpp"
#include "bg3vr/corpse.hpp"
#include "bg3vr/dice.hpp"
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
#include "bg3vr/paperdoll.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/resolution.hpp"
#include "bg3vr/stereo.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/views.hpp"
#include "bg3vr/vrmenu.hpp"
#include "bg3vr/vrplay.hpp"
#include "bg3vr/watchdog.hpp"
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
double g_head_prediction_s = 0.0; // [xr] head_prediction_ms
bool g_async_wait = false; // [xr] async_wait: xrWaitFrame on its own thread (off: on 2026-10-23 it gained nothing, SteamVR's wait returned at once, and its frame timing went stale)

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
std::atomic<bool> g_ui_dialogue{false};       // a conversation is on
std::atomic<bool> g_ui_dialogue_front{true};  // [ui] dialogue_in_front
std::atomic<bool> g_ui_wrist{false};         // [ui] placement = wrist
// [ui] placement = hidden (the default): the game UI stays hidden until the "panel" binding (X) reveals it, and comes
// up by itself in combat and in conversations; shown, it is in front of you.
std::atomic<bool> g_ui_on_demand{true};
std::atomic<bool> g_ui_revealed{false};       // the "panel" binding showed it
std::atomic<bool> g_ui_combat{false};         // the controlled character is in combat (Lua: incombat=)
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
std::atomic<bool> g_first_run{false};           // open the VR menu on the Guide at the first VR start
std::atomic<bool> g_hand_tracking{false};       // the session has hand trackers (for report(), any thread)
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
    g_session->set_async_wait(g_async_wait);
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
            g_hand_tracking = g_session->has_hand_tracking();
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
std::uint64_t g_beam_on_panel = 0;
// Where the panels the laser can point at are, in LOCAL space, as last placed (presenting thread): the beam stops on
// the nearest of them, the world or the range.
struct PanelTarget {
    xr::PanelPlacement place;
    float height_m = 0.0f;
};
std::optional<PanelTarget> g_ui_target;   // the game UI panel (wrist or in front), while shown
bool g_ui_laser_through = false;
float g_drag_hold_s = 0.35f; // [ui] drag_hold_s: a trigger press on the UI is a click until held this long and moved
// The flat character pictures the 3D UI dolls replace (source uv x0, y0, x1, y1; x1 <= x0 for none): blanked on the
// panel so only the 3D character shows there (the 3D one sits behind the panel surface, so the two never lined up).
std::array<std::array<float, 4>, 2> g_doll_fill{};          // the laser is over a transparent part of the UI: it goes on into the world
std::optional<PanelTarget> g_menu_target; // the VR menu, while open

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
                      const blit::Blitter::Rect& rect = {}, const blit::Blitter::Overlay* overlay_in = nullptr,
                      bool reuse_copy = false) {
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
                           overlay_in ? *overlay_in : overlay, reuse_copy);
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

// [xr] desktop_view = left_eye: after both eyes went to the headset, the game's window shows only the left eye,
// cropped to the window's shape (the back buffer holds both eyes side by side). One full-screen draw from the copy
// the eyes were taken from (no extra copy, nothing re-rendered), just before the game's Present.
std::atomic<bool> g_desktop_left_eye{true}; // the VR menu changes it live (configure_ui)
bool g_mirror_ready = false; // this Present's eyes came from the back buffer's halves (the blitter's copy is current)
std::uint64_t g_mirror_frames = 0;

void show_left_eye_on_desktop(IDXGISwapChain* swapchain) {
    // Whenever the back buffer holds both eyes side by side (stereo layout on), not only on frames whose eyes went to
    // the headset: frames that couldn't be matched to a head pose, or that the runtime said not to render, kept the
    // two eyes on the monitor (2026-10-26: "jumping back to split view very frequently"). Those frames copy afresh.
    const bool side_by_side = g_stereo.load(std::memory_order_relaxed) && stereo::active();
    if (!g_desktop_left_eye.load(std::memory_order_relaxed) || !side_by_side || !g_blitter.ready()) {
        return;
    }
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);
    DXGI_SWAP_CHAIN_DESC chain{};
    RECT client{};
    swapchain->GetDesc(&chain);
    const bool have_window = chain.OutputWindow && GetClientRect(chain.OutputWindow, &client) && client.right > 0 &&
                             client.bottom > 0;
    ID3D11RenderTargetView* target = nullptr;
    if (desc.Width == g_image_width && desc.Height == g_image_height && have_window &&
        SUCCEEDED(render::device()->CreateRenderTargetView(back_buffer, nullptr, &target))) {
        // The left half, cut to the window's aspect around its centre (the window stretches the back buffer).
        const double half = static_cast<double>(desc.Width / 2), height = static_cast<double>(desc.Height);
        const double window = static_cast<double>(client.right) / static_cast<double>(client.bottom);
        blit::Blitter::Rect rect{0, 0, desc.Width / 2, desc.Height};
        if (window > half / height) {
            rect.height = static_cast<UINT>(half / window);
            rect.y = (desc.Height - rect.height) / 2;
        } else {
            rect.width = static_cast<UINT>(height * window);
            rect.x = (desc.Width / 2 - rect.width) / 2;
        }
        g_blitter.draw(back_buffer, rect, target, desc.Width, desc.Height, false, {}, g_mirror_ready);
        target->Release();
        static std::uint64_t fresh_copies = 0;
        fresh_copies += g_mirror_ready ? 0 : 1;
        if (g_mirror_frames % 1800 == 1799) {
            log::info("xr: desktop view: {} frames, {} of them with no eye copy this frame (copied afresh)", g_mirror_frames + 1,
                      fresh_copies);
        }
        if (++g_mirror_frames == 1) {
            log::info("xr: the desktop window shows the left eye ({}x{} at ({}, {}) of the {}x{} back buffer, window {}x{})",
                      rect.width, rect.height, rect.x, rect.y, desc.Width, desc.Height, client.right, client.bottom);
        }
    }
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
                copy_back_buffer(swapchain, image, g_eye_width, g_eye_height, rect, &overlay, eye == 1);
                g_session->release(static_cast<std::size_t>(eye));
                g_rendered[eye] = xr_view(rendered->head.eye_pose[eye], rendered->head.eye_fov[eye]);
                g_has_image[eye] = true;
            }
            g_stereo_frames.fetch_add(1, std::memory_order_relaxed);
            g_mirror_ready = true;
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

    // The view for the frame the game renders next, predicted for when that frame will be shown: about one game
    // frame from now (the game renders it and presents it next Present; agent W 4.1: the measured "lag" counts the
    // ~5 camera updates per frame, not frames, so the old 1 + lag display periods predicted ~67 ms ahead at 90 Hz)
    // plus one display period for the compositor. Without the runtime's clock, the old estimate with one frame.
    static std::chrono::steady_clock::time_point last_present{};
    static double frame_s = 1.0 / 30.0;
    const auto present_now = std::chrono::steady_clock::now();
    if (last_present.time_since_epoch().count() != 0) {
        const double dt = std::chrono::duration<double>(present_now - last_present).count();
        if (dt > 0.0 && dt < 0.25) {
            frame_s += (dt - frame_s) * 0.1; // smoothed time between Presents
        }
    }
    last_present = present_now;
    std::vector<XrView> views;
    // The head pose as it is now, not extrapolated ([xr] head_prediction_ms, default 0): the camera moves only as the
    // head did; the compositor reprojects each image from the pose it was rendered with to the head at display time.
    // (SteamVR stretches the frame's display period when it throttles a slow game: 55.6 ms on 2026-10-23, which made
    // the old prediction 84 ms.)
    const XrTime xr_now = g_session->now();
    const double ahead_s = g_head_prediction_s;
    const XrTime when = xr_now ? xr_now + static_cast<XrTime>(ahead_s * 1e9) : frame.state.predictedDisplayTime;
    {
        // Every 5 s: the time between Presents (min / average / max, how many over 50 ms) and the display period
        // SteamVR asks for (it stretches it when it throttles the game), to measure judder in one run.
        static std::chrono::steady_clock::time_point window_start = present_now;
        static double min_s = 1e9, max_s = 0.0, sum_s = 0.0, period_max = 0.0;
        static std::uint64_t frames = 0, slow = 0;
        static auto previous = present_now;
        const double interval = std::chrono::duration<double>(present_now - previous).count();
        previous = present_now;
        if (interval > 0.0 && interval < 1.0) {
            min_s = std::min(min_s, interval);
            max_s = std::max(max_s, interval);
            sum_s += interval;
            ++frames;
            slow += interval > 0.05 ? 1 : 0;
        }
        period_max = std::max(period_max, static_cast<double>(frame.state.predictedDisplayPeriod) * 1e-9);
        if (std::chrono::duration<double>(present_now - window_start).count() >= 5.0 && frames) {
            log::info("xr: frames: {} in 5 s, between Presents min {:.1f} / avg {:.1f} / max {:.1f} ms, {} over 50 ms; "
                      "SteamVR display period up to {:.1f} ms",
                      frames, min_s * 1000.0, sum_s / static_cast<double>(frames) * 1000.0, max_s * 1000.0, slow,
                      period_max * 1000.0);
            window_start = present_now;
            min_s = 1e9;
            max_s = sum_s = period_max = 0.0;
            frames = slow = 0;
        }
    }
    if (static std::uint64_t predictions = 0; ++predictions == 120 || predictions % 3600 == 0) {
        log::info("xr: head pose predicted {:.1f} ms ahead (game frame {:.1f} ms, display period {:.1f} ms, {})",
                  xr_now ? static_cast<double>(when - xr_now) * 1e-6 : -1.0, frame_s * 1000.0,
                  static_cast<double>(frame.state.predictedDisplayPeriod) * 1e-6,
                  xr_now ? "from the runtime's clock" : "the runtime has no clock conversion: from the frame's display time");
    }
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
    // First person <-> third person, both in VR (the stereo, head-tracked view behind the character). From the old
    // flat-screen third person (only reachable by hand now) it goes back to first person.
    // First person -> third person (VR) -> free camera -> first person.
    const char* request = camera::free_camera()                          ? "view_first"
                          : camera::chase()                              ? "view_free"
                          : vrplay::third_person()                       ? "view_first"
                                                                         : "view_chase";
    bridge::push_request(request);
    log::info("vr: asking the Lua side for {}", std::string_view(request) == "view_first"   ? "first person"
                                                : std::string_view(request) == "view_free" ? "the free camera"
                                                                                            : "third person (in VR)");
}

// The game UI panel between the world and the left wrist; returns true when it is now on the wrist.
// [ui] placement: hidden (shown on demand), world (always in front) or wrist.
void apply_placement(const std::string& placement) {
    const bool wrist = placement == "wrist";
    const bool on_demand = placement != "wrist" && placement != "world";
    const bool was_wrist = g_ui_wrist.exchange(wrist, std::memory_order_relaxed);
    const bool was_on_demand = g_ui_on_demand.exchange(on_demand, std::memory_order_relaxed);
    if (was_wrist != wrist || was_on_demand != on_demand) {
        g_ui_place_request.store(true, std::memory_order_relaxed);
        log::info("ui: the game interface {}", on_demand ? "is hidden until the panel button (X) shows it, and comes up in "
                                                          "combat and conversations"
                                               : wrist ? "is on your wrist" : "is always in front of you");
    }
}

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
            body_hands[h] = {now[h].tracked, now[h].grip.position,
                             (now[h].active || now[h].hand_tracked) && now[h].squeeze > 0.5f && !hold::holding(h) &&
                             !vrplay::in_scene()};
        }
        holsters = holster::on_frame(centre_view(frame.views).pose, body_hands);
        for (std::size_t h = 0; h < 2; ++h) {
            if (holsters.haptic[h] > 0.0f) {
                g_session->haptic(h, holsters.haptic[h], 0.02f);
            }
        }
    }
    else {
        holster::reset(); // no stale grip edge when holsters resume
    }
    // Grips and aims for picking things up (world, game convention).
    for (std::size_t h = 0; h < 2; ++h) {
        std::optional<vr::RigidPose> aim_world;
        if (now[h].aim_valid) {
            aim_world = camera::tracked_to_world(now[h].aim);
        }
        // No grabbing, pulling or body dragging while a scene holds the character (a squeeze mid-conversation took
        // items off tables); the physical die still works (dice.cpp, its own grip).
        const bool scene = vrplay::in_scene();
        if (scene) {
            aim_world.reset(); // no glove highlight either
        }
        const bool grab = (now[h].active || now[h].hand_tracked) && now[h].squeeze > 0.5f &&
                          !g_menu_is_open.load(std::memory_order_relaxed) && !holsters.grip_taken[h] && !scene;
        // Bodies (corpse.cpp): the hand's velocity in the world for throws.
        std::optional<vr::Vec3> world_velocity;
        if (now[h].world_valid && now[h].velocity_valid) {
            vr::RigidPose ahead = now[h].grip;
            for (int i = 0; i < 3; ++i) {
                ahead.position[static_cast<std::size_t>(i)] += now[h].velocity[static_cast<std::size_t>(i)];
            }
            if (const auto moved = camera::tracked_to_world(ahead)) {
                world_velocity = vr::Vec3{moved->position[0] - now[h].world_grip.position[0],
                                          moved->position[1] - now[h].world_grip.position[1],
                                          moved->position[2] - now[h].world_grip.position[2]};
            }
        }
        corpse::set_hand(h, grab, now[h].world_valid ? std::optional<vr::RigidPose>(now[h].world_grip) : std::nullopt,
                         aim_world, world_velocity);
        // Over a shoulder or behind the back (the holster zones): letting go of a held item there stows it.
        bool in_pack = false;
        if (now[h].tracked && frame.views.size() == 2) {
            const holster::Zone zone = holster::zone_of(centre_view(frame.views).pose, now[h].grip.position);
            in_pack = zone == holster::Zone::ShoulderRight || zone == holster::Zone::ShoulderLeft || zone == holster::Zone::Back;
        }
        // The physical die (dice.cpp) is in the pointing hand while a roll waits: that grip throws it, not the gloves.
        const bool die_hand = h == pointer_hand();
        if (die_hand) {
            dice::set_hand(grab, now[h].world_valid ? std::optional<vr::RigidPose>(now[h].world_grip) : std::nullopt,
                           static_cast<double>(frame.state.predictedDisplayTime) * 1e-9);
        }
        hold::set_hand(h, grab && !corpse::holding(h) && !(die_hand && dice::wants_grip()),
                       now[h].world_valid ? std::optional<vr::RigidPose>(now[h].world_grip) : std::nullopt, aim_world,
                       static_cast<double>(frame.state.predictedDisplayTime) * 1e-9, in_pack);
        if (const auto buzz = hold::take_haptic(h)) {
            g_session->haptic(h, buzz->amplitude, buzz->seconds);
        }
    }
    // The right hand's swing for physical melee.
    melee::set_hand(now[pointer_hand()].aim_valid && !g_menu_is_open.load(std::memory_order_relaxed)
                        ? camera::tracked_to_world(now[pointer_hand()].aim)
                        : std::nullopt,
                    static_cast<double>(frame.state.predictedDisplayTime) * 1e-9);
    // Combat: swings and the bow (combat.cpp), from the head and both grips.
    combat::Feedback combat_feedback;
    if (frame.views.size() == 2) {
        combat::Frame cf;
        cf.head_world = camera::tracked_to_world(centre_view(frame.views).pose);
        const HandState& ph = now[pointer_hand()];
        const HandState& oh = now[wrist_hand()];
        if (ph.world_valid) {
            cf.pointing_world = ph.world_grip;
            cf.pointing_tracked = ph.grip.position;
        }
        if (oh.world_valid) {
            cf.other_world = oh.world_grip;
            cf.other_tracked = oh.grip.position;
        }
        cf.pointing_speed = ph.velocity_valid ? std::sqrt(ph.velocity[0] * ph.velocity[0] + ph.velocity[1] * ph.velocity[1] +
                                                          ph.velocity[2] * ph.velocity[2])
                                              : 0.0f;
        cf.pointing_grip = ph.squeeze > 0.5f;
        cf.time_s = static_cast<double>(frame.state.predictedDisplayTime) * 1e-9;
        if (!g_menu_is_open.load(std::memory_order_relaxed)) {
            const combat::Feedback cfb = combat::on_headset(cf);
            combat_feedback = cfb;
            if (cfb.haptic_pointing > 0.0f) {
                g_session->haptic(pointer_hand(), cfb.haptic_pointing, 0.02f);
            }
            if (cfb.haptic_other > 0.0f) {
                g_session->haptic(wrist_hand(), cfb.haptic_other, 0.02f);
            }
        }
    }
    // Finger curl: the index follows the trigger, the others the grip (a tracked hand: how closed it is).
    for (std::size_t h = 0; h < 2; ++h) {
        if (now[h].hand_tracked) {
            const float closed = std::max(now[h].trigger, now[h].squeeze);
            bodyik::set_fingers(h, closed, closed);
        } else {
            bodyik::set_fingers(h, now[h].active ? now[h].trigger : 0.0f, now[h].active ? now[h].squeeze : 0.0f);
        }
    }
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
        // A grip holding something (a holster, a body, the bowstring) sends no binding and isn't a combo modifier.
        const bool grip_busy = holsters.grip_taken[h] || corpse::holding(h) ||
                               (h == pointer_hand() && combat_feedback.pointing_grip_busy);
        play[slot] = {hs.active || hs.hand_tracked, hs.trigger, hs.hand_tracked ? 0.0f : hs.squeeze, hs.stick_x, hs.stick_y,
                      hs.stick_click, hs.primary, hs.secondary, menu, grip_busy};
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
        if (g_ui_on_demand.load(std::memory_order_relaxed)) {
            const bool revealed = !g_ui_revealed.load(std::memory_order_relaxed);
            g_ui_revealed.store(revealed, std::memory_order_relaxed);
            if (revealed) {
                g_ui_place_request.store(true, std::memory_order_relaxed); // in front of where you look now
            }
            log::info("ui: the game interface {} (panel button)", revealed ? "shown" : "hidden");
        } else {
            bridge::post(std::format("set=ui.placement {}\n", toggle_ui_wrist() ? "wrist" : "world"));
        }
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
std::atomic<float> g_ui_wrist_width{0.8f}; // [ui] wrist_width (m); 2x the first 0.4 m (too small to read); 1.2 m was so close to the face it doubled
std::atomic<float> g_ui_wrist_out{0.045f};  // [ui] wrist_out: off the back of the hand (m)
std::atomic<float> g_ui_wrist_along{0.2f};  // [ui] wrist_along: the panel centre up the forearm from the grip (m)
float g_ui_distance = 1.0f;        // [ui] distance (m)
float g_ui_width = 1.1f;           // [ui] width (m)
float g_ui_drop = 0.15f;           // [ui] below_eyes (m)
constexpr UINT kUiMaxWidth = 2560;
blit::Blitter g_ui_blitter;
UINT g_ui_texture_width = 0, g_ui_texture_height = 0; // the UI texture (Noesis' surface): the dolls' rects are in its pixels

// The 3D UI characters (dolls.cpp): up to two shown at once, each as a pair of one-eye quads over its picture's place
// on the UI panel (panel slots 2 + 2 * slot + eye). Presenting thread.
struct DollSlot {
    std::uintptr_t id = 0;
    std::array<blit::Blitter, 2> blitter;
    std::array<UINT, 2> width{}, height{};
    std::array<DXGI_FORMAT, 2> format{};
    std::array<bool, 2> panel{};
};
std::array<DollSlot, 2> g_doll_slots;
std::uint64_t g_doll_frames = 0;

void hide_doll_slot(DollSlot& slot, std::size_t k) {
    for (std::size_t e = 0; e < 2; ++e) {
        g_session->set_panel(std::nullopt, 2 + 2 * k + e);
    }
    if (slot.id) {
        dolls::set_window(slot.id, std::nullopt, 0.0f);
        slot.id = 0;
    }
}
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
    gpu_trace::set_ui_hidden(!show);
    // The old way (GlobalSwitches+0x0D) only to give back a value an older build may have cleared.
    if (!show) {
        return;
    }
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

// The panel on the back of the off hand's forearm, like a watch face (grip space, OpenXR: -Z up the handle on the thumb
// side, +Y back toward the elbow, +X to the right for both hands, so the back of the left hand faces -X and the right
// +X). The panel faces out of the back of the hand, starts just behind the wrist and runs up the forearm; its top is
// the little-finger side and its text runs toward the hand, so it reads upright when the arm is raised to look at it.
xr::PanelPlacement wrist_place() {
    xr::PanelPlacement place;
    const bool left = wrist_hand() == kLeftHand;
    place.anchor = left ? xr::PanelPlacement::Anchor::LeftGrip : xr::PanelPlacement::Anchor::RightGrip;
    const float out = g_ui_wrist_out.load(std::memory_order_relaxed);
    const float along = g_ui_wrist_along.load(std::memory_order_relaxed);
    place.pose.position = {left ? -out : out, along, 0.0f};
    // Columns (panel x, y, z) in grip axes: left (-Y, +Z, -X), right (+Y, +Z, +X).
    place.pose.orientation = left ? XrQuaternionf{0.5f, -0.5f, -0.5f, 0.5f} : XrQuaternionf{0.5f, 0.5f, 0.5f, 0.5f};
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

// Where the aim ray meets the panel's front face: panel uv (0..1, v down) and the distance along the ray (metres).
std::optional<std::array<float, 3>> panel_hit(const vr::RigidPose& aim, const xr::PanelPlacement& place, float height_m);

// Where the aim ray meets the panel's front face, in panel uv (0..1, v down), or nullopt.
std::optional<std::array<float, 2>> panel_uv(const vr::RigidPose& aim, const xr::PanelPlacement& place, float height_m) {
    const auto hit = panel_hit(aim, place, height_m);
    return hit ? std::optional<std::array<float, 2>>{std::array<float, 2>{(*hit)[0], (*hit)[1]}} : std::nullopt;
}

std::optional<std::array<float, 3>> panel_hit(const vr::RigidPose& aim, const xr::PanelPlacement& place, float height_m) {
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
    return std::array<float, 3>{u, v, t};
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

void release_touch() {
    if (g_ui_touch_down) {
        vrplay::mouse_left(false);
        g_ui_touch_down = false;
    }
}

// What is under the laser on the UI panel (presenting thread, the immediate context, like the panel blit): the UI
// texture is transparent outside what the game draws (agent AB 2), so only drawn things (buttons, portraits, the
// hotbar, windows) stop the laser and take the cursor; over the transparent rest it goes on into the world. A 17x17
// pixel box around the laser is copied into one of two small staging textures each frame and read back a frame later
// without waiting. [ui] laser_through_transparent.
struct AlphaProbe {
    std::array<ID3D11Texture2D*, 2> staging{};
    std::array<bool, 2> pending{};
    int next = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool unsupported = false;
    std::optional<float> alpha; // the latest read: the most opaque pixel of the box
    bool opaque = true;         // with hysteresis
    std::uint64_t reads = 0, through = 0;
};
AlphaProbe g_alpha;
bool g_laser_through_transparent = true;
constexpr UINT kProbeBox = 17;

float alpha_of(const std::uint8_t* pixel, DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return static_cast<float>(pixel[3]) / 255.0f;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
        std::uint32_t v = 0;
        std::memcpy(&v, pixel, 4);
        return static_cast<float>(v >> 30) / 3.0f;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: {
        std::uint16_t h = 0;
        std::memcpy(&h, pixel + 6, 2);
        const int exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        const float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                                          : std::ldexp(static_cast<float>(mantissa | 0x400), exponent - 25);
        return (h & 0x8000) ? -value : value;
    }
    default:
        return -1.0f;
    }
}

UINT bytes_per_pixel(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? 8 : 4;
}

void probe_alpha(ID3D11Texture2D* ui, const D3D11_TEXTURE2D_DESC& desc, std::optional<std::array<float, 2>> uv) {
    if (g_alpha.unsupported) {
        return;
    }
    if (desc.SampleDesc.Count != 1 || alpha_of(std::array<std::uint8_t, 8>{}.data(), desc.Format) < 0.0f) {
        g_alpha.unsupported = true;
        log::warn("ui: the UI texture (format {}, {} samples) can't be read for the laser; the whole panel stops it",
                  static_cast<int>(desc.Format), desc.SampleDesc.Count);
        return;
    }
    ID3D11DeviceContext* context = nullptr;
    render::device()->GetImmediateContext(&context);
    if (g_alpha.format != desc.Format) {
        for (auto*& s : g_alpha.staging) {
            if (s) {
                s->Release();
                s = nullptr;
            }
        }
        g_alpha.pending = {};
        g_alpha.format = desc.Format;
    }
    // The read of an earlier copy, without waiting for the GPU.
    for (int i = 0; i < 2; ++i) {
        if (!g_alpha.pending[i] || !g_alpha.staging[i]) {
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (context->Map(g_alpha.staging[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) == S_OK) {
            float most = 0.0f;
            const UINT bpp = bytes_per_pixel(desc.Format);
            for (UINT y = 0; y < kProbeBox; ++y) {
                const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                for (UINT x = 0; x < kProbeBox; ++x) {
                    most = std::max(most, alpha_of(row + x * bpp, desc.Format));
                }
            }
            context->Unmap(g_alpha.staging[i], 0);
            g_alpha.pending[i] = false;
            g_alpha.alpha = most;
            ++g_alpha.reads;
            g_alpha.opaque = g_alpha.opaque ? most > 0.02f : most > 0.06f;
        }
    }
    if (uv && desc.Width > kProbeBox && desc.Height > kProbeBox) {
        const int slot = g_alpha.next;
        if (!g_alpha.staging[slot]) {
            D3D11_TEXTURE2D_DESC s{};
            s.Width = kProbeBox;
            s.Height = kProbeBox;
            s.MipLevels = 1;
            s.ArraySize = 1;
            s.Format = desc.Format;
            s.SampleDesc.Count = 1;
            s.Usage = D3D11_USAGE_STAGING;
            s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(render::device()->CreateTexture2D(&s, nullptr, &g_alpha.staging[slot]))) {
                g_alpha.unsupported = true;
                log::warn("ui: no staging texture for the laser probe; the whole panel stops the laser");
                context->Release();
                return;
            }
        }
        if (!g_alpha.pending[slot]) {
            const UINT cx = static_cast<UINT>(std::clamp((*uv)[0], 0.0f, 1.0f) * static_cast<float>(desc.Width - 1));
            const UINT cy = static_cast<UINT>(std::clamp((*uv)[1], 0.0f, 1.0f) * static_cast<float>(desc.Height - 1));
            const UINT left = std::min(cx > kProbeBox / 2 ? cx - kProbeBox / 2 : 0u, desc.Width - kProbeBox);
            const UINT top = std::min(cy > kProbeBox / 2 ? cy - kProbeBox / 2 : 0u, desc.Height - kProbeBox);
            const D3D11_BOX box{left, top, 0, left + kProbeBox, top + kProbeBox, 1};
            context->CopySubresourceRegion(g_alpha.staging[slot], 0, 0, 0, 0, ui, 0, &box);
            g_alpha.pending[slot] = true;
            g_alpha.next = 1 - slot;
        }
    }
    context->Release();
}

// Snapshots of the UI image (what the panel shows) for the log, because the SteamVR mirror records only the eye
// images, not the panels: every 20 s while the panel is shown and right after it comes up, half size, over grey,
// as bin\bg3vr\ui_shots\ui_<time>.bmp (at most 60 a session). [ui] snapshots.
struct Snapshot {
    ID3D11Texture2D* staging = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool pending = false;
    std::chrono::steady_clock::time_point next{};
    int written = 0;
    std::string reason;
};
Snapshot g_snapshot;
bool g_snapshots = true;
std::filesystem::path g_root_dir;

void write_snapshot(const D3D11_MAPPED_SUBRESOURCE& mapped) {
    const UINT w = g_snapshot.width / 2, h = g_snapshot.height / 2;
    const UINT row_bytes = (w * 3 + 3) & ~3u;
    std::vector<std::uint8_t> bmp(54 + static_cast<std::size_t>(row_bytes) * h);
    auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(bmp.data() + at, &v, 4); };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, static_cast<std::uint32_t>(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    bmp[26] = 1;
    bmp[28] = 24;
    put32(34, static_cast<std::uint32_t>(bmp.size() - 54));
    const UINT bpp = bytes_per_pixel(g_snapshot.format);
    const bool bgr = g_snapshot.format == DXGI_FORMAT_B8G8R8A8_UNORM || g_snapshot.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                     g_snapshot.format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    for (UINT y = 0; y < h; ++y) {
        const auto* src = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y * 2) * mapped.RowPitch;
        std::uint8_t* dst = bmp.data() + 54 + static_cast<std::size_t>(h - 1 - y) * row_bytes;
        for (UINT x = 0; x < w; ++x) {
            const std::uint8_t* p = src + static_cast<std::size_t>(x * 2) * bpp;
            float r = 0, g = 0, b = 0;
            if (bpp == 4 && g_snapshot.format != DXGI_FORMAT_R10G10B10A2_UNORM) {
                r = p[bgr ? 2 : 0] / 255.0f;
                g = p[1] / 255.0f;
                b = p[bgr ? 0 : 2] / 255.0f;
            }
            const float a = std::clamp(alpha_of(p, g_snapshot.format), 0.0f, 1.0f);
            // Over a dark grey checker, so transparent areas show; the UI is premultiplied (agent F), so colour + (1-a) bg.
            const float bg = ((x / 16 + y / 16) & 1) ? 0.18f : 0.12f;
            dst[x * 3 + 0] = static_cast<std::uint8_t>(std::clamp(b + (1 - a) * bg, 0.0f, 1.0f) * 255.0f);
            dst[x * 3 + 1] = static_cast<std::uint8_t>(std::clamp(g + (1 - a) * bg, 0.0f, 1.0f) * 255.0f);
            dst[x * 3 + 2] = static_cast<std::uint8_t>(std::clamp(r + (1 - a) * bg, 0.0f, 1.0f) * 255.0f);
        }
    }
    std::error_code ec;
    const auto dir = g_root_dir / "ui_shots";
    std::filesystem::create_directories(dir, ec);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const auto file = dir / std::format("ui_{:02}{:02}{:02}_{:03}.bmp", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") == 0 && f) {
        fwrite(bmp.data(), 1, bmp.size(), f);
        fclose(f);
        ++g_snapshot.written;
        log::info("ui: snapshot {} ({}; {}x{} from the UI texture, format {})", file.filename().string(), g_snapshot.reason, w,
                  h, static_cast<int>(g_snapshot.format));
    }
}

void snapshot_ui(ID3D11Texture2D* ui, const D3D11_TEXTURE2D_DESC& desc, bool shown_now) {
    if (!g_snapshots || g_root_dir.empty() || g_snapshot.written >= 60 || desc.SampleDesc.Count != 1 ||
        alpha_of(std::array<std::uint8_t, 8>{}.data(), desc.Format) < 0.0f) {
        return;
    }
    ID3D11DeviceContext* context = nullptr;
    render::device()->GetImmediateContext(&context);
    if (g_snapshot.pending) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (context->Map(g_snapshot.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) == S_OK) {
            write_snapshot(mapped);
            context->Unmap(g_snapshot.staging, 0);
            g_snapshot.pending = false;
        }
        context->Release();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!shown_now && now < g_snapshot.next) {
        context->Release();
        return;
    }
    if (!g_snapshot.staging || g_snapshot.width != desc.Width || g_snapshot.height != desc.Height || g_snapshot.format != desc.Format) {
        if (g_snapshot.staging) {
            g_snapshot.staging->Release();
            g_snapshot.staging = nullptr;
        }
        D3D11_TEXTURE2D_DESC s = desc;
        s.MipLevels = 1;
        s.ArraySize = 1;
        s.BindFlags = 0;
        s.MiscFlags = 0;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(render::device()->CreateTexture2D(&s, nullptr, &g_snapshot.staging))) {
            g_snapshots = false;
            log::warn("ui: no staging texture for UI snapshots; snapshots off");
            context->Release();
            return;
        }
        g_snapshot.width = desc.Width;
        g_snapshot.height = desc.Height;
        g_snapshot.format = desc.Format;
    }
    context->CopySubresourceRegion(g_snapshot.staging, 0, 0, 0, 0, ui, 0, nullptr); // mip 0 only
    g_snapshot.pending = true;
    // One more a second after the panel came up: windows fade in, so the first one showed the HUD without the window
    // (2026-10-26, the game menu).
    static bool followup = false;
    g_snapshot.reason = shown_now ? "the panel just came up" : followup ? "a second after the panel came up" : "every 20 s while shown";
    followup = shown_now;
    g_snapshot.next = now + (shown_now ? std::chrono::seconds(1) : std::chrono::seconds(20));
    context->Release();
}

void update_ui_panel(IDXGISwapChain* swapchain, const xr::Frame& frame) {
    update_screen_laser();
    const bool want = g_ui_enabled && !g_ui_failed && vrplay::active() && !vrplay::third_person() && g_mode != Mode::Screen;
    gpu_trace::watch_ui(want);
    if (!want) {
        release_touch();
        vrplay::set_stick_capture(0);
        g_ui_target.reset();
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
    D3D11_TEXTURE2D_DESC ui_desc{};
    ui->GetDesc(&ui_desc);
    static UINT seen_width = 0, seen_height = 0;
    static DXGI_FORMAT seen_format = DXGI_FORMAT_UNKNOWN;
    if (ui != g_ui_source_seen || ui_desc.Width != seen_width || ui_desc.Height != seen_height || ui_desc.Format != seen_format) {
        seen_width = ui_desc.Width;
        seen_height = ui_desc.Height;
        seen_format = ui_desc.Format;
        g_ui_texture_width = ui_desc.Width;
        g_ui_texture_height = ui_desc.Height;
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
    // Hidden until revealed (or combat, or a conversation): no panel, no laser on it, and the UI stays out of the eyes.
    const bool conversation = g_ui_dialogue.load(std::memory_order_relaxed) && g_ui_dialogue_front.load(std::memory_order_relaxed);
    const bool combat = g_ui_combat.load(std::memory_order_relaxed);
    // A game window (inventory, map, trade, loot, game menu, ...) open brings it up too (gameui.cpp, agent AB).
    const bool window = gameui::window_open();
    // While the physical die tumbles, the game's own roll animation stays out of the way (dice.cpp).
    const bool visible = (!g_ui_on_demand.load(std::memory_order_relaxed) || g_ui_revealed.load(std::memory_order_relaxed) ||
                          combat || conversation || window) &&
                         !dice::hide_ui();
    // The laser on the panel (last frame): otherwise the invisible screen UI under the cursor mustn't eat world clicks.
    gameui::set_pointer(vrplay::active() && !vrplay::third_person(), visible && g_ui_cursor_on_panel);
    if (!visible) {
        release_touch();
        vrplay::set_stick_capture(0);
        g_ui_target.reset();
        if (g_ui_shown) {
            g_session->set_panel(std::nullopt);
            g_ui_shown = false;
            log::info("ui: panel hidden (press the panel button, X, to show it)");
        }
        if (g_ui_cursor_on_panel) {
            move_cursor(0.5f, 0.5f);
            g_ui_cursor_on_panel = false;
        }
        ui_set_in_eyes(!g_ui_remove_from_eyes);
        ui->Release();
        return;
    }
    if (!g_ui_shown && g_ui_on_demand.load(std::memory_order_relaxed)) {
        g_ui_place_request.store(true, std::memory_order_relaxed); // shown again: in front of where you look now
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
    const bool wrist = g_ui_wrist.load(std::memory_order_relaxed) && !conversation && !combat && !window;
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
    if (!g_ui_laser || g_menu_is_open.load(std::memory_order_relaxed)) {
        release_touch();
        g_ui_target.reset();
    }
    if (g_ui_laser && !g_menu_is_open.load(std::memory_order_relaxed)) {
        const HandState& right = both[pointer_hand()];
        std::optional<xr::PanelPlacement> target;
        if (!wrist || wrist_hand_tracked) {
            target = place;
        } else if (both[wrist_hand()].tracked) {
            target = in_local(place, both[wrist_hand()].grip);
        }
        g_ui_target = target ? std::optional<PanelTarget>{PanelTarget{*target, height_m}} : std::nullopt;
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
        const auto aimed = right.aim_valid && target ? panel_uv(right.aim, *target, height_m) : std::nullopt;
        if (g_laser_through_transparent) {
            probe_alpha(ui, ui_desc, aimed);
        }
        // Over a transparent part the laser goes on into the world (no cursor on the panel, the beam isn't stopped). Not
        // while a game window is open (game menu, inventory, map, trade...): the window is what you point at, and the
        // game menu's text on a faint background read as see-through, so its clicks went to the (paused) world
        // (2026-10-26: "clicks didn't register on the menu").
        const bool window_open = gameui::window_open();
        g_ui_laser_through = g_laser_through_transparent && !g_alpha.unsupported && aimed && !touch && !g_alpha.opaque &&
                             !window_open;
        if (g_ui_laser_through) {
            ++g_alpha.through;
        }
        auto uv = touch ? touch : g_ui_laser_through ? std::nullopt : aimed;
        // A trigger press on the UI pins the cursor where it was pressed: squeezing the trigger moves the hand a little,
        // and the game took the few pixels the cursor moved with the button held as a drag (portraits picked up instead
        // of selected). A quick press is a clean click; holding past [ui] drag_hold_s and then moving the laser drags.
        {
            static bool was_down = false, pinned = false;
            static std::array<float, 2> pinned_uv{};
            static std::chrono::steady_clock::time_point down_at{};
            const bool down = right.active && (was_down ? right.trigger > 0.4f : right.trigger > 0.6f); // as vrplay
            const auto now_time = std::chrono::steady_clock::now();
            if (down && !was_down && uv && !touch) {
                pinned = true;
                pinned_uv = *uv;
                down_at = now_time;
            }
            // Every press while a game window is open: where it lands (the next run tells a missed panel from a click
            // the game ignored).
            if (down && !was_down && window_open) {
                static std::uint64_t window_clicks = 0;
                if (window_clicks++ < 60) {
                    POINT cursor{};
                    GetCursorPos(&cursor);
                    log::info("ui: trigger with a window open: laser {} (aimed {}, alpha {}), game window {}, cursor at ({}, {})",
                              uv ? std::format("on the panel at ({:.3f}, {:.3f})", (*uv)[0], (*uv)[1]) : std::string("OFF the panel"),
                              aimed ? "yes" : "no", g_alpha.alpha ? std::format("{:.2f}", *g_alpha.alpha) : std::string("-"),
                              g_window && GetForegroundWindow() == g_window ? "in front" : "NOT in front (the cursor isn't moved)",
                              cursor.x, cursor.y);
                }
            }
            if (!down || !uv || touch) {
                pinned = false;
            }
            if (pinned && uv) {
                const float du = (*uv)[0] - pinned_uv[0], dv = (*uv)[1] - pinned_uv[1];
                const bool moved = std::sqrt(du * du + dv * dv) > 0.012f;
                if (std::chrono::duration<float>(now_time - down_at).count() >= g_drag_hold_s && moved) {
                    pinned = false; // a deliberate hold and move: the cursor follows again (dragging)
                } else {
                    uv = pinned_uv;
                }
            }
            was_down = down;
        }
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
    for (std::size_t k = 0; k < g_doll_fill.size(); ++k) {
        for (std::size_t j = 0; j < 4; ++j) {
            overlay.fill[k][j] = g_doll_fill[k][j];
        }
        // The flat picture (the left eye's doll image, copied last frame) masks the fill to its character.
        overlay.fill_mask[k] = g_doll_slots[k].panel[0] ? g_doll_slots[k].blitter[0].source_view() : nullptr;
    }
    // The right stick while the laser is on the UI: no turning; on a 3D character it turns and zooms it.
    std::uintptr_t doll_under_laser = 0;
    if (g_ui_cursor_on_panel && !g_ui_laser_through) {
        for (std::size_t k = 0; k < g_doll_fill.size(); ++k) {
            const auto& f = g_doll_fill[k];
            if (f[2] > f[0] && overlay.cursor_u >= f[0] && overlay.cursor_u <= f[2] && overlay.cursor_v >= f[1] &&
                overlay.cursor_v <= f[3]) {
                doll_under_laser = g_doll_slots[k].id;
            }
        }
    }
    vrplay::set_stick_capture(doll_under_laser ? 2 : g_ui_cursor_on_panel && !g_ui_laser_through ? 1 : 0);
    if (doll_under_laser) {
        const auto stick = vrplay::captured_stick();
        static auto last = std::chrono::steady_clock::now();
        const auto now_time = std::chrono::steady_clock::now();
        const float dt = std::min(0.1f, std::chrono::duration<float>(now_time - last).count());
        last = now_time;
        const float turn = std::abs(stick[0]) > 0.15f ? stick[0] * 2.5f * dt : 0.0f;    // up to 2.5 rad/s
        const float zoom = std::abs(stick[1]) > 0.15f ? std::exp(stick[1] * 1.2f * dt) : 1.0f; // up: closer
        if (turn != 0.0f || zoom != 1.0f) {
            dolls::adjust(doll_under_laser, turn, zoom);
        }
    }
    if (frame.state.shouldRender) {
        const xr::EyeImage image = g_session->acquire_panel();
        g_ui_blitter.draw(ui, {}, image.view, g_ui_panel_width, g_ui_panel_height, g_ui_linearize, overlay);
        g_session->release_panel();
    }
    snapshot_ui(ui, ui_desc, !g_ui_shown);
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
vrmenu::Renderer* g_menu_renderer = nullptr; // never deleted: joining its thread at process exit could hang
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
    g_menu_renderer = new vrmenu::Renderer();
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
    if (!open) {
        g_menu_target.reset();
    }
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
    if (g_first_run.load(std::memory_order_relaxed) && vrplay::active() && !g_menu_open && g_menu_enabled && !g_menu_failed) {
        g_first_run = false;
        if (!g_menu_renderer) {
            g_menu_failed = !create_menu();
        }
        if (!g_menu_failed) {
            g_menu.show_guide();
            g_menu.show_lesson(0);
            set_menu_open(true);
            bridge::post("set=menu.welcomed true\n"); // once
            log::info("menu: first VR start: opening the VR menu on the Guide");
        }
    }
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
    g_menu_target = PanelTarget{*g_menu_place, height_m};
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
    const bool trigger = (right.active || right.hand_tracked) && right.trigger > (g_menu_trigger ? 0.4f : 0.6f);
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
    // Controller navigation (no laser needed): either stick up / down moves the highlight, A activates, B closes.
    const HandState other = hands()[wrist_hand()];
    static bool was_a = false, was_b = false;
    const bool a_down = (right.active && right.primary) || (other.active && other.primary);
    const bool b_down = (right.active && right.secondary) || (other.active && other.secondary);
    vrmenu::Menu::Nav nav;
    nav.stick_y = right.active && std::abs(right.stick_y) > std::abs(other.active ? other.stick_y : 0.0f)
                      ? right.stick_y
                      : (other.active ? other.stick_y : 0.0f);
    nav.accept = a_down && !was_a;
    nav.back = b_down && !was_b;
    was_a = a_down;
    was_b = b_down;
    const float stick_x = right.active && std::abs(right.stick_x) >= std::abs(other.active ? other.stick_x : 0.0f)
                              ? right.stick_x
                              : (other.active ? other.stick_x : 0.0f);
    const vrmenu::Actions actions = g_menu.update(pointer, click, stick_x,
                                                  {vrplay::third_person() || camera::chase(), g_ui_wrist.load(std::memory_order_relaxed), profile},
                                                  nav);
    if (click || nav.accept || nav.back) {
        ++g_menu_clicks;
        log::info("menu: {} on '{}'{}{}", click ? "laser click" : nav.accept ? "A" : "B", g_menu.hovered_label(),
                  pointer ? std::format(" (laser at {:.2f}, {:.2f})", (*pointer)[0], (*pointer)[1]) : std::string(" (no laser hit)"),
                  actions.settings.empty() ? std::string() : std::format(": {} = {}", actions.settings[0].first, actions.settings[0].second));
    }
    // Where the laser is, now and then, so a log shows whether it reaches the menu at all.
    if (static std::uint64_t frames = 0; ++frames % 180 == 1) {
        log::info("menu: open, {} hand aim {}, laser {}, trigger {:.2f}, sticks ({:.2f}, {:.2f})", pointer_hand() == 0 ? "left" : "right",
                  right.aim_valid ? "tracked" : "not tracked",
                  pointer ? std::format("on the menu at ({:.2f}, {:.2f})", (*pointer)[0], (*pointer)[1]) : std::string("off the menu"),
                  right.trigger, stick_x, nav.stick_y);
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
    if (actions.unlock_travel) {
        bridge::push_request("unlock_travel");
        log::info("menu: testing: unlock every waypoint and the map (Lua: Server/Testing.lua)");
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
    const bool menu_open = g_menu_is_open.load(std::memory_order_relaxed);
    if (style <= 0 || !vrplay::active() || vrplay::third_person()) {
        return overlay;
    }
    const HandState hand = hands()[pointer_hand()];
    if (!hand.aim_valid) {
        return overlay;
    }
    const vr::Vec3 dir = vr::rotate(hand.aim.orientation, {0.0f, 0.0f, -1.0f});
    const float scale = std::max(camera::world_scale(), 0.01f);
    // The world hit (or the range), unless a panel is nearer along the ray: the panels are drawn over the eye images by
    // the compositor, so a beam that went on past one would vanish behind it. With the menu open only the menu counts.
    const auto world_hit = menu_open ? std::nullopt : rayprobe::beam_distance();
    float length = world_hit ? *world_hit / scale : (menu_open ? 2.0f : g_beam_range.load(std::memory_order_relaxed) / scale);
    bool hit = world_hit.has_value();
    for (const auto& target : {menu_open ? g_menu_target : g_ui_laser_through ? std::optional<PanelTarget>{} : g_ui_target}) {
        if (const auto at = target ? panel_hit(hand.aim, target->place, target->height_m) : std::nullopt;
            at && (*at)[2] < length) {
            length = (*at)[2];
            hit = true;
            ++g_beam_on_panel;
        }
    }
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
    overlay.beam_end = hit ? 0.012f : 0.0f;
    // Gloves (hold.cpp): an item the hand can pull lights the laser up (cyan), a locked-on one turns it amber, with a
    // bigger dot where it ends.
    if (!menu_open) {
        const hold::Aim aim = hold::aim_state(pointer_hand());
        if (aim == hold::Aim::Highlight || aim == hold::Aim::Locked) {
            const bool locked = aim == hold::Aim::Locked;
            overlay.beam_rgba[0] = locked ? 1.0f : 0.25f;
            overlay.beam_rgba[1] = locked ? 0.55f : 0.9f;
            overlay.beam_rgba[2] = locked ? 0.1f : 1.0f;
            overlay.beam_rgba[3] = std::max(overlay.beam_rgba[3], 0.85f);
            overlay.beam_end = locked ? 0.024f : 0.018f;
        }
    }
    ++g_beam_frames;
    return overlay;
}

// Just before the game's Present.
// The 3D UI characters: each shown doll's two eye images as one-eye quads exactly over its picture on the UI panel
// (the game's flat picture is hidden there), and where the eyes are relative to that window, for its eye cameras.
void update_dolls(const xr::Frame& frame) {
    std::vector<dolls::Shown> list;
    const bool usable = dolls::enabled() && g_ui_target && g_ui_shown && frame.views.size() == 2 && g_ui_texture_width &&
                        g_ui_texture_height && !g_menu_is_open.load(std::memory_order_relaxed);
    if (usable) {
        list = dolls::shown();
    }
    std::array<const dolls::Shown*, 2> assigned{};
    for (const dolls::Shown& d : list) {
        std::size_t k = 2;
        for (std::size_t i = 0; i < 2; ++i) {
            if (g_doll_slots[i].id == d.id) {
                k = i;
            }
        }
        for (std::size_t i = 0; i < 2 && k == 2; ++i) {
            if (!g_doll_slots[i].id && !assigned[i]) {
                k = i;
            }
        }
        if (k < 2 && !assigned[k]) {
            assigned[k] = &d;
        }
    }
    const PanelTarget target = usable ? *g_ui_target : PanelTarget{};
    const vr::Quat q{target.place.pose.orientation.x, target.place.pose.orientation.y, target.place.pose.orientation.z,
                     target.place.pose.orientation.w};
    const vr::Vec3 centre{target.place.pose.position.x, target.place.pose.position.y, target.place.pose.position.z};
    const vr::Vec3 right = vr::rotate(q, {1.0f, 0.0f, 0.0f}), up = vr::rotate(q, {0.0f, 1.0f, 0.0f}),
                   normal = vr::rotate(q, {0.0f, 0.0f, 1.0f});
    const vr::Quat unrotate{-q[0], -q[1], -q[2], q[3]};
    for (std::size_t k = 0; k < 2; ++k) {
        DollSlot& slot = g_doll_slots[k];
        const dolls::Shown* d = assigned[k];
        if (!d) {
            hide_doll_slot(slot, k);
            g_doll_fill[k] = {};
            continue;
        }
        if (slot.id && slot.id != d->id) {
            dolls::set_window(slot.id, std::nullopt, 0.0f);
        }
        slot.id = d->id;
        // The window: the picture's rectangle on the panel, a few millimetres in front of it.
        const float u0 = d->rect[0] / static_cast<float>(g_ui_texture_width), u1 = d->rect[2] / static_cast<float>(g_ui_texture_width);
        const float v0 = d->rect[1] / static_cast<float>(g_ui_texture_height), v1 = d->rect[3] / static_cast<float>(g_ui_texture_height);
        g_doll_fill[k] = {u0, v0, u1, v1};
        const float width_m = (u1 - u0) * target.place.width_m;
        const float du = (u0 + u1) * 0.5f - 0.5f, dv = 0.5f - (v0 + v1) * 0.5f;
        const vr::Vec3 c{centre[0] + right[0] * du * target.place.width_m + up[0] * dv * target.height_m + normal[0] * 0.004f,
                         centre[1] + right[1] * du * target.place.width_m + up[1] * dv * target.height_m + normal[1] * 0.004f,
                         centre[2] + right[2] * du * target.place.width_m + up[2] * dv * target.height_m + normal[2] * 0.004f};
        std::array<vr::Vec3, 2> eyes{};
        for (std::size_t e = 0; e < 2; ++e) {
            const auto& p = frame.views[e].pose.position;
            eyes[e] = vr::rotate(unrotate, {p.x - c[0], p.y - c[1], p.z - c[2]});
        }
        dolls::set_window(d->id, eyes, width_m);
        for (std::size_t e = 0; e < 2; ++e) {
            const std::size_t index = 2 + 2 * k + e;
            D3D11_TEXTURE2D_DESC desc{};
            d->texture[e]->GetDesc(&desc);
            if (desc.Width != slot.width[e] || desc.Height != slot.height[e] || desc.Format != slot.format[e]) {
                std::string error;
                g_session->set_panel(std::nullopt, index);
                if (slot.panel[e]) {
                    g_session->destroy_panel(index);
                    slot.panel[e] = false;
                }
                if (!slot.blitter[e].create(render::device(), desc.Width, desc.Height, desc.Format, error) ||
                    !g_session->create_panel(static_cast<int32_t>(desc.Width), static_cast<int32_t>(desc.Height), error, index)) {
                    log::warn("dolls: can't show a {}x{} doll image (format {}): {}", desc.Width, desc.Height,
                              static_cast<int>(desc.Format), error);
                    slot.width[e] = slot.height[e] = 0;
                    continue;
                }
                slot.panel[e] = true;
                slot.width[e] = desc.Width;
                slot.height[e] = desc.Height;
                slot.format[e] = desc.Format;
                log::info("dolls: showing doll 0x{:X}'s {} eye ({}x{}) on panel slot {}", d->id, e ? "right" : "left",
                          desc.Width, desc.Height, index);
            }
            if (frame.state.shouldRender) {
                blit::Blitter::Overlay overlay;
                overlay.key_black = true;
                const xr::EyeImage image = g_session->acquire_panel(index);
                slot.blitter[e].draw(d->texture[e], {}, image.view, slot.width[e], slot.height[e],
                                     blit::is_srgb(g_session->format()) && !blit::is_srgb(desc.Format), overlay);
                g_session->release_panel(index);
            }
            xr::PanelPlacement place;
            place.anchor = xr::PanelPlacement::Anchor::Local;
            place.pose.orientation = target.place.pose.orientation;
            place.pose.position = {c[0], c[1], c[2]};
            place.width_m = width_m;
            place.eye = e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
            g_session->set_panel(place, index);
        }
        if (g_doll_frames++ % 900 == 0) {
            log::info("dolls: doll 0x{:X} shown on the panel at uv ({:.3f}, {:.3f})-({:.3f}, {:.3f}), {:.3f} m wide; eyes "
                      "({:.3f}, {:.3f}, {:.3f}) / ({:.3f}, {:.3f}, {:.3f}) m from it",
                      d->id, u0, v0, u1, v1, width_m, eyes[0][0], eyes[0][1], eyes[0][2], eyes[1][0], eyes[1][1], eyes[1][2]);
        }
    }
    for (const dolls::Shown& d : list) {
        for (ID3D11Texture2D* t : d.texture) {
            if (t) {
                t->Release();
            }
        }
    }
}

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
    watchdog::mark("headset: xrPollEvent");
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
    watchdog::mark("headset: begin_frame (xrWaitFrame / xrBeginFrame in the VR runtime)");
    if (!g_session->running() || !g_session->begin_frame(frame)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        publish_status(false);
        return;
    }
    // Frame pacing: how long the game's render thread waited here for the runtime (presenting thread, so no races).
    if (static std::uint64_t paced = 0; ++paced == 60 || paced % 1800 == 0) {
        log::info("xr: pacing: {}", g_session->pacing_report());
    }
    publish_status(true);
    follow_back_buffer(swapchain);
    paperdoll::on_present();
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
    watchdog::mark("headset: controller input (xrSyncActions)");
    update_input(frame);
    {
        static XrTime last = 0;
        const double dt = last ? static_cast<double>(frame.state.predictedDisplayTime - last) * 1e-9 : 0.0;
        last = frame.state.predictedDisplayTime;
        g_frame_vignette = comfort_vignette_now(std::clamp(dt, 0.0, 0.1));
        g_beam_time += static_cast<float>(std::clamp(dt, 0.0, 0.1));
    }
    watchdog::mark("headset: VR menu");
    update_menu(frame);
    watchdog::mark("headset: UI panel");
    update_ui_panel(swapchain, frame);
    watchdog::mark("headset: 3D UI characters");
    update_dolls(frame);
    g_mirror_ready = false;
    watchdog::mark("headset: submit (eye copies, xrEndFrame in the VR runtime)");
    if (g_mode == Mode::Screen) {
        submit_screen(swapchain, frame);
    } else {
        submit_tracked(swapchain, render_frame, frame);
    }
    watchdog::mark("headset: desktop view");
    show_left_eye_on_desktop(swapchain);
    watchdog::mark("headset: done");
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
    g_desktop_left_eye = text::to_lower(config.get("xr", "desktop_view", "left_eye")) != "both";
    g_async_wait = config.get_bool("xr", "async_wait", false);
    g_head_prediction_s = std::clamp(static_cast<double>(config.get_int("xr", "head_prediction_ms", 0)), 0.0, 60.0) / 1000.0;
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
    g_ui_wrist_width = ui_number("wrist_width", 0.8f, 0.1f, 2.0f);
    apply_placement(text::to_lower(config.get("ui", "placement", "hidden")));
    g_ui_dialogue_front.store(config.get_bool("ui", "dialogue_in_front", true), std::memory_order_relaxed);
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

void set_root(const std::filesystem::path& root) {
    g_root_dir = root;
}

void set_in_combat(bool on) {
    if (g_ui_combat.exchange(on, std::memory_order_relaxed) != on) {
        if (on && g_ui_on_demand.load(std::memory_order_relaxed)) {
            g_ui_place_request.store(true, std::memory_order_relaxed);
        }
        log::info("ui: combat {}{}", on ? "started" : "ended",
                  g_ui_on_demand.load(std::memory_order_relaxed) ? (on ? ": the game interface comes up in front of you"
                                                                       : ": the game interface hides again unless shown")
                                                                 : "");
    }
}

void set_first_run(bool first) {
    g_first_run.store(first, std::memory_order_relaxed);
}

void set_dialogue(bool on) {
    if (g_ui_dialogue.exchange(on, std::memory_order_relaxed) == on) {
        return;
    }
    const bool front = g_ui_dialogue_front.load(std::memory_order_relaxed);
    if (on && front) {
        g_ui_place_request.store(true, std::memory_order_relaxed); // in front of where the head is now
    }
    log::info("ui: conversation {}{}", on ? "on" : "off",
              !front ? "" : on ? ": the game interface comes in front of you" : ": the game interface goes back where it was");
}

void configure_ui(const Config& config) {
    const bool left_eye = text::to_lower(config.get("xr", "desktop_view", "left_eye")) != "both";
    if (g_desktop_left_eye.exchange(left_eye) != left_eye) {
        log::info("xr: the desktop window shows {}", left_eye ? "the left eye" : "both eyes side by side");
    }
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
    const auto ui_value = [&](const char* key, float fallback, float lo, float hi) {
        const std::string text = config.get("ui", key);
        return text.empty() ? fallback : std::clamp(std::strtof(text.c_str(), nullptr), lo, hi);
    };
    apply_placement(text::to_lower(config.get("ui", "placement", "hidden")));
    g_ui_wrist_out = ui_value("wrist_out", 0.045f, -0.2f, 0.3f);
    g_laser_through_transparent = config.get_bool("ui", "laser_through_transparent", true);
    g_drag_hold_s = ui_value("drag_hold_s", 0.35f, 0.0f, 2.0f);
    g_snapshots = config.get_bool("ui", "snapshots", true);
    g_ui_wrist_along = ui_value("wrist_along", 0.2f, -0.3f, 0.6f);
    const std::string v = config.get("ui", "wrist_width");
    const float width = v.empty() ? 0.8f : std::clamp(std::strtof(v.c_str(), nullptr), 0.1f, 2.0f);
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
    log::info("pointer: aiming beam drawn in {} eye copies ({} stopped on a panel); last hit {}", g_beam_frames, g_beam_on_panel,
              rayprobe::beam_distance() ? std::format("{:.2f} m", *rayprobe::beam_distance()) : std::string("none"));
    log::info("xr input: hand tracking {}; tracked-hand clicks left {} right {}, grabs left {} right {}",
              g_hand_tracking.load() ? "on" : "off", g_hand_clicks[0], g_hand_clicks[1],
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
