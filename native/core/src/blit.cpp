#include "bg3vr/blit.hpp"

#include <d3dcompiler.h>

#include <cstring>

namespace bg3vr::blit {

namespace {

// A full-screen triangle; the constants pick the source rectangle (uv offset and scale) and
// whether to decode sRGB.
constexpr char kShader[] = R"(
cbuffer Params : register(b0) {
    float4 uv_rect;  // offset.xy, scale.zw
    float4 options;  // x: linearize
};
Texture2D source : register(t0);
SamplerState linear_clamp : register(s0);

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Varyings vs_main(uint id : SV_VertexID) {
    const float2 t = float2((id << 1) & 2, id & 2);
    Varyings o;
    o.position = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv_rect.xy + t * uv_rect.zw;
    return o;
}

float3 srgb_to_linear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4);
}

float4 ps_main(Varyings i) : SV_Target {
    float3 c = source.Sample(linear_clamp, i.uv).rgb;
    if (options.x > 0.5) {
        c = srgb_to_linear(c);
    }
    return float4(c, 1.0);
}
)";

template <class T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

ID3DBlob* compile(const char* entry, const char* target, std::string& error) {
    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;
    const HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "bg3vr_blit", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
    if (FAILED(hr)) {
        error = std::string("shader ") + entry + " failed to compile";
        if (messages) {
            error += ": ";
            error.append(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
        }
    }
    release(messages);
    return code;
}

struct Constants {
    float uv_rect[4];
    float options[4];
};

} // namespace

bool is_srgb(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC7_UNORM_SRGB: return true;
    default: return false;
    }
}

Blitter::~Blitter() {
    destroy();
}

bool Blitter::create(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, std::string& error) {
    destroy();
    device_ = device;
    width_ = width;
    height_ = height;

    ID3D11Device1* device1 = nullptr;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const bool have_11_1 =
        SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1))) &&
        SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context_)));
    release(context);
    if (!have_11_1) {
        release(device1);
        error = "the game's device has no D3D11.1 interfaces (needed to save and restore its state)";
        destroy();
        return false;
    }
    const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    const HRESULT state_hr = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                               nullptr, &state_);
    release(device1);
    if (FAILED(state_hr)) {
        error = "CreateDeviceContextState failed";
        destroy();
        return false;
    }

    // The back buffer itself may not be sampleable, so it's copied into this first.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &copy_)) ||
        FAILED(device->CreateShaderResourceView(copy_, nullptr, &copy_view_))) {
        error = "can't create a sampleable copy of the back buffer (format " + std::to_string(format) + ")";
        destroy();
        return false;
    }

    ID3DBlob* vs = compile("vs_main", "vs_5_0", error);
    ID3DBlob* ps = vs ? compile("ps_main", "ps_5_0", error) : nullptr;
    const bool shaders = vs && ps &&
                         SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                                              &vertex_shader_)) &&
                         SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr,
                                                             &pixel_shader_));
    release(vs);
    release(ps);
    if (!shaders) {
        if (error.empty()) {
            error = "can't create the blit shaders";
        }
        destroy();
        return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(Constants);
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    if (FAILED(device->CreateSamplerState(&sampler, &sampler_)) ||
        FAILED(device->CreateBuffer(&constants, nullptr, &constants_)) ||
        FAILED(device->CreateRasterizerState(&rasterizer, &rasterizer_))) {
        error = "can't create the blit's sampler, constants or rasterizer state";
        destroy();
        return false;
    }
    return true;
}

void Blitter::destroy() {
    release(rasterizer_);
    release(constants_);
    release(sampler_);
    release(pixel_shader_);
    release(vertex_shader_);
    release(copy_view_);
    release(copy_);
    release(state_);
    release(context_);
    device_ = nullptr;
}

void Blitter::draw(ID3D11Texture2D* source, const Rect& rect, ID3D11RenderTargetView* target, UINT target_width,
                   UINT target_height, bool linearize) {
    if (!state_ || !source || !target) {
        return;
    }
    // On the game's own state: the copy (CopyResource touches no pipeline state).
    context_->CopyResource(copy_, source);

    // Everything else on our own state object; swapping back restores the game's state exactly.
    ID3DDeviceContextState* game_state = nullptr;
    context_->SwapDeviceContextState(state_, &game_state);

    const UINT w = rect.width ? rect.width : width_;
    const UINT h = rect.height ? rect.height : height_;
    const Constants constants{{static_cast<float>(rect.x) / static_cast<float>(width_),
                               static_cast<float>(rect.y) / static_cast<float>(height_),
                               static_cast<float>(w) / static_cast<float>(width_),
                               static_cast<float>(h) / static_cast<float>(height_)},
                              {linearize ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}};
    context_->UpdateSubresource(constants_, 0, nullptr, &constants, 0, 0);

    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(target_width), static_cast<float>(target_height),
                                  0.0f, 1.0f};
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(vertex_shader_, nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &constants_);
    context_->PSSetShader(pixel_shader_, nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &constants_);
    context_->PSSetShaderResources(0, 1, &copy_view_);
    context_->PSSetSamplers(0, 1, &sampler_);
    context_->RSSetState(rasterizer_);
    context_->RSSetViewports(1, &viewport);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->OMSetDepthStencilState(nullptr, 0);
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->Draw(3, 0);

    // Unbind before handing the state object back, so nothing keeps the eye image bound.
    ID3D11RenderTargetView* no_target = nullptr;
    ID3D11ShaderResourceView* no_view = nullptr;
    context_->OMSetRenderTargets(1, &no_target, nullptr);
    context_->PSSetShaderResources(0, 1, &no_view);

    context_->SwapDeviceContextState(game_state, nullptr);
    release(game_state);
}

} // namespace bg3vr::blit
