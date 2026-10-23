#include "bg3vr/holster.hpp"

#include "bg3vr/bridge.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/vrplay.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <format>

namespace bg3vr::holster {

namespace {

Settings g_settings;
std::atomic<bool> g_enabled{true};
Tracker g_tracker;
std::atomic<std::uint64_t> g_events[3]{}; // grab, stow, tap

float distance(const vr::Vec3& a, const vr::Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

vr::Vec3 mirrored(const vr::Vec3& v) {
    return {-v[0], v[1], v[2]};
}

} // namespace

const char* zone_name(Zone zone) {
    switch (zone) {
    case Zone::ShoulderRight:
        return "shoulder_right";
    case Zone::ShoulderLeft:
        return "shoulder_left";
    case Zone::HipRight:
        return "hip_right";
    case Zone::HipLeft:
        return "hip_left";
    case Zone::Back:
        return "back";
    case Zone::Chest:
        return "chest";
    default:
        return "none";
    }
}

Settings load_settings(const Config& config) {
    Settings s;
    s.enabled = config.get_bool("holster", "enabled", true);
    const auto number = [&](const char* key, float fallback, float lo, float hi) {
        const std::string v = config.get("holster", key);
        return v.empty() ? fallback : std::clamp(std::strtof(v.c_str(), nullptr), lo, hi);
    };
    s.shoulder_radius = number("shoulder_radius", s.shoulder_radius, 0.05f, 0.5f);
    s.hip_radius = number("hip_radius", s.hip_radius, 0.05f, 0.5f);
    s.back_radius = number("back_radius", s.back_radius, 0.05f, 0.5f);
    s.chest_radius = number("chest_radius", s.chest_radius, 0.0f, 0.3f);
    s.hip[1] = -number("hip_drop", -s.hip[1], 0.3f, 1.3f);
    s.hip[0] = number("hip_width", s.hip[0], 0.05f, 0.5f);
    for (int z = 0; z < static_cast<int>(Zone::kCount); ++z) {
        const std::string v = text::to_lower(config.get("holster", zone_name(static_cast<Zone>(z))));
        if (v == "melee" || v == "ranged" || v == "inventory" || v == "light" || v == "none") {
            s.holds[static_cast<std::size_t>(z)] = v;
        }
    }
    return s;
}

Zone classify(const Settings& settings, const vr::RigidPose& head, const vr::Vec3& hand) {
    // The body frame: the head's facing (yaw only); x right, y up, z backwards (OpenXR: -Z is forward).
    const vr::Vec3 forward = vr::rotate(head.orientation, {0.0f, 0.0f, -1.0f});
    const float yaw = std::atan2(-forward[0], -forward[2]);
    const vr::Quat unyaw{0.0f, -std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    const vr::Vec3 local = vr::rotate(unyaw, {hand[0] - head.position[0], hand[1] - head.position[1], hand[2] - head.position[2]});
    struct Candidate {
        Zone zone;
        vr::Vec3 centre;
        float radius;
    };
    const Candidate candidates[] = {
        {Zone::ShoulderRight, settings.shoulder, settings.shoulder_radius},
        {Zone::ShoulderLeft, mirrored(settings.shoulder), settings.shoulder_radius},
        {Zone::HipRight, settings.hip, settings.hip_radius},
        {Zone::HipLeft, mirrored(settings.hip), settings.hip_radius},
        {Zone::Back, settings.back, settings.back_radius},
        {Zone::Chest, settings.chest, settings.chest_radius},
    };
    Zone best = Zone::None;
    float best_fraction = 1.0f;
    for (const Candidate& c : candidates) {
        const float fraction = distance(local, c.centre) / c.radius;
        if (c.radius <= 0.0f || fraction >= best_fraction) {
            continue;
        }
        // Over the shoulder means behind the face: not a hand held up in front of the shoulder.
        if ((c.zone == Zone::ShoulderRight || c.zone == Zone::ShoulderLeft) && local[2] < -0.02f) {
            continue;
        }
        best = c.zone;
        best_fraction = fraction;
    }
    return best;
}

Output Tracker::update(const Settings& settings, const vr::RigidPose& head, const Hand (&hands)[2]) {
    Output out;
    for (int h = 0; h < 2; ++h) {
        const Hand& hand = hands[h];
        const Zone zone = hand.tracked ? classify(settings, head, hand.position) : Zone::None;
        const bool grip = hand.tracked && hand.grip;
        if (zone != inside_[h] && zone != Zone::None && (!grip || carrying_[h] != Zone::None)) {
            out.haptic[h] = std::max(out.haptic[h], 0.12f); // you feel the holster under your hand
        }
        if (grip && !grip_[h]) {
            // Closed: in a zone, take hold of what is kept there. Zones without a weapon act at once.
            const std::string& holds = zone != Zone::None ? settings.holds[static_cast<std::size_t>(zone)] : std::string();
            if (zone != Zone::None && holds != "melee" && holds != "ranged") {
                out.events.push_back({Event::Kind::Grab, zone, h});
                out.haptic[h] = 0.4f;
            } else if (zone != Zone::None) {
                armed_[h] = zone;
                out.haptic[h] = 0.3f;
            } else {
                holding_[h] = true; // closed in the open: holding whatever is in the hand
            }
        } else if (grip && armed_[h] != Zone::None && zone != armed_[h]) {
            // Pulled out of the zone with the grip closed: drawn.
            out.events.push_back({Event::Kind::Grab, armed_[h], h});
            out.haptic[h] = 0.5f;
            carrying_[h] = armed_[h];
            armed_[h] = Zone::None;
        } else if (!grip && grip_[h]) {
            // Opened.
            if (armed_[h] != Zone::None) {
                out.events.push_back({Event::Kind::Tap, armed_[h], h});
                out.haptic[h] = 0.25f;
            } else if ((carrying_[h] != Zone::None || holding_[h]) && zone != Zone::None &&
                       (settings.holds[static_cast<std::size_t>(zone)] == "melee" ||
                        settings.holds[static_cast<std::size_t>(zone)] == "ranged")) {
                out.events.push_back({Event::Kind::Stow, zone, h});
                out.haptic[h] = 0.4f;
            }
            armed_[h] = Zone::None;
            carrying_[h] = Zone::None;
            holding_[h] = false;
        }
        out.grip_taken[h] = grip && (armed_[h] != Zone::None || carrying_[h] != Zone::None ||
                                     (zone != Zone::None && settings.holds[static_cast<std::size_t>(zone)] != "melee" &&
                                      settings.holds[static_cast<std::size_t>(zone)] != "ranged"));
        inside_[h] = zone;
        grip_[h] = grip;
    }
    return out;
}

void install(const Config& config) {
    g_settings = load_settings(config);
    g_enabled.store(g_settings.enabled, std::memory_order_relaxed);
    health::set("holsters", g_settings.enabled ? health::State::Ready : health::State::Off, "[holster] enabled");
    log::info("holster: {} (shoulders r {:.2f} m, hips r {:.2f} m {:.2f} m below the eyes, back r {:.2f} m)",
              g_settings.enabled ? "on" : "off ([holster] enabled)", g_settings.shoulder_radius, g_settings.hip_radius,
              -g_settings.hip[1], g_settings.back_radius);
}

Zone zone_of(const vr::RigidPose& head, const vr::Vec3& hand) {
    return classify(g_settings, head, hand);
}

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void set_enabled(bool on) {
    if (g_enabled.exchange(on) != on) {
        log::info("holster: {}", on ? "on" : "off");
    }
}

void reset() {
    g_tracker.reset();
}

Output on_frame(const vr::RigidPose& head, const Hand (&hands)[2]) {
    if (!enabled()) {
        return {};
    }
    Output out = g_tracker.update(g_settings, head, hands);
    for (const Event& e : out.events) {
        const char* kind = e.kind == Event::Kind::Grab ? "grab" : e.kind == Event::Kind::Stow ? "stow" : "tap";
        g_events[static_cast<int>(e.kind)].fetch_add(1, std::memory_order_relaxed);
        bridge::push_request(std::format("holster {} {} {} {}", kind, zone_name(e.zone), e.hand == 0 ? "left" : "right",
                                         g_settings.holds[static_cast<std::size_t>(e.zone)]));
        log::info("holster: {} {} with the {} hand", kind, zone_name(e.zone), e.hand == 0 ? "left" : "right");
        if (e.kind == Event::Kind::Grab && g_settings.holds[static_cast<std::size_t>(e.zone)] == "inventory") {
            // The game's default inventory key (Engine.pak PlayerProfiles/Eoc/keyboard_1.json: I; agent L 1.5).
            const bool ok = vrplay::tap_key("i");
            log::info("holster: inventory key {}", ok ? "pressed" : "NOT pressed (VR play off or input unavailable)");
        }
    }
    return out;
}

void report() {
    log::info("holster: {} grab(s), {} stow(s), {} tap(s)", g_events[0].load(), g_events[1].load(), g_events[2].load());
}

} // namespace bg3vr::holster
