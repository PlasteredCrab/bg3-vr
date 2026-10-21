#include "bg3vr/combat.hpp"

#include "bg3vr/bodyik.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/picking.hpp"
#include "bg3vr/rayprobe.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/vrplay.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <format>
#include <mutex>

namespace bg3vr::combat {

namespace {

enum class SwingMode { Off, Gaze, Hit };
std::atomic<SwingMode> g_swing{SwingMode::Gaze};
std::atomic<bool> g_bow{true};
std::atomic<float> g_swing_speed{2.0f}; // m/s of the pointing grip
std::atomic<float> g_reach{2.5f};       // metres from the head to a swing's target
std::atomic<float> g_cooldown{0.7f};    // seconds between swings
std::atomic<float> g_full_draw{0.45f};  // metres between the hands at full draw
std::atomic<float> g_min_draw{0.18f};   // a release below this doesn't shoot
std::atomic<float> g_bow_range{40.0f};

std::mutex g_mutex;
std::string g_wield = "none";

// A shot waiting for the game thread: a ray (world) and how far a character may be.
struct Shot {
    vr::Vec3 from{};
    vr::Vec3 direction{};
    float range = 0.0f;
    const char* kind = "";
};
std::optional<Shot> g_pending;
bool g_release_next = false; // the click's release goes in on the next game frame

// Presenting-thread state.
double g_last_swing = -10.0;
bool g_nocked = false;
float g_draw = 0.0f;

std::atomic<std::uint64_t> g_swings{0}, g_shots{0}, g_clicks{0}, g_misses{0};

float length(const vr::Vec3& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

} // namespace

void configure(const Config& config) {
    const std::string swing = text::to_lower(config.get("combat", "swing", config.get_bool("combat", "physical", false) ? "hit" : "gaze"));
    g_swing = swing == "off" ? SwingMode::Off : swing == "hit" ? SwingMode::Hit : SwingMode::Gaze;
    g_bow = text::to_lower(config.get("combat", "bow", "draw")) != "off";
    const auto number = [&](const char* key, float fallback, float lo, float hi) {
        const std::string v = config.get("combat", key);
        return v.empty() ? fallback : std::clamp(std::strtof(v.c_str(), nullptr), lo, hi);
    };
    g_swing_speed = number("swing_speed", 2.0f, 0.5f, 10.0f);
    g_reach = number("reach", 2.5f, 1.0f, 10.0f);
    g_cooldown = number("cooldown", 0.7f, 0.1f, 5.0f);
    g_full_draw = number("full_draw", 0.45f, 0.2f, 1.0f);
    g_min_draw = number("min_draw", 0.18f, 0.05f, 0.6f);
    log::info("combat: swing {} (speed {:.1f} m/s, reach {:.1f} m, cooldown {:.1f} s), bow {} (full draw {:.2f} m)",
              swing == "off" ? "off" : swing == "hit" ? "hit (physical blade contact)" : "gaze (click on who you look at)",
              g_swing_speed.load(), g_reach.load(), g_cooldown.load(), g_bow.load() ? "draw" : "off", g_full_draw.load());
}

void install(const Config& config) {
    configure(config);
    health::set("combat", health::State::Ready, "swing / bow through the game's own click");
}

void set_wield(const std::string& kind) {
    std::scoped_lock lock(g_mutex);
    if (kind != g_wield) {
        log::info("combat: wielding {}", kind);
        g_wield = kind;
    }
    bodyik::set_weapon_drawn(kind != "none"); // the game's grip pose holds the weapon; no extra finger curl
}

Feedback on_headset(const Frame& f) {
    Feedback out;
    std::string wield;
    {
        std::scoped_lock lock(g_mutex);
        wield = g_wield;
    }
    if (!vrplay::active() || vrplay::third_person()) {
        g_nocked = false;
        return out;
    }
    // Melee: a swing clicks on whoever the head looks at.
    if (wield == "melee" && g_swing.load() == SwingMode::Gaze && f.head_world && f.pointing_speed >= g_swing_speed.load() &&
        f.time_s - g_last_swing >= g_cooldown.load()) {
        g_last_swing = f.time_s;
        const vr::Vec3 forward = vr::rotate(f.head_world->orientation, {0.0f, 0.0f, 1.0f}); // game convention: +Z
        // Start a little ahead of the eyes so the player's own body isn't the first hit.
        const vr::Vec3 from{f.head_world->position[0] + forward[0] * 0.4f, f.head_world->position[1] + forward[1] * 0.4f,
                            f.head_world->position[2] + forward[2] * 0.4f};
        std::scoped_lock lock(g_mutex);
        g_pending = Shot{from, forward, g_reach.load(), "swing"};
        g_swings.fetch_add(1, std::memory_order_relaxed);
        out.haptic_pointing = 0.3f;
    }
    // Bow: nock with the pointing grip at the bow hand, draw back, let go.
    if (wield == "bow" && g_bow.load() && f.pointing_tracked && f.other_tracked && f.pointing_world && f.other_world) {
        const vr::Vec3 between{(*f.pointing_tracked)[0] - (*f.other_tracked)[0], (*f.pointing_tracked)[1] - (*f.other_tracked)[1],
                               (*f.pointing_tracked)[2] - (*f.other_tracked)[2]};
        const float distance = length(between);
        if (!g_nocked) {
            if (f.pointing_grip && distance < 0.15f) {
                g_nocked = true;
                g_draw = 0.0f;
                out.haptic_pointing = 0.4f;
                out.haptic_other = 0.2f;
            }
        } else if (f.pointing_grip) {
            const float draw = std::clamp(distance / g_full_draw.load(), 0.0f, 1.0f);
            if (draw >= 1.0f && g_draw < 1.0f) {
                out.haptic_pointing = 0.5f; // full draw
            } else if (std::floor(draw * 5.0f) > std::floor(g_draw * 5.0f)) {
                out.haptic_pointing = 0.08f + draw * 0.15f; // creaking as it draws
            }
            g_draw = draw;
        } else {
            // Released: the arrow flies along drawing hand -> bow hand, from the bow hand.
            if (distance >= g_min_draw.load()) {
                const vr::Vec3 d{f.other_world->position[0] - f.pointing_world->position[0],
                                 f.other_world->position[1] - f.pointing_world->position[1],
                                 f.other_world->position[2] - f.pointing_world->position[2]};
                const float n = length(d);
                if (n > 1e-3f) {
                    const vr::Vec3 dir{d[0] / n, d[1] / n, d[2] / n};
                    const vr::Vec3 from{f.other_world->position[0] + dir[0] * 0.3f, f.other_world->position[1] + dir[1] * 0.3f,
                                        f.other_world->position[2] + dir[2] * 0.3f};
                    std::scoped_lock lock(g_mutex);
                    g_pending = Shot{from, dir, g_bow_range.load(), "arrow"};
                    g_shots.fetch_add(1, std::memory_order_relaxed);
                    out.haptic_other = 0.6f;
                    out.haptic_pointing = 0.3f;
                }
            }
            g_nocked = false;
            g_draw = 0.0f;
        }
    } else {
        g_nocked = false;
    }
    out.pointing_grip_busy = g_nocked;
    return out;
}

void on_frame() {
    if (g_release_next) {
        g_release_next = false;
        vrplay::mouse_left(false);
    }
    std::optional<Shot> shot;
    {
        std::scoped_lock lock(g_mutex);
        shot.swap(g_pending);
    }
    if (!shot) {
        return;
    }
    const vr::Vec3 to{shot->from[0] + shot->direction[0] * shot->range, shot->from[1] + shot->direction[1] * shot->range,
                      shot->from[2] + shot->direction[2] * shot->range};
    // Characters and items, as the picking objects job casts (agent I 1.4).
    auto hit = rayprobe::raycast(shot->from, to);
    if (hit && hit->distance < 0.35f) {
        // That close is the player's own body (looking down at a swing): look past it once.
        const vr::Vec3 past{hit->position[0] + shot->direction[0] * 0.05f, hit->position[1] + shot->direction[1] * 0.05f,
                            hit->position[2] + shot->direction[2] * 0.05f};
        hit = rayprobe::raycast(past, to);
    }
    if (!hit || hit->entity == rayprobe::kNullEntity) {
        g_misses.fetch_add(1, std::memory_order_relaxed);
        log::info("combat: {} at nothing within {:.1f} m; no click", shot->kind, shot->range);
        return;
    }
    // The game's own click, along this ray: the picking sees the target, the default action (or the prepared ability)
    // happens with the game's rolls.
    picking::override_ray(shot->from, shot->direction, 0.25f);
    vrplay::mouse_left(true);
    g_release_next = true;
    g_clicks.fetch_add(1, std::memory_order_relaxed);
    log::info("combat: {} -> click on entity 0x{:X} at {:.2f} m", shot->kind, hit->entity, hit->distance);
}

void report() {
    log::info("combat: {} swings, {} arrows, {} clicks on a target, {} at nothing", g_swings.load(), g_shots.load(),
              g_clicks.load(), g_misses.load());
}

} // namespace bg3vr::combat
