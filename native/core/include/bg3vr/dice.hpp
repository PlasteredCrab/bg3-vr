#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/vr_math.hpp"

#include <cstdint>
#include <optional>

// Physical dice, the roll side (agent AQ, AQ_dice_rolls.md): the game's ActiveRoll screen (skill and ability checks)
// followed through its view-model gui::DCActiveRoll. Its per-frame update 0x1548FC0 (vtable 0x55B3CD8 + 0x58,
// `void(dc, ctx)`) is detoured; the phase is the u32 at +0x7A8 (0 intro, 1 waiting for the roll, 2 waiting for a
// reroll, 3 rolling (the UI die animates), 4 stopping, 5 result ready). The game's own "Roll" button handler 0x15475D0
// (UserTriggeredNextPhaseCommand) is what a physical throw calls: in phase 1 it sends the roll request (net message
// 0xEA, action 0) and the server rolls by the normal rules; in phase 3 the same call is the game's own skip (action 5).
// The result is in the view-model when the phase turns 3 (FinalResult +0x880, RolledNumber1/2 +0x838 / +0x850, total
// +0xD58, success +0x8E0), about 1.9 s (3.2 s with advantage) before the UI die shows it. [dice] physical.
namespace bg3vr::dice {

void install(const Config& config, const ModuleInfo& module);
void configure(const Config& config);
bool enabled();

struct Roll {
    int phase = -1;            // the view-model's phase (see above); -1: no roll screen
    int target = 0;            // the d20 face needed (TargetNumber)
    bool can_respond = false;  // the screen takes commands (CanRespondToCommands)
    // From phase 3 on:
    bool has_result = false;
    int face = 0;              // the natural d20 the roll keeps (FinalResult)
    int dice[2] = {0, 0};      // both natural dice (advantage / disadvantage), random order
    int total = 0;
    bool success = false;
    std::uint64_t serial = 0;  // a new roll screen gets a new serial
};
// Any thread: the current roll screen, if one is open.
std::optional<Roll> current();

// Any thread: the physical die was thrown (rolls, like pressing "Roll") / came to rest (skips the UI's own animation).
// Taken by the next view-model update; ignored when the screen isn't in the matching phase.
void request_roll();
void request_skip();

// ---- The physical die (agent AR, AR_dice_visual.md): a client-only clone of the game's UI d20 model
// (a61032d1-98f3-6d0b-55f0-75e62b623f26) in the level scene, created, moved and removed after the paperdoll-manager
// update (where the game builds its own clones); its tumble is the mod's own (gravity, bounces on the game's raycast,
// then it rolls onto the face the game rolled). No physics component: the laser and the game's picking don't see it.

// Presenting thread: the pointing hand's grip and its world pose (game convention), for holding and throwing the die.
void set_hand(bool grip, std::optional<vr::RigidPose> grip_pose, double time_s);
// The grip belongs to the die (it is in the hand or was just thrown): no gloves, no right-click.
bool wants_grip();
// The game's own roll animation should stay hidden: the physical die is showing the roll.
bool hide_ui();
// Game thread, after the paperdoll-manager update.
void on_manager_update();
// Drop the die without touching it (a level is unloading).
void forget();

void report();

} // namespace bg3vr::dice
