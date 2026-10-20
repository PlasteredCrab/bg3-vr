#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/vr_math.hpp"

#include <array>
#include <cstdint>

// OpenXR in the game (Phase 1 steps 2-4). With [xr] enabled = true in bg3vr.ini, the first presented
// frame opens an OpenXR session on the game's own D3D11 device. Then, by [xr] mode:
//   screen     the finished back buffer on a flat virtual screen (a quad layer) in front of the player.
//   mono       head tracking: the headset's pose and field of view drive the game camera (while a pose
//              is held, e.g. after !vr_cam_set here), and each frame is shown to both eyes with the pose
//              it was rendered with. Correct scale and rotation, no depth.
//   alternate  as mono, but frames alternate between the left and right eye's pose and field of view:
//              stereo at half the frame rate per eye, ghosting in motion.
// mono and alternate show the virtual screen while no pose is held. Off by default; openxr_loader.dll
// is only loaded when enabled.
namespace bg3vr::headset {

void install(const Config& config);
// [ui] wrist_width and the like, again whenever the VR menu changes one (any thread).
void configure_ui(const Config& config);

// Frames between the camera update a frame was rendered from and its Present: 0..7, or -1 to find
// it by looking for the applied view matrix in the presenting RenderFrame (the default).
void set_lag(int frames);

// Takes the current head position and facing as the new centre (also done automatically whenever
// head tracking starts driving the camera).
void recenter();

// Same-frame stereo (stereo.hpp) in a head-tracked mode: the camera holds the head centre, the two
// eye views draw the back buffer's halves, and each half goes to its eye with that eye's pose. Takes
// effect while stereo::active() (both eye views assigned). False if the mode is screen.
bool set_stereo(bool on);

void report();

// Motion controllers ([xr] input = true, the default), sampled once per submitted frame on the presenting
// thread at the frame's predicted display time. Poses are in the headset's LOCAL space (OpenXR convention:
// right-handed, +Y up, -Z forward, metres), like the head views before they're mapped into the game.
struct HandState {
    bool active = false;         // a controller is bound to this hand
    bool tracked = false;        // grip position and orientation valid
    vr::RigidPose grip;          // the held-object pose
    vr::RigidPose aim;           // pointing ray (origin, -Z)
    bool aim_valid = false;
    vr::Vec3 velocity{};         // grip linear velocity, m/s
    vr::Vec3 angular_velocity{}; // rad/s
    bool velocity_valid = false;
    float trigger = 0.0f;
    float squeeze = 0.0f;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    bool stick_click = false;
    bool primary = false;   // A / X
    bool secondary = false; // B / Y
    bool menu = false;
    // The grip in the game world (game convention and units), placed like the head (camera::tracked_to_world);
    // invalid while no pose is held or the grip isn't tracked.
    vr::RigidPose world_grip;
    bool world_valid = false;
    std::uint64_t sample = 0; // increases with every sample (0: never sampled)
};
inline constexpr std::size_t kLeftHand = 0;
inline constexpr std::size_t kRightHand = 1;

// The latest sample of both hands (any thread).
std::array<HandState, 2> hands();
// Queues a vibration on one hand (any thread); it starts on the next submitted frame.
void haptic(std::size_t hand, float amplitude, float seconds);
// Logs both hands' current state and bound controller profiles; `buzz` also pulses each hand once.
void report_hands(bool buzz);

} // namespace bg3vr::headset
