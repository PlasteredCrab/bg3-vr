#include "bg3vr/views.hpp"

#include "bg3vr/camera.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/signatures.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace bg3vr::views {

namespace {

// rf::RenderViewState, per the Script Extender (RenderFramework.h); offsets from the state's start.
namespace state {
inline constexpr std::size_t kEnabled = 0x00;          // bool
inline constexpr std::size_t kScale = 0x08;            // vec2
inline constexpr std::size_t kOffset = 0x10;           // vec2
inline constexpr std::size_t kRenderResolution = 0x18; // Viewport (4 floats, then Width, Height, OffsetX, OffsetY)
inline constexpr std::size_t kUpscaledResolution = 0x38;
inline constexpr std::size_t kDisplayResolution = 0x58;
inline constexpr std::size_t kOwnerPlayer = 0x78;      // uint8
inline constexpr std::size_t kController = 0x80;       // CameraController*
inline constexpr std::size_t kSize = 0x88;
} // namespace state

// After WriteState in rf::RenderView, from WriteState's start.
inline constexpr std::size_t kSplitScreenIndex = state::kSize + 0x00; // uint8
inline constexpr std::size_t kViewIndex = state::kSize + 0x04;        // int
inline constexpr std::size_t kDebugName = state::kSize + 0x08;        // MSVC std::string
// ReadState's offset in the view. The Script Extender says 0x2148; in 4.1.1.7631656 the game render
// view's vtable (exe+0x55F42F8) sits 0x2158 before ReadState, and its methods use fields at +0x2150.
inline constexpr std::size_t kReadStateInView = 0x2158;
// How far past the view's start to look for the camera's matrices (GameRenderView::PerCamera).
inline constexpr std::size_t kMatrixSearchBytes = 0x4000;

std::atomic<bool> g_running{false};
std::mutex g_mutex;
std::vector<std::uintptr_t> g_found; // ReadState addresses
std::uintptr_t g_controller = 0;

// Per-view overrides (step 5 experiments), written into both states of the view every frame on the
// game thread. A view's index is its place in found() (by address). Under g_mutex.
struct Override {
    bool active = false;
    bool once = false; // write one frame, then stop (restoring the defaults)
    Placement placement;
    std::optional<bool> enabled;
};
std::array<Override, 4> g_overrides{};

void write_state(std::uintptr_t state_address, const Override& o) {
    std::uint8_t probe = 0;
    if (!mem::try_copy(&probe, state_address, 1)) {
        return; // the view is gone
    }
    const float scale[2] = {o.placement.scale_x, o.placement.scale_y};
    const float offset[2] = {o.placement.offset_x, o.placement.offset_y};
    std::memcpy(reinterpret_cast<void*>(state_address + state::kScale), scale, sizeof(scale));
    std::memcpy(reinterpret_cast<void*>(state_address + state::kOffset), offset, sizeof(offset));
    if (o.enabled) {
        *reinterpret_cast<std::uint8_t*>(state_address + state::kEnabled) = *o.enabled ? 1 : 0;
    }
}
ModuleInfo g_module;

struct Viewport {
    float f[4];
    int width, height, x, y;
};

bool plausible_viewport(const Viewport& v) {
    return v.width >= 16 && v.width <= 16384 && v.height >= 16 && v.height <= 16384 && v.x >= 0 && v.x <= 16384 &&
           v.y >= 0 && v.y <= 16384;
}

// Does `address` look like the start of a RenderViewState?
bool plausible_state(std::uintptr_t address) {
    std::uint8_t bytes[state::kSize];
    if (!mem::try_copy(bytes, address, sizeof(bytes)) || bytes[state::kEnabled] > 1) {
        return false;
    }
    float scale[2];
    Viewport render, display;
    std::memcpy(scale, bytes + state::kScale, sizeof(scale));
    std::memcpy(&render, bytes + state::kRenderResolution, sizeof(render));
    std::memcpy(&display, bytes + state::kDisplayResolution, sizeof(display));
    return std::isfinite(scale[0]) && std::isfinite(scale[1]) && scale[0] >= 0.0f && scale[0] <= 8.0f &&
           scale[1] >= 0.0f && scale[1] <= 8.0f && plausible_viewport(render) && plausible_viewport(display);
}

std::string read_std_string(std::uintptr_t address) {
    struct {
        union {
            char buffer[16];
            std::uintptr_t pointer;
        };
        std::size_t size;
        std::size_t capacity;
    } raw{};
    if (!mem::try_copy(&raw, address, sizeof(raw)) || raw.size > 256 || raw.capacity < raw.size) {
        return "?";
    }
    std::string text(std::min<std::size_t>(raw.size, 64), '\0');
    if (raw.capacity < 16) {
        std::memcpy(text.data(), raw.buffer, text.size());
    } else if (!mem::try_copy(text.data(), raw.pointer, text.size())) {
        return "?";
    }
    return text;
}

std::string describe_viewport(const Viewport& v) {
    return std::format("{}x{}+{}+{}", v.width, v.height, v.x, v.y);
}

void log_state(const char* label, std::uintptr_t address) {
    std::uint8_t bytes[state::kSize];
    if (!mem::try_copy(bytes, address, sizeof(bytes))) {
        log::info("views:   {} 0x{:X}: unreadable", label, address);
        return;
    }
    float scale[2], offset[2];
    Viewport render, upscaled, display;
    std::uintptr_t controller = 0;
    std::memcpy(scale, bytes + state::kScale, sizeof(scale));
    std::memcpy(offset, bytes + state::kOffset, sizeof(offset));
    std::memcpy(&render, bytes + state::kRenderResolution, sizeof(render));
    std::memcpy(&upscaled, bytes + state::kUpscaledResolution, sizeof(upscaled));
    std::memcpy(&display, bytes + state::kDisplayResolution, sizeof(display));
    std::memcpy(&controller, bytes + state::kController, sizeof(controller));
    log::info("views:   {} 0x{:X}: enabled {} scale ({:.3f}, {:.3f}) offset ({:.3f}, {:.3f}) render {} upscaled {} "
              "display {} owner player {} controller 0x{:X}{}",
              label, address, bytes[state::kEnabled], scale[0], scale[1], offset[0], offset[1],
              describe_viewport(render), describe_viewport(upscaled), describe_viewport(display),
              bytes[state::kOwnerPlayer], controller, controller == g_controller ? " (driven)" : "");
}

// Where in the view's first kMatrixSearchBytes the camera's current view matrix (and its inverse)
// appear: the per-camera constants the renderer uses.
void log_matrix_copies(std::uintptr_t view, std::uintptr_t controller) {
    camera::Mat4 view_matrix, inverse;
    if (!controller || !mem::try_copy(view_matrix.data(), controller + camera::layout::kViewMatrix, sizeof(view_matrix)) ||
        !mem::try_copy(inverse.data(), controller + camera::layout::kInvViewMatrix, sizeof(inverse))) {
        return;
    }
    static std::vector<float> buffer(kMatrixSearchBytes / sizeof(float));
    std::size_t readable = kMatrixSearchBytes;
    while (readable >= 0x400 && !mem::try_copy(buffer.data(), view, readable)) {
        readable /= 2;
    }
    if (readable < 0x400) {
        return;
    }
    const auto same = [](const float* a, const camera::Mat4& b) {
        for (std::size_t i = 0; i < 16; ++i) {
            if (std::abs(a[i] - b[i]) > 1e-3f * (std::max)(1.0f, std::abs(b[i]))) {
                return false;
            }
        }
        return true;
    };
    int hits = 0;
    for (std::size_t at = 0; at + 16 <= readable / sizeof(float) && hits < 16; ++at) {
        if (same(&buffer[at], view_matrix)) {
            log::info("views:   view matrix at view+0x{:X}", at * sizeof(float));
            ++hits;
        } else if (same(&buffer[at], inverse)) {
            log::info("views:   inverse view matrix at view+0x{:X}", at * sizeof(float));
            ++hits;
        }
    }
    if (hits == 0) {
        log::info("views:   the camera's current view matrix is not in the view's first 0x{:X} bytes", readable);
    }
}

void log_view(std::uintptr_t read_state) {
    const std::uintptr_t write_state = read_state + state::kSize;
    const std::uintptr_t view = read_state - kReadStateInView;
    std::uintptr_t vtable = 0;
    std::uint8_t split_index = 0;
    int view_index = 0;
    mem::try_copy(&vtable, view, sizeof(vtable));
    mem::try_copy(&split_index, write_state + kSplitScreenIndex, sizeof(split_index));
    mem::try_copy(&view_index, write_state + kViewIndex, sizeof(view_index));
    log::info("views: render view 0x{:X} (ReadState at +0x{:X}): first word {} name '{}' split-screen index {} "
              "view index {}",
              view, kReadStateInView, mem::describe(g_module, vtable), read_std_string(write_state + kDebugName),
              split_index, view_index);
    // The Script Extender's offset for ReadState may be off for this build: list the pointers into
    // the exe's .rdata (vtables) in the 0x2400 bytes before ReadState, nearest last.
    std::vector<std::uintptr_t> before(0x2400 / sizeof(std::uintptr_t));
    if (mem::try_copy(before.data(), read_state - 0x2400, 0x2400)) {
        int listed = 0;
        for (std::size_t i = 0; i < before.size() && listed < 12; ++i) {
            if (mem::in_section(g_module, ".rdata", before[i], sizeof(void*))) {
                log::info("views:   .rdata pointer at ReadState-0x{:X}: {}", 0x2400 - i * sizeof(std::uintptr_t),
                          mem::describe(g_module, before[i]));
                ++listed;
            }
        }
    }
    log_state("ReadState ", read_state);
    log_state("WriteState", write_state);
    std::uintptr_t controller = 0;
    mem::try_copy(&controller, read_state + state::kController, sizeof(controller));
    log_matrix_copies(view, controller ? controller : g_controller);
}

// Every aligned qword equal to `value` in committed private read-write memory (up to 4096).
std::vector<std::uintptr_t> scan_memory(std::uintptr_t value, std::size_t& scanned, std::size_t& regions) {
    std::vector<std::uintptr_t> hits;
    constexpr std::size_t kChunk = 4 << 20;
    std::vector<std::uintptr_t> buffer(kChunk / sizeof(std::uintptr_t));

    MEMORY_BASIC_INFORMATION info{};
    for (std::uintptr_t address = 0x10000; address < 0x7FFFFFFF0000ull && hits.size() < 4096;
         address += info.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) {
            break;
        }
        address = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const DWORD protect = info.Protect & 0xFF;
        if (info.State != MEM_COMMIT || info.Type != MEM_PRIVATE || (info.Protect & PAGE_GUARD) ||
            (protect != PAGE_READWRITE && protect != PAGE_EXECUTE_READWRITE)) {
            continue;
        }
        ++regions;
        // Our own copy buffer holds copies of what it scanned; never scan it.
        const auto own = reinterpret_cast<std::uintptr_t>(buffer.data());
        for (std::size_t at = 0; at < info.RegionSize; at += kChunk) {
            const std::size_t size = std::min(kChunk, info.RegionSize - at);
            const std::uintptr_t from = address + at;
            if ((from < own + kChunk && own < from + size) || !mem::try_copy(buffer.data(), from, size)) {
                continue;
            }
            scanned += size;
            for (std::size_t i = 0; i < size / sizeof(std::uintptr_t); ++i) {
                if (buffer[i] == value) {
                    hits.push_back(from + i * sizeof(std::uintptr_t));
                }
            }
        }
    }
    return hits;
}

} // namespace

std::vector<std::uintptr_t> scan_now(std::uintptr_t controller) {
    const auto start = std::chrono::steady_clock::now();
    std::size_t scanned = 0, regions = 0;
    const std::vector<std::uintptr_t> hits = scan_memory(controller, scanned, regions);

    // Keep the hits that sit at the end of a RenderViewState followed (or preceded) by another one:
    // the ReadState / WriteState pair of a render view.
    std::set<std::uintptr_t> views;
    for (const std::uintptr_t hit : hits) {
        const std::uintptr_t candidate = hit - state::kController;
        if (!plausible_state(candidate)) {
            continue;
        }
        if (plausible_state(candidate + state::kSize)) {
            views.insert(candidate);
        } else if (plausible_state(candidate - state::kSize)) {
            views.insert(candidate - state::kSize);
        }
    }
    // Views of other cameras (a second local player's): every object with the same vtable as the
    // views found, whose states sit where ours do.
    std::size_t by_vtable = 0;
    std::uintptr_t vtable = 0;
    if (!views.empty() && mem::try_copy(&vtable, *views.begin() - kReadStateInView, sizeof(vtable)) &&
        mem::in_section(g_module, ".rdata", vtable, sizeof(void*))) {
        for (const std::uintptr_t hit : scan_memory(vtable, scanned, regions)) {
            const std::uintptr_t read_state = hit + kReadStateInView;
            if (plausible_state(read_state) && plausible_state(read_state + state::kSize) && views.insert(read_state).second) {
                ++by_vtable;
            }
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    log::info("views: scanned {} MB in {} regions in {:.1f} s: {} pointers to controller 0x{:X}, {} render view(s) "
              "({} more found by vtable {})",
              scanned >> 20, regions, seconds, hits.size(), controller, views.size(), by_vtable,
              mem::describe(g_module, vtable));
    for (std::size_t i = 0; i < hits.size() && i < 32; ++i) {
        log::debug("views:   pointer at 0x{:X}", hits[i]);
    }
    {
        std::scoped_lock lock(g_mutex);
        g_found.assign(views.begin(), views.end());
    }
    for (const std::uintptr_t read_state : views) {
        log_view(read_state);
    }
    return {views.begin(), views.end()};
}

namespace {

// The game pipeline's stage-list builder (vtable slot 87, 0x3AC51C0): builds the stages whose ids are
// the bytes in [list[0], list[1]) into `view`. Calling the full setup (slot 88) for an extra view
// crashed the game on the next frame: its tail rewires the special view's output to the view being
// set up, freeing a stage the special view still runs. So only the stage lists are built, the ones
// 0x3AC4EA0 builds for a normal view, including the two (ids 0x45, 0x07) it builds only when
// [[view+0x10]+0x40] is set and otherwise leaves to the special view: the new view must put its own
// image on screen. RVAs of the lists in 4.1.1.7631656 (.rdata).
using BuildStagesFn = void (*)(std::uintptr_t pipeline, std::uintptr_t view, const std::uintptr_t* list);
constexpr std::uint32_t kStageLists[][2] = {
    {0x5899BE0, 0x5899BE7}, // 47 08 48 35 1E 05 0B
    {0x5899BE7, 0x5899BE8}, // 45
    {0x5899BE8, 0x5899BEF}, // 46 36 44 18 17 40 37
    {0x5899BEF, 0x5899BF0}, // 02
    {0x5899BF0, 0x5899BF8}, // 41 03 1B 1A 00 21 3E 3F
    {0x5899BF8, 0x5899BF9}, // 07
};
constexpr std::uint32_t kBuildStagesRva = 0x3AC51C0; // where the lists above are valid

// No C++ objects with destructors in here (SEH): an access violation is caught and reported instead
// of crashing the game (the state afterwards may still be broken).
bool call_build_stages(BuildStagesFn build, std::uintptr_t pipeline, std::uintptr_t view, const std::uintptr_t* list) {
    __try {
        build(pipeline, view, list);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

std::uint32_t stage_count(std::uintptr_t view) {
    std::uint32_t count = 0;
    mem::try_copy(&count, view + 0x34, sizeof(count));
    return count;
}

} // namespace

void build(std::size_t index) {
    const auto views = found();
    if (index != kAutomatic && index >= views.size()) {
        log::warn("views: no view {} ({} found; !vr_views find first)", index, views.size());
        return;
    }
    const auto setup = sig::lookup("game_pipeline_setup");
    if (!setup) {
        log::warn("views: signature game_pipeline_setup not resolved");
        return;
    }
    std::thread([views, index, setup = *setup] {
        // The game pipeline holds the main view at +0xA0 and its special 3-stage view at +0xA8; its
        // vtable has the setup (0x588E50 in 4.1.1.7631656) among its slots.
        std::vector<std::uintptr_t> bases;
        for (const std::uintptr_t read_state : views) {
            bases.push_back(read_state - kReadStateInView);
        }
        std::uintptr_t pipeline = 0;
        std::size_t slot = 0;
        for (const std::uintptr_t base : bases) {
            std::size_t scanned = 0, regions = 0;
            for (const std::uintptr_t hit : scan_memory(base, scanned, regions)) {
                std::uintptr_t pair[2]{};
                std::uintptr_t vtable = 0;
                if (!mem::try_copy(pair, hit, sizeof(pair)) || !mem::try_copy(&vtable, hit - 0xA0, sizeof(vtable)) ||
                    std::find(bases.begin(), bases.end(), pair[1]) == bases.end() ||
                    !mem::in_section(g_module, ".rdata", vtable, sizeof(void*))) {
                    continue;
                }
                std::uintptr_t slots[128]{};
                if (!mem::try_copy(slots, vtable, sizeof(slots))) {
                    continue;
                }
                for (std::size_t s = 0; s < std::size(slots); ++s) {
                    if (slots[s] == setup) {
                        pipeline = hit - 0xA0;
                        slot = s;
                    }
                }
                if (pipeline) {
                    break;
                }
            }
            if (pipeline) {
                break;
            }
        }
        if (!pipeline) {
            log::warn("views: game pipeline not found (no object holding two views at +0xA0/+0xA8 with the setup in "
                      "its vtable)");
            return;
        }
        std::uintptr_t main_view = 0, special_view = 0;
        mem::try_copy(&main_view, pipeline + 0xA0, sizeof(main_view));
        mem::try_copy(&special_view, pipeline + 0xA8, sizeof(special_view));
        std::uintptr_t target = index == kAutomatic ? 0 : bases[index];
        if (index == kAutomatic) {
            for (const std::uintptr_t base : bases) {
                if (base != main_view && base != special_view) {
                    target = base;
                    break;
                }
            }
            if (!target) {
                log::warn("views: every found view is the pipeline's main or special view (!vr_split on, wait, "
                          "!vr_views find)");
                return;
            }
        }
        log::info("views: game pipeline 0x{:X} (setup is vtable slot {}): main view 0x{:X}, special view 0x{:X}",
                  pipeline, slot, main_view, special_view);
        if (target == main_view || target == special_view) {
            log::warn("views: view 0x{:X} is the pipeline's {} view; pick another", target,
                      target == main_view ? "main" : "special");
            return;
        }
        const auto build_stages = sig::lookup("pipeline_build_stages");
        if (!build_stages || *build_stages - g_module.base != kBuildStagesRva) {
            log::warn("views: pipeline_build_stages isn't at exe+0x{:X} (another game build?); the stage lists would be "
                      "wrong, not building",
                      kBuildStagesRva);
            return;
        }
        render::run_on_render_thread([pipeline, target, build = *build_stages, driven = g_controller] {
            const std::uint32_t before = stage_count(target);
            bool ok = true;
            for (const auto& range : kStageLists) {
                const std::uintptr_t list[2] = {g_module.base + range[0], g_module.base + range[1]};
                ok = call_build_stages(reinterpret_cast<BuildStagesFn>(build), pipeline, target, list);
                if (!ok) {
                    break;
                }
            }
            log::info("views: stage lists for view 0x{:X} {}: {} stage(s) before, {} after", target,
                      ok ? "built" : "FAULTED (access violation caught)", before, stage_count(target));
            // A view needs a camera: give it ours if it has none.
            for (const std::size_t state_offset : {kReadStateInView, kReadStateInView + state::kSize}) {
                std::uintptr_t controller = 0;
                if (driven && mem::try_copy(&controller, target + state_offset + state::kController, sizeof(controller)) &&
                    controller == 0) {
                    *reinterpret_cast<std::uintptr_t*>(target + state_offset + state::kController) = driven;
                }
            }
        });
        log::info("views: pipeline setup for view 0x{:X} queued for the render thread", target);
    }).detach();
}
void refs() {
    const auto views = found();
    if (views.empty()) {
        log::info("views: none found yet (!vr_views find)");
        return;
    }
    std::thread([views] {
        for (std::size_t v = 0; v < views.size(); ++v) {
            const std::uintptr_t base = views[v] - kReadStateInView;
            std::size_t scanned = 0, regions = 0;
            const auto hits = scan_memory(base, scanned, regions);
            log::info("views: {} pointer(s) to view {} (0x{:X}):", hits.size(), v, base);
            for (std::size_t i = 0; i < hits.size() && i < 48; ++i) {
                // The neighbours show whether it's inside an array (other pointers, a count) or an object.
                std::uintptr_t around[6]{};
                mem::try_copy(around, hits[i] - 3 * sizeof(std::uintptr_t), sizeof(around));
                std::string note;
                for (std::size_t other = 0; other < views.size(); ++other) {
                    for (const std::uintptr_t word : around) {
                        if (other != v && word == views[other] - kReadStateInView) {
                            note = std::format("  <- next to view {}", other);
                        }
                    }
                }
                log::info("views:   at 0x{:X}: [-3] {:X} [-2] {:X} [-1] {:X} [0] view [+1] {:X} [+2] {:X}{}", hits[i],
                          around[0], around[1], around[2], around[4], around[5], note);
            }
        }
        log::info("views: refs done");
    }).detach();
}

void diff() {
    const auto views = found();
    if (views.size() < 2) {
        log::info("views: diff needs two views (!vr_split on, wait, !vr_views find)");
        return;
    }
    constexpr std::size_t kBytes = 0x3400;
    std::vector<std::uint32_t> a(kBytes / 4), b(kBytes / 4);
    const std::uintptr_t base_a = views[0] - kReadStateInView, base_b = views[1] - kReadStateInView;
    if (!mem::try_copy(a.data(), base_a, kBytes) || !mem::try_copy(b.data(), base_b, kBytes)) {
        log::warn("views: can't read the views");
        return;
    }
    // Small values only (flags, counts, indices, sizes): pointers and floats differ everywhere.
    int shown = 0;
    for (std::size_t i = 0; i < a.size() && shown < 80; ++i) {
        if (a[i] != b[i] && a[i] < 0x10000 && b[i] < 0x10000) {
            log::info("views:   diff at view+0x{:04X}: view 0 = {} (0x{:X}), view 1 = {} (0x{:X})", i * 4, a[i], a[i], b[i],
                      b[i]);
            ++shown;
        }
    }
    log::info("views: diff done ({} small differences shown)", shown);
}

void find(const ModuleInfo& module, std::uintptr_t controller) {
    if (!controller) {
        log::warn("views: no camera controller to look for (attach one first)");
        return;
    }
    if (g_running.exchange(true)) {
        log::info("views: a scan is already running");
        return;
    }
    g_module = module;
    g_controller = controller;
    log::info("views: scanning memory for render views of controller 0x{:X} (takes a few seconds)", controller);
    std::thread([controller] {
        scan_now(controller);
        g_running.store(false);
    }).detach();
}

void set_layout(Layout layout) {
    const auto views = found();
    {
        std::scoped_lock lock(g_mutex);
        if (layout == Layout::Full) {
            for (Override& o : g_overrides) {
                if (o.active) {
                    o = {true, true, Placement{}, true}; // scale 1, offset 0, enabled, for one frame
                }
            }
        } else {
            const bool swap = layout == Layout::SideBySideSwapped;
            g_overrides[swap ? 1 : 0] = {true, false, {0.5f, 1.0f, 0.0f, 0.0f}, std::nullopt};
            g_overrides[swap ? 0 : 1] = {true, false, {0.5f, 1.0f, 0.5f, 0.0f}, std::nullopt};
        }
    }
    log::info("views: layout {} for {} view(s){}",
              layout == Layout::Full ? "full screen" : layout == Layout::SideBySide ? "side by side" : "side by side, swapped",
              views.size(),
              views.size() < 2 && layout != Layout::Full ? " (needs 2: !vr_split on, wait a few seconds; scanning again now)"
                                                         : "");
}

void set_view(std::size_t index, std::optional<Placement> placement, std::optional<bool> enabled) {
    if (index >= std::size(g_overrides)) {
        log::warn("views: no view {} (0..{})", index, std::size(g_overrides) - 1);
        return;
    }
    std::scoped_lock lock(g_mutex);
    Override& o = g_overrides[index];
    if (!o.active) {
        o = {true, false, Placement{}, std::nullopt};
    }
    if (placement) {
        o.placement = *placement;
    }
    if (enabled) {
        o.enabled = enabled;
    }
    log::info("views: view {} -> scale ({:.3f}, {:.3f}) offset ({:.3f}, {:.3f}){}", index, o.placement.scale_x,
              o.placement.scale_y, o.placement.offset_x, o.placement.offset_y,
              o.enabled ? (*o.enabled ? ", enabled" : ", disabled") : "");
}

void on_frame() {
    std::vector<std::uintptr_t> views;
    std::array<Override, std::size(g_overrides)> overrides;
    {
        std::scoped_lock lock(g_mutex);
        bool any = false;
        for (const Override& o : g_overrides) {
            any = any || o.active;
        }
        if (!any) {
            return;
        }
        views = g_found;
        overrides = g_overrides;
        for (Override& o : g_overrides) {
            if (o.once) {
                o = {};
            }
        }
    }
    for (std::size_t i = 0; i < views.size() && i < overrides.size(); ++i) {
        if (overrides[i].active) {
            write_state(views[i], overrides[i]);
            write_state(views[i] + state::kSize, overrides[i]);
        }
    }
}

void dump() {
    const auto views = found();
    if (views.empty()) {
        log::info("views: none found yet (!vr_views find)");
    }
    for (const std::uintptr_t read_state : views) {
        log_view(read_state);
    }
}

std::vector<std::uintptr_t> found() {
    std::scoped_lock lock(g_mutex);
    return g_found;
}

} // namespace bg3vr::views
