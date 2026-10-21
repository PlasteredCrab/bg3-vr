#include "bg3vr/corpse.hpp"

#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/rayprobe.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace bg3vr::corpse {

namespace {

// 4.1.1.7631656 (agents D and Q).
constexpr std::size_t kSceneManager = 0x90;          // main scene -> ragdoll manager
constexpr std::uintptr_t kManagerVtable = 0x5894878;
constexpr std::size_t kManagerLock = 0x18;           // CRITICAL_SECTION
constexpr std::size_t kManagerEntries = 0x40, kManagerCount = 0x4C, kEntrySize = 0x70;
constexpr std::size_t kEntryContainer = 0x00, kEntryTemplate = 0x08, kEntryEntity = 0x10;
constexpr std::size_t kEntryHardLimit = 0x58, kEntryElapsed = 0x60, kEntryFlags = 0x6C;
constexpr std::size_t kContainerWrappers = 0x28, kContainerWrapperCount = 0x34;
constexpr std::size_t kWrapperActor = 0x40;
// Wrapper virtuals (D 4.3).
constexpr std::size_t kWakeUp = 0x68, kMakeDynamic = 0x100, kSetLinearVelocity = 0x120, kGetLinearVelocity = 0x128;
constexpr std::size_t kTemplateCount = 0x08; // template->vf[8](template, category) -> actor count
constexpr std::size_t kGetGlobalPose = 0x90; // PxRigidActor::getGlobalPose(PxTransform* out)

struct PxTransform {
    float q[4];
    float p[3];
};

ModuleInfo g_module;
std::atomic<bool> g_enabled{true};
float g_reach = 0.45f;     // metres from the hand along its aim
float g_gain = 14.0f;      // 1/s: velocity toward the target per metre of error
float g_max_speed = 9.0f;  // m/s
float g_throw = 1.0f;      // the hand's velocity times this on release

struct HandInput {
    bool grab = false;
    std::optional<vr::RigidPose> grip, aim;
    std::optional<vr::Vec3> velocity;
};
std::mutex g_mutex;
std::array<HandInput, 2> g_input{};

struct Held {
    std::uintptr_t container = 0;
    std::uintptr_t wrapper = 0;   // the grabbed body
    int body = -1;
    vr::Vec3 offset{};            // body position - hand position at the grab, in the hand's frame
    std::uint64_t entity = 0;
};
std::array<std::optional<Held>, 2> g_held{};
std::array<bool, 2> g_was_grab{};
std::array<std::atomic<bool>, 2> g_holding{};
std::atomic<std::uint64_t> g_grabs{0}, g_throws{0}, g_misses{0}, g_frames{0};

template <typename R, typename... A>
R vcall(std::uintptr_t object, std::size_t slot, A... args) {
    const auto vtable = mem::read<std::uintptr_t>(object);
    const auto fn = mem::read<std::uintptr_t>(vtable + slot);
    return reinterpret_cast<R (*)(std::uintptr_t, A...)>(fn)(object, args...);
}

std::uintptr_t manager() {
    const std::uintptr_t scene = rayprobe::main_scene();
    const auto m = scene ? mem::read<std::uintptr_t>(scene + kSceneManager) : 0;
    return m && mem::read<std::uintptr_t>(m) == g_module.base + kManagerVtable ? m : 0;
}

// The manager entry holding `container`, with the manager locked for `body` (returns false if none).
template <typename F>
bool with_entry(std::uintptr_t container, F&& body) {
    const std::uintptr_t m = manager();
    if (!m) {
        return false;
    }
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(m + kManagerLock);
    EnterCriticalSection(lock);
    bool found = false;
    const auto entries = mem::read<std::uintptr_t>(m + kManagerEntries);
    const auto count = mem::read<std::uint32_t>(m + kManagerCount);
    for (std::uint32_t k = 0; entries && k < count && k < 4096; ++k) {
        const std::uintptr_t entry = entries + k * kEntrySize;
        if (mem::read<std::uintptr_t>(entry + kEntryContainer) == container) {
            body(entry);
            found = true;
            break;
        }
    }
    LeaveCriticalSection(lock);
    return found;
}

std::optional<vr::Vec3> body_position(std::uintptr_t wrapper) {
    const auto actor = mem::read<std::uintptr_t>(wrapper + kWrapperActor);
    if (!actor) {
        return std::nullopt;
    }
    PxTransform pose{};
    vcall<void*>(actor, kGetGlobalPose, &pose);
    if (!std::isfinite(pose.p[0]) || !std::isfinite(pose.p[1]) || !std::isfinite(pose.p[2])) {
        return std::nullopt;
    }
    return vr::Vec3{pose.p[0], pose.p[1], pose.p[2]};
}

void set_velocity(std::uintptr_t wrapper, const vr::Vec3& v) {
    const float value[3] = {v[0], v[1], v[2]};
    vcall<void>(wrapper, kSetLinearVelocity, value);
}

vr::Quat inverse(const vr::Quat& q) {
    return {-q[0], -q[1], -q[2], q[3]};
}

void try_grab(std::size_t h, const HandInput& in) {
    if (!in.aim || !in.grip) {
        return;
    }
    const vr::Vec3 dir = vr::rotate(in.aim->orientation, {0.0f, 0.0f, 1.0f}); // game convention: forward +Z
    // From a little behind the hand so a body part the hand is already inside of is found too.
    const vr::Vec3 from{in.aim->position[0] - dir[0] * 0.1f, in.aim->position[1] - dir[1] * 0.1f, in.aim->position[2] - dir[2] * 0.1f};
    const vr::Vec3 to{in.aim->position[0] + dir[0] * g_reach, in.aim->position[1] + dir[1] * g_reach,
                      in.aim->position[2] + dir[2] * g_reach};
    // Dynamic shapes, the ragdoll group (0x4004 & 0x4000), excluding only 0x8 (agent Q 6.1).
    const auto hit = rayprobe::raycast(from, to, 5, 0x4000, 0x8);
    if (!hit || !hit->wrapper || !hit->container) {
        g_misses.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uintptr_t container = hit->container;
    const auto wrappers = mem::read<std::uintptr_t>(container + kContainerWrappers);
    const auto count = mem::read<std::uint32_t>(container + kContainerWrapperCount);
    Held held;
    held.container = container;
    held.wrapper = hit->wrapper;
    // The bodies to wake, collected under the manager's lock and woken after it (their methods take the physics scene's
    // lock; the game takes the manager's lock first and then the scene's, so never the other way round here).
    std::vector<std::uintptr_t> to_wake;
    bool was_frozen = false;
    bool is_ragdoll = with_entry(container, [&](std::uintptr_t entry) {
        held.entity = mem::read<std::uint64_t>(entry + kEntryEntity);
        const auto tmpl = mem::read<std::uintptr_t>(entry + kEntryTemplate);
        const int bodies = tmpl ? vcall<int>(tmpl, kTemplateCount, 2) + vcall<int>(tmpl, kTemplateCount, 8) : 0;
        const auto flags = mem::read<std::uint32_t>(entry + kEntryFlags);
        for (int i = 0; i < bodies && i < static_cast<int>(count); ++i) {
            const auto w = mem::read<std::uintptr_t>(wrappers + 8 * static_cast<std::size_t>(i));
            if (!w) {
                continue;
            }
            if (w == hit->wrapper) {
                held.body = i;
            }
            to_wake.push_back(w); // only the bodies: the tether (index `bodies`) stays kinematic
        }
        was_frozen = (flags & 1) != 0;
        *reinterpret_cast<std::uint32_t*>(entry + kEntryFlags) = flags & ~1u;
        *reinterpret_cast<float*>(entry + kEntryElapsed) = 0.0f;
        log::info("corpse: {} hand grabs body {} of {} (wrapper 0x{:X}) of the ragdoll of entity 0x{:X}{} at {:.2f} m; "
                  "freeze limit {:.1f} s",
                  h ? "right" : "left", held.body, bodies, hit->wrapper, held.entity, flags & 1 ? " (was frozen: woken)" : "",
                  hit->distance, mem::read<float>(entry + kEntryHardLimit));
    });
    if (!is_ragdoll || held.body < 0) {
        log::info("corpse: {} hand hit wrapper 0x{:X} (container 0x{:X}) that isn't a ragdoll body", h ? "right" : "left",
                  hit->wrapper, container);
        return;
    }
    for (const std::uintptr_t w : to_wake) {
        if (was_frozen) {
            vcall<void>(w, kMakeDynamic); // frozen bodies were made kinematic
        }
        vcall<void>(w, kWakeUp);
    }
    const auto at = body_position(held.wrapper);
    if (!at) {
        return;
    }
    const vr::Vec3 rel{(*at)[0] - in.grip->position[0], (*at)[1] - in.grip->position[1], (*at)[2] - in.grip->position[2]};
    held.offset = vr::rotate(inverse(in.grip->orientation), rel);
    g_held[h] = held;
    g_holding[h] = true;
    g_grabs.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    g_enabled = config.get_bool("grab", "bodies", true);
    const auto number = [&](const char* key, float fallback, float lo, float hi) {
        const std::string v = config.get("grab", key);
        return v.empty() ? fallback : std::clamp(std::strtof(v.c_str(), nullptr), lo, hi);
    };
    g_reach = number("body_reach", 0.45f, 0.1f, 2.0f);
    g_throw = number("body_throw", 1.0f, 0.0f, 3.0f);
    health::set("grabbing bodies", g_enabled ? health::State::Ready : health::State::Off, "[grab] bodies");
    log::info("corpse: grabbing bodies {} (reach {:.2f} m from the hand, throw x{:.1f}; within ~1 m of where the body lies "
              "for now)",
              g_enabled.load() ? "on" : "off ([grab] bodies)", g_reach, g_throw);
}

void set_enabled(bool on) {
    if (g_enabled.exchange(on) != on) {
        log::info("corpse: grabbing bodies {}", on ? "on" : "off");
    }
}

void set_hand(std::size_t hand, bool grab, std::optional<vr::RigidPose> grip, std::optional<vr::RigidPose> aim,
              std::optional<vr::Vec3> velocity) {
    if (hand > 1) {
        return;
    }
    std::scoped_lock lock(g_mutex);
    g_input[hand] = {grab, grip, aim, velocity};
}

bool holding(std::size_t hand) {
    return hand < 2 && g_holding[hand].load(std::memory_order_relaxed);
}

void on_frame() {
    std::array<HandInput, 2> input;
    {
        std::scoped_lock lock(g_mutex);
        input = g_input;
    }
    for (std::size_t h = 0; h < 2; ++h) {
        const HandInput& in = input[h];
        const bool grab = in.grab && g_enabled.load(std::memory_order_relaxed);
        if (grab && !g_was_grab[h] && !g_held[h]) {
            try_grab(h, in);
        } else if (g_held[h] && (!grab || !in.grip)) {
            // Let go: the hand's velocity carries on (a throw when it moved fast).
            const vr::Vec3 v = in.velocity.value_or(vr::Vec3{0.0f, 0.0f, 0.0f});
            const vr::Vec3 thrown{v[0] * g_throw, v[1] * g_throw, v[2] * g_throw};
            if (with_entry(g_held[h]->container, [](std::uintptr_t) {})) {
                set_velocity(g_held[h]->wrapper, thrown);
                const float speed = std::sqrt(thrown[0] * thrown[0] + thrown[1] * thrown[1] + thrown[2] * thrown[2]);
                if (speed > 1.5f) {
                    g_throws.fetch_add(1, std::memory_order_relaxed);
                }
                log::info("corpse: {} hand lets go ({:.1f} m/s)", h ? "right" : "left", speed);
            }
            g_held[h].reset();
            g_holding[h] = false;
        } else if (g_held[h] && in.grip) {
            // Held: drive the body toward the hand (the joints bring the rest), keep the ragdoll awake.
            const Held& held = *g_held[h];
            const bool alive = with_entry(held.container, [](std::uintptr_t entry) {
                *reinterpret_cast<float*>(entry + kEntryElapsed) = 0.0f;
                *reinterpret_cast<std::uint32_t*>(entry + kEntryFlags) &= ~1u;
            });
            const auto at = alive ? body_position(held.wrapper) : std::nullopt;
            if (!at) {
                log::info("corpse: the held ragdoll is gone; letting go");
                g_held[h].reset();
                g_holding[h] = false;
            } else {
                const vr::Vec3 offset = vr::rotate(in.grip->orientation, held.offset);
                const vr::Vec3 target{in.grip->position[0] + offset[0], in.grip->position[1] + offset[1],
                                      in.grip->position[2] + offset[2]};
                vr::Vec3 v{(target[0] - (*at)[0]) * g_gain, (target[1] - (*at)[1]) * g_gain, (target[2] - (*at)[2]) * g_gain};
                const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (speed > g_max_speed) {
                    for (float& c : v) {
                        c *= g_max_speed / speed;
                    }
                }
                set_velocity(held.wrapper, v);
                g_frames.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_was_grab[h] = grab;
    }
}

void report() {
    log::info("corpse: {} grabs, {} throws, {} grips with no body in reach, {} frames driven", g_grabs.load(), g_throws.load(),
              g_misses.load(), g_frames.load());
}

} // namespace bg3vr::corpse
