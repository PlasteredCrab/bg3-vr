#pragma once

#include <d3d11_1.h>

#include <cstdint>
#include <string>

// Copying the game's back buffer into OpenXR images with a shader instead of CopyResource, so the
// formats don't have to match: BG3's back buffer is R10G10B10A2_UNORM (gamma-encoded values, no
// sRGB form), the headset images are 8-bit sRGB. Also copies a sub-rectangle and scales, for split
// views. Runs on the game's immediate context just before Present and leaves the game's pipeline
// state exactly as it found it (SwapDeviceContextState), since the renderer caches its state.
namespace bg3vr::blit {

class Blitter {
public:
    Blitter() = default;
    ~Blitter();
    Blitter(const Blitter&) = delete;
    Blitter& operator=(const Blitter&) = delete;

    // Compiles the shaders (d3dcompiler_47.dll, delay-loaded) and creates a sampleable copy of a
    // back buffer of this size and format.
    bool create(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, std::string& error);
    void destroy();
    bool ready() const { return state_ != nullptr; }

    struct Rect {
        UINT x = 0, y = 0, width = 0, height = 0; // pixels in the source; width 0 = the whole source
    };

    // Options for panels: keep the source's alpha (the UI is premultiplied), and draw a cursor dot at
    // (cursor_u, cursor_v) in target uv with radius cursor_radius (in uv of the target's height; 0 = none).
    struct Overlay {
        bool keep_alpha = false;
        float cursor_u = 0.0f;
        float cursor_v = 0.0f;
        float cursor_radius = 0.0f;
        float vignette = 0.0f; // comfort vignette strength, 0..1 (darkens toward the edges)
        // An aiming beam from (beam_u0, beam_v0) to (beam_u1, beam_v1) in target uv: linear colour and opacity,
        // width at its start (uv of the target's height; it tapers to a third), style 0 off, 1 solid, 2 dashed,
        // 3 flowing dots, 4 pulse; animation time in seconds; an end ring of radius beam_end (uv, 0 = none).
        float beam_u0 = 0.0f, beam_v0 = 0.0f, beam_u1 = 0.0f, beam_v1 = 0.0f;
        float beam_rgba[4] = {1.0f, 0.8f, 0.4f, 0.5f};
        float beam_width = 0.0f;
        int beam_style = 0;
        float beam_time = 0.0f;
        float beam_end = 0.0f;
    };

    // Draws `rect` of `source` (same size and format as create()) over the whole `target` view of a
    // `target_width` x `target_height` image. `linearize`: the source holds gamma-encoded values in a
    // non-sRGB format and the target view is sRGB, so decode them first.
    void draw(ID3D11Texture2D* source, const Rect& rect, ID3D11RenderTargetView* target, UINT target_width,
              UINT target_height, bool linearize, const Overlay& overlay = {});

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext1* context_ = nullptr;
    ID3DDeviceContextState* state_ = nullptr;
    ID3D11Texture2D* copy_ = nullptr;
    ID3D11ShaderResourceView* copy_view_ = nullptr;
    ID3D11VertexShader* vertex_shader_ = nullptr;
    ID3D11PixelShader* pixel_shader_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11Buffer* constants_ = nullptr;
    ID3D11RasterizerState* rasterizer_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
};

// True for the *_SRGB formats (sampling them already returns linear values).
bool is_srgb(DXGI_FORMAT format);

} // namespace bg3vr::blit
