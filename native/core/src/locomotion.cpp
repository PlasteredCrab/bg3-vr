#include "bg3vr/locomotion.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/signatures.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bg3vr::locomotion {

namespace {

ModuleInfo g_module;
std::uintptr_t g_patch_site = 0; // the jz after the controller-mode check
std::array<std::uint8_t, 6> g_original{};
constexpr std::array<std::uint8_t, 6> kNop6{0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};

std::atomic<bool> g_wanted{false};
std::atomic<float> g_stick_x{0.0f};
std::atomic<float> g_stick_y{0.0f};
float g_deadzone = 0.15f;
bool g_invert_forward = false;

// Game thread only.
bool g_patched = false;
bool g_moving = false;
std::atomic<float> g_speed{0.0f};
std::uint64_t g_frames = 0;
std::uint64_t g_moving_frames = 0;

bool write_code(std::uintptr_t address, const std::uint8_t* bytes, std::size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(address), bytes, size);
    VirtualProtect(reinterpret_cast<void*>(address), size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    return true;
}

std::uintptr_t switches() {
    std::uintptr_t gs = 0;
    mem::try_copy(&gs, g_module.base + layout::kGlobalSwitches, sizeof(gs));
    return gs;
}

void write_override(float x, float z, bool enable) {
    const std::uintptr_t gs = switches();
    if (!gs) {
        return;
    }
    *reinterpret_cast<volatile float*>(gs + layout::kOverrideX) = x;
    *reinterpret_cast<volatile float*>(gs + layout::kOverrideZ) = z;
    *reinterpret_cast<volatile std::uint8_t*>(gs + layout::kOverrideEnable) = enable ? 1 : 0;
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    const std::string deadzone = config.get("locomotion", "deadzone");
    if (!deadzone.empty()) {
        g_deadzone = std::clamp(std::strtof(deadzone.c_str(), nullptr), 0.0f, 0.9f);
    }
    g_invert_forward = config.get_bool("locomotion", "invert_forward", false);
    const auto site = sig::lookup("move_controller_mode_check");
    if (!site) {
        log::warn("locomotion: signature move_controller_mode_check not resolved; stick locomotion unavailable");
        return;
    }
    // cmp byte [rip+disp32], 0 (7 bytes) must name the controller-mode byte; then jz rel32 (0F 84).
    std::int32_t disp = 0;
    std::array<std::uint8_t, 6> jz{};
    mem::try_copy(&disp, *site + 2, sizeof(disp));
    mem::try_copy(jz.data(), *site + 7, jz.size());
    const std::uintptr_t operand = *site + 7 + static_cast<std::intptr_t>(disp);
    if (operand != module.base + layout::kControllerModeByte || jz[0] != 0x0F || jz[1] != 0x84) {
        log::warn("locomotion: the check at exe+0x{:X} reads {} (expected exe+0x{:X}) / jz bytes {:02X} {:02X}; stick "
                  "locomotion unavailable",
                  *site - module.base, mem::describe(module, operand), layout::kControllerModeByte, jz[0], jz[1]);
        return;
    }
    g_patch_site = *site + 7;
    g_original = jz;
    log::info("locomotion: controller-mode check at exe+0x{:X}; stick locomotion via the movement override "
              "(deadzone {:.2f}{})",
              *site - module.base, g_deadzone, g_invert_forward ? ", forward inverted" : "");
    set_enabled(config.get_bool("locomotion", "stick", false));
}

void set_enabled(bool on) {
    g_wanted.store(on, std::memory_order_relaxed);
    log::info("locomotion: stick locomotion {} (applied on the next game frame)", on ? "on" : "off");
}

void set_stick(float x, float y) {
    g_stick_x.store(x, std::memory_order_relaxed);
    g_stick_y.store(y, std::memory_order_relaxed);
}

void on_frame() {
    if (!g_patch_site) {
        return;
    }
    const bool wanted = g_wanted.load(std::memory_order_relaxed);
    if (wanted != g_patched) {
        const bool ok = wanted ? write_code(g_patch_site, kNop6.data(), kNop6.size())
                               : write_code(g_patch_site, g_original.data(), g_original.size());
        if (!wanted) {
            write_override(0.0f, 0.0f, false);
            g_moving = false;
            g_speed.store(0.0f, std::memory_order_relaxed);
        }
        if (ok) {
            g_patched = wanted;
        }
        log::info("locomotion: controller-mode check {} at exe+0x{:X}{}", wanted ? "bypassed" : "restored",
                  g_patch_site - g_module.base, ok ? "" : " (FAILED to write)");
        if (!ok) {
            g_wanted.store(g_patched, std::memory_order_relaxed);
        }
    }
    if (!g_patched) {
        return;
    }
    ++g_frames;
    float x = g_stick_x.load(std::memory_order_relaxed);
    float z = g_stick_y.load(std::memory_order_relaxed) * (g_invert_forward ? -1.0f : 1.0f);
    const float length = std::sqrt(x * x + z * z);
    if (length < g_deadzone) {
        x = z = 0.0f;
    } else {
        // Rescale so the speed starts at 0 at the deadzone edge, capped at 1.
        const float scaled = std::min(1.0f, (length - g_deadzone) / (1.0f - g_deadzone));
        x *= scaled / length;
        z *= scaled / length;
    }
    const bool moving = x != 0.0f || z != 0.0f;
    g_speed.store(std::sqrt(x * x + z * z), std::memory_order_relaxed);
    write_override(x, z, moving);
    if (moving) {
        ++g_moving_frames;
    }
    if (moving != g_moving) {
        g_moving = moving;
        log::info("locomotion: {} (stick x {:.2f} forward {:.2f})", moving ? "moving" : "stopped", x, z);
    }
}

float speed() {
    return g_speed.load(std::memory_order_relaxed);
}

void report() {
    log::info("locomotion: stick {}, {} frames, {} moving", g_patched ? "on" : "off", g_frames, g_moving_frames);
}

} // namespace bg3vr::locomotion
