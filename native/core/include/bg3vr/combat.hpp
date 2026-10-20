#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/vr_math.hpp"

#include <optional>
#include <string>

// Physical combat that always goes through the game's own rules: every attack is the game's normal click on a target,
// so attack rolls, ranges, action costs, prepared abilities and spells all behave exactly as in the base game.
//   - Swing (melee weapon drawn, [combat] swing = gaze): a quick swing of the pointing hand clicks on the character
//     the head is looking at, within reach. With nothing prepared the click is the weapon attack; with a weapon ability
//     or spell prepared it uses that on the target.
//   - Bow ([combat] bow = draw): bring the pointing hand's grip to the other (bow) hand to nock, pull it back and let
//     go; the shot is a click along the arrow's line (bow hand <- drawing hand) on the character it points at.
//   - Crossbows and spells: aim with the pointing hand and click (the hand's picking ray), as everywhere else.
// The click lands where the picking ray is pointed for a moment (picking::override_ray) and only when a character is
// actually there (a raycast with the game's picking masks), so a swing at nothing never walks the character off.
// What is in hand comes from the Lua side (bridge wield=melee|bow|crossbow|none).
namespace bg3vr::combat {

void install(const Config& config);
void configure(const Config& config); // the VR menu
void set_wield(const std::string& kind); // bridge wield=

// Presenting thread, every headset frame: the head and both hands in the game world (game convention) and in
// tracked space for distances; the pointing hand's grip speed (m/s) and whether each hand grips.
struct Frame {
    std::optional<vr::RigidPose> head_world;
    std::optional<vr::RigidPose> pointing_world, other_world; // grips
    std::optional<vr::Vec3> pointing_tracked, other_tracked;   // grip positions, metres
    float pointing_speed = 0.0f;
    bool pointing_grip = false;
    double time_s = 0.0;
};
struct Feedback {
    float haptic_pointing = 0.0f;
    float haptic_other = 0.0f;
};
Feedback on_headset(const Frame& frame);

// Game thread: raycasts the pending swing / shot and clicks if a character is there.
void on_frame();
void report();

} // namespace bg3vr::combat
