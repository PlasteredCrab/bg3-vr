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
#include <string>

namespace bg3vr::dialogue {

namespace {

ModuleInfo g_module;
safetyhook::MidHook g_hook;
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

void report() {
    log::info("dialogue: {} timeline joins, {} kept in first person", g_joins.load(std::memory_order_relaxed),
              g_kept.load(std::memory_order_relaxed));
}

} // namespace bg3vr::dialogue
