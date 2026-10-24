#include "bg3vr/foveation.hpp"

#include "bg3vr/gpu_trace.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/stage_timing.hpp"
#include "bg3vr/stereo.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace bg3vr::foveation {

namespace {

// ------------------------------------------------------------------------------------------------ NVAPI (agent AU 2.1)
constexpr std::uint32_t kInitialize = 0x0150E828, kGetCaps = 0x52B1499A, kSetViewportRates = 0x34F7938F,
                        kCreateRateView = 0x99CA2DFF, kSetRateView = 0x1B0C2F83;
struct ViewportRates {
    std::uint8_t enable = 0;
    std::uint8_t pad[3]{};
    std::uint32_t table[16]{};
};
static_assert(sizeof(ViewportRates) == 0x44);
struct ViewportsRates {
    std::uint32_t version = 0x10010;
    std::uint32_t count = 0;
    ViewportRates* viewports = nullptr;
};
static_assert(sizeof(ViewportsRates) == 0x10);
struct RateViewDesc {
    std::uint32_t version = 0x10018;
    std::uint32_t format = DXGI_FORMAT_R8_UINT;
    std::uint32_t dimension = 4; // Texture2D
    std::uint32_t mip = 0, first = 0, size = 0;
};
static_assert(sizeof(RateViewDesc) == 0x18);

using QueryInterfaceFn = void* (*)(std::uint32_t id);
using InitializeFn = int (*)();
using GetCapsFn = int (*)(IUnknown* device, std::uint32_t version, void* caps);
using SetViewportRatesFn = int (*)(ID3D11DeviceContext* context, const ViewportsRates* desc);
using CreateRateViewFn = int (*)(ID3D11Device* device, ID3D11Resource* resource, const RateViewDesc* desc, IUnknown** view);
using SetRateViewFn = int (*)(ID3D11DeviceContext* context, IUnknown* view);

SetViewportRatesFn g_set_rates = nullptr;
CreateRateViewFn g_create_view = nullptr;
SetRateViewFn g_set_view = nullptr;

enum class Mode : int { Off, GBuffer, Scene };
enum class State : int { Idle, Starting, Ready, Failed };
std::atomic<Mode> g_mode{Mode::GBuffer};
std::atomic<State> g_state{State::Idle};
std::atomic<float> g_inner_deg{25.0f}, g_outer_deg{35.0f};
std::atomic<bool> g_debug{false};
std::atomic<bool> g_dirty{true}; // zones changed: rebuild the image

// Measured by the self-test: the NVAPI rate value giving each block size.
std::uint32_t g_rate_1x1 = 0, g_rate_2x1 = 0, g_rate_2x2 = 0, g_rate_4x4 = 0;
constexpr UINT kTile = 16;

// Render thread (binds inside the game's executor, the self-test at Present).
ID3D11DeviceContext* g_immediate = nullptr;
ID3D11Texture2D* g_image = nullptr;
IUnknown* g_image_view = nullptr;
UINT g_image_w = 0, g_image_h = 0; // the render target the image was made for
struct EyeRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool known = false;
};
std::array<EyeRect, 2> g_rects{};
std::array<std::array<float, 4>, 2> g_image_fov{}; // the FOVs the image was made with
bool g_on = false;
bool g_in_window = false;    // the last bind was in an enabled window (the viewport re-send applies)
bool g_gbuffer_bind = false; // ... and was an eye's G-buffer bind (its viewport is that eye's rectangle)
std::uint64_t g_built_frame = 0;

std::uint64_t g_turned_on = 0, g_turned_off = 0, g_resends = 0, g_refused = 0, g_nvapi_errors = 0, g_rebuilds = 0;
std::array<std::uint64_t, 2> g_on_binds{};
int g_logged_binds = 0;
std::uint64_t g_last_report_frame = 0;
std::array<double, 3> g_zone_share{}; // share of each eye's pixels per zone, last build

template <class T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void count_error(const char* what, int status) {
    if (status != 0 && g_nvapi_errors++ < 10) {
        log::warn("foveation: NVAPI {} returned {}", what, status);
    }
}

void set_vrs(ID3D11DeviceContext* context, bool on) {
    ViewportRates rates{};
    if (on) {
        rates.enable = 1;
        const std::uint32_t outer = g_debug.load(std::memory_order_relaxed) ? g_rate_4x4 : g_rate_2x2;
        const std::uint32_t middle = g_debug.load(std::memory_order_relaxed) ? g_rate_4x4 : g_rate_2x1;
        std::fill(std::begin(rates.table), std::end(rates.table), g_rate_1x1);
        rates.table[1] = middle;
        rates.table[2] = outer;
        rates.table[3] = g_rate_4x4;
    }
    const ViewportsRates desc{0x10010, 1, &rates};
    if (on) {
        count_error("RSSetShadingRateResourceView", g_set_view(context, g_image_view));
        count_error("RSSetViewportsPixelShadingRates", g_set_rates(context, &desc));
    } else {
        count_error("RSSetViewportsPixelShadingRates (off)", g_set_rates(context, &desc));
        count_error("RSSetShadingRateResourceView (off)", g_set_view(context, nullptr));
    }
}

// ------------------------------------------------------------------------------------------------ the image
float angle_deg(float tx, float ty) {
    return std::atan(std::sqrt(tx * tx + ty * ty)) * 57.29578f;
}

// The finest zone a tile needs: the smallest angle from any eye's forward axis over the tile's corners (clamped into
// that eye's rectangle). A tile in no known eye rectangle stays at full rate.
std::uint8_t tile_zone(UINT tx, UINT ty, float inner, float outer) {
    float best = 1e9f;
    for (std::size_t e = 0; e < 2; ++e) {
        const EyeRect& r = g_rects[e];
        if (!r.known || r.w <= 0 || r.h <= 0) {
            continue;
        }
        const float x0 = static_cast<float>(tx * kTile), x1 = static_cast<float>((tx + 1) * kTile);
        const float y0 = static_cast<float>(ty * kTile), y1 = static_cast<float>((ty + 1) * kTile);
        if (x1 <= r.x || x0 >= r.x + r.w || y1 <= r.y || y0 >= r.y + r.h) {
            continue;
        }
        const auto& f = g_image_fov[e]; // tangents left, right, up, down
        for (const float px : {std::clamp(x0, r.x, r.x + r.w), std::clamp(x1, r.x, r.x + r.w)}) {
            for (const float py : {std::clamp(y0, r.y, r.y + r.h), std::clamp(y1, r.y, r.y + r.h)}) {
                const float tanx = f[0] + (px - r.x) / r.w * (f[1] - f[0]);
                const float tany = f[2] - (py - r.y) / r.h * (f[2] - f[3]);
                best = std::min(best, angle_deg(tanx, tany));
            }
        }
        // The forward axis inside the tile: angle 0.
        const float ax = r.x + r.w * (-f[0]) / (f[1] - f[0]), ay = r.y + r.h * f[2] / (f[2] - f[3]);
        if (ax >= x0 && ax < x1 && ay >= y0 && ay < y1) {
            best = 0.0f;
        }
    }
    if (best >= 1e8f) {
        return 0;
    }
    return best < inner ? 0 : best < outer ? 1 : 2;
}

bool build_image(ID3D11Device* device, UINT width, UINT height) {
    const auto eyes = stereo::eyes();
    if (!eyes) {
        return false;
    }
    for (std::size_t e = 0; e < 2; ++e) {
        const auto& fov = eyes->fov[e];
        g_image_fov[e] = {std::tan(fov.left_deg / 57.29578f), std::tan(fov.right_deg / 57.29578f),
                          std::tan(fov.up_deg / 57.29578f), std::tan(fov.down_deg / 57.29578f)};
    }
    const UINT tw = (width + kTile - 1) / kTile, th = (height + kTile - 1) / kTile;
    std::vector<std::uint8_t> texels(static_cast<std::size_t>(tw) * th);
    const float inner = g_inner_deg.load(), outer = std::max(g_inner_deg.load(), g_outer_deg.load());
    std::array<std::uint64_t, 3> counts{};
    for (UINT y = 0; y < th; ++y) {
        for (UINT x = 0; x < tw; ++x) {
            const std::uint8_t z = tile_zone(x, y, inner, outer);
            texels[static_cast<std::size_t>(y) * tw + x] = z;
            ++counts[z];
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = tw;
    desc.Height = th;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{texels.data(), tw, 0};
    ID3D11Texture2D* image = nullptr;
    IUnknown* view = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &data, &image))) {
        log::warn("foveation: the {}x{} shading-rate image couldn't be made", tw, th);
        return false;
    }
    const RateViewDesc view_desc{};
    const int status = g_create_view(device, image, &view_desc, &view);
    if (status != 0 || !view) {
        log::warn("foveation: CreateShadingRateResourceView returned {}", status);
        image->Release();
        return false;
    }
    release(g_image_view);
    release(g_image);
    g_image = image;
    g_image_view = view;
    g_image_w = width;
    g_image_h = height;
    ++g_rebuilds;
    const double total = static_cast<double>(tw) * th;
    for (std::size_t z = 0; z < 3; ++z) {
        g_zone_share[z] = static_cast<double>(counts[z]) / total;
    }
    // Pixel-shader invocations relative to full rate (2x1 halves them, 2x2 quarters; the debug table 4x4).
    const bool debug = g_debug.load();
    const double ratio = g_zone_share[0] + g_zone_share[1] * (debug ? 1.0 / 16 : 0.5) + g_zone_share[2] * (debug ? 1.0 / 16 : 0.25);
    log::info("foveation: shading-rate image {}x{} tiles for a {}x{} target; left eye ({:.0f}, {:.0f}) {:.0f}x{:.0f}{}, right eye "
              "({:.0f}, {:.0f}) {:.0f}x{:.0f}{}; FOV L {:.1f}/{:.1f}/{:.1f}/{:.1f} R {:.1f}/{:.1f}/{:.1f}/{:.1f}; zones < {:.0f} "
              "deg full, < {:.0f} deg 2x1, beyond 2x2{}: {:.0f} / {:.0f} / {:.0f} % of the tiles, about {:.0f} % of the pixel "
              "shading in those passes",
              tw, th, width, height, g_rects[0].x, g_rects[0].y, g_rects[0].w, g_rects[0].h, g_rects[0].known ? "" : " (not seen)",
              g_rects[1].x, g_rects[1].y, g_rects[1].w, g_rects[1].h, g_rects[1].known ? "" : " (not seen)", eyes->fov[0].left_deg,
              eyes->fov[0].right_deg, eyes->fov[0].up_deg, eyes->fov[0].down_deg, eyes->fov[1].left_deg, eyes->fov[1].right_deg,
              eyes->fov[1].up_deg, eyes->fov[1].down_deg, inner, outer, debug ? " (debug: both outer zones 4x4)" : "",
              g_zone_share[0] * 100, g_zone_share[1] * 100, g_zone_share[2] * 100, ratio * 100);
    return true;
}

bool fov_changed() {
    const auto eyes = stereo::eyes();
    if (!eyes) {
        return false;
    }
    for (std::size_t e = 0; e < 2; ++e) {
        const auto& fov = eyes->fov[e];
        const std::array<float, 4> t{std::tan(fov.left_deg / 57.29578f), std::tan(fov.right_deg / 57.29578f),
                                     std::tan(fov.up_deg / 57.29578f), std::tan(fov.down_deg / 57.29578f)};
        for (std::size_t i = 0; i < 4; ++i) {
            if (std::abs(t[i] - g_image_fov[e][i]) > 0.005f) {
                return true;
            }
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ gating
bool phase_stage(int stage, Mode mode, bool& gbuffer) {
    gbuffer = stage == 0x0B || stage == 0x46;
    if (gbuffer) {
        return true;
    }
    return mode == Mode::Scene && (stage == 0x00 || stage == 0x17 || stage == 0x18 || stage == 0x3C || stage == 0x36);
}

bool target_size(ID3D11RenderTargetView* view, UINT& w, UINT& h, DXGI_FORMAT& format) {
    if (!view) {
        return false;
    }
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    ID3D11Texture2D* texture = nullptr;
    if (resource) {
        resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        resource->Release();
    }
    if (!texture) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    texture->Release();
    w = desc.Width;
    h = desc.Height;
    format = desc.Format;
    return true;
}

void periodic_report() {
    const std::uint64_t frame = render::frame_count();
    if (frame < g_last_report_frame + 600) {
        return;
    }
    g_last_report_frame = frame;
    log::info("foveation: on {} times (left eye binds {}, right {}), off {}; viewport re-sends {}; binds refused by the size "
              "rule {}; image builds {}; NVAPI errors {}",
              g_turned_on, g_on_binds[0], g_on_binds[1], g_turned_off, g_resends, g_refused, g_rebuilds, g_nvapi_errors);
}

void on_targets(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth) {
    if (g_state.load(std::memory_order_relaxed) != State::Ready || context != g_immediate) {
        return;
    }
    const Mode mode = g_mode.load(std::memory_order_relaxed);
    const stage_timing::Window w = stage_timing::window();
    bool want = false;
    bool gbuffer = false;
    if (mode != Mode::Off && (w.eye == 0 || w.eye == 1) && phase_stage(w.stage, mode, gbuffer) && count && views && views[0]) {
        UINT rw = 0, rh = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        target_size(views[0], rw, rh, format);
        if (gbuffer) {
            want = count == 4 && depth != nullptr;
            // Rebuilt for a new target size at once; for new rectangles, zones or FOV at most once a second.
            const bool resized = rw != g_image_w || rh != g_image_h;
            const bool settled = render::frame_count() > g_built_frame + 60;
            if (want && (resized || (settled && (g_dirty.load() || fov_changed())))) {
                g_dirty.store(false);
                g_built_frame = render::frame_count();
                ID3D11Device* device = nullptr;
                context->GetDevice(&device);
                if (!device || !build_image(device, rw, rh)) {
                    want = false;
                }
                release(device);
            }
        } else {
            want = g_image && rw == g_image_w && rh == g_image_h;
            if (!want) {
                ++g_refused;
            }
        }
        if (g_logged_binds < 40) {
            ++g_logged_binds;
            log::info("foveation: eye {} stage 0x{:02X} bind: {} target(s), RT0 {}x{} format {}, depth {}: {}", w.eye, w.stage,
                      count, rw, rh, static_cast<int>(format), depth ? "yes" : "no", want ? "VRS ON" : "off");
        }
    }
    g_in_window = want;
    g_gbuffer_bind = want && gbuffer;
    if (want) {
        ++g_on_binds[static_cast<std::size_t>(w.eye)];
    }
    if (want != g_on) {
        set_vrs(context, want);
        g_on = want;
        ++(want ? g_turned_on : g_turned_off);
    }
    periodic_report();
}

void on_viewports(ID3D11DeviceContext* context, UINT count, const D3D11_VIEWPORT* viewports) {
    if (g_state.load(std::memory_order_relaxed) != State::Ready || context != g_immediate || !count || !viewports) {
        return;
    }
    const stage_timing::Window w = stage_timing::window();
    if (g_gbuffer_bind && (w.eye == 0 || w.eye == 1)) {
        EyeRect& r = g_rects[static_cast<std::size_t>(w.eye)];
        const D3D11_VIEWPORT& v = viewports[0];
        if (!r.known || std::abs(r.x - v.TopLeftX) > 0.5f || std::abs(r.y - v.TopLeftY) > 0.5f || std::abs(r.w - v.Width) > 0.5f ||
            std::abs(r.h - v.Height) > 0.5f) {
            log::info("foveation: eye {} renders at ({:.0f}, {:.0f}) {:.0f}x{:.0f} (stage 0x{:02X})", w.eye, v.TopLeftX, v.TopLeftY,
                      v.Width, v.Height, w.stage);
            r = {v.TopLeftX, v.TopLeftY, v.Width, v.Height, true};
            g_dirty.store(true);
        }
    }
    // The driver may drop the per-viewport rates with a new viewport (AU 3.4): sent again while on.
    if (g_on && g_in_window) {
        set_vrs(context, true);
        ++g_resends;
    }
}

// ------------------------------------------------------------------------------------------------ self-test (AU 3.2)
constexpr char kTestShader[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float2 ps(float4 p : SV_Position) : SV_Target { return p.xy; }
)";

ID3DBlob* compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;
    if (FAILED(D3DCompile(kTestShader, sizeof(kTestShader) - 1, "bg3vr_vrs_test", nullptr, nullptr, entry, target, 0, 0, &code,
                          &messages))) {
        log::warn("foveation: test shader {} didn't compile: {}", entry,
                  messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize()) : "");
    }
    release(messages);
    return code;
}

struct Block {
    int w = 0, h = 0; // 0: nothing drawn (culled)
};

// The coarse-pixel size in [x0, x0 + 8) x [0, 8): distinct positions along a row and down a column.
Block measure(const float* px, UINT pitch_floats, UINT x0) {
    std::vector<float> row, column;
    for (UINT x = x0; x < x0 + 8; ++x) {
        const float v = px[x * 2];
        if (v >= 0.0f && std::find(row.begin(), row.end(), v) == row.end()) {
            row.push_back(v);
        }
    }
    for (UINT y = 0; y < 8; ++y) {
        const float v = px[y * pitch_floats + x0 * 2 + 1];
        if (v >= 0.0f && std::find(column.begin(), column.end(), v) == column.end()) {
            column.push_back(v);
        }
    }
    if (row.empty() || column.empty()) {
        return {};
    }
    return {static_cast<int>(8 / row.size()), static_cast<int>(8 / column.size())};
}

// Draws the 192x32 test twice (viewport over the whole target, then only its right half) with table[i] = i and
// texel column i = i, reads both back and works out the block per rate value, the tile size and the indexing.
bool self_test(ID3D11Device* device, ID3D11DeviceContext* context) {
    constexpr UINT kW = 192, kH = 32;
    ID3D11Device1* device1 = nullptr;
    ID3D11DeviceContext1* context1 = nullptr;
    ID3DDeviceContextState* own = nullptr;
    ID3DDeviceContextState* game = nullptr;
    ID3DBlob* vs_code = compile("vs", "vs_5_0");
    ID3DBlob* ps_code = compile("ps", "ps_5_0");
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    std::array<ID3D11Texture2D*, 2> targets{};
    std::array<ID3D11RenderTargetView*, 2> rtvs{};
    std::array<ID3D11Texture2D*, 2> staging{};
    ID3D11Texture2D* image = nullptr;
    IUnknown* image_view = nullptr;
    bool ok = false;
    std::string why;
    do {
        if (!vs_code || !ps_code ||
            FAILED(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs)) ||
            FAILED(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps))) {
            why = "test shaders";
            break;
        }
        if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1))) ||
            FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1)))) {
            why = "no D3D11.1 interfaces";
            break;
        }
        const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
        if (FAILED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &own))) {
            why = "CreateDeviceContextState";
            break;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kW;
        desc.Height = kH;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_TEXTURE2D_DESC sdesc = desc;
        sdesc.Usage = D3D11_USAGE_STAGING;
        sdesc.BindFlags = 0;
        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bool made = true;
        for (std::size_t i = 0; i < 2; ++i) {
            made = made && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &targets[i])) &&
                   SUCCEEDED(device->CreateRenderTargetView(targets[i], nullptr, &rtvs[i])) &&
                   SUCCEEDED(device->CreateTexture2D(&sdesc, nullptr, &staging[i]));
        }
        std::array<std::uint8_t, 24> texels{};
        for (std::size_t i = 0; i < 24; ++i) {
            texels[i] = static_cast<std::uint8_t>(i % 12);
        }
        D3D11_TEXTURE2D_DESC idesc{};
        idesc.Width = 12;
        idesc.Height = 2;
        idesc.MipLevels = idesc.ArraySize = 1;
        idesc.Format = DXGI_FORMAT_R8_UINT;
        idesc.SampleDesc.Count = 1;
        idesc.Usage = D3D11_USAGE_DEFAULT;
        idesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA idata{texels.data(), 12, 0};
        made = made && SUCCEEDED(device->CreateTexture2D(&idesc, &idata, &image));
        if (!made) {
            why = "test textures";
            break;
        }
        const RateViewDesc vdesc{};
        const int status = g_create_view(device, image, &vdesc, &image_view);
        if (status != 0 || !image_view) {
            why = std::format("CreateShadingRateResourceView returned {}", status);
            break;
        }
        context1->SwapDeviceContextState(own, &game);
        ViewportRates rates{};
        rates.enable = 1;
        for (std::uint32_t i = 0; i < 16; ++i) {
            rates.table[i] = i < 12 ? i : 5;
        }
        const ViewportsRates rdesc{0x10010, 1, &rates};
        const float clear[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
        int s1 = 0, s2 = 0;
        for (std::size_t pass = 0; pass < 2; ++pass) {
            context1->ClearRenderTargetView(rtvs[pass], clear);
            const D3D11_VIEWPORT vp{pass == 0 ? 0.0f : kW / 2.0f, 0.0f, pass == 0 ? static_cast<float>(kW) : kW / 2.0f,
                                    static_cast<float>(kH), 0.0f, 1.0f};
            context1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context1->VSSetShader(vs, nullptr, 0);
            context1->PSSetShader(ps, nullptr, 0);
            context1->OMSetRenderTargets(1, &rtvs[pass], nullptr);
            context1->RSSetViewports(1, &vp);
            s1 = g_set_view(context1, image_view);
            s2 = g_set_rates(context1, &rdesc);
            context1->Draw(3, 0);
        }
        const ViewportsRates off{0x10010, 1, &rates};
        rates.enable = 0;
        g_set_rates(context1, &off);
        g_set_view(context1, nullptr);
        ID3D11RenderTargetView* none = nullptr;
        context1->OMSetRenderTargets(1, &none, nullptr);
        context1->SwapDeviceContextState(game, nullptr);
        if (s1 != 0 || s2 != 0) {
            why = std::format("RSSetShadingRateResourceView {} / RSSetViewportsPixelShadingRates {}", s1, s2);
            break;
        }
        for (std::size_t i = 0; i < 2; ++i) {
            context->CopyResource(staging[i], targets[i]);
        }
        std::array<std::vector<float>, 2> pixels;
        UINT pitch = 0;
        bool mapped = true;
        for (std::size_t i = 0; i < 2; ++i) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(context->Map(staging[i], 0, D3D11_MAP_READ, 0, &m))) {
                mapped = false;
                break;
            }
            pitch = m.RowPitch / 4;
            pixels[i].assign(static_cast<const float*>(m.pData), static_cast<const float*>(m.pData) + pitch * kH);
            context->Unmap(staging[i], 0);
        }
        if (!mapped) {
            why = "read-back";
            break;
        }
        // Pass 1: rate value i over pixels [16 i, 16 i + 16); both 8-px halves must agree (a 16 px tile).
        std::array<Block, 12> blocks{};
        bool tile16 = true;
        std::string map;
        for (UINT i = 0; i < 12; ++i) {
            const Block a = measure(pixels[0].data(), pitch, i * 16), b = measure(pixels[0].data(), pitch, i * 16 + 8);
            tile16 = tile16 && a.w == b.w && a.h == b.h;
            blocks[i] = a;
            map += a.w ? std::format(" {}={}x{}", i, a.w, a.h) : std::format(" {}=none", i);
        }
        // Pass 2: the viewport over the right half. By render-target pixel, x 96..111 is rate 6 (2x1 per AU's header
        // list); by viewport it would be rate 0 (nothing drawn).
        const Block right = measure(pixels[1].data(), pitch, 96), left_of_vp = measure(pixels[1].data(), pitch, 0);
        const bool by_target = right.w == blocks[6].w && right.h == blocks[6].h &&
                               (blocks[6].w != blocks[0].w || blocks[6].h != blocks[0].h);
        const auto find = [&](int w, int h) -> int {
            for (int i = 5; i < 12; ++i) {
                if (blocks[static_cast<std::size_t>(i)].w == w && blocks[static_cast<std::size_t>(i)].h == h) {
                    return i;
                }
            }
            return -1;
        };
        const int r11 = find(1, 1), r21 = find(2, 1), r22 = find(2, 2), r44 = find(4, 4);
        log::info("foveation: self-test: rate value -> coarse pixel{}; 16 px tiles {}; indexed by {}; 1x1 = {}, 2x1 = {}, 2x2 "
                  "= {}, 4x4 = {} (right-half pass: {}x{} at x 96, {}x{} at x 0)",
                  map, tile16 ? "yes" : "NO", by_target ? "render-target pixel" : "NOT the render-target pixel", r11, r21, r22,
                  r44, right.w, right.h, left_of_vp.w, left_of_vp.h);
        if (!tile16 || !by_target || r11 < 0 || r21 < 0 || r22 < 0 || r44 < 0) {
            why = "the self-test's rates, tiles or indexing aren't what the image needs";
            break;
        }
        g_rate_1x1 = static_cast<std::uint32_t>(r11);
        g_rate_2x1 = static_cast<std::uint32_t>(r21);
        g_rate_2x2 = static_cast<std::uint32_t>(r22);
        g_rate_4x4 = static_cast<std::uint32_t>(r44);
        ok = true;
    } while (false);
    if (!ok) {
        log::warn("foveation: self-test failed ({}); foveated rendering off", why);
    }
    release(game);
    release(own);
    release(image_view);
    release(image);
    for (std::size_t i = 0; i < 2; ++i) {
        release(staging[i]);
        release(rtvs[i]);
        release(targets[i]);
    }
    release(ps);
    release(vs);
    release(ps_code);
    release(vs_code);
    release(context1);
    release(device1);
    return ok;
}

// On the render thread before a Present: NVAPI, the capability, the self-test, then the bind listener.
void start() {
    const auto fail = [](const std::string& why) {
        g_state.store(State::Failed);
        health::set("Foveation", health::State::Unavailable, why);
        log::warn("foveation: off: {}", why);
    };
    ID3D11Device* device = render::device();
    if (!device) {
        fail("no device");
        return;
    }
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) {
        nvapi = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    const auto query = nvapi ? reinterpret_cast<QueryInterfaceFn>(GetProcAddress(nvapi, "nvapi_QueryInterface")) : nullptr;
    if (!query) {
        fail("no nvapi64.dll (not an NVIDIA GPU)");
        return;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nvapi, path, MAX_PATH);
    const auto initialize = reinterpret_cast<InitializeFn>(query(kInitialize));
    const auto caps_fn = reinterpret_cast<GetCapsFn>(query(kGetCaps));
    g_set_rates = reinterpret_cast<SetViewportRatesFn>(query(kSetViewportRates));
    g_create_view = reinterpret_cast<CreateRateViewFn>(query(kCreateRateView));
    g_set_view = reinterpret_cast<SetRateViewFn>(query(kSetRateView));
    const int init = initialize ? initialize() : -999;
    std::array<std::uint32_t, 8> caps{};
    const int caps_status = caps_fn ? caps_fn(device, 0x10020, caps.data()) : -999;
    log::info("foveation: {} at 0x{:X}; Initialize {}, GetGraphicsCapabilities {} (caps {:08X} {:08X} ...: VRS bit {}); "
              "SetViewportRates 0x{:X}, CreateRateView 0x{:X}, SetRateView 0x{:X}",
              std::filesystem::path(path).string(), reinterpret_cast<std::uintptr_t>(nvapi), init, caps_status, caps[0], caps[1],
              (caps[0] >> 1) & 1, reinterpret_cast<std::uintptr_t>(g_set_rates), reinterpret_cast<std::uintptr_t>(g_create_view),
              reinterpret_cast<std::uintptr_t>(g_set_view));
    if (init != 0 || !g_set_rates || !g_create_view || !g_set_view) {
        fail("NVAPI functions missing");
        return;
    }
    if (caps_status == 0 && !((caps[0] >> 1) & 1)) {
        log::info("foveation: the capability bit reads 0; the self-test decides");
    }
    device->GetImmediateContext(&g_immediate);
    if (!self_test(device, g_immediate)) {
        release(g_immediate);
        fail("self-test failed (see the foveation: lines)");
        return;
    }
    if (!gpu_trace::set_bind_listener({&on_targets, &on_viewports})) {
        release(g_immediate);
        fail("the context hooks aren't installed");
        return;
    }
    g_state.store(State::Ready);
    health::set("Foveation", g_mode.load() == Mode::Off ? health::State::Off : health::State::Ready, "[render] foveation");
    log::info("foveation: ready ({})", g_mode.load() == Mode::Scene ? "G-buffer and scene passes" : g_mode.load() == Mode::GBuffer ? "G-buffer passes" : "off");
}

void request_start() {
    State expected = State::Idle;
    if (g_mode.load() != Mode::Off && g_state.compare_exchange_strong(expected, State::Starting)) {
        render::run_on_render_thread(&start);
    }
}

} // namespace

void configure(const Config& config) {
    const std::string mode = config.get("render", "foveation", "gbuffer");
    g_mode.store(mode == "off" ? Mode::Off : mode == "scene" ? Mode::Scene : Mode::GBuffer);
    g_inner_deg.store(std::clamp(static_cast<float>(config.get_int("render", "foveation_inner_deg", 25)), 5.0f, 80.0f));
    g_outer_deg.store(std::clamp(static_cast<float>(config.get_int("render", "foveation_outer_deg", 35)), 5.0f, 89.0f));
    g_debug.store(config.get_bool("render", "foveation_debug", false));
    g_dirty.store(true);
    if (g_state.load() == State::Ready) {
        health::set("Foveation", g_mode.load() == Mode::Off ? health::State::Off : health::State::Ready, "[render] foveation");
    }
    log::info("foveation: {} (full rate within {:.0f} deg, 2x1 to {:.0f} deg, 2x2 beyond{})", mode, g_inner_deg.load(),
              g_outer_deg.load(), g_debug.load() ? "; debug: outer zones 4x4" : "");
    request_start();
}

void install(const Config& config, const ModuleInfo&) {
    configure(config);
}

void report() {
    log::info("foveation: state {}, on {} / off {}, left / right eye binds {} / {}, re-sends {}, refused {}, builds {}, NVAPI "
              "errors {}",
              static_cast<int>(g_state.load()), g_turned_on, g_turned_off, g_on_binds[0], g_on_binds[1], g_resends, g_refused,
              g_rebuilds, g_nvapi_errors);
}

} // namespace bg3vr::foveation
