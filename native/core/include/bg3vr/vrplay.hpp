#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

#include <cstdint>

// Playing with the motion controllers once VR is started (bridge vr=start, sent automatically by the Lua side when
// the headset is ready): controller buttons become the game's own mouse/keyboard raw input (InputManager::InjectRaw
// 0x40F1F00 on [0x604B1A8], device 0 keyboard / 1 mouse), so the game stays in mouse mode and the laser UI keeps
// working (agent F); snap or smooth turning and mouse-wheel scrolling on the right stick; hold the right stick
// click to recentre; and the eye height calibrated from the character's head bone (skeleton.hpp). Bindings and
// comfort settings are in [vr] / [bindings] in bg3vr.ini.
namespace bg3vr::vrplay {

// 4.1.1.7631656 (InjectRaw and its argument layout re-disassembled 2026-10-20 from 0x57DC00's calls).
namespace layout {
inline constexpr std::uintptr_t kInputManager = 0x604B1A8; // ls::InputManager*
inline constexpr std::uint16_t kDeviceKeyboard = 0;
inline constexpr std::uint16_t kDeviceMouse = 1;
inline constexpr std::uint16_t kMouseLeft = 123;  // SE InputRawType left2
inline constexpr std::uint16_t kMouseMiddle = 124;
inline constexpr std::uint16_t kMouseRight = 125; // right2
inline constexpr std::uint16_t kWheelUp = 135;    // wheel_ypos
inline constexpr std::uint16_t kWheelDown = 136;  // wheel_yneg
} // namespace layout

// One controller as the headset samples it (kept free of OpenXR types).
struct Hand {
    bool active = false;
    float trigger = 0.0f;
    float squeeze = 0.0f;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    bool stick_click = false;
    bool primary = false;   // A / X
    bool secondary = false; // B / Y
    bool menu = false;
};

// What the headset should do in response (haptics and recentring live there).
struct Feedback {
    bool recenter = false;
    float haptic[2] = {0.0f, 0.0f}; // amplitude for a short pulse per hand (0 = none)
};

void install(const Config& config, const ModuleInfo& module);

// VR play on/off (bridge vr=start / vr=stop). Turning it off releases every held key and button.
void set_active(bool on);
bool active();

// Presenting thread, once per headset frame with both hands (0 = left, 1 = right) and the frame time in seconds.
Feedback on_controllers(const Hand (&hands)[2], double time_s);

// Game thread: eye-height calibration from the head bone.
void on_frame();

void report();

} // namespace bg3vr::vrplay
