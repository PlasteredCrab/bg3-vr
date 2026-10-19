#include "bg3vr/quality.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/skeleton.hpp"
#include "bg3vr/views.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>

namespace bg3vr::quality {

namespace {

ModuleInfo g_module;

// Settings (written by install() before the game thread runs on_frame).
bool g_force_lod0 = false;
bool g_detail_cull = true;
float g_texture_distance = 0.0f;      // 0: leave the game's value
long long g_texture_budget_mb = 0;    // 0: leave the game's value

std::atomic<bool> g_report{false};
std::uint32_t g_last_apply_frame = 0;
std::uint32_t g_last_log_frame = 0;
bool g_logged_cache = false;
constexpr std::uint32_t kApplyEvery = 120; // game frames
constexpr std::uint32_t kLogEvery = 1800;  // ~30 s at 60 fps

template <class T>
std::optional<T> peek(std::uintptr_t address) {
    T value{};
    if (!address || !mem::try_copy(&value, address, sizeof(T))) {
        return std::nullopt;
    }
    return value;
}

template <class T>
bool poke(std::uintptr_t address, T value) {
    // Plain data the game reads each frame; the address was validated by the caller.
    T current{};
    if (!mem::try_copy(&current, address, sizeof(T))) {
        return false;
    }
    if (current != value) {
        *reinterpret_cast<volatile T*>(address) = value;
    }
    return true;
}

std::uintptr_t rva(std::uintptr_t offset) {
    return g_module.base + offset;
}

// The texture cache, vtable-checked. The report's path ends in "+ id*0xF8"; accept the slot itself or a
// pointer stored in it, whichever carries the TextureCache vtable (logged once).
std::uintptr_t texture_cache() {
    const auto root = peek<std::uintptr_t>(rva(layout::kTextureCacheRoot)).value_or(0);
    const auto world = peek<std::uintptr_t>(root + layout::kRootWorld).value_or(0);
    const auto systems = peek<std::uintptr_t>(world + layout::kWorldSystems).value_or(0);
    const auto id = peek<std::uint32_t>(rva(layout::kTextureCacheTypeId));
    if (!systems || !id) {
        return 0;
    }
    const std::uintptr_t slot = systems + static_cast<std::uintptr_t>(*id) * layout::kSystemStride;
    const std::uintptr_t vtable = rva(layout::kTextureCacheVtable);
    std::uintptr_t cache = 0;
    const char* how = "";
    if (peek<std::uintptr_t>(slot).value_or(0) == vtable) {
        cache = slot;
        how = "in place";
    } else if (const auto p = peek<std::uintptr_t>(slot).value_or(0); p && peek<std::uintptr_t>(p).value_or(0) == vtable) {
        cache = p;
        how = "through a pointer";
    }
    if (!g_logged_cache) {
        g_logged_cache = true;
        log::info("quality: texture cache lookup: root 0x{:X} world 0x{:X} systems 0x{:X} id {} slot 0x{:X} -> {}",
                  root, world, systems, *id, slot,
                  cache ? std::format("0x{:X} ({})", cache, how) : std::string("not found (vtable mismatch)"));
    }
    return cache;
}

float lod_fov_factor(float fov) {
    fov = std::clamp(fov, 1e-6f, 3.14159265f);
    return std::tan(0.47996554f) / std::max(std::tan(fov * 0.5f), 1e-8f);
}

void apply() {
    const auto switches = peek<std::uintptr_t>(rva(layout::kGlobalSwitches)).value_or(0);
    if (switches) {
        if (g_force_lod0) {
            poke<std::uint8_t>(switches + layout::kLodTestEnabled, 0);
        }
        if (!g_detail_cull) {
            poke<std::uint8_t>(switches + layout::kDetailCullEnabled, 0);
        }
    }
    if (g_texture_distance > 0.0f || g_texture_budget_mb > 0) {
        if (const std::uintptr_t cache = texture_cache()) {
            if (g_texture_distance > 0.0f) {
                poke<float>(cache + layout::kCacheScale, g_texture_distance);
            }
            if (g_texture_budget_mb > 0) {
                poke<std::int64_t>(cache + layout::kCacheBudget, g_texture_budget_mb * 1024 * 1024);
            }
        }
    }
}

void log_state(const char* why) {
    const auto switches = peek<std::uintptr_t>(rva(layout::kGlobalSwitches)).value_or(0);
    const auto options = peek<std::uintptr_t>(rva(layout::kGraphicsOptions)).value_or(0);
    log::info("quality ({}): LOD test {}, detail cull {}; TextureDetail {}, ModelDetail {}, DetailDistance {}, "
              "AnimationLODDetail {}; overrides: force_lod0 {}, detail_cull {}, texture_distance {}, "
              "texture_budget_mb {}",
              why, peek<std::uint8_t>(switches + layout::kLodTestEnabled).value_or(0xFF),
              peek<std::uint8_t>(switches + layout::kDetailCullEnabled).value_or(0xFF),
              peek<std::uint8_t>(options + 0x36).value_or(0xFF), peek<std::uint8_t>(options + 0x3C).value_or(0xFF),
              peek<std::uint8_t>(options + 0x3D).value_or(0xFF), peek<std::uint8_t>(options + 0x3E).value_or(0xFF),
              g_force_lod0, g_detail_cull, g_texture_distance, g_texture_budget_mb);
    if (const std::uintptr_t cache = texture_cache()) {
        log::info("quality: texture cache budget {} MiB, in use {} MiB, adaptive scale {:.3f}, distance scale {:.2f}",
                  peek<std::int64_t>(cache + layout::kCacheBudget).value_or(0) >> 20,
                  peek<std::int64_t>(cache + layout::kCacheUsage).value_or(0) >> 20,
                  peek<float>(cache + layout::kCacheAdaptive).value_or(-1.0f),
                  peek<float>(cache + layout::kCacheScale).value_or(-1.0f));
    }
    const auto eyes = views::eye_views();
    if (!eyes) {
        log::info("quality: no eye views yet (per-view numbers come after !vr_go / !vr_views create)");
        return;
    }
    for (int i = 0; i < 2; ++i) {
        const std::uintptr_t view = (*eyes)[i];
        const auto camera = peek<std::uintptr_t>(view + layout::kViewCamera).value_or(0);
        const auto fov = peek<float>(camera + layout::kCameraFov);
        const auto w = peek<std::int32_t>(view + layout::kViewViewport).value_or(0);
        const auto h = peek<std::int32_t>(view + layout::kViewViewport + 4).value_or(0);
        if (!fov) {
            log::info("quality: view {} (0x{:X}) has no camera", i, view);
            continue;
        }
        const float lod = lod_fov_factor(*fov);
        const float k = std::clamp(std::sqrt(static_cast<float>(w) * w + static_cast<float>(h) * h) /
                                       std::max(std::tan(*fov * 0.5f), 1e-8f) / 2200.0f,
                                   0.1f, 10.0f);
        log::info("quality: view {} (0x{:X}) camera 0x{:X} fov {:.1f} deg -> LOD band factor {:.3f} (distances x{:.2f}); "
                  "viewport {}x{} -> texture factor {:.2f} (desktop 1920x1080 at 55 deg: 1.92)",
                  i, view, camera, *fov * 57.29578f, lod, std::sqrt(lod), w, h, k);
    }
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    g_force_lod0 = config.get_bool("quality", "force_lod0", false);
    g_detail_cull = config.get_bool("quality", "detail_cull", true);
    const std::string distance = config.get("quality", "texture_distance");
    g_texture_distance = distance.empty() ? 0.0f : std::clamp(std::strtof(distance.c_str(), nullptr), 0.0f, 8.0f);
    g_texture_budget_mb = std::clamp<long long>(config.get_int("quality", "texture_budget_mb", 0), 0, 16384);
    log::info("quality: force_lod0 {}, detail_cull {}, texture_distance {}, texture_budget_mb {} ([quality] in bg3vr.ini)",
              g_force_lod0, g_detail_cull, g_texture_distance, g_texture_budget_mb);
}

void request_report() {
    g_report.store(true, std::memory_order_relaxed);
}

void on_frame() {
    const auto frame = peek<std::uint32_t>(rva(skeleton::layout::kFrameCounter)).value_or(0);
    if (frame - g_last_apply_frame >= kApplyEvery) {
        g_last_apply_frame = frame;
        apply();
    }
    if (g_report.exchange(false, std::memory_order_relaxed)) {
        log_state("report");
    } else if (frame - g_last_log_frame >= kLogEvery) {
        g_last_log_frame = frame;
        log_state("periodic");
    }
}

} // namespace bg3vr::quality
