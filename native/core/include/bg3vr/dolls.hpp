#pragma once

#include "bg3vr/camera.hpp"
#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/vr_math.hpp"

#include <d3d11.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// The characters in UI screens (inventory, character sheet, party, trade, ...) in stereo 3D (agents P and V:
// re-local/ghidra/agents/P_ui_character_3d.md, V_stereo_dolls_items.md). Each LSPaperdoll is a live offscreen render
// (PaperdollRenderer: its own RenderFrame and GameRenderView over a private scene with the character's clone). For
// the right eye the mod adds a second offscreen frame with one view sharing that scene, atmosphere, lighting and
// camera; the game's own frame is the left eye. Both eye views get a proxy camera at that eye (stereo.cpp), placed
// as if the doll's picture on the UI panel were a window: each eye looks through the window's rectangle (off-axis
// frustum), so the character has depth behind the panel. The two images go to the headset as one quad layer per eye
// over the picture's place on the panel, the black background keyed out, and the game's flat picture is hidden.
namespace bg3vr::dolls {

void install(const Config& config, const ModuleInfo& module);
void configure(const Config& config); // [ui] stereo_dolls (the VR menu)
bool enabled();

// Game thread, right after the paperdoll manager's update 0x13E1A40 (paperdoll.cpp): new dolls, binding, camera
// geometry, the dolls' rectangles on the UI.
void on_manager_update(std::uintptr_t manager);

// LSPaperdoll::OnRender: true when a 3D doll replaces this element's flat picture.
bool replaces(std::uintptr_t element);

// Render thread (the proxy and projection hooks): for one of a doll's two views, that eye's offset from the doll's
// own camera (OpenXR camera axes: x right, y up, z back; doll units) and its off-axis field of view.
struct EyeCamera {
    int eye = 0;
    vr::Vec3 offset{};
    camera::EyeFov fov{};
};
std::optional<EyeCamera> eye_camera(std::uintptr_t view, std::uintptr_t controller);

// Presenting thread: the dolls to show (both eyes' images AddRef'd; the caller releases them), with the picture's
// rectangle on the UI in UI pixels.
struct Shown {
    std::uintptr_t id = 0;
    std::array<float, 4> rect{}; // x0, y0, x1, y1
    std::array<ID3D11Texture2D*, 2> texture{};
};
std::vector<Shown> shown();
// Presenting thread: where the eyes are relative to doll `id`'s window on the panel (window axes: x right, y up,
// z toward the viewer; metres) and how wide the window is (metres). nullopt: not shown.
void set_window(std::uintptr_t id, std::optional<std::array<vr::Vec3, 2>> eyes_m, float width_m);

void report();

} // namespace bg3vr::dolls
