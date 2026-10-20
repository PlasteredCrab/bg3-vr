#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

#include <cstdint>
#include <string_view>

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
    bool grip_taken = false; // the grip is drawing from / stowing in a holster: its binding must not fire
};

// What the headset should do in response (haptics and recentring live there).
struct Feedback {
    bool recenter = false;
    bool toggle_panel = false; // switch the UI panel between the world and the wrist
    bool toggle_view = false;  // switch between first person and the game's own (third-person) camera
    bool toggle_menu = false;  // open / close the VR menu
    float haptic[2] = {0.0f, 0.0f}; // amplitude for a short pulse per hand (0 = none)
};

void install(const Config& config, const ModuleInfo& module);
// Turning, comfort and eye-height settings ([vr]); again whenever the VR menu changes one (any thread).
void configure(const Config& config);
// [bindings] again (the VR menu's Controls page); taken on the next controller sample.
void configure_bindings(const Config& config);
// Calibrate the eye height from the head bone again (e.g. after the eye offset changed).
void recalibrate();

// While the VR menu is open the controllers belong to it: no bindings reach the game (held ones are released), no
// turning or scrolling. The "vrmenu" binding still closes it.
void set_menu_open(bool open);

// The left mouse button from elsewhere on the presenting thread (touching the UI panel with the controller tip).
void mouse_left(bool down);
// Press and release a key by name (kKeys: "i", "escape", ...) while VR play is on; false if unknown or not active.
bool tap_key(std::string_view name);

// VR play on/off (bridge vr=start / vr=stop). Turning it off releases every held key and button.
void set_active(bool on);
bool active();

// Presenting thread, once per headset frame with both hands (0 = left, 1 = right) and the frame time in seconds.
Feedback on_controllers(const Hand (&hands)[2], double time_s);

// Game thread: eye-height calibration from the head bone.
void on_frame();

// Third person: the game's own camera on a big flat screen in the headset (no head tracking, the UI in the image, the
// laser on the screen). First person is the default. Set by the Lua side (bridge view=first|third) once it has
// switched the camera over.
void set_third_person(bool on);
bool third_person();

// How hard the right stick is smooth-turning right now, 0..1 (snap turns: 0); and the [vr] comfort_vignette strength.
float turn_rate();
float comfort_vignette();

void report();

} // namespace bg3vr::vrplay
