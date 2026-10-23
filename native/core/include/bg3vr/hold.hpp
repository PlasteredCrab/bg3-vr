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
//   gloves:  (Half-Life: Alyx gravity gloves) every frame each free hand's aim ray, with a small cone around it, looks
//            for an item up to [grab] pull_range away. The one it finds is highlighted: a tick in the hand and the
//            laser changes colour. Squeeze the grip: within [grab] reach the item is taken at once; further away it
//            is locked on (a firmer pulse, the laser turns amber). Flick the hand back toward you and it flies on an
//            arc into the hand, steering into it over the last metre; keep the grip closed (or close it as it
//            arrives) to catch it. Let go to drop it, let go mid-swing to throw it, and let go over a shoulder or
//            behind your back to put it in the inventory (the server's ToInventory, through Lua).
// [grab] probe = true only logs what a grip would grab; [grab] hold = true picks it up. While enabled a grip belongs
// to grabbing only when the gloves want it (something highlighted, held or arriving); otherwise it keeps its binding
// (the right grip's right-click).
namespace bg3vr::hold {

void install(const Config& config, const ModuleInfo& module);
bool enabled(); // probe or hold
bool hold_on(); // picking up is on (not just the probe)
// Picking things up on / off at runtime (the VR menu); [grab] hold at start-up.
void set_hold(bool on);

// From the headset (presenting thread): grip state, the hand's grip and aim poses in the game world, and the sample
// time in seconds (for the hand's velocity: throws and flicks).
// `in_pack`: the hand is over a shoulder or behind the back (holster zones): letting go of an item there stows it.
void set_hand(std::size_t hand, bool grip, std::optional<vr::RigidPose> grip_pose, std::optional<vr::RigidPose> aim_pose,
              double time_s, bool in_pack = false);

// Any thread. What the hand's gloves are doing, for the laser colour and the grip binding.
enum class Aim : int { None, Highlight, Locked, Holding };
Aim aim_state(std::size_t hand);
// The grip would be used by the gloves if it closed now (something highlighted, held or flying to this hand).
bool wants_grip(std::size_t hand);
// The hand holds an item (the holsters leave its grip alone).
bool holding(std::size_t hand);
// A queued vibration for the hand (amplitude 0..1, seconds), taken once by the headset.
struct Haptic {
    float amplitude = 0.0f;
    float seconds = 0.0f;
};
std::optional<Haptic> take_haptic(std::size_t hand);

// Game thread: grabs, holds and releases.
void on_frame();

// Drop everything without touching the items (a level is unloading).
void forget();

void report();

} // namespace bg3vr::hold
