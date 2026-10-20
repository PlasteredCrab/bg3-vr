#include "bg3vr/rayprobe.hpp"

#include "bg3vr/camera.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/skeleton.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <tuple>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

namespace bg3vr::rayprobe {

namespace {

ModuleInfo g_module;

std::atomic<Source> g_source{Source::Off};
std::atomic<bool> g_report{false};
std::mutex g_mutex;
std::optional<vr::RigidPose> g_hand_aim; // under g_mutex

// Game thread only.
std::uint32_t g_last_frame = 0;
std::uint64_t g_casts = 0;
std::uint64_t g_hits = 0;
std::uint64_t g_failures = 0;
std::uintptr_t g_logged_scene = 0;
constexpr std::uint32_t kEveryFrames = 6;  // ~10 casts a second at 60 fps
constexpr std::uint64_t kFullLineEvery = 50;
constexpr float kRayLength = 10.0f;        // metres

// Query parameter sets seen at real call sites (D_physics.md 8.2, E_items_interaction.md 11) plus a fully permissive one, all cast every
// time, so the log shows which masks see items and characters.
struct Masks {
    const char* name;
    std::uint32_t query_flags, group, exclude;
    std::int32_t context;
    std::uint32_t word0, word3;
};
constexpr std::array<Masks, 4> kMasks{{
    {"solid", 0xF, 0xCE30, 0x80010080, 1, 0xFFFFFFFF, 0xFFFFFFFF},  // 0x1FE35C0
    {"broad", 5, 0xFFFFFFFF, 0xC, 1, 0xFFFFFFFF, 0xFFFFFFFF},       // 0x1957FD0, AddRagdoll node
    {"item", 0xF, 0xFFFF, 0x4280, 1, 0xFFFFFFFF, 0xFFFFFFFF},       // client item ground snap 0x1C7C0F0 (agent E)
    {"all", 0xF, 0xFFFFFFFF, 0, 1, 0xFFFFFFFF, 0xFFFFFFFF},         // permissive
}};
std::array<std::uintptr_t, kMasks.size()> g_last_wrapper{};
std::array<bool, kMasks.size()> g_last_hit{};

using RaycastFn = bool (*)(std::uintptr_t scene, const float* from, const float* to, void* hit,
                           std::uint32_t query_flags, std::uint32_t group, std::uint32_t exclude, std::int32_t context,
                           std::uint32_t word0, std::uint32_t word3, void* filter);

// No C++ objects with destructors in here, so structured exception handling can guard the call.
bool guarded_raycast(RaycastFn fn, std::uintptr_t scene, const float* from, const float* to, void* hit,
                     const Masks& m, void* filter, bool& faulted) {
    faulted = false;
    __try {
        return fn(scene, from, to, hit, m.query_flags, m.group, m.exclude, m.context, m.word0, m.word3, filter);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return false;
    }
}

template <class T>
T peek(std::uintptr_t address, T fallback = {}) {
    T value = fallback;
    mem::try_copy(&value, address, sizeof(T));
    return value;
}

std::uintptr_t rva(std::uintptr_t offset) {
    return g_module.base + offset;
}

// The client level's main scene (path A), checked by vtable; logs paths A and C once per scene change.
std::uintptr_t find_scene() {
    const auto manager = peek<std::uintptr_t>(rva(layout::kLevelManager));
    const auto level = manager ? peek<std::uintptr_t>(manager + layout::kLevelManagerLevel) : 0;
    const auto scene = level ? peek<std::uintptr_t>(level + layout::kLevelScene) : 0;
    const bool valid = scene && peek<std::uintptr_t>(scene) == rva(layout::kMainSceneVtable);
    if (scene != g_logged_scene) {
        g_logged_scene = scene;
        const auto phx = peek<std::uintptr_t>(rva(layout::kPhxGlobal));
        const auto active = phx ? peek<std::uintptr_t>(phx) : 0;
        log::info("ray: level manager 0x{:X}, level 0x{:X}, scene 0x{:X} (vtable {}{}); phx active scene 0x{:X} ({})",
                  manager, level, scene, mem::describe(g_module, scene ? peek<std::uintptr_t>(scene) : 0),
                  valid ? ", the main scene class" : ", NOT the main scene class", active,
                  active == scene ? "same" : "different");
    }
    return valid ? scene : 0;
}

struct Ray {
    vr::Vec3 from{};
    vr::Vec3 to{};
};

std::optional<Ray> current_ray(Source source) {
    vr::RigidPose pose;
    if (source == Source::Camera) {
        const std::uintptr_t controller = camera::controller();
        float m[16];
        if (!controller || !mem::try_copy(m, controller + layout::kCameraToWorld, sizeof(m))) {
            return std::nullopt;
        }
        // Column-major camera-to-world: column 2 is the view direction (+Z), column 3 the position.
        const vr::Vec3 from{m[12], m[13], m[14]};
        const vr::Vec3 dir{m[8], m[9], m[10]};
        return Ray{from, {from[0] + dir[0] * kRayLength, from[1] + dir[1] * kRayLength, from[2] + dir[2] * kRayLength}};
    }
    {
        std::scoped_lock lock(g_mutex);
        if (!g_hand_aim) {
            return std::nullopt;
        }
        pose = *g_hand_aim;
    }
    const vr::Vec3 dir = vr::rotate(pose.orientation, {0.0f, 0.0f, 1.0f});
    return Ray{pose.position,
               {pose.position[0] + dir[0] * kRayLength, pose.position[1] + dir[1] * kRayLength,
                pose.position[2] + dir[2] * kRayLength}};
}

std::string describe_hit(const std::array<std::uint8_t, layout::kHitSize>& hit) {
    float normal[3], position[3], fraction;
    std::uint32_t group0, group1;
    std::uintptr_t wrapper;
    std::memcpy(normal, hit.data() + layout::kHitNormal, sizeof(normal));
    std::memcpy(position, hit.data() + layout::kHitPosition, sizeof(position));
    std::memcpy(&fraction, hit.data() + layout::kHitFraction, sizeof(fraction));
    std::memcpy(&group0, hit.data() + layout::kHitGroup0, sizeof(group0));
    std::memcpy(&group1, hit.data() + layout::kHitGroup1, sizeof(group1));
    std::memcpy(&wrapper, hit.data() + layout::kHitWrapper, sizeof(wrapper));
    const auto container = wrapper ? peek<std::uintptr_t>(wrapper + layout::kWrapperContainer) : 0;
    const auto actor = wrapper ? peek<std::uintptr_t>(wrapper + layout::kWrapperActor) : 0;
    const auto type = actor ? peek<std::uint16_t>(actor + layout::kActorConcreteType, 0xFFFF) : 0xFFFF;
    const char* kind = type == 5 ? "dynamic" : type == 6 ? "static" : type == 13 ? "articulation link" : "?";
    const auto entity = container ? peek<std::uint64_t>(container + layout::kContainerEntity) : 0;
    return std::format("at ({:.3f}, {:.3f}, {:.3f}) {:.2f} m, normal ({:.2f}, {:.2f}, {:.2f}), groups 0x{:X}/0x{:X}; "
                       "wrapper 0x{:X} ({}), actor 0x{:X} type {} {}, container 0x{:X} ({}), entity slot 0x{:016X}{}",
                       position[0], position[1], position[2], fraction * kRayLength, normal[0], normal[1], normal[2],
                       group0, group1, wrapper, mem::describe(g_module, wrapper ? peek<std::uintptr_t>(wrapper) : 0),
                       actor, type, kind, container,
                       mem::describe(g_module, container ? peek<std::uintptr_t>(container) : 0), entity,
                       entity == 0xFFC0000000000000ull ? " (null handle)" : "");
}

} // namespace

void init(const ModuleInfo& module) {
    g_module = module;
}

void set_source(Source source) {
    g_source.store(source, std::memory_order_relaxed);
    log::info("ray: source {}", source == Source::Camera ? "camera (screen centre)"
                                : source == Source::Hand ? "right hand aim"
                                                         : "off");
}

void request_report() {
    g_report.store(true, std::memory_order_relaxed);
}

void set_hand_aim(std::optional<vr::RigidPose> aim) {
    std::scoped_lock lock(g_mutex);
    g_hand_aim = aim;
}

std::optional<vr::RigidPose> hand_aim() {
    std::scoped_lock lock(g_mutex);
    return g_hand_aim;
}

std::optional<Hit> raycast(const vr::Vec3& from, const vr::Vec3& to, std::uint32_t query_flags, std::uint32_t group,
                           std::uint32_t exclude) {
    const std::uintptr_t scene = find_scene();
    const auto slot = peek<std::uintptr_t>(rva(layout::kMainSceneVtable) + layout::kRaycastSlot);
    if (!scene || slot != rva(layout::kRaycastSlotTarget)) {
        return std::nullopt;
    }
    std::array<std::uint8_t, layout::kHitSize> hit{};
    const float no_hit = FLT_MAX;
    std::memcpy(hit.data() + layout::kHitFraction, &no_hit, sizeof(no_hit));
    alignas(16) std::array<std::uint8_t, 0x40> filter{};
    const Masks masks{"api", query_flags, group, exclude, 1, 0xFFFFFFFF, 0xFFFFFFFF};
    bool faulted = false;
    const bool result = guarded_raycast(reinterpret_cast<RaycastFn>(slot), scene, from.data(), to.data(), hit.data(),
                                        masks, filter.data(), faulted);
    std::uintptr_t wrapper = 0;
    std::memcpy(&wrapper, hit.data() + layout::kHitWrapper, sizeof(wrapper));
    if (faulted || !result || !wrapper) {
        return std::nullopt;
    }
    Hit out;
    float fraction = 0.0f;
    std::memcpy(out.normal.data(), hit.data() + layout::kHitNormal, sizeof(float) * 3);
    std::memcpy(out.position.data(), hit.data() + layout::kHitPosition, sizeof(float) * 3);
    std::memcpy(&fraction, hit.data() + layout::kHitFraction, sizeof(fraction));
    const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    out.distance = fraction * std::sqrt(dx * dx + dy * dy + dz * dz);
    out.wrapper = wrapper;
    out.container = peek<std::uintptr_t>(wrapper + layout::kWrapperContainer);
    out.entity = out.container ? peek<std::uint64_t>(out.container + layout::kContainerEntity, kNullEntity) : kNullEntity;
    const auto actor = peek<std::uintptr_t>(wrapper + layout::kWrapperActor);
    out.actor_type = actor ? peek<std::uint16_t>(actor + layout::kActorConcreteType, 0xFFFF) : 0xFFFF;
    return out;
}

std::atomic<bool> g_beam{false};
std::atomic<float> g_beam_range{12.0f};
std::atomic<float> g_beam_distance{-1.0f};

std::uintptr_t main_scene() {
    return find_scene();
}

void set_beam(bool on, float range) {
    g_beam.store(on, std::memory_order_relaxed);
    g_beam_range.store(range, std::memory_order_relaxed);
    if (!on) {
        g_beam_distance.store(-1.0f, std::memory_order_relaxed);
    }
}

std::optional<float> beam_distance() {
    const float d = g_beam_distance.load(std::memory_order_relaxed);
    return d >= 0.0f ? std::optional<float>(d) : std::nullopt;
}

void on_frame() {
    if (g_beam.load(std::memory_order_relaxed)) {
        float best = -1.0f;
        if (const auto aim = hand_aim()) {
            const vr::Vec3 dir = vr::rotate(aim->orientation, {0.0f, 0.0f, 1.0f}); // game convention: forward +Z
            const float range = g_beam_range.load(std::memory_order_relaxed);
            // From 0.3 m ahead of the hand: the player's own body is the first thing the ray meets otherwise (the
            // 2026-10-20 run logged hits at 0.00 m).
            constexpr float kSkip = 0.3f;
            const vr::Vec3 from{aim->position[0] + dir[0] * kSkip, aim->position[1] + dir[1] * kSkip,
                                aim->position[2] + dir[2] * kSkip};
            const vr::Vec3 to{aim->position[0] + dir[0] * range, aim->position[1] + dir[1] * range,
                              aim->position[2] + dir[2] * range};
            for (const auto& [flags, group, exclude] : {std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>{5, 0x820, 0x194280},
                                                        {0xF, 0x4CA20, 0x210080}}) {
                if (const auto hit = raycast(from, to, flags, group, exclude)) {
                    if (best < 0.0f || hit->distance + kSkip < best) {
                        best = hit->distance + kSkip;
                    }
                }
            }
        }
        g_beam_distance.store(best, std::memory_order_relaxed);
    }
    if (g_report.exchange(false, std::memory_order_relaxed)) {
        log::info("ray: {} casts, {} with a hit, {} failed", g_casts, g_hits, g_failures);
    }
    const Source source = g_source.load(std::memory_order_relaxed);
    if (source == Source::Off) {
        return;
    }
    const auto frame = peek<std::uint32_t>(rva(skeleton::layout::kFrameCounter));
    if (frame - g_last_frame < kEveryFrames) {
        return;
    }
    g_last_frame = frame;
    const std::uintptr_t scene = find_scene();
    if (!scene) {
        return;
    }
    const auto slot = peek<std::uintptr_t>(rva(layout::kMainSceneVtable) + layout::kRaycastSlot);
    if (slot != rva(layout::kRaycastSlotTarget)) {
        log::error("ray: scene slot +0x38 is {}, expected exe+0x{:X}; probe off", mem::describe(g_module, slot),
                   layout::kRaycastSlotTarget);
        g_source.store(Source::Off, std::memory_order_relaxed);
        return;
    }
    const auto ray = current_ray(source);
    if (!ray) {
        return;
    }
    ++g_casts;
    const bool full = g_casts % kFullLineEvery == 1;
    if (full) {
        log::info("ray: cast {} from ({:.3f}, {:.3f}, {:.3f}) to ({:.3f}, {:.3f}, {:.3f})", g_casts, ray->from[0],
                  ray->from[1], ray->from[2], ray->to[0], ray->to[1], ray->to[2]);
    }
    const auto fn = reinterpret_cast<RaycastFn>(slot);
    bool any = false;
    for (std::size_t i = 0; i < kMasks.size(); ++i) {
        std::array<std::uint8_t, layout::kHitSize> hit{};
        const float no_hit = FLT_MAX;
        std::memcpy(hit.data() + layout::kHitFraction, &no_hit, sizeof(no_hit));
        alignas(16) std::array<std::uint8_t, 0x40> filter{}; // "no custom filter": a zeroed callable slot
        bool faulted = false;
        const bool result = guarded_raycast(fn, scene, ray->from.data(), ray->to.data(), hit.data(), kMasks[i],
                                            filter.data(), faulted);
        if (faulted) {
            ++g_failures;
            log::error("ray: RaycastClosest faulted ({} masks); probe off", kMasks[i].name);
            g_source.store(Source::Off, std::memory_order_relaxed);
            return;
        }
        std::uintptr_t wrapper = 0;
        std::memcpy(&wrapper, hit.data() + layout::kHitWrapper, sizeof(wrapper));
        const bool hit_something = result && wrapper;
        any = any || hit_something;
        if (full || hit_something != g_last_hit[i] || wrapper != g_last_wrapper[i]) {
            log::info("ray: [{}] {}", kMasks[i].name,
                      hit_something ? describe_hit(hit) : std::format("miss (returned {})", result));
        }
        g_last_hit[i] = hit_something;
        g_last_wrapper[i] = wrapper;
    }
    if (any) {
        ++g_hits;
    }
}

} // namespace bg3vr::rayprobe
