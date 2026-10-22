#include "bg3vr/gpu_trace.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/render.hpp"

#include <d3d11.h>
#include <safetyhook.hpp>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bg3vr::gpu_trace {

namespace {

// ID3D11DeviceContext vtable slots.
constexpr std::size_t kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21,
                      kOMSetRenderTargets = 33, kOMSetRenderTargetsAndUAVs = 34, kDispatch = 41, kRSSetViewports = 44,
                      kCopySubresourceRegion = 46, kCopyResource = 47, kClearRenderTargetView = 50,
                      kPSSetShaderResources = 8, kCSSetShaderResources = 67, kCSSetUnorderedAccessViews = 68;

safetyhook::InlineHook g_hooks[14];
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
    ID3D11DeviceContext* context = nullptr; // the context that drew it (raw)
    std::string inputs; // shader resources (and UAVs) bound for the run's draws / dispatches
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

// Bound shader inputs: pixel-shader resources for draws, compute-shader resources and UAVs for dispatches.
constexpr UINT kSlots = 8;
std::uintptr_t g_ps_srv[kSlots]{}, g_cs_srv[kSlots]{}, g_cs_uav[kSlots]{};
std::string g_ps_text, g_cs_text;

std::string short_texture(std::uintptr_t resource) {
    if (!resource) {
        return "-";
    }
    auto* r = reinterpret_cast<ID3D11Resource*>(resource);
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        texture->Release();
        return std::format("{:X}:{}x{}f{}", resource & 0xFFFFFFF, desc.Width, desc.Height, static_cast<int>(desc.Format));
    }
    return std::format("{:X}:buf", resource & 0xFFFFFFF);
}

std::string slots_text(const std::uintptr_t* slots) {
    std::string text;
    for (UINT i = 0; i < kSlots; ++i) {
        if (slots[i]) {
            text += std::format(" {}={}", i, short_texture(slots[i]));
        }
    }
    return text;
}

template <typename View>
void note_views(std::uintptr_t* slots, UINT start, UINT count, View* const* views) {
    for (UINT i = 0; i < count && start + i < kSlots; ++i) {
        slots[start + i] = 0;
        if (views && views[i]) {
            ID3D11Resource* resource = nullptr;
            views[i]->GetResource(&resource);
            if (resource) {
                slots[start + i] = reinterpret_cast<std::uintptr_t>(resource);
                resource->Release(); // the view keeps it alive while bound
            }
        }
    }
}

// One pixel of a run's render target, read back right after the run: at 25% across (the left view's half)
// and 75% across (the right view's), to see which pass first produces the split view's white. Copies go
// through the copy hook's trampoline (no re-entry into these hooks while g_mutex is held).
std::map<DXGI_FORMAT, ID3D11Texture2D*> g_staging;

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                  : exponent == 31 ? (mantissa ? NAN : INFINITY)
                                   : std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return sign ? -value : value;
}
float small_float(std::uint32_t bits, int mantissa_bits) { // unsigned 5-bit exponent floats (R11G11B10)
    const std::uint32_t exponent = bits >> mantissa_bits, mantissa = bits & ((1u << mantissa_bits) - 1);
    if (exponent == 0) {
        return std::ldexp(static_cast<float>(mantissa), -14 - mantissa_bits);
    }
    if (exponent == 31) {
        return mantissa ? NAN : INFINITY;
    }
    return std::ldexp(static_cast<float>(mantissa | (1u << mantissa_bits)), static_cast<int>(exponent) - 15 - mantissa_bits);
}

std::string decode_pixel(DXGI_FORMAT format, const std::uint8_t* p) {
    std::uint32_t u32;
    std::memcpy(&u32, p, 4);
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: {
        float v[4];
        std::memcpy(v, p, 16);
        return std::format("({:.3g},{:.3g},{:.3g},{:.3g})", v[0], v[1], v[2], v[3]);
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        std::uint16_t v[4];
        std::memcpy(v, p, 8);
        return std::format("({:.3g},{:.3g},{:.3g},{:.3g})", half_to_float(v[0]), half_to_float(v[1]), half_to_float(v[2]),
                           half_to_float(v[3]));
    }
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return std::format("({:.3g},{:.3g},{:.3g})", small_float(u32 & 0x7FF, 6), small_float((u32 >> 11) & 0x7FF, 6),
                           small_float(u32 >> 22, 5));
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return std::format("({:.3f},{:.3f},{:.3f})", (u32 & 0x3FF) / 1023.0f, ((u32 >> 10) & 0x3FF) / 1023.0f,
                           ((u32 >> 20) & 0x3FF) / 1023.0f);
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return std::format("({},{},{},{})", p[0], p[1], p[2], p[3]);
    case DXGI_FORMAT_R16G16_FLOAT: {
        std::uint16_t v[2];
        std::memcpy(v, p, 4);
        return std::format("({:.3g},{:.3g})", half_to_float(v[0]), half_to_float(v[1]));
    }
    case DXGI_FORMAT_R32_FLOAT: {
        float v;
        std::memcpy(&v, p, 4);
        return std::format("({:.4g})", v);
    }
    case DXGI_FORMAT_R16_FLOAT: {
        std::uint16_t v;
        std::memcpy(&v, p, 2);
        return std::format("({:.3g})", half_to_float(v));
    }
    default:
        return std::format("raw {:08X}", u32);
    }
}

// Under g_mutex, on the thread that drew the run.
std::string sample_target(std::uintptr_t resource) {
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(resource);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Width < 256 || desc.SampleDesc.Count != 1 || (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) ||
        desc.Width > 4096) {
        return {};
    }
    ID3D11Device* device = render::device();
    ID3D11Texture2D*& staging = g_staging[desc.Format];
    if (!staging) {
        D3D11_TEXTURE2D_DESC s{};
        s.Width = s.Height = 1;
        s.MipLevels = s.ArraySize = 1;
        s.Format = desc.Format;
        s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&s, nullptr, &staging))) {
            staging = nullptr;
            return " px n/a";
        }
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    std::string text = " px";
    for (const float at : {0.25f, 0.75f}) {
        const UINT x = static_cast<UINT>(desc.Width * at), y = desc.Height / 2;
        const D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
        g_hooks[10].call<void>(context, static_cast<ID3D11Resource*>(staging), 0u, 0u, 0u, 0u,
                               static_cast<ID3D11Resource*>(texture), 0u, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            text += std::format(" {}={}", at < 0.5f ? "L" : "R",
                                decode_pixel(desc.Format, static_cast<const std::uint8_t*>(mapped.pData)));
            context->Unmap(staging, 0);
        }
    }
    context->Release();
    return text;
}

void close_run() {
    if (g_run_open && (g_run.draws || g_run.clears || g_run.dispatches)) {
        g_lines.push_back(std::format("  t{} rt 0x{:X} {}x{} fmt {} (x{}) vp ({:.0f},{:.0f} {:.0f}x{:.0f}) draws {} clears {} "
                                      "dispatches {} in[{} ]",
                                      g_run.thread, g_run.target.resource, g_run.target.width, g_run.target.height,
                                      static_cast<int>(g_run.target.format), g_run.target.count, g_run.viewport.TopLeftX,
                                      g_run.viewport.TopLeftY, g_run.viewport.Width, g_run.viewport.Height, g_run.draws,
                                      g_run.clears, g_run.dispatches, g_run.inputs));
        if (g_run.target.resource && g_run.draws) {
            // Read back only on the thread that drew it, through the immediate context it drew with: another thread
            // (a later run's draw closes this one) or a deferred context would race the game on the immediate context.
            ID3D11DeviceContext* immediate = nullptr;
            render::device()->GetImmediateContext(&immediate);
            if (immediate) {
                immediate->Release();
            }
            g_lines.back() += GetCurrentThreadId() == g_run.thread && g_run.context == immediate
                                  ? sample_target(g_run.target.resource)
                                  : std::string(" px n/a (other thread or deferred context)");
        }
    }
    if (g_run_open && g_run.target.resource) {
        reinterpret_cast<ID3D11Resource*>(g_run.target.resource)->Release(); // held for the run
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

Run& run(ID3D11DeviceContext* context, const std::string& inputs) {
    if (!g_run_open || g_run.inputs != inputs || g_run.target.resource != g_target.resource || g_run.viewport.TopLeftX != g_viewport.TopLeftX ||
        g_run.viewport.TopLeftY != g_viewport.TopLeftY || g_run.viewport.Width != g_viewport.Width ||
        g_run.viewport.Height != g_viewport.Height || g_run.thread != GetCurrentThreadId() || g_run.context != context) {
        close_run();
        g_run = {g_target, g_viewport, GetCurrentThreadId(), context, inputs};
        if (g_run.target.resource) {
            reinterpret_cast<ID3D11Resource*>(g_run.target.resource)->AddRef(); // still bound here, so alive
        }
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


// UI watch (separate from the trace; its own small lock).
std::atomic<bool> g_watch_ui{false};
std::mutex g_ui_mutex;
std::uintptr_t g_ui_back_buffer = 0;     // under g_ui_mutex
std::uintptr_t g_ui_target = 0;          // current RT0 resource (raw, not held)
std::uintptr_t g_ui_slot1 = 0;           // current PS slot 1 view (raw, not held)
ID3D11Texture2D* g_ui_texture = nullptr; // held
std::uint64_t g_ui_changes = 0;

void ui_note_targets(UINT count, ID3D11RenderTargetView* const* views) {
    std::uintptr_t target = 0;
    if (count && views && views[0]) {
        ID3D11Resource* resource = nullptr;
        views[0]->GetResource(&resource);
        target = reinterpret_cast<std::uintptr_t>(resource);
        if (resource) {
            resource->Release();
        }
    }
    std::scoped_lock lock(g_ui_mutex);
    g_ui_target = target;
}

void ui_note_views(UINT start, UINT count, ID3D11ShaderResourceView* const* views) {
    if (start > 1 || start + count <= 1) {
        return;
    }
    std::scoped_lock lock(g_ui_mutex);
    g_ui_slot1 = views ? reinterpret_cast<std::uintptr_t>(views[1 - start]) : 0;
}

void ui_on_draw() {
    std::scoped_lock lock(g_ui_mutex);
    if (!g_ui_back_buffer || g_ui_target != g_ui_back_buffer || !g_ui_slot1) {
        return;
    }
    ID3D11Resource* resource = nullptr;
    reinterpret_cast<ID3D11ShaderResourceView*>(g_ui_slot1)->GetResource(&resource);
    if (!resource) {
        return;
    }
    ID3D11Texture2D* texture = nullptr;
    resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    resource->Release();
    if (!texture) {
        return;
    }
    if (texture == g_ui_texture) {
        texture->Release();
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Width < 640 || desc.Height < 360) {
        // With the UI kept out of the eye images the composite binds a small placeholder in slot 1 (16x16 seen in
        // game, 2026-10-20): not the UI.
        texture->Release();
        return;
    }
    if (g_ui_texture) {
        g_ui_texture->Release();
    }
    g_ui_texture = texture; // keeps the reference
    ++g_ui_changes;
    log::info("gpu: UI texture (composite PS slot 1) 0x{:X}: {}x{} format {} mips {} bind 0x{:X} ({} change(s))",
              reinterpret_cast<std::uintptr_t>(texture), desc.Width, desc.Height, static_cast<int>(desc.Format),
              desc.MipLevels, desc.BindFlags, g_ui_changes);
}

void STDMETHODCALLTYPE om_set_render_targets(ID3D11DeviceContext* self, UINT count, ID3D11RenderTargetView* const* views,
                                              ID3D11DepthStencilView* depth) {
    if (g_watch_ui.load(std::memory_order_relaxed)) {
        ui_note_targets(count, views);
    }
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
    if (g_watch_ui.load(std::memory_order_relaxed) && count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        ui_note_targets(count, views);
    }
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
void count_draw(ID3D11DeviceContext* self) {
    if (g_watch_ui.load(std::memory_order_relaxed)) {
        ui_on_draw();
    }
    std::scoped_lock lock(g_mutex);
    if (active()) {
        ++run(self, slots_text(g_ps_srv)).draws;
    }
}
void STDMETHODCALLTYPE draw(ID3D11DeviceContext* self, UINT vertices, UINT start) {
    count_draw(self);
    g_hooks[3].call<void>(self, vertices, start);
}
void STDMETHODCALLTYPE draw_indexed(ID3D11DeviceContext* self, UINT indices, UINT start, INT base) {
    count_draw(self);
    g_hooks[4].call<void>(self, indices, start, base);
}
void STDMETHODCALLTYPE draw_instanced(ID3D11DeviceContext* self, UINT vertices, UINT instances, UINT start, UINT start_instance) {
    count_draw(self);
    g_hooks[5].call<void>(self, vertices, instances, start, start_instance);
}
void STDMETHODCALLTYPE draw_indexed_instanced(ID3D11DeviceContext* self, UINT indices, UINT instances, UINT start, INT base,
                                              UINT start_instance) {
    count_draw(self);
    g_hooks[6].call<void>(self, indices, instances, start, base, start_instance);
}
void STDMETHODCALLTYPE dispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
    {
        std::scoped_lock lock(g_mutex);
        if (active()) {
            ++run(self, " cs" + slots_text(g_cs_srv) + " uav" + slots_text(g_cs_uav)).dispatches;
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

void STDMETHODCALLTYPE ps_set_shader_resources(ID3D11DeviceContext* self, UINT start, UINT count,
                                               ID3D11ShaderResourceView* const* views) {
    if (g_watch_ui.load(std::memory_order_relaxed)) {
        ui_note_views(start, count, views);
    }
    {
        std::scoped_lock lock(g_mutex);
        if (active()) {
            note_views(g_ps_srv, start, count, views);
        }
    }
    g_hooks[11].call<void>(self, start, count, views);
}
void STDMETHODCALLTYPE cs_set_shader_resources(ID3D11DeviceContext* self, UINT start, UINT count,
                                               ID3D11ShaderResourceView* const* views) {
    {
        std::scoped_lock lock(g_mutex);
        if (active()) {
            note_views(g_cs_srv, start, count, views);
        }
    }
    g_hooks[12].call<void>(self, start, count, views);
}
void STDMETHODCALLTYPE cs_set_unordered_access_views(ID3D11DeviceContext* self, UINT start, UINT count,
                                                     ID3D11UnorderedAccessView* const* views, const UINT* initial) {
    {
        std::scoped_lock lock(g_mutex);
        if (active()) {
            note_views(g_cs_uav, start, count, views);
        }
    }
    g_hooks[13].call<void>(self, start, count, views, initial);
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
        {kPSSetShaderResources, reinterpret_cast<void*>(&ps_set_shader_resources)},
        {kCSSetShaderResources, reinterpret_cast<void*>(&cs_set_shader_resources)},
        {kCSSetUnorderedAccessViews, reinterpret_cast<void*>(&cs_set_unordered_access_views)},
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

void watch_ui(bool on) {
    if (on) {
        std::call_once(g_install_once, [] { g_installed = install(); });
        if (!g_installed) {
            return;
        }
    }
    if (g_watch_ui.exchange(on) != on) {
        log::info("gpu: UI texture watch {}", on ? "on" : "off");
    }
}

void set_back_buffer(std::uintptr_t resource) {
    std::scoped_lock lock(g_ui_mutex);
    g_ui_back_buffer = resource;
}

ID3D11Texture2D* ui_texture() {
    std::scoped_lock lock(g_ui_mutex);
    if (g_ui_texture) {
        g_ui_texture->AddRef();
    }
    return g_ui_texture;
}

void marker(const std::string& text) {
    std::scoped_lock lock(g_mutex);
    if (active()) {
        close_run();
        g_lines.push_back(std::format("-- t{} {}", GetCurrentThreadId(), text));
    }
}

} // namespace bg3vr::gpu_trace
