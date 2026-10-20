#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/vr_math.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Holsters on the body: reach over a shoulder, to a hip or behind your back and squeeze the grip. The zones are
// fixed to the body (the head's position and facing, yaw only, in tracked space), so they move with you. The core
// only says which zone a grip closed or opened in; the Lua side decides what that means for the character's
// equipment (draw the weapon kept there, open the inventory, ...), through to_lua requests:
//   holster grab <zone> <hand>   the grip closed in a zone and the hand pulled out of it (drawing); zones that hold
//                                no weapon (the back: inventory, the chest: light) fire as soon as the grip closes
//   holster stow <zone> <hand>   a hand that pulled something out carried it into a zone and opened there
//   holster tap <zone> <hand>    the grip closed and opened again without leaving the zone
// each followed by what the zone holds ([holster] <zone> = melee | ranged | inventory | light | none).
// Zones: shoulder_right, shoulder_left, hip_right, hip_left, back, chest.
namespace bg3vr::holster {

enum class Zone : int { None = -1, ShoulderRight, ShoulderLeft, HipRight, HipLeft, Back, Chest, kCount };
const char* zone_name(Zone zone);

struct Settings {
    bool enabled = true;
    // Body frame (metres from the head; x right, y up, z backwards), sphere centres and radii.
    vr::Vec3 shoulder{0.17f, -0.10f, 0.12f};
    float shoulder_radius = 0.17f;
    vr::Vec3 hip{0.22f, -0.78f, 0.02f};
    float hip_radius = 0.15f;
    vr::Vec3 back{0.0f, -0.40f, 0.20f};
    float back_radius = 0.13f;
    vr::Vec3 chest{0.0f, -0.33f, -0.10f};
    float chest_radius = 0.09f;
    // What each zone holds ([holster] <zone> = melee | ranged | inventory | light | none), passed to Lua with the
    // event. Zones that don't hold a weapon act as soon as the grip closes (nothing to pull out).
    std::array<std::string, static_cast<std::size_t>(Zone::kCount)> holds{"melee", "ranged", "melee", "melee", "inventory", "light"};
};
Settings load_settings(const Config& config);

// The zone a hand at `hand` (tracked space) is in, for a head at `head` (tracked space). Pure; unit-tested.
Zone classify(const Settings& settings, const vr::RigidPose& head, const vr::Vec3& hand);

struct Hand {
    bool tracked = false;
    vr::Vec3 position{};
    bool grip = false;
};

struct Event {
    enum class Kind { Grab, Stow, Tap } kind = Kind::Grab;
    Zone zone = Zone::None;
    int hand = 0; // 0 left, 1 right
};

struct Output {
    std::vector<Event> events;
    float haptic[2] = {0.0f, 0.0f}; // a tick when a hand enters a zone, a pulse on grab / stow
    bool grip_taken[2] = {false, false}; // the grip belongs to a holster this frame (don't grab world items with it)
};

// The per-frame state machine (pure; unit-tested).
class Tracker {
public:
    Output update(const Settings& settings, const vr::RigidPose& head, const Hand (&hands)[2]);

private:
    Zone inside_[2] = {Zone::None, Zone::None};
    bool grip_[2] = {false, false};
    Zone armed_[2] = {Zone::None, Zone::None};   // the grip closed in this zone and the hand hasn't left it yet
    Zone carrying_[2] = {Zone::None, Zone::None}; // pulled out of this zone, grip still closed
};

void install(const Config& config);
bool enabled();
// Presenting thread, once per headset frame while VR play is on: posts the events as to_lua requests.
Output on_frame(const vr::RigidPose& head, const Hand (&hands)[2]);
void report();

} // namespace bg3vr::holster
