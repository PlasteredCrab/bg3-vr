#pragma once

#include "bg3vr/module_info.hpp"

#include <dxgi.h>
#include <d3d11.h>

// The game's D3D11 output (Phase 1 step 2). Hooks the game's own IDXGISwapChain::Present call in
// rf::dx11::RenderFrame::FlipBuffers (signature dx11_present_call) rather than DXGI itself, which
// the Script Extender already patches. At that point the back buffer holds the finished frame.
namespace bg3vr::render {

void install(const ModuleInfo& module);

// The game's swapchain and device, once a frame has been presented (nullptr before). Not AddRef'd:
// they live as long as the game's renderer.
IDXGISwapChain* swapchain();
ID3D11Device* device();

// Called on the presenting thread just before every Present, with the swapchain about to present.
using PresentCallback = void (*)(IDXGISwapChain* swapchain);
void set_present_callback(PresentCallback callback);

// Logs presented frames and the frame rate since the last report.
void report();

} // namespace bg3vr::render
