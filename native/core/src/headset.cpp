#include "bg3vr/headset.hpp"

#include "bg3vr/camera.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/win_util.hpp"
#include "bg3vr/xr.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
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

// Everything below runs on the game's presenting thread, except install(), set_lag() and report().
bool g_failed = false;
bool g_ready = false;
xr::System g_system;
std::unique_ptr<xr::Session> g_session;
UINT g_image_width = 0;
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
int g_searches = 0;
int g_offset_misses = 0;
std::atomic<int> g_measured_lag{-1};

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

camera::HeadView head_view(const XrView& view, int eye) {
    camera::HeadView head;
    head.pose.position = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
    head.pose.orientation = {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
                             view.pose.orientation.w};
    head.fov = {view.fov.angleLeft * kDegrees, view.fov.angleRight * kDegrees, view.fov.angleUp * kDegrees,
                view.fov.angleDown * kDegrees};
    head.eye = eye;
    return head;
}

XrView xr_view(const camera::HeadView& head) {
    XrView view{XR_TYPE_VIEW};
    view.pose.position = {head.pose.position[0], head.pose.position[1], head.pose.position[2]};
    view.pose.orientation = {head.pose.orientation[0], head.pose.orientation[1], head.pose.orientation[2],
                             head.pose.orientation[3]};
    view.fov = {head.fov.left_deg / kDegrees, head.fov.right_deg / kDegrees, head.fov.up_deg / kDegrees,
                head.fov.down_deg / kDegrees};
    return view;
}

// Mono: one view between the eyes that covers both eyes' fields of view.
camera::HeadView centre_view(const std::vector<XrView>& views) {
    XrView centre = views[0];
    centre.pose.position = {(views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
                            (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
                            (views[0].pose.position.z + views[1].pose.position.z) * 0.5f};
    centre.fov.angleLeft = (std::min)(views[0].fov.angleLeft, views[1].fov.angleLeft);
    centre.fov.angleRight = (std::max)(views[0].fov.angleRight, views[1].fov.angleRight);
    centre.fov.angleUp = (std::max)(views[0].fov.angleUp, views[1].fov.angleUp);
    centre.fov.angleDown = (std::min)(views[0].fov.angleDown, views[1].fov.angleDown);
    return head_view(centre, 0);
}

bool matrix_matches(const float* candidate, const camera::Mat4& expected) {
    for (std::size_t i = 0; i < 16; ++i) {
        if (std::abs(candidate[i] - expected[i]) > 1e-3f * (std::max)(1.0f, std::abs(expected[i]))) {
            return false;
        }
    }
    return true;
}

// Which applied view the RenderFrame being presented was built from: look for one of their view
// matrices inside it (at the offset found earlier, or by searching now and then).
std::optional<std::size_t> match_in_frame(std::uintptr_t render_frame,
                                          const std::array<camera::AppliedView, camera::kAppliedHistory>& applied,
                                          std::size_t count) {
    if (!render_frame || count == 0) {
        return std::nullopt;
    }
    if (g_frame_offset) {
        float matrix[16];
        if (mem::try_copy(matrix, render_frame + *g_frame_offset, sizeof(matrix))) {
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
    std::size_t readable = kSearchBytes;
    while (readable >= 0x1000 && !mem::try_copy(buffer.data(), render_frame, readable)) {
        readable /= 2;
    }
    if (readable < 0x1000) {
        return std::nullopt;
    }
    const std::size_t floats = readable / sizeof(float);
    for (std::size_t at = 0; at + 16 <= floats; ++at) {
        for (std::size_t i = 0; i < count; ++i) {
            if (matrix_matches(&buffer[at], applied[i].view_matrix)) {
                g_frame_offset = at * sizeof(float);
                log::info("xr: found the rendered view matrix at RenderFrame+0x{:X} (frame 0x{:X}); it was applied "
                          "{} camera update(s) before this Present",
                          *g_frame_offset, render_frame, i);
                return i;
            }
        }
    }
    if (g_searches >= kSearchAttempts) {
        log::warn("xr: no applied view matrix found in the first 0x{:X} bytes of the RenderFrame; assuming a lag of 1 "
                  "(adjust with !vr_xr_lag)",
                  readable);
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
    g_session = std::make_unique<xr::Session>();
    if (!g_session->create(g_system, device, error, desc.BufferDesc.Format, width, height) ||
        !g_session->create_quad(width, height, error)) {
        log::error("xr: {}", error);
        g_session.reset();
        g_failed = true;
        return;
    }
    if (!copy_compatible(g_session->format(), desc.BufferDesc.Format)) {
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
    log::info("xr: session open on '{}', mode {}: images {}x{} format {} (game back buffer format {}); screen {:.1f} m "
              "wide at {:.1f} m",
              adapter, mode_name(g_mode), width, height, static_cast<int>(g_session->format()),
              static_cast<int>(desc.BufferDesc.Format), g_screen_width_m, g_screen_distance_m);
    if (g_mode != Mode::Screen) {
        log::info("xr: head tracking starts when a pose is held (!vr_cam_set here); until then, the screen");
    }
    g_ready = true;
}

void copy_back_buffer(IDXGISwapChain* swapchain, ID3D11Texture2D* target) {
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) {
        return;
    }
    // The window can be resized after the images were made; copy the overlapping part.
    D3D11_TEXTURE2D_DESC source{};
    back_buffer->GetDesc(&source);
    const D3D11_BOX box{0, 0, 0, (std::min)(source.Width, g_image_width), (std::min)(source.Height, g_image_height), 1};
    ID3D11DeviceContext* context = nullptr;
    render::device()->GetImmediateContext(&context);
    context->CopySubresourceRegion(target, 0, 0, 0, 0, back_buffer, 0, &box);
    context->Release();
    back_buffer->Release();
}

void submit_screen(IDXGISwapChain* swapchain, const xr::Frame& frame) {
    if (frame.state.shouldRender) {
        const xr::EyeImage image = g_session->acquire_quad();
        copy_back_buffer(swapchain, image.texture);
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
        if (rendered && frame.state.shouldRender) {
            const int first = g_mode == Mode::Mono ? 0 : rendered->head.eye;
            const int last = g_mode == Mode::Mono ? 1 : rendered->head.eye;
            for (int eye = first; eye <= last; ++eye) {
                const xr::EyeImage image = g_session->acquire(static_cast<std::size_t>(eye));
                copy_back_buffer(swapchain, image.texture);
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
        if (g_mode == Mode::Mono) {
            camera::set_head(centre_view(views));
        } else {
            camera::set_head(head_view(views[g_next_eye], g_next_eye));
            g_next_eye ^= 1;
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
        g_session.reset();
        g_failed = true;
        return;
    }
    xr::Frame frame;
    if (!g_session->running() || !g_session->begin_frame(frame)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
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
    const std::string lag = text::to_lower(config.get("xr", "lag", "auto"));
    if (lag != "auto") {
        set_lag(static_cast<int>(std::strtol(lag.c_str(), nullptr, 10)));
    }
    render::set_present_callback(&on_present);
    log::info("xr: enabled, mode {}; the session opens on the first presented frame", mode_name(g_mode));
}

void set_lag(int frames) {
    g_lag.store(std::clamp(frames, -1, static_cast<int>(camera::kAppliedHistory) - 1), std::memory_order_relaxed);
    if (frames < 0) {
        log::info("xr: lag auto (find the rendered view in the RenderFrame)");
    } else {
        log::info("xr: lag {} camera update(s) between a frame's pose and its Present", frames);
    }
}

void report() {
    log::info("xr: {} frames submitted ({} as the screen), {} skipped (session not running); rendered view "
              "matched {} / unmatched {} / assumed {}; measured lag {}",
              g_submitted.load(std::memory_order_relaxed), g_screen_frames.load(std::memory_order_relaxed),
              g_skipped.load(std::memory_order_relaxed), g_matched.load(std::memory_order_relaxed),
              g_unmatched.load(std::memory_order_relaxed), g_guessed.load(std::memory_order_relaxed),
              g_measured_lag.load(std::memory_order_relaxed));
}

} // namespace bg3vr::headset
