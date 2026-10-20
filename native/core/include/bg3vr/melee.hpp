#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/vr_math.hpp"

#include <optional>

// Physical melee (docs/design/physical-interaction.md 2c, agent I): a fast swing of the right hand that passes through
// a character starts the game's own weapon attack, with its dice. The "blade" is a segment along the right hand's aim
// ([combat] blade metres long). While its tip moves faster than [combat] min_speed, the game's picking ray is cast
// along the blade and along the tip's path since the last frame; a hit is published to the Lua side as
// hit=<sequence> <x> <y> <z> <speed> (bridge::set_status). Client/Combat.lua finds the character at that point and
// asks the server to run Osi.Attack(attacker, target, 0), the equipped weapon's attack with a normal roll.
// [combat] physical = false by default.
namespace bg3vr::melee {

void install(const Config& config);
bool enabled();

// From the headset: the right hand's aim in the game world (game convention, forward +Z) and the sample time.
void set_hand(std::optional<vr::RigidPose> aim, double time_s);

// Game thread.
void on_frame();

void report();

} // namespace bg3vr::melee
