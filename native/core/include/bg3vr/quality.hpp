#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

#include <cstddef>
#include <cstdint>

// Model and texture quality near the player (agent H, re-local/ghidra/agents/H_lod_streaming.md). Logs what
// decides LOD and texture levels for each render view (camera FOV, viewport, the texture cache's budget and
// adaptive scale) and applies opt-in overrides from [quality] in bg3vr.ini on the game thread:
//   force_lod0 = true        GlobalSwitches+0x126 = 0: only LOD-0 meshes draw, no max-distance test
//   detail_cull = false      GlobalSwitches+0x15 = 0: no small-object ("detail distance") culling
//   texture_distance = 2.0   TextureCache+0x2404: multiplies every texture level distance
//   texture_budget_mb = 3072 TextureCache+0x10: the streaming budget (the game caps it at ~1.25 GiB)
// All are plain data the game reads each frame; nothing is hooked.
namespace bg3vr::quality {

// 4.1.1.7631656, all from agent H's report (Confirmed there).
namespace layout {
inline constexpr std::uintptr_t kGlobalSwitches = 0x604B1B8;  // ls::GlobalSwitches*
inline constexpr std::size_t kDetailCullEnabled = 0x15;       // byte, default 1
inline constexpr std::size_t kLodTestEnabled = 0x126;         // byte, default 1
inline constexpr std::uintptr_t kGraphicsOptions = 0x604AA10; // options object*; +0x36 TextureDetail, +0x3C ModelDetail
inline constexpr std::uintptr_t kTextureCacheRoot = 0x6046F98; // [[[root]+0x1D8]+0x30] + [kTextureCacheTypeId]*0xF8
inline constexpr std::size_t kRootWorld = 0x1D8;
inline constexpr std::size_t kWorldSystems = 0x30;
inline constexpr std::uintptr_t kTextureCacheTypeId = 0x6059304;
inline constexpr std::size_t kSystemStride = 0xF8;
inline constexpr std::uintptr_t kTextureCacheVtable = 0x58B18D8;
inline constexpr std::size_t kCacheBudget = 0x10;   // int64 bytes
inline constexpr std::size_t kCacheUsage = 0x18;    // int64 bytes
inline constexpr std::size_t kCacheAdaptive = 0x20; // float, shrinks while over budget
inline constexpr std::size_t kCacheScale = 0x2404;  // float, 1.0
inline constexpr std::size_t kViewCamera = 0x21D8;  // render view -> camera controller
inline constexpr std::size_t kCameraFov = 0x14;     // float radians
inline constexpr std::size_t kViewViewport = 0x2180; // int width, int height (render viewport)
} // namespace layout

void install(const Config& config, const ModuleInfo& module);

// Game thread (camera view update): applies the overrides once the objects exist (re-applied every few
// seconds), and logs the per-view numbers periodically.
void on_frame();

void request_report();

} // namespace bg3vr::quality
