#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/vr_math.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

// Picking items up with the hands (docs/design/physical-interaction.md 2a/2b; agents D, E, I). On a grip press the hand
// casts the game's own picking ray (rayprobe::raycast with the picking objects masks) along its aim, up to [grab]
// reach; a hit whose physics container carries an entity that resolves to a client item (ecl entity helper 0x704BD0,
// checked by the item's own handle at +0x10) is grabbed:
//   grab:    client physics suspend 0x1C7DD00, and the item's pose relative to the hand is kept;
//   hold:    every game frame the item is put at hand * offset with ecl::Item SetTranslate 0x1C77050 / SetRotate
//            0x1C77130 (the client's own transform path, as for server-driven moves);
//   release: physics resume 0x1C7DE60 and SetFlags(0x40000) 0x1C76D50, which reports the final pose to the server once
//            (the game's own ItemUpdate path).
//   throw:   let go while the hand moves faster than [grab] throw_min_speed and the item flies on with the hand's
//            velocity under gravity until the game's raycast finds something solid, where it settles as above;
//   pull:    aim at an item up to [grab] pull_range away, hold the grip and flick the hand back (Half-Life: Alyx):
//            it flies on an arc into the hand and is caught when it comes within [grab] catch_radius.
// [grab] probe = true only logs what a grip would grab; [grab] hold = true picks it up. Both off by default until the
// first in-game run confirms the chain. While enabled the grips belong to grabbing (no mouse binding).
namespace bg3vr::hold {

void install(const Config& config, const ModuleInfo& module);
bool enabled(); // probe or hold

// From the headset (presenting thread): grip state, the hand's grip and aim poses in the game world, and the sample
// time in seconds (for the hand's velocity: throws and flicks).
void set_hand(std::size_t hand, bool grip, std::optional<vr::RigidPose> grip_pose, std::optional<vr::RigidPose> aim_pose,
              double time_s);

// Game thread: grabs, holds and releases.
void on_frame();

// Drop everything without touching the items (a level is unloading).
void forget();

void report();

} // namespace bg3vr::hold
