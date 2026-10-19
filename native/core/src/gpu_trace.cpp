#include "bg3vr/gpu_trace.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/render.hpp"

#include <d3d11.h>
#include <safetyhook.hpp>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace bg3vr::gpu_trace {

namespace {

// ID3D11DeviceContext vtable slots.
constexpr std::size_t kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21,
                      kOMSetRenderTargets = 33, kOMSetRenderTargetsAndUAVs = 34, kDispatch = 41, kRSSetViewports = 44,
                      kCopySubresourceRegion = 46, kCopyResource = 47, kClearRenderTargetView = 50;

std::mutex g_mutex; // guards everything below; the hooks run on whichever thread uses the context
std::atomic<bool> g_armed{false};
std::uint64_t g_start_frame = 0;
int g_frames = 0;
std::uint64_t g_current_frame = 0;

struct Target {
    std::uintptr_t resource = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT count = 0; // render targets bound
};
Target g_target;
D3D11_VIEWPORT g_viewport{};

// The current run: same target and viewport.
struct Run {
    Target target;
    D3D11_VIEWPORT viewport{};
    DWORD thread = 0;
    std::uint32_t draws = 0, clears = 0, dispatches = 0;
};
Run g_run;
bool g_run_open = false;
std::vector<std::string> g_lines; // this frame's trace

std::string describe_texture(ID3D11Resource* resource) {
    if (!resource) {
        return "none";
    }
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        texture->Release();
        return std::format("tex 0x{:X} {}x{} fmt {}", reinterpret_cast<std::uintptr_t>(resource), desc.Width, desc.Height,
                           static_cast<int>(desc.Format));
    }
    return std::format("res 0x{:X}", reinterpret_cast<std::uintptr_t>(resource));
}

void close_run() {
    if (g_run_open && (g_run.draws || g_run.clears || g_run.dispatches)) {
        g_lines.push_back(std::format("  t{} rt 0x{:X} {}x{} fmt {} (x{}) vp ({:.0f},{:.0f} {:.0f}x{:.0f}) draws {} clears {} "
                                      "dispatches {}",
                                      g_run.thread, g_run.target.resource, g_run.target.width, g_run.target.height,
                                      static_cast<int>(g_run.target.format), g_run.target.count, g_run.viewport.TopLeftX,
                                      g_run.viewport.TopLeftY, g_run.viewport.Width, g_run.viewport.Height, g_run.draws,
                                      g_run.clears, g_run.dispatches));
    }
    g_run_open = false;
}

void flush_frame() {
    close_run();
    if (!g_lines.empty()) {
        log::info("gpu: frame {} ({} lines):", g_current_frame - g_start_frame, g_lines.size());
        for (const std::string& line : g_lines) {
            log::info("gpu: {}", line);
        }
    }
    g_lines.clear();
}

// Under g_mutex: is the trace on for this call (and has the frame changed)?
bool active() {
    if (!g_armed.load(std::memory_order_relaxed)) {
        return false;
    }
    const std::uint64_t frame = render::frame_count();
    if (frame != g_current_frame) {
        flush_frame();
        g_current_frame = frame;
        if (frame - g_start_frame > static_cast<std::uint64_t>(g_frames)) {
            g_armed.store(false);
            log::info("gpu: trace done");
            return false;
        }
    }
    return g_lines.size() < 4000;
}

Run& run() {
    if (!g_run_open || g_run.target.resource != g_target.resource || g_run.viewport.TopLeftX != g_viewport.TopLeftX ||
        g_run.viewport.TopLeftY != g_viewport.TopLeftY || g_run.viewport.Width != g_viewport.Width ||
        g_run.viewport.Height != g_viewport.Height || g_run.thread != GetCurrentThreadId()) {
        close_run();
        g_run = {g_target, g_viewport, GetCurrentThreadId()};
        g_run_open = true;
    }
    return g_run;
}

void note_targets(UINT count, ID3D11RenderTargetView* const* views) {
    g_target = {};
    g_target.count = count;
    if (count == 0 || !views || !views[0]) {
        return;
    }
    ID3D11Resource* resource = nullptr;
    views[0]->GetResource(&resource);
    if (!resource) {
        return;
    }
    g_target.resource = reinterpret_cast<std::uintptr_t>(resource);
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        g_target.width = desc.Width;
        g_target.height = desc.Height;
        g_target.format = desc.Format;
        texture->Release();
    }
    resource->Release();
}

safetyhook::InlineHook g_hooks[11];

void STDMETHODCALLTYPE om_set_render_targets(ID3D11DeviceContext* self, UINT count, ID3D11RenderTargetView* const* views,
                                              ID3D11DepthStencilView* depth) {
    {
        std::scoped_lock lock(g_mutex);
        if (active()) {
            note_targets(count, views);
        }
    }
    g_hooks[0].call<void>(self, count, views, depth);
}
void STDMETHODCALLTYPE om_set_render_targets_uav(ID3D11DeviceContext* self, UINT count, ID3D11RenderTargetView* const* views,
                                                 ID3D11DepthStencilView* depth, UINT uav_start, UINT uav_count,
                                                 ID3D11UnorderedAccessView* const* uavs, const UINT* initial) {
    {
        std::scoped_lock lock(g_mutex);
        if (active() && count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
            note_targets(count, views);
        }
    }
    g_hooks[1].call<void>(self, count, views, depth, uav_start, uav_count, uavs, initial);
}
void STDMETHODCALLTYPE rs_set_viewports(ID3D11DeviceContext* self, UINT count, const D3D11_VIEWPORT* viewports) {
    {
        std::scoped_lock lock(g_mutex);
        if (active() && count && viewports) {
            g_viewport = viewports[0];
        }
    }
    g_hooks[2].call<void>(self, count, viewports);
}
void count_draw() {
    std::scoped_lock lock(g_mutex);
    if (active()) {
        ++run().draws;
    }
}
void STDMETHODCALLTYPE draw(ID3D11DeviceContext* self, UINT vertices, UINT start) {
    count_draw();
    g_hooks[3].call<void>(self, vertices, start);
}
void STDMETHODCALLTYPE draw_indexed(ID3D11DeviceContext* self, UINT indices, UINT start, INT base) {
    count_draw();
    g_hooks[4].call<void>(self, indices, start, base);
}
void STDMETHODCALLTYPE draw_instanced(ID3D11DeviceContext* self, UINT vertices, UINT instances, UINT start, UINT start_instance) {
    count_draw();
    g_hooks[5].call<void>(self, vertices, instances, start, start_instance);
}
void STDMETHODCALLTYPE draw_indexed_instanced(ID3D11DeviceContext* self, UINT indices, UINT instances, UINT start, INT base,
                                              UINT start_instance) {
    count_draw();
    g_hooks[6].call<void>(self, indices, instances, start, base, start_instance);
}
void STDMETHODCALLTYPE dispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
    {
        std::scoped_lock lock(g_mutex);
        if (active()) {
            ++run().dispatches;
        }
    }
    g_hooks[7].call<void>(self, x, y, z);
}
void STDMETHODCALLTYPE clear_render_target_view(ID3D11DeviceContext* self, ID3D11RenderTargetView* view, const FLOAT color[4]) {
    {
        std::scoped_lock lock(g_mutex);
        if (active() && view) {
            close_run();
            ID3D11Resource* resource = nullptr;
            view->GetResource(&resource);
            g_lines.push_back(std::format("  t{} clear {} to ({:.2f},{:.2f},{:.2f},{:.2f})", GetCurrentThreadId(),
                                          describe_texture(resource), color[0], color[1], color[2], color[3]));
            if (resource) {
                resource->Release();
            }
        }
    }
    g_hooks[8].call<void>(self, view, color);
}
void STDMETHODCALLTYPE copy_resource(ID3D11DeviceContext* self, ID3D11Resource* destination, ID3D11Resource* source) {
    {
        std::scoped_lock lock(g_mutex);
        if (active()) {
            close_run();
            g_lines.push_back(std::format("  t{} copy {} -> {}", GetCurrentThreadId(), describe_texture(source),
                                          describe_texture(destination)));
        }
    }
    g_hooks[9].call<void>(self, destination, source);
}
void STDMETHODCALLTYPE copy_subresource_region(ID3D11DeviceContext* self, ID3D11Resource* destination, UINT dst_sub, UINT x,
                                               UINT y, UINT z, ID3D11Resource* source, UINT src_sub, const D3D11_BOX* box) {
    {
        std::scoped_lock lock(g_mutex);
        if (active()) {
            close_run();
            g_lines.push_back(std::format("  t{} copy region {} -> {} at ({},{}){}", GetCurrentThreadId(),
                                          describe_texture(source), describe_texture(destination), x, y,
                                          box ? std::format(" box ({},{})-({},{})", box->left, box->top, box->right, box->bottom)
                                              : std::string()));
        }
    }
    g_hooks[10].call<void>(self, destination, dst_sub, x, y, z, source, src_sub, box);
}

bool install() {
    ID3D11Device* device = render::device();
    if (!device) {
        log::warn("gpu: no device yet");
        return false;
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(context);
    const std::pair<std::size_t, void*> targets[] = {
        {kOMSetRenderTargets, reinterpret_cast<void*>(&om_set_render_targets)},
        {kOMSetRenderTargetsAndUAVs, reinterpret_cast<void*>(&om_set_render_targets_uav)},
        {kRSSetViewports, reinterpret_cast<void*>(&rs_set_viewports)},
        {kDraw, reinterpret_cast<void*>(&draw)},
        {kDrawIndexed, reinterpret_cast<void*>(&draw_indexed)},
        {kDrawInstanced, reinterpret_cast<void*>(&draw_instanced)},
        {kDrawIndexedInstanced, reinterpret_cast<void*>(&draw_indexed_instanced)},
        {kDispatch, reinterpret_cast<void*>(&dispatch)},
        {kClearRenderTargetView, reinterpret_cast<void*>(&clear_render_target_view)},
        {kCopyResource, reinterpret_cast<void*>(&copy_resource)},
        {kCopySubresourceRegion, reinterpret_cast<void*>(&copy_subresource_region)},
    };
    bool ok = true;
    for (std::size_t i = 0; i < std::size(targets); ++i) {
        auto hook = safetyhook::InlineHook::create(vtable[targets[i].first], targets[i].second);
        if (!hook) {
            log::error("gpu: hooking context slot {} failed (error type {})", targets[i].first,
                       static_cast<int>(hook.error().type));
            ok = false;
            continue;
        }
        g_hooks[i] = std::move(*hook);
    }
    context->Release();
    log::info("gpu: context functions hooked{}", ok ? "" : " (some failed)");
    return true;
}

std::once_flag g_install_once;
bool g_installed = false;

} // namespace

void arm(int frames) {
    std::call_once(g_install_once, [] { g_installed = install(); });
    if (!g_installed) {
        return;
    }
    std::scoped_lock lock(g_mutex);
    g_lines.clear();
    g_run_open = false;
    g_start_frame = render::frame_count();
    g_current_frame = g_start_frame;
    g_frames = frames;
    g_armed.store(true);
    log::info("gpu: tracing the next {} frame(s)", frames);
}

void marker(const std::string& text) {
    std::scoped_lock lock(g_mutex);
    if (active()) {
        close_run();
        g_lines.push_back(std::format("-- t{} {}", GetCurrentThreadId(), text));
    }
}

} // namespace bg3vr::gpu_trace
