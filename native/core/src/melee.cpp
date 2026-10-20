#include "bg3vr/melee.hpp"

#include "bg3vr/bridge.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/rayprobe.hpp"
#include "bg3vr/vrplay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>

namespace bg3vr::melee {

namespace {

bool g_enabled = false;
float g_blade = 0.9f;
float g_min_speed = 2.5f;
float g_cooldown = 0.8f;

std::mutex g_mutex;
std::optional<vr::RigidPose> g_aim; // under g_mutex
double g_time = 0.0;                // under g_mutex

// Game thread.
std::optional<vr::Vec3> g_last_tip;
double g_last_time = 0.0;
double g_last_hit = -100.0;
std::uint64_t g_sequence = 0;
std::uint64_t g_swings = 0;
bool g_swinging = false;

vr::Vec3 along(const vr::Vec3& from, const vr::Vec3& dir, float length) {
    return {from[0] + dir[0] * length, from[1] + dir[1] * length, from[2] + dir[2] * length};
}

} // namespace

void install(const Config& config) {
    g_enabled = config.get_bool("combat", "physical", false);
    const auto number = [&](const char* key, float fallback, float lo, float hi) {
        const std::string v = config.get("combat", key);
        return v.empty() ? fallback : std::clamp(std::strtof(v.c_str(), nullptr), lo, hi);
    };
    g_blade = number("blade", 0.9f, 0.2f, 2.5f);
    g_min_speed = number("min_speed", 2.5f, 0.5f, 20.0f);
    g_cooldown = number("cooldown", 0.8f, 0.1f, 5.0f);
    log::info("combat: physical melee {} (blade {:.2f} m, swing speed {:.1f} m/s, cooldown {:.1f} s)",
              g_enabled ? "on" : "off ([combat] physical)", g_blade, g_min_speed, g_cooldown);
}

bool enabled() {
    return g_enabled;
}

void set_hand(std::optional<vr::RigidPose> aim, double time_s) {
    std::scoped_lock lock(g_mutex);
    g_aim = aim;
    g_time = time_s;
}

void on_frame() {
    if (!g_enabled || !vrplay::active()) {
        g_last_tip.reset();
        return;
    }
    std::optional<vr::RigidPose> aim;
    double now = 0.0;
    {
        std::scoped_lock lock(g_mutex);
        aim = g_aim;
        now = g_time;
    }
    if (!aim) {
        g_last_tip.reset();
        return;
    }
    if (now <= g_last_time) {
        return; // no new hand sample since the last view update
    }
    const vr::Vec3 dir = vr::rotate(aim->orientation, {0.0f, 0.0f, 1.0f});
    const vr::Vec3 base = aim->position;
    const vr::Vec3 tip = along(base, dir, g_blade);
    const double dt = now - g_last_time;
    const std::optional<vr::Vec3> last = g_last_tip;
    g_last_tip = tip;
    g_last_time = now;
    if (!last || dt <= 0.0 || dt > 0.1) {
        return;
    }
    const float dx = tip[0] - (*last)[0], dy = tip[1] - (*last)[1], dz = tip[2] - (*last)[2];
    const float speed = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) / dt);
    const bool swinging = speed >= g_min_speed;
    if (swinging && !g_swinging) {
        ++g_swings;
    }
    g_swinging = swinging;
    if (!swinging || now - g_last_hit < g_cooldown) {
        return;
    }
    // The tip's path since the last frame, then the blade itself.
    std::optional<rayprobe::Hit> hit = rayprobe::raycast(*last, tip);
    if (!hit || hit->entity == rayprobe::kNullEntity) {
        hit = rayprobe::raycast(base, tip);
    }
    if (!hit || hit->entity == rayprobe::kNullEntity) {
        return;
    }
    g_last_hit = now;
    ++g_sequence;
    bridge::set_status("hit", std::format("{} {:.3f} {:.3f} {:.3f} {:.2f}", g_sequence, hit->position[0], hit->position[1],
                                          hit->position[2], speed));
    log::info("combat: swing hit at ({:.2f}, {:.2f}, {:.2f}), {:.1f} m/s, entity 0x{:016X}, actor type {} (hit {} sent "
              "to Lua)",
              hit->position[0], hit->position[1], hit->position[2], speed, hit->entity, hit->actor_type, g_sequence);
}

void report() {
    log::info("combat: physical melee {}, {} swings, {} hits", g_enabled ? "on" : "off", g_swings, g_sequence);
}

} // namespace bg3vr::melee
