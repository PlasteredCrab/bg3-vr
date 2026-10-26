#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

#include <dxgi.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>

// The game's D3D11 output (Phase 1 step 2). Hooks the game's own IDXGISwapChain::Present call in
// rf::dx11::RenderFrame::FlipBuffers (signature dx11_present_call) rather than DXGI itself, which
// the Script Extender already patches. At that point the back buffer holds the finished frame.
namespace bg3vr::render {

// [render] max_frame_latency (the GPU frame queue set on the game's device; 0 = DXGI's default). Before install.
void configure(const Config& config);
void install(const ModuleInfo& module);

// The game's swapchain and device, once a frame has been presented (nullptr before). Not AddRef'd:
// they live as long as the game's renderer.
IDXGISwapChain* swapchain();
// The rf::dx11::RenderFrame of the last Present (rdi at the call): holds the list of views the render loop
// draws (+0x42E0, count +0x42EC) and the stage resource manager (+0x4250).
std::uintptr_t render_frame();
// Frames presented so far.
std::uint64_t frame_count();
// The thread that last presented (0 before the first frame).
std::uint32_t presenting_thread();
ID3D11Device* device();

// Called on the presenting thread just before every Present, with the swapchain about to present
// and the rf::dx11::RenderFrame presenting it (rdi at the call site).
using PresentCallback = void (*)(IDXGISwapChain* swapchain, std::uintptr_t render_frame);
void set_present_callback(PresentCallback callback);

// While set, every Present uses this sync interval instead of the game's (0: don't wait for the
// monitor's vblank, so a headset session paces the game instead of the desktop refresh rate).
void set_sync_interval_override(int interval); // -1 clears it

// Runs 	ask on the presenting (render) thread just before the next Present, between frames.
void run_on_render_thread(std::function<void()> task);

// Logs presented frames and the frame rate since the last report.
void report();

} // namespace bg3vr::render
