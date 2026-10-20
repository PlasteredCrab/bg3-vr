#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

#include <windows.h>

#include <cstdint>

// The game's render resolution set by the mod for VR (agent O, re-local/ghidra/agents/O_render_resolution.md):
// the back buffer becomes 2 x eye width by eye height, so each eye's half of the same-frame stereo layout has the
// headset's resolution (times [render] vr_resolution percent), whatever the monitor or the game's resolution setting.
// Through the game's own resize entry, the renderer's OnWindowResize 0x585510 (renderer = [0x6046F98], vtable
// 0x5499F58; event {HWND, +8 display w, +0xC display h, +0x10 output w, +0x14 output h}), which recreates the swapchain
// buffers, every screen-sized target, the views' viewports and aspect, and the per-view DLSS features. A hook on it
// keeps the size when the game resizes for its own reasons (window events, options, alt-tab); a hook on the SDL
// event pump 0x57AAA0 (the thread the game resizes from) applies a new size. The swapchain uses DXGI scaling
// stretch, so the desktop window shows the larger back buffer scaled down.
namespace bg3vr::resolution {

void install(const Config& config, const ModuleInfo& module);
// [render] vr_resolution again (the VR menu); applied on the next event pump.
void configure(const Config& config);
// From the headset: the per-eye size the runtime recommends, and the game's window.
void set_headset_eye(std::uint32_t width, std::uint32_t height);
void set_window(HWND window);
// Third person: the game's own size (one view on a flat screen), first person: the VR size again.
void set_suspended(bool suspended);
void report();

} // namespace bg3vr::resolution
