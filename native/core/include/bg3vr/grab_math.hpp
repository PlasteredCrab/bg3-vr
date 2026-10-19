#pragma once

#include "bg3vr/vr_math.hpp"

#include <array>
#include <cstddef>
#include <optional>

// Math for physical interaction (docs/design/physical-interaction.md): holding a body by driving its velocity
// toward the hand, throw velocity on release, and the gravity-glove pull. Everything is in one space (the
// game world: metres, +Y up), with vr::Quat rotations; nothing here touches the game.
namespace bg3vr::grab {

struct Velocity {
    vr::Vec3 linear{};
    vr::Vec3 angular{}; // axis * radians per second
};

struct DriveParams {
    float follow = 0.7f;            // fraction of the remaining error closed per step (0..1]
    float max_linear = 20.0f;       // m/s
    float max_angular = 30.0f;      // rad/s
    float teleport_distance = 1.5f; // beyond this error the hold has failed (snag): report it
};

// Lower follow for heavier bodies: `mass` kg against `reference_mass` (follow halves there), clamped to
// [min_follow, base_follow].
float follow_for_mass(float base_follow, float mass, float reference_mass = 20.0f, float min_follow = 0.1f);

struct Drive {
    Velocity velocity;
    float position_error = 0.0f; // metres, before this step
    float angle_error = 0.0f;    // radians
    bool snagged = false;        // position error above teleport_distance
};

// The velocities that move a body from `current` toward `target` within one physics step `dt`, plus the
// hand's own velocity (feed-forward, so a moving hand doesn't drag the object behind).
Drive drive(const vr::RigidPose& current, const vr::RigidPose& target, const Velocity& hand, float dt,
            const DriveParams& params = {});

// The rotation from `from` to `to` (to * inverse(from)) as axis * angle, angle in [0, pi].
vr::Vec3 rotation_vector(const vr::Quat& from, const vr::Quat& to);

// Hand motion history for throwing: the release velocity is the least-squares slope of the positions over
// the last `window` seconds (robust to tracking noise), angular velocity from the first and last rotation
// in the window.
class ThrowTracker {
public:
    void add(double time, const vr::RigidPose& pose);
    void clear() { count_ = 0; }
    std::optional<Velocity> velocity(double window = 0.1) const;

private:
    struct Sample {
        double time = 0.0;
        vr::RigidPose pose;
    };
    static constexpr std::size_t kCapacity = 32;
    std::array<Sample, kCapacity> samples_{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

// Gravity gloves: the initial velocity that flies a body from `from` to `to` under `gravity` (m/s^2,
// pointing down, e.g. 9.81) on an arc peaking `apex` metres above the higher of the two points. Also gives
// the flight time.
struct Launch {
    vr::Vec3 velocity{};
    float time = 0.0f;
};
Launch launch_velocity(const vr::Vec3& from, const vr::Vec3& to, float gravity = 9.81f, float apex = 0.4f);

// A flick: the hand moving toward the player, away from the item, faster than `threshold` m/s along the
// item-to-hand direction (Half-Life: Alyx style pull gesture).
bool is_flick(const vr::Vec3& hand_position, const vr::Vec3& hand_velocity, const vr::Vec3& item_position,
              float threshold = 1.2f);

} // namespace bg3vr::grab
