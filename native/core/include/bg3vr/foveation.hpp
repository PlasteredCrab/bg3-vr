#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

// Fixed foveated rendering through NVIDIA variable-rate shading (agent AU, AU_foveated.md). The game doesn't use VRS;
// nvapi64.dll (already loaded by the game) is driven through nvapi_QueryInterface with the ids and struct layouts AU
// read from the driver DLL: RSSetViewportsPixelShadingRates 0x34F7938F (version 0x10010, 0x44 bytes per viewport),
// CreateShadingRateResourceView 0x99CA2DFF (version 0x10018, R8_UINT, Texture2D), RSSetShadingRateResourceView
// 0x1B0C2F83, GetGraphicsCapabilities 0x52B1499A (version 0x10020). What the DLL can't show (the rate values, the
// tile size, whether the image is indexed by render-target pixel) a self-test measures once before anything is used.
//
// One R8_UINT image over the eyes' shared render target, a 16 px tile per texel: full rate within [render]
// foveation_inner_deg of each eye's forward axis, 2x1 out to foveation_outer_deg, 2x2 beyond. VRS is only ever on while
// a render-target bind of an eye view's G-buffer pass (stages 0x0B / 0x46, four targets and a depth buffer) or, with
// `scene`, a full-size target in the forward, emissive and sky stages (0x00, 0x17, 0x18, 0x3C, 0x36) is current; any
// other bind turns it off (stage windows from stage_timing.cpp, binds from gpu_trace.cpp's context hooks).
// [render] foveation = off | gbuffer | scene.
namespace bg3vr::foveation {

void install(const Config& config, const ModuleInfo& module);
void configure(const Config& config);
void report();

} // namespace bg3vr::foveation
