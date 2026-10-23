#include "bg3vr/resolution.hpp"

#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/pattern.hpp"
#include "bg3vr/text.hpp"

#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>

namespace bg3vr::resolution {

namespace {

// 4.1.1.7631656 (agent O; prologues from bg3vr_disasm 2026-10-20).
constexpr std::uintptr_t kOnWindowResize = 0x585510; // renderer vf[0x118]
constexpr const char* kOnWindowResizePrologue = "40 53 41 56 48 83 EC 38 8B 05";
constexpr std::uintptr_t kEventPump = 0x57AAA0;      // SDL event pump (posts the window resizes)
constexpr const char* kEventPumpPrologue =
    "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 98 FE FF FF 48 81 EC 40 02 00 00";
constexpr std::uintptr_t kRenderer = 0x6046F98;      // renderer*
constexpr std::uintptr_t kRendererVtable = 0x5499F58;
constexpr std::uintptr_t kRenderSettings = 0x604AA10; // settings*: +0x10 / +0x14 ScreenWidth / ScreenHeight

struct ResizeEvent {
    HWND window;
    std::int32_t display_width;
    std::int32_t display_height;
    std::int32_t width;
    std::int32_t height;
};
static_assert(sizeof(ResizeEvent) == 0x18);

using ResizeFn = void (*)(std::uintptr_t renderer, ResizeEvent* event);
using PumpFn = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

ModuleInfo g_module;
safetyhook::InlineHook g_resize_hook;
safetyhook::InlineHook g_pump_hook;
bool g_ready = false;
std::atomic<int> g_percent{80};               // [render] vr_resolution; 0 = the game's own size
std::atomic<std::uint32_t> g_eye_width{0}, g_eye_height{0};
std::atomic<HWND> g_window{nullptr};
std::atomic<bool> g_pending{false};           // apply on the next pump
std::atomic<bool> g_suspended{true}; // until first-person VR play starts
std::atomic<bool> g_applied{false};           // the mod's size is in force
std::atomic<std::int32_t> g_width{0}, g_height{0};
std::atomic<std::uint64_t> g_resizes{0}, g_overridden{0};
DWORD g_resize_thread = 0;

// The VR graphics profile (agent X, re-local/ghidra/agents/X_pixel_cost.md 1-4), applied with the VR size and undone
// when the game's own size comes back. In the settings object [0x604AA10]:
//   +0x77 upscaler type (3 = DLSS), +0x78..+0x7B per-upscaler modes (+0x7A DLSS: 3 Balanced, 4 Performance),
//   +0x7C sharpness, +0x80 AntiAliasing, +0x81 CAS: written exactly as the game's DLSS setter 0x406FC70 does;
//   +0x50 DOF (int; 0 = off), +0x59 LensFlareEnabled. The prepares read them every frame; the upscaler change takes
//   effect with the resize that follows on the same pump. The user's file had AntiAliasing 3 and no upscaler: DLAA,
//   DLSS at full resolution for each eye.
constexpr std::size_t kUpscalerBytes = 0x77; // .. +0x81
constexpr std::size_t kUpscalerSize = 0x81 - 0x77 + 1;
constexpr std::size_t kDof = 0x50;
constexpr std::size_t kLensFlare = 0x59;
struct GameSettings {
    std::array<std::uint8_t, kUpscalerSize> upscaler{};
    std::int32_t dof = 0;
    std::uint8_t lens_flare = 0;
};
std::optional<GameSettings> g_saved;          // the user's values while the profile is applied (pump thread)
std::atomic<int> g_dlss_mode{3};              // [render] vr_upscaler: 3 Balanced, 4 Performance, 0 = the game's setting
std::atomic<bool> g_dof{false};               // [render] vr_depth_of_field
std::atomic<bool> g_lens_flare{false};        // [render] vr_lens_flare

void apply_profile(bool vr) {
    const auto settings = mem::read<std::uintptr_t>(g_module.base + kRenderSettings);
    if (!settings) {
        return;
    }
    auto* s = reinterpret_cast<std::uint8_t*>(settings);
    if (vr) {
        if (!g_saved) {
            GameSettings saved;
            std::memcpy(saved.upscaler.data(), s + kUpscalerBytes, kUpscalerSize);
            std::memcpy(&saved.dof, s + kDof, sizeof(saved.dof));
            saved.lens_flare = s[kLensFlare];
            g_saved = saved;
        }
        const int mode = g_dlss_mode.load(std::memory_order_relaxed);
        if (mode > 0) {
            const std::uint8_t previous = s[0x7A];
            s[0x77] = 3;
            s[0x78] = 4;
            s[0x79] = 5;
            s[0x7A] = static_cast<std::uint8_t>(mode);
            s[0x7B] = 6;
            s[0x80] = 0;
            s[0x81] = 0;
            if (previous != mode) {
                const float sharpness = 0.35f;
                std::memcpy(s + 0x7C, &sharpness, sizeof(sharpness));
            }
        } else {
            std::memcpy(s + kUpscalerBytes, g_saved->upscaler.data(), kUpscalerSize);
        }
        const std::int32_t dof = g_dof.load(std::memory_order_relaxed) ? g_saved->dof : 0;
        std::memcpy(s + kDof, &dof, sizeof(dof));
        s[kLensFlare] = g_lens_flare.load(std::memory_order_relaxed) ? g_saved->lens_flare : 0;
        log::info("resolution: VR graphics: {}, depth of field {}, lens flare {} (the game's own: upscaler {} mode {}, "
                  "AntiAliasing {}, DOF {}, lens flare {}; put back when VR stops)",
                  mode == 4 ? "DLSS Performance" : mode == 3 ? "DLSS Balanced" : "the game's upscaling",
                  dof ? "on" : "off", s[kLensFlare] ? "on" : "off", g_saved->upscaler[0], g_saved->upscaler[0x7A - 0x77],
                  g_saved->upscaler[0x80 - 0x77], g_saved->dof, g_saved->lens_flare);
    } else if (g_saved) {
        std::memcpy(s + kUpscalerBytes, g_saved->upscaler.data(), kUpscalerSize);
        std::memcpy(s + kDof, &g_saved->dof, sizeof(g_saved->dof));
        s[kLensFlare] = g_saved->lens_flare;
        g_saved.reset();
        log::info("resolution: the game's own graphics settings are back");
    }
}

bool check(std::uintptr_t rva, const char* prologue, const char* name) {
    const auto pattern = parse_pattern(prologue);
    std::array<std::uint8_t, 64> bytes{};
    if (!pattern || pattern->size() > bytes.size() || !mem::try_copy(bytes.data(), g_module.base + rva, pattern->size())) {
        return false;
    }
    const auto at = find_first(std::span<const std::uint8_t>(bytes.data(), pattern->size()), *pattern);
    if (!at || *at != 0) {
        log::error("resolution: {} at exe+0x{:X} doesn't have the expected prologue (game updated?)", name, rva);
        return false;
    }
    return true;
}

// The back buffer the mod wants: both eyes side by side at the chosen share of the headset's size (even numbers).
bool target(std::int32_t& width, std::int32_t& height) {
    const int percent = g_percent.load(std::memory_order_relaxed);
    const std::uint32_t ew = g_eye_width.load(std::memory_order_relaxed), eh = g_eye_height.load(std::memory_order_relaxed);
    if (percent <= 0 || !ew || !eh || g_suspended.load(std::memory_order_relaxed)) {
        return false;
    }
    const auto eye_w = static_cast<std::int32_t>(std::lround(ew * percent / 100.0)) & ~1;
    const auto eye_h = static_cast<std::int32_t>(std::lround(eh * percent / 100.0)) & ~1;
    width = std::min(2 * eye_w, 16384); // the game's texture size limit
    height = std::min(eye_h, 16384);
    return width >= 256 && height >= 256;
}

std::uintptr_t renderer() {
    const auto r = mem::read<std::uintptr_t>(g_module.base + kRenderer);
    return r && mem::read<std::uintptr_t>(r) == g_module.base + kRendererVtable ? r : 0;
}

void on_resize(std::uintptr_t r, ResizeEvent* event) {
    g_resize_thread = GetCurrentThreadId();
    g_resizes.fetch_add(1, std::memory_order_relaxed);
    std::int32_t width = 0, height = 0;
    const HWND window = g_window.load(std::memory_order_relaxed);
    if (event && window && event->window == window && target(width, height)) {
        log::info("resolution: the game resizes to display {}x{}, output {}x{}; VR keeps {}x{} ({}% of the headset's "
                  "{}x{} per eye; thread {})",
                  event->display_width, event->display_height, event->width, event->height, width, height,
                  g_percent.load(), g_eye_width.load(), g_eye_height.load(), g_resize_thread);
        event->display_width = event->width = width;
        event->display_height = event->height = height;
        g_overridden.fetch_add(1, std::memory_order_relaxed);
        g_width = width;
        g_height = height;
        g_applied = true;
    } else if (event) {
        log::info("resolution: the game resizes to display {}x{}, output {}x{} (thread {})", event->display_width,
                  event->display_height, event->width, event->height, g_resize_thread);
        g_applied = false;
    }
    g_resize_hook.call<void>(r, event);
}

std::atomic<void (*)()> g_main_task{nullptr};
std::atomic<bool> g_main_task_logged{false};

// The game's event pump: a pending change goes through the resize entry from here, where the game itself resizes.
std::uintptr_t on_pump(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) {
    if (const auto task = g_main_task.load(std::memory_order_acquire)) {
        if (!g_main_task_logged.exchange(true)) {
            log::info("resolution: main-thread work (item and body moves) runs from the event pump on thread {}",
                      GetCurrentThreadId());
        }
        task();
    }
    if (g_pending.exchange(false, std::memory_order_relaxed)) {
        const std::uintptr_t r = renderer();
        const HWND window = g_window.load(std::memory_order_relaxed);
        if (r && window) {
            ResizeEvent event{window, 0, 0, 0, 0};
            std::int32_t width = 0, height = 0;
            const bool vr = target(width, height);
            apply_profile(vr); // before the resize, which applies an upscaler change
            if (vr) {
                event.display_width = event.width = width;
                event.display_height = event.height = height;
            } else {
                // Back to the game's own size: the window's client area, the resolution setting.
                RECT client{};
                GetClientRect(window, &client);
                event.display_width = std::max<std::int32_t>(1, client.right);
                event.display_height = std::max<std::int32_t>(1, client.bottom);
                const auto settings = mem::read<std::uintptr_t>(g_module.base + kRenderSettings);
                event.width = settings ? mem::read<std::int32_t>(settings + 0x10) : event.display_width;
                event.height = settings ? mem::read<std::int32_t>(settings + 0x14) : event.display_height;
            }
            log::info("resolution: applying {}x{} (display {}x{})", event.width, event.height, event.display_width,
                      event.display_height);
            on_resize(r, &event); // through the hook's logic, then the game's resize
        } else {
            g_pending = true; // not ready yet; try again on the next pump
        }
    }
    return g_pump_hook.call<std::uintptr_t>(a, b, c, d);
}

} // namespace

bool set_main_thread_task(void (*task)()) {
    if (!g_pump_hook) {
        return false;
    }
    g_main_task.store(task, std::memory_order_release);
    return true;
}

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    configure(config);
    if (!check(kOnWindowResize, kOnWindowResizePrologue, "OnWindowResize") || !check(kEventPump, kEventPumpPrologue, "event pump")) {
        health::set("VR resolution", health::State::Unavailable, "a function didn't match");
        return;
    }
    auto resize = safetyhook::InlineHook::create(reinterpret_cast<void*>(g_module.base + kOnWindowResize),
                                                 reinterpret_cast<void*>(&on_resize));
    auto pump = safetyhook::InlineHook::create(reinterpret_cast<void*>(g_module.base + kEventPump),
                                               reinterpret_cast<void*>(&on_pump));
    if (!resize || !pump) {
        log::error("resolution: couldn't hook the resize entry or the event pump");
        health::set("VR resolution", health::State::Unavailable, "hook failed");
        return;
    }
    g_resize_hook = std::move(*resize);
    g_pump_hook = std::move(*pump);
    g_ready = true;
    health::set("VR resolution", health::State::Ready, std::format("{}% of the headset's per-eye size", g_percent.load()));
    log::info("resolution: hooked the game's resize (exe+0x{:X}) and event pump (exe+0x{:X}); VR resolution {}",
              kOnWindowResize, kEventPump, g_percent.load() > 0 ? std::format("{}% of the headset's", g_percent.load()) : std::string("off (the game's)"));
}

void configure(const Config& config) {
    const std::string upscaler = text::to_lower(config.get("render", "vr_upscaler", "dlss_balanced"));
    const int mode = upscaler == "dlss_performance" ? 4 : upscaler == "game" ? 0 : 3;
    const bool dof = config.get_bool("render", "vr_depth_of_field", false);
    const bool flare = config.get_bool("render", "vr_lens_flare", false);
    if (g_dlss_mode.exchange(mode) != mode || g_dof.exchange(dof) != dof || g_lens_flare.exchange(flare) != flare) {
        g_pending = g_ready;
    }
    const std::string v = config.get("render", "vr_resolution", "80");
    const int percent = std::clamp(static_cast<int>(std::strtol(v.c_str(), nullptr, 10)), 0, 200);
    if (g_percent.exchange(percent) != percent) {
        log::info("resolution: VR resolution {}", percent > 0 ? std::format("{}% of the headset's", percent) : std::string("off"));
        g_pending = g_ready;
    }
}

void set_headset_eye(std::uint32_t width, std::uint32_t height) {
    // Both stored (an || here once skipped the height, so the VR size never applied).
    const bool width_changed = g_eye_width.exchange(width) != width;
    const bool height_changed = g_eye_height.exchange(height) != height;
    if (width_changed || height_changed) {
        log::info("resolution: headset recommends {}x{} per eye", width, height);
        g_pending = g_ready;
    }
}

void set_window(HWND window) {
    if (g_window.exchange(window) != window) {
        g_pending = g_ready;
    }
}

void set_suspended(bool suspended) {
    if (g_suspended.exchange(suspended) != suspended) {
        log::info("resolution: {}", suspended ? "the game's own size (third person)" : "the VR size again");
        g_pending = g_ready;
    }
}

void report() {
    log::info("resolution: {}, {}% -> {} ({} resizes seen, {} kept at the VR size, last on thread {})",
              g_ready ? "ready" : "unavailable", g_percent.load(),
              g_applied.load() ? std::format("{}x{}", g_width.load(), g_height.load()) : std::string("the game's size"),
              g_resizes.load(), g_overridden.load(), g_resize_thread);
}

} // namespace bg3vr::resolution
