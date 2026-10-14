#pragma once

#include "bg3vr/config.hpp"

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

} // namespace bg3vr::headset
