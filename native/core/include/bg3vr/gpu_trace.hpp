#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string>

// What the game actually does on the GPU for a few frames: every run of draws on the immediate context
// grouped by render target (texture, size, format) and viewport, with draw / clear / dispatch counts,
// copies between textures, and markers from the renderer's own events (a view's begin and end
// recording). Answers "where do a view's draws land" without a graphics debugger. Hooks the
// ID3D11DeviceContext functions on the first arm(); they stay, idle, afterwards.
namespace bg3vr::gpu_trace {

void arm(int frames);
// A line in the trace between draw runs (no-op unless armed).
void marker(const std::string& text);

// Finding the game's UI texture (agent F, F_ui.md 1.3): the final composite (stage 0x07) draws into the back buffer
// with the UI target bound as pixel-shader resource 1. While watching, every draw into `back_buffer` with something in
// PS slot 1 records that texture. Hooks the context on first use (needs the game's device).
void watch_ui(bool on);
void set_back_buffer(std::uintptr_t resource);
// The latest UI texture seen (AddRef'd for the caller, who releases it), or nullptr.
ID3D11Texture2D* ui_texture();
// Keep the UI out of the eye images: a blank image stands in for it at the composite draw (the UI is still copied).
void set_ui_hidden(bool hidden);

} // namespace bg3vr::gpu_trace
