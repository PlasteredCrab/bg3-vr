#include "bg3vr/headset.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/win_util.hpp"
#include "bg3vr/xr.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace bg3vr::headset {

namespace {

// Everything below runs on the game's presenting thread, except install() and report().
bool g_failed = false;
bool g_ready = false;
xr::System g_system;
std::unique_ptr<xr::Session> g_session;
float g_screen_width_m = 3.0f;
float g_screen_distance_m = 2.5f;
std::atomic<std::uint64_t> g_submitted{0};
std::atomic<std::uint64_t> g_skipped{0};
UINT g_quad_width = 0;
UINT g_quad_height = 0;

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

    std::string adapter;
    if (!same_adapter(device, g_system.adapter, adapter)) {
        log::error("xr: the game renders on '{}', but the headset needs a different GPU; set the game and SteamVR "
                   "to the same GPU",
                   adapter);
        g_failed = true;
        return;
    }

    g_session = std::make_unique<xr::Session>();
    if (!g_session->create(g_system, device, error, desc.BufferDesc.Format) ||
        !g_session->create_quad(static_cast<int32_t>(desc.BufferDesc.Width),
                                static_cast<int32_t>(desc.BufferDesc.Height), error)) {
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
    g_quad_width = desc.BufferDesc.Width;
    g_quad_height = desc.BufferDesc.Height;
    log::info("xr: session open on '{}': screen {}x{} format {} (game back buffer format {}), {:.1f} m wide at "
              "{:.1f} m",
              adapter, desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<int>(g_session->format()),
              static_cast<int>(desc.BufferDesc.Format), g_screen_width_m, g_screen_distance_m);
    g_ready = true;
}

// Just before the game's Present: copy the finished frame onto the virtual screen and submit it.
void on_present(IDXGISwapChain* swapchain) {
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
        g_session.reset();
        g_failed = true;
        return;
    }
    xr::Frame frame;
    if (!g_session->running() || !g_session->begin_frame(frame)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (frame.state.shouldRender) {
        ID3D11Texture2D* back_buffer = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) {
            const xr::EyeImage image = g_session->acquire_quad();
            ID3D11DeviceContext* context = nullptr;
            render::device()->GetImmediateContext(&context);
            // The window can be resized after the quad was made; copy the overlapping part.
            D3D11_TEXTURE2D_DESC source{};
            back_buffer->GetDesc(&source);
            const D3D11_BOX box{0, 0, 0, (std::min)(source.Width, g_quad_width),
                                (std::min)(source.Height, g_quad_height), 1};
            context->CopySubresourceRegion(image.texture, 0, 0, 0, 0, back_buffer, 0, &box);
            context->Release();
            g_session->release_quad();
            back_buffer->Release();
        }
    }
    g_session->end_frame_quad(frame, g_screen_width_m, g_screen_distance_m);
    g_submitted.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void install(const Config& config) {
    if (!config.get_bool("xr", "enabled", false)) {
        log::info("xr: disabled ([xr] enabled = false in bg3vr.ini)");
        return;
    }
    const std::string width = config.get("xr", "screen_width_m");
    const std::string distance = config.get("xr", "screen_distance_m");
    if (!width.empty()) {
        g_screen_width_m = std::strtof(width.c_str(), nullptr);
    }
    if (!distance.empty()) {
        g_screen_distance_m = std::strtof(distance.c_str(), nullptr);
    }
    render::set_present_callback(&on_present);
    log::info("xr: enabled; the session opens on the first presented frame");
}

void report() {
    log::info("xr: {} frames submitted, {} skipped (session not running)", g_submitted.load(std::memory_order_relaxed),
              g_skipped.load(std::memory_order_relaxed));
}

} // namespace bg3vr::headset
