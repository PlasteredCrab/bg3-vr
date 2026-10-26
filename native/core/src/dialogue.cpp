#include "bg3vr/dialogue.hpp"

#include "bg3vr/camera.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/text.hpp"

#include <safetyhook.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>

namespace bg3vr::dialogue {

namespace {

ModuleInfo g_module;
safetyhook::MidHook g_hook;
safetyhook::MidHook g_switch_hook;
safetyhook::MidHook g_lighting_hook;
// AV v20: TimelineDummyAnimationThrottleSystem 0x1F1B2C0 marks each scene stand-in active (animates) or not;
// with GlobalSwitches+0x132E zero every stand-in is active, otherwise it can be throttled by tests against the
// scene's shot, which in VR has nothing to do with where you look. The byte is the game's setting: it is zero
// only for the duration of the call, only while VR drives the camera ([dialogue] animate_all_actors).
safetyhook::InlineHook g_throttle_hook;
std::atomic<bool> g_animate_all{true};
std::atomic<std::uint64_t> g_throttle_calls{0}, g_throttle_overridden{0};
std::atomic<bool> g_uniform_lighting{true}; // [dialogue] scene_lighting = uniform | shots
std::atomic<std::uint64_t> g_lighting_cleared{0};
bool g_keep_view_in_vr = true;            // [dialogue] keep_view_in_vr
std::atomic<std::uint64_t> g_switches{0}, g_switches_kept{0};
std::array<bool, 256> g_first_person{};  // by timeline type
bool g_only_when_driven = true;           // [dialogue] only_in_vr
std::atomic<std::uint64_t> g_joins{0};
std::atomic<std::uint64_t> g_kept{0};

template <class T>
T peek(std::uintptr_t address, T fallback = {}) {
    T value = fallback;
    if (address) {
        mem::try_copy(&value, address, sizeof(T));
    }
    return value;
}

// Entry of 0x1D5B890: r8 = Timeline*, r9 = timeline data (runs on the game thread, in the ECS update).
void on_player_joined(safetyhook::Context& ctx) {
    const std::uintptr_t timeline = ctx.r8;
    const std::uintptr_t data = ctx.r9;
    const auto gate = peek<std::uint32_t>(data + layout::kDataGate);
    const auto type = peek<std::uint8_t>(data + layout::kDataType, 0xFF);
    const bool driven = camera::controller() != 0 && camera::pose_held();
    const bool want = g_first_person[type] && (driven || !g_only_when_driven);
    const auto before = peek<std::uint8_t>(timeline + layout::kTimelineFreeCam, 0xFF);
    bool kept = false;
    if (want && gate != 0 && before == 0) { // before == 0 also means the byte was readable
        *reinterpret_cast<volatile std::uint8_t*>(timeline + layout::kTimelineFreeCam) = 1;
        kept = true;
        g_kept.fetch_add(1, std::memory_order_relaxed);
    }
    g_joins.fetch_add(1, std::memory_order_relaxed);
    log::info("dialogue: player joins timeline 0x{:X} (data 0x{:X}, gate {}): type {} playback {} instance 0x{:X}; "
              "preview {} gameplay {} combat {}/{}; camera actor 0x{:X} override 0x{:X}; free-cam byte {} -> {}{}",
              timeline, data, gate, type, peek<std::uint8_t>(data + layout::kDataPlayback, 0xFF),
              peek<std::uint32_t>(data + layout::kDataInstance), peek<std::uint8_t>(timeline + layout::kTimelinePreview, 0xFF),
              peek<std::uint8_t>(timeline + layout::kTimelineGameplay, 0xFF),
              peek<std::uint8_t>(timeline + layout::kTimelineCombat, 0xFF),
              peek<std::uint8_t>(timeline + layout::kTimelineCombat + 1, 0xFF),
              peek<std::uintptr_t>(timeline + layout::kTimelineCamera),
              peek<std::uintptr_t>(timeline + layout::kTimelineCamera + 0x10), before,
              peek<std::uint8_t>(timeline + layout::kTimelineFreeCam, 0xFF),
              kept ? " (kept in first person)"
                   : want ? "" : (g_first_person[type] ? " (not driving the camera: game camera)" : " (game camera)"));
}

// Entry of 0x2007570 (agent AV F6): rcx = camera GUID*, dx = player, r8 = Timeline*. Every path that gives the view
// to a scene's camera comes through it (the join, shots, re-activation); with Timeline+0x4C5 set it skips the switch
// (0x2007AE9). While VR drives the camera, every scene type keeps the view, so the headset stays with the body.
void on_camera_switch(safetyhook::Context& ctx) {
    const std::uintptr_t timeline = ctx.r8;
    const bool driven = camera::controller() != 0 && camera::pose_held();
    const auto before = peek<std::uint8_t>(timeline + layout::kTimelineFreeCam, 0xFF);
    bool kept = false;
    if (g_keep_view_in_vr && driven && before == 0) {
        *reinterpret_cast<volatile std::uint8_t*>(timeline + layout::kTimelineFreeCam) = 1;
        kept = true;
        g_switches_kept.fetch_add(1, std::memory_order_relaxed);
    }
    const std::uint64_t n = g_switches.fetch_add(1, std::memory_order_relaxed);
    if (n < 60 || kept) {
        std::uintptr_t ret = 0;
        mem::try_copy(&ret, ctx.rsp, sizeof(ret));
        log::info("dialogue: scene camera switch for player {} timeline 0x{:X} from exe+0x{:X}: free-cam byte {}{}",
                  static_cast<std::int16_t>(ctx.rdx & 0xFFFF), timeline, ret - g_module.base, before,
                  kept ? " -> 1 (the headset keeps the view)" : driven ? "" : " (the mod isn't driving the camera)");
    }
}

constexpr std::uintptr_t kCameraSwitchRva = 0x2007570;

// Agent AY (AY_cutscene_lighting.md): each shot change stores the shot camera as the scene's lighting camera
// (Timeline+0x500, store at 0x2007792, before the +0x4C5 test) and raises +0x510; 0x2002690 then turns on only the
// scene lights linked to that camera and every other scene light off, and 0x2001D50 (TimelinePost, on +0x513) gives
// the character actors the scene's default key / fill / rim lights only while the camera has no linked lights. With
// the cinematic camera the cuts hid it; in VR every cut relit every actor in view. While the headset keeps the view,
// the lighting camera is cleared right after that store (both paths meet at 0x200779A, r13 = Timeline): every shot
// lights the scene the same way, with each actor's default lights, as the game does once a scene has no camera.
constexpr std::uintptr_t kLightingStoredRva = 0x200779A;
constexpr std::size_t kLightingCamera = 0x500, kUpdateCustomLighting = 0x510, kUpdateAutoLighting = 0x513;

void on_dummy_throttle(std::uintptr_t system, std::uintptr_t ctx) {
    std::uintptr_t switches = 0;
    g_throttle_calls.fetch_add(1, std::memory_order_relaxed);
    const bool driven = camera::controller() != 0 && camera::pose_held();
    if (!g_animate_all.load(std::memory_order_relaxed) || !driven ||
        !mem::try_copy(&switches, g_module.base + 0x604B1B8, sizeof(switches)) || !switches) {
        g_throttle_hook.call<void>(system, ctx);
        return;
    }
    auto* flag = reinterpret_cast<std::uint8_t*>(switches + 0x132E);
    const std::uint8_t saved = *flag;
    *flag = 0;
    g_throttle_hook.call<void>(system, ctx);
    *flag = saved;
    if (saved != 0 && g_throttle_overridden.fetch_add(1, std::memory_order_relaxed) == 0) {
        log::info("dialogue: scene stand-ins all animate while VR drives the camera (the game's throttle switch "
                  "GlobalSwitches+0x132E was {}, zero during the throttle update only)", saved);
    }
}

void on_lighting_camera_stored(safetyhook::Context& ctx) {
    const std::uintptr_t timeline = ctx.r13;
    if (!timeline || !g_uniform_lighting.load(std::memory_order_relaxed) || !g_keep_view_in_vr ||
        !(camera::controller() != 0 && camera::pose_held())) {
        return;
    }
    std::array<std::uint8_t, 16> camera_guid{};
    mem::try_copy(camera_guid.data(), timeline + kLightingCamera, camera_guid.size());
    std::memset(reinterpret_cast<void*>(timeline + kLightingCamera), 0, camera_guid.size());
    *reinterpret_cast<volatile std::uint8_t*>(timeline + kUpdateCustomLighting) = 1;
    *reinterpret_cast<volatile std::uint8_t*>(timeline + kUpdateAutoLighting) = 1;
    const std::uint64_t n = g_lighting_cleared.fetch_add(1, std::memory_order_relaxed);
    if (n < 20 || n % 500 == 0) {
        std::uintptr_t switches = 0;
        std::uint8_t auto_lights = 0xFF;
        if (mem::try_copy(&switches, g_module.base + 0x604B1B8, sizeof(switches)) && switches) {
            mem::try_copy(&auto_lights, switches + 0x131A, 1);
        }
        log::info("dialogue: timeline 0x{:X} shot camera {:02X}{:02X}{:02X}{:02X}... as its lighting camera cleared: "
                  "same lighting every shot (actor default lights; GlobalSwitches+0x131A = {}) ({} times)",
                  timeline, camera_guid[0], camera_guid[1], camera_guid[2], camera_guid[3], auto_lights, n + 1);
    }
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    const std::string types = config.get("dialogue", "first_person_types", "1");
    std::string listed;
    for (std::size_t start = 0; start < types.size();) {
        const std::size_t end = types.find_first_of(", ", start);
        const std::string item = types.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (const auto value = text::parse_int(item); value && *value >= 0 && *value < 256) {
            g_first_person[static_cast<std::size_t>(*value)] = true;
            listed += (listed.empty() ? "" : ",") + item;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    g_only_when_driven = config.get_bool("dialogue", "only_in_vr", true);
    g_keep_view_in_vr = config.get_bool("dialogue", "keep_view_in_vr", true);
    configure(config);
    {
        std::array<std::uint8_t, 11> bytes{};
        // 0x2007792 movups [r13+0x500], xmm0 / 0x200779A xor edi, edi; mov [rbp-0x79], rdi
        constexpr std::array<std::uint8_t, 11> expected{0x41, 0x0F, 0x11, 0x85, 0x00, 0x05, 0x00, 0x00, 0x33, 0xFF, 0x48};
        if (mem::try_copy(bytes.data(), module.base + 0x2007792, bytes.size()) && bytes == expected) {
            if (auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(module.base + kLightingStoredRva),
                                                     &on_lighting_camera_stored)) {
                g_lighting_hook = std::move(*h);
            }
        }
        std::array<std::uint8_t, 8> throttle{};
        constexpr std::array<std::uint8_t, 8> throttle_expected{0x40, 0x55, 0x56, 0x57, 0x41, 0x55, 0x48, 0x8D};
        if (mem::try_copy(throttle.data(), module.base + 0x1F1B2C0, throttle.size()) && throttle == throttle_expected) {
            if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(module.base + 0x1F1B2C0),
                                                        reinterpret_cast<void*>(&on_dummy_throttle))) {
                g_throttle_hook = std::move(*h);
            }
        }
        log::info("dialogue: scene stand-in animation throttle exe+0x1F1B2C0 {}{}", g_throttle_hook ? "hooked" : "NOT hooked",
                  g_animate_all.load() ? "; every stand-in animates while VR drives the camera" : "");
        log::info("dialogue: scene lighting camera {} (exe+0x{:X}); {}",
                  g_lighting_hook ? "hooked" : "NOT hooked (bytes differ)", kLightingStoredRva,
                  g_uniform_lighting.load() ? "in VR every shot lights the scene the same way" : "the game's shot lighting");
    }
    {
        std::array<std::uint8_t, 12> bytes{};
        constexpr std::array<std::uint8_t, 12> expected{0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
        if (mem::try_copy(bytes.data(), module.base + kCameraSwitchRva, bytes.size()) && bytes == expected) {
            if (auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(module.base + kCameraSwitchRva), &on_camera_switch)) {
                g_switch_hook = std::move(*h);
                log::info("dialogue: scene camera switches watched at exe+0x{:X}{}", kCameraSwitchRva,
                          g_keep_view_in_vr ? "; in VR every scene keeps the headset's view" : "");
            }
        } else {
            log::warn("dialogue: the scene camera switch 0x{:X} differs in this build; only the join is handled", kCameraSwitchRva);
        }
    }
    const auto site = sig::lookup("timeline_player_joined");
    if (!site) {
        health::set("first-person dialogue", health::State::Unavailable, "signature timeline_player_joined");
        log::warn("dialogue: signature timeline_player_joined not resolved; dialogues use the game camera");
        return;
    }
    auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(*site), &on_player_joined);
    if (!hook) {
        health::set("first-person dialogue", health::State::Unavailable, "hook failed");
        log::error("dialogue: mid-hook failed (error type {})", static_cast<int>(hook.error().type));
        return;
    }
    g_hook = std::move(*hook);
    health::set("first-person dialogue", health::State::Ready);
    log::info("dialogue: hook at exe+0x{:X}; first person for timeline types [{}]{}", *site - module.base,
              listed.empty() ? "none" : listed, g_only_when_driven ? " while the mod drives the camera" : "");
}

void configure(const Config& config) {
    g_animate_all.store(config.get_bool("dialogue", "animate_all_actors", true));
    const bool uniform = text::to_lower(config.get("dialogue", "scene_lighting", "uniform")) != "shots";
    if (g_uniform_lighting.exchange(uniform) != uniform) {
        log::info("dialogue: scene lighting {}", uniform ? "the same every shot" : "the game's per-shot lighting");
    }
}

void report() {
    log::info("dialogue: {} timeline joins, {} kept in first person; {} scene camera switches, {} kept off the view",
              g_joins.load(std::memory_order_relaxed), g_kept.load(std::memory_order_relaxed), g_switches.load(),
              g_switches_kept.load());
    log::info("dialogue: stand-in throttle updates {}, overridden {}", g_throttle_calls.load(), g_throttle_overridden.load());
}

} // namespace bg3vr::dialogue
