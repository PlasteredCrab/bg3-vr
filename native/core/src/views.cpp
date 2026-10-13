#include "bg3vr/views.hpp"

#include "bg3vr/camera.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
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

// Screen layout override (step 5 experiment), applied every frame on the game thread.
enum class Layout { None, SideBySide, Restore };
std::atomic<Layout> g_layout{Layout::None};
std::atomic<std::uint64_t> g_layout_writes{0};

void write_scale_offset(std::uintptr_t state_address, float scale_x, float offset_x) {
    const float scale[2] = {scale_x, 1.0f};
    const float offset[2] = {offset_x, 0.0f};
    std::uint8_t probe = 0;
    if (!mem::try_copy(&probe, state_address, 1)) {
        return; // the view is gone
    }
    std::memcpy(reinterpret_cast<void*>(state_address + state::kScale), scale, sizeof(scale));
    std::memcpy(reinterpret_cast<void*>(state_address + state::kOffset), offset, sizeof(offset));
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
    }    log_state("ReadState ", read_state);
    log_state("WriteState", write_state);
    std::uintptr_t controller = 0;
    mem::try_copy(&controller, read_state + state::kController, sizeof(controller));
    log_matrix_copies(view, controller ? controller : g_controller);
}

} // namespace

std::vector<std::uintptr_t> scan_now(std::uintptr_t controller) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::uintptr_t> hits;
    std::size_t scanned = 0, regions = 0;
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
                if (buffer[i] == controller) {
                    hits.push_back(from + i * sizeof(std::uintptr_t));
                }
            }
        }
    }

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
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    log::info("views: scanned {} MB in {} regions in {:.1f} s: {} pointers to controller 0x{:X}, {} render view(s)",
              scanned >> 20, regions, seconds, hits.size(), controller, views.size());
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

void set_layout(bool side_by_side) {
    g_layout.store(side_by_side ? Layout::SideBySide : Layout::Restore, std::memory_order_relaxed);
    const auto views = found();
    log::info("views: layout {} for {} view(s){}", side_by_side ? "side by side" : "full screen", views.size(),
              views.size() < 2 && side_by_side ? " (needs 2: !vr_split on, wait a few seconds; scanning again now)" : "");
}

void on_frame() {
    const Layout layout = g_layout.load(std::memory_order_relaxed);
    if (layout == Layout::None) {
        return;
    }
    const auto views = found();
    for (std::size_t i = 0; i < views.size() && i < 2; ++i) {
        const bool split = layout == Layout::SideBySide && views.size() >= 2;
        for (const std::uintptr_t state_address : {views[i], views[i] + state::kSize}) {
            write_scale_offset(state_address, split ? 0.5f : 1.0f, split ? 0.5f * static_cast<float>(i) : 0.0f);
        }
    }
    g_layout_writes.fetch_add(1, std::memory_order_relaxed);
    if (layout == Layout::Restore) {
        g_layout.store(Layout::None, std::memory_order_relaxed); // written once
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
