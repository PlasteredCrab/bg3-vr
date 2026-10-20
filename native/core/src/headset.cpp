#include "bg3vr/headset.hpp"

#include "bg3vr/blit.hpp"
#include "bg3vr/camera.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/locomotion.hpp"
#include "bg3vr/rayprobe.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/stereo.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/views.hpp"
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
UINT g_image_width = 0;
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
    if (!device || !load_loader()) {
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
        g_failed = true;
        return;
    }
    log::info("xr: headset {}, {} views, recommended {}x{}", g_system.headset, g_system.views.size(),
              g_system.views.empty() ? 0 : g_system.views[0].recommendedImageRectWidth,
              g_system.views.empty() ? 0 : g_system.views[0].recommendedImageRectHeight);
    if (g_system.views.size() != 2) {
        log::error("xr: expected 2 views (stereo), the runtime has {}", g_system.views.size());
        g_failed = true;
        return;
    }

    std::string adapter;
    if (!same_adapter(device, g_system.adapter, adapter)) {
        log::error("xr: the game renders on '{}', but the headset needs a different GPU; set the game and SteamVR "
                   "to the same GPU",
                   adapter);
        g_failed = true;
        return;
    }

    // Every image the game hands us is a whole back buffer, so the eye images and the screen are
    // made the same size.
    const auto width = static_cast<int32_t>(desc.BufferDesc.Width);
    const auto height = static_cast<int32_t>(desc.BufferDesc.Height);
    std::string blit_error;
    const bool blitting = g_blitter.create(device, desc.BufferDesc.Width, desc.BufferDesc.Height,
                                           desc.BufferDesc.Format, blit_error);
    if (!blitting) {
        log::warn("xr: no shader copy ({}); trying a plain copy", blit_error);
    }
    g_session = std::make_unique<xr::Session>();
    // With the blitter the images can be 8-bit sRGB whatever the back buffer is.
    const DXGI_FORMAT wanted = blitting ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : desc.BufferDesc.Format;
    if (!g_session->create(g_system, device, error, wanted, width, height) ||
        !g_session->create_quad(width, height, error)) {
        log::error("xr: {}", error);
        g_session.reset();
        g_failed = true;
        return;
    }
    g_linearize = blitting && blit::is_srgb(g_session->format()) && !blit::is_srgb(desc.BufferDesc.Format);
    if (!blitting && !copy_compatible(g_session->format(), desc.BufferDesc.Format)) {
        log::error("xr: the runtime offers no swapchain format the game's back buffer (format {}) can be copied into "
                   "(runtime picked {})",
                   static_cast<int>(desc.BufferDesc.Format), static_cast<int>(g_session->format()));
        g_session.reset();
        g_failed = true;
        return;
    }
    g_image_width = desc.BufferDesc.Width;
    g_image_height = desc.BufferDesc.Height;
    g_rendered.assign(2, {XR_TYPE_VIEW});
    if (g_input_wanted) {
        std::string input_error;
        g_input = g_session->create_input(input_error);
        if (g_input) {
            log::info("xr input: controller actions attached (Touch, Index, Vive and simple profiles suggested); "
                      "changes are logged as 'xr input:' lines, poses every {} frames",
                      kHandLogEvery);
        } else {
            log::warn("xr input: no controllers ({}); the headset still works", input_error);
        }
    } else {
        log::info("xr input: off ([xr] input = false)");
    }
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

// `rect` (back buffer pixels; width 0 = all of it) into the whole image.
void copy_back_buffer(IDXGISwapChain* swapchain, const xr::EyeImage& target, const blit::Blitter::Rect& rect = {}) {
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) {
        return;
    }
    if (g_blitter.ready()) {
        D3D11_TEXTURE2D_DESC source{};
        back_buffer->GetDesc(&source);
        if (source.Width == g_image_width && source.Height == g_image_height) {
            g_blitter.draw(back_buffer, rect, target.view, g_image_width, g_image_height, g_linearize);
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
        copy_back_buffer(swapchain, image);
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
                copy_back_buffer(swapchain, image, rect);
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
                copy_back_buffer(swapchain, image);
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

HandState hand_state(const xr::Hand& hand, std::uint64_t sample) {
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
    const std::array<HandState, 2> now{hand_state(raw[0], g_hand_samples), hand_state(raw[1], g_hand_samples)};
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
    if (now[kRightHand].aim_valid) {
        aim = camera::tracked_to_world(now[kRightHand].aim);
    }
    rayprobe::set_hand_aim(aim);
    // The left stick drives locomotion (when it is on); zero while the hand isn't active.
    locomotion::set_stick(now[kLeftHand].active ? now[kLeftHand].stick_x : 0.0f,
                          now[kLeftHand].active ? now[kLeftHand].stick_y : 0.0f);
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
        camera::set_head(std::nullopt);
        render::set_sync_interval_override(-1);
        g_session.reset();
        g_blitter.destroy();
        g_failed = true;
        return;
    }
    xr::Frame frame;
    if (!g_session->running() || !g_session->begin_frame(frame)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    update_input(frame);
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
    const std::string lag = text::to_lower(config.get("xr", "lag", "auto"));
    if (lag != "auto") {
        set_lag(static_cast<int>(std::strtol(lag.c_str(), nullptr, 10)));
    }
    render::set_present_callback(&on_present);
    log::info("xr: enabled, mode {}; the session opens on the first presented frame", mode_name(g_mode));
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
