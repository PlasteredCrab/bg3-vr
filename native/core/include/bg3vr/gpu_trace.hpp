#pragma once

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

} // namespace bg3vr::gpu_trace
