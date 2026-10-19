#include "bg3vr/views.hpp"

#include "bg3vr/camera.hpp"
#include "bg3vr/events.hpp"
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
#include <utility>

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
// The split-screen index byte right after WriteState (0x21E0 + 0x88); outside the part the frame sync copies.
inline constexpr std::size_t kSplitScreenIndexInView = kReadStateInView + 2 * state::kSize;
// How far past the view's start to look for the camera's matrices (GameRenderView::PerCamera).
inline constexpr std::size_t kMatrixSearchBytes = 0x4000;

std::atomic<bool> g_running{false};
std::mutex g_mutex;
std::vector<std::uintptr_t> g_found; // ReadState addresses
std::uintptr_t g_controller = 0;
ModuleInfo g_module;

// Per-view overrides (step 5 experiments), written into both states of the view every frame on the
// game thread. A view's index is its place in found() (by address). Under g_mutex.
struct Override {
    bool active = false;
    bool once = false; // write one frame, then stop (restoring the defaults)
    Placement placement;
    std::optional<bool> enabled;
};
std::array<Override, 4> g_overrides{};

// Giving a view a camera the game's way: GameRenderView vtable slot 10 (0x17F2B30 in 4.1.1.7631656,
// SetCamera(view, controller, keep)) calls 0x3CC90B0, which detaches the old controller (its Exit),
// attaches the new one (its Enter: the gameplay controller keeps a list of its views), writes
// WriteState's controller (+0x2260; the frame sync 0x4072CB0 copies WriteState into ReadState on every
// view of the frame) and updates the controller's aspect from the view. The game calls it from game-side
// code (0x17F7CD0 runs it directly when the view exists, otherwise queues it), so it's done on the game
// thread (on_frame). Under g_mutex.
inline constexpr std::size_t kSetCameraSlot = 10;
inline constexpr std::uintptr_t kSetCameraRva = 0x17F2B30;
using SetCameraFn = void (*)(std::uintptr_t view, std::uintptr_t controller, bool keep);
std::uintptr_t g_pending_camera_view = 0;
std::uintptr_t g_pending_camera = 0;
// The pair a completed build leaves: main view, split view.
std::optional<std::array<std::uintptr_t, 2>> g_eye_views;
std::uintptr_t g_special_view = 0; // the pipeline's special (compositing) view, for dump labels

bool call_set_camera(SetCameraFn fn, std::uintptr_t view, std::uintptr_t controller) {
    __try {
        fn(view, controller, false);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// Giving the split view the level's scene, as the game's set-player-view-scene (0x17F8160) does for a
// player's view: the stage prepare (0x17DA690, used by every view stage) runs a stage only when
// [view+0x2B10] (the view's scene) is set and its byte +8 is set; the split view, a copy of the special
// view, has none, so its 28 stages issued no GPU work (gpu trace, 2026-10-19). 0x17F8160 for a found view:
// 0x3CCC550(view, scene) with scene = [[DAT_146007A28]+0x90]+0x28 (what the add-player path 0x17F5CB0
// passes), then 0x3CC9D30(view, X+0x28, &id, 0.0f, false), releasing id via 0x41950A0([DAT_14604B2E0]+0xC600,
// &id) when set, then 0x3CCAFA0(view, X+0x24, &id, 0.0f, false), X = [[[level+0x18]+8].
using SetSceneFn = void (*)(std::uintptr_t view, std::uintptr_t scene);
using AttachFn = void (*)(std::uintptr_t view, std::uintptr_t what, std::int32_t* id, float zero, bool flag);
using ReleaseIdFn = void (*)(std::uintptr_t pool, std::int32_t* id);
constexpr std::uint32_t kSetSceneRva = 0x3CCC550, kAttachARva = 0x3CC9D30, kAttachBRva = 0x3CCAFA0,
                        kReleaseIdRva = 0x41950A0, kLevelGlobalRva = 0x6007A28, kIdPoolGlobalRva = 0x604B2E0;
// 0x3CC9D30 and 0x3CCAFA0 share their first bytes (no unique signature): checked by prologue instead.
constexpr std::uint8_t kAttachPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
                                            0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40};

bool prologue_matches(std::uintptr_t address) {
    std::uint8_t bytes[sizeof(kAttachPrologue)]{};
    return mem::try_copy(bytes, address, sizeof(bytes)) && std::memcmp(bytes, kAttachPrologue, sizeof(bytes)) == 0;
}

struct SceneCall {
    std::uintptr_t view, scene, x;
    SetSceneFn set_scene;
    AttachFn attach_a, attach_b;
    ReleaseIdFn release_id;
    std::uintptr_t id_pool;
};

// SEH only (no destructors): the three calls 0x17F8160 makes. Returns the step that faulted, or 0.
int call_set_scene(const SceneCall& call) {
    int step = 1;
    __try {
        call.set_scene(call.view, call.scene);
        step = 2;
        std::int32_t id = -1;
        call.attach_a(call.view, call.x + 0x28, &id, 0.0f, false);
        if (id != -1) {
            step = 3;
            call.release_id(call.id_pool, &id);
        }
        step = 4;
        id = -1;
        call.attach_b(call.view, call.x + 0x24, &id, 0.0f, false);
        return 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return step;
    }
}

void give_scene(std::uintptr_t view) {
    const auto set_scene = sig::lookup("view_set_scene");
    const auto release_id = sig::lookup("id_pool_release");
    const std::uintptr_t base = g_module.base;
    if (!set_scene || *set_scene - base != kSetSceneRva || !release_id || *release_id - base != kReleaseIdRva ||
        !prologue_matches(base + kAttachARva) || !prologue_matches(base + kAttachBRva)) {
        log::warn("views: the scene functions aren't where this build has them; the split view gets no scene");
        return;
    }
    std::uintptr_t level_global = 0, level = 0, scene = 0, x18 = 0, x = 0, pool_global = 0, main_scene = 0;
    mem::try_copy(&level_global, base + kLevelGlobalRva, sizeof(level_global));
    mem::try_copy(&level, level_global + 0x90, sizeof(level));
    mem::try_copy(&scene, level + 0x28, sizeof(scene));
    mem::try_copy(&x18, level + 0x18, sizeof(x18));
    mem::try_copy(&x, x18 + 0x08, sizeof(x));
    mem::try_copy(&pool_global, base + kIdPoolGlobalRva, sizeof(pool_global));
    std::optional<std::array<std::uintptr_t, 2>> eyes;
    {
        std::scoped_lock lock(g_mutex);
        eyes = g_eye_views;
    }
    if (eyes) {
        mem::try_copy(&main_scene, (*eyes)[0] + 0x2B10, sizeof(main_scene));
    }
    std::uintptr_t before = 0;
    mem::try_copy(&before, view + 0x2B10, sizeof(before));
    log::info("views: scene for view 0x{:X}: level 0x{:X}, level scene 0x{:X} (main view's 0x{:X}), split view had 0x{:X}",
              view, level, scene, main_scene, before);
    if (!level || !scene || !x || !pool_global) {
        log::warn("views:   no level scene to give it");
        return;
    }
    const SceneCall call{view,
                         scene,
                         x,
                         reinterpret_cast<SetSceneFn>(*set_scene),
                         reinterpret_cast<AttachFn>(base + kAttachARva),
                         reinterpret_cast<AttachFn>(base + kAttachBRva),
                         reinterpret_cast<ReleaseIdFn>(*release_id),
                         pool_global + 0xC600};
    const int faulted = call_set_scene(call);
    std::uintptr_t after = 0;
    std::uint8_t ready = 0;
    mem::try_copy(&after, view + 0x2B10, sizeof(after));
    mem::try_copy(&ready, after + 8, 1);
    log::info("views:   set-player-view-scene steps {}; [view+0x2B10] now 0x{:X} (byte +8 = {})",
              faulted ? std::format("FAULTED at step {}", faulted) : std::string("done"), after, ready);
}

// On the game thread: the pending SetCamera, if the view's vtable has the expected function there.
void apply_pending_camera() {
    std::uintptr_t view = 0, controller = 0;
    {
        std::scoped_lock lock(g_mutex);
        view = std::exchange(g_pending_camera_view, 0);
        controller = std::exchange(g_pending_camera, 0);
    }
    if (!view) {
        return;
    }
    std::uintptr_t vtable = 0, fn = 0, current = 0;
    mem::try_copy(&vtable, view, sizeof(vtable));
    mem::try_copy(&fn, vtable + kSetCameraSlot * sizeof(void*), sizeof(fn));
    mem::try_copy(&current, view + kReadStateInView + state::kSize + state::kController, sizeof(current));
    if (fn != g_module.base + kSetCameraRva) {
        log::warn("views: view 0x{:X} vtable slot {} is {} (expected exe+0x{:X}); not setting its camera", view,
                  kSetCameraSlot, mem::describe(g_module, fn), kSetCameraRva);
        return;
    }
    if (current == controller) {
        log::info("views: view 0x{:X} already has camera 0x{:X}", view, controller);
        return;
    }
    const bool ok = call_set_camera(reinterpret_cast<SetCameraFn>(fn), view, controller);
    std::uintptr_t after = 0;
    mem::try_copy(&after, view + kReadStateInView + state::kSize + state::kController, sizeof(after));
    log::info("views: SetCamera(view 0x{:X}, camera 0x{:X}) {}; WriteState camera now 0x{:X} (was 0x{:X})", view,
              controller, ok ? "returned" : "FAULTED (caught)", after, current);
    give_scene(view);
}

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
// Building 0x45 and 0x07 too crashed the game the same way (2026-10-13), so they're optional.
struct StageList {
    std::uint32_t begin, end;
    bool compositing; // 0x45 / 0x07: the stages the special view runs for the main view
};
// With `compositing`, 0x45 and 0x07 are built first: stage 0x0B faulted while being added to a view
// without them (a null signal source under its setup, 2026-10-13), and the special view, whose copy
// the split view starts as, has them.
constexpr StageList kStageLists[] = {
    {0x5899BE7, 0x5899BE8, true},  // 45
    {0x5899BF8, 0x5899BF9, true},  // 07
    {0x5899BE0, 0x5899BE7, false}, // 47 08 48 35 1E 05 0B
    {0x5899BE8, 0x5899BEF, false}, // 46 36 44 18 17 40 37
    {0x5899BEF, 0x5899BF0, false}, // 02
    {0x5899BF0, 0x5899BF8, false}, // 41 03 1B 1A 00 21 3E 3F
};
constexpr std::uint32_t kBuildStagesRva = 0x3AC51C0; // where the lists above are valid

// No C++ objects with destructors in here (SEH): an access violation is caught and reported instead
// of crashing the game (the state afterwards may still be broken).
struct Fault {
    std::uintptr_t code_address = 0; // where it faulted
    std::uintptr_t data_address = 0; // what it tried to read or write
    std::uintptr_t rcx = 0, rdx = 0, r8 = 0;
    std::uintptr_t init_stage = 0; // rdi in the frame of stage 0x0B's init (0x3E4DBA0), if it's on the stack
    std::array<std::uintptr_t, 12> stack{}; // return addresses into the exe, innermost first
};

int record_fault(EXCEPTION_POINTERS* info, Fault& fault) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    fault.code_address = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    fault.data_address = info->ExceptionRecord->NumberParameters >= 2
                             ? static_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionInformation[1])
                             : 0;
    fault.rcx = info->ContextRecord->Rcx;
    fault.rdx = info->ContextRecord->Rdx;
    fault.r8 = info->ContextRecord->R8;
    // The faulting frames still exist while the filter runs (not after unwinding): walk them now,
    // exactly, with the exe's unwind data (RtlVirtualUnwind), innermost first.
    CONTEXT context = *info->ContextRecord;
    std::size_t n = 0;
    while (n < fault.stack.size() && context.Rip) {
        fault.stack[n++] = context.Rip;
        // Stage 0x0B's init keeps the stage in rdi (mov rdi, rcx at 0x3E4DBE4).
        if (!fault.init_stage && context.Rip - g_module.base >= 0x3E4DBA0 && context.Rip - g_module.base < 0x3E4F0AE) {
            fault.init_stage = context.Rdi;
        }
        DWORD64 image_base = 0;
        const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (!function) {
            break; // a leaf function or no unwind data: stop rather than guess
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher,
                         nullptr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

bool call_build_stages(BuildStagesFn build, std::uintptr_t pipeline, std::uintptr_t view, const std::uintptr_t* list,
                       Fault& fault) {
    __try {
        build(pipeline, view, list);
        return true;
    } __except (record_fault(GetExceptionInformation(), fault)) {
        return false;
    }
}

// The game's own stage setup for a normal view (0x3AC4EA0 in 4.1.1.7631656, the first thing the full
// setup 0x588E50 does; rcx = pipeline, rdx = view). It builds the lists above through slot 87, the
// compositing ones (0x45, 0x07) only when [[view+0x10]+0x40] is set, and otherwise links the view's job
// counters into the frame's job graph (0x41074A0: the special view's [ctx+0x40] waits for the view's
// [ctx+0x78], the frame counter DAT_146048680 for its [ctx+0x80]; ctx = [view+0x40]). Building 0x45 into
// the split view by hand (build full) hung the render thread in that stage's job wait (DownsampleResolver,
// 0x3E4A930 -> 0x410E880; watchdog stacks 2026-10-19), so the game's function is used instead.
using ChooseStagesFn = void (*)(std::uintptr_t pipeline, std::uintptr_t view);
constexpr std::uint32_t kFullSetupRva = 0x588E50; // game pipeline vtable slot 88, same arguments
std::atomic<bool> g_full_setup{true};
constexpr std::uint32_t kChooseStagesRva = 0x3AC4EA0;

// A render view's job context ([view+0x40]: 17 job counters the render loop arms and releases each
// frame and the frame end waits on). The root view constructor 0x407B080 allocates 0x88 bytes from the
// game allocator (0x4188700 on arena 0x40EA160()+0xE8, align 0x10), zeroes them and fills them with
// 0x40878C0; the destructor 0x407B320 destroys the counters and frees the block with the same
// allocator. The split view, a copy of the special view, shares the special view's context: the
// stage setup then made that context's +0x40 counter wait for its own +0x78, which needs the split
// view's stages, and the split view's stage 0x08 waited on +0x40 during end-recording: a cycle (job dumps,
// 2026-10-19). So it gets a context of its own, made the constructor's way.
using GameAllocatorFn = std::uintptr_t (*)();
using AllocateFn = void* (*)(std::uintptr_t arena, std::uint64_t size, std::uint64_t align, std::int64_t* slack);
using InitJobContextFn = void (*)(void* context);
constexpr std::uint32_t kGameAllocatorRva = 0x40EA160;
constexpr std::uint32_t kAllocateRva = 0x4188700;
constexpr std::uint32_t kInitJobContextRva = 0x40878C0;
constexpr std::size_t kJobContextSize = 0x88;

std::uintptr_t new_job_context(GameAllocatorFn allocator, AllocateFn allocate, InitJobContextFn init) {
    __try {
        std::int64_t slack = 0;
        void* context = allocate(allocator() + 0xE8, kJobContextSize, 0x10, &slack);
        if (!context) {
            return 0;
        }
        std::memset(context, 0, kJobContextSize);
        init(context);
        return reinterpret_cast<std::uintptr_t>(context);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
}

// The three functions, if the signatures resolved where this build has them.
std::optional<std::array<std::uintptr_t, 3>> job_context_functions() {
    const auto allocator = sig::lookup("game_allocator");
    const auto allocate = sig::lookup("allocator_alloc");
    const auto init = sig::lookup("job_context_init");
    if (!allocator || !allocate || !init || *allocator - g_module.base != kGameAllocatorRva ||
        *allocate - g_module.base != kAllocateRva || *init - g_module.base != kInitJobContextRva) {
        return std::nullopt;
    }
    return std::array<std::uintptr_t, 3>{*allocator, *allocate, *init};
}

bool call_choose_stages(ChooseStagesFn choose, std::uintptr_t pipeline, std::uintptr_t view, Fault& fault) {
    __try {
        choose(pipeline, view);
        return true;
    } __except (record_fault(GetExceptionInformation(), fault)) {
        return false;
    }
}

std::string stack_trace(const Fault& fault) {
    std::string text;
    for (const std::uintptr_t address : fault.stack) {
        if (address) {
            text += " " + mem::describe(g_module, address);
        }
    }
    return text;
}

// A stage's id is what its vtable slot 1 returns (the builder matches stages by it).
using StageIdFn = std::uint8_t (*)(std::uintptr_t stage);
int stage_id(std::uintptr_t stage) {
    __try {
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(stage);
        return reinterpret_cast<StageIdFn>(*reinterpret_cast<const std::uintptr_t*>(vtable + 8))(stage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Logs a view's stage array: pointer, vtable, id.
void log_stages(const char* label, std::uintptr_t view) {
    std::uintptr_t array = 0;
    std::int32_t count = 0;
    if (!mem::try_copy(&array, view + 0x28, sizeof(array)) || !mem::try_copy(&count, view + 0x34, sizeof(count)) ||
        count < 0 || count > 64) {
        log::info("views:   {} 0x{:X}: stage array unreadable", label, view);
        return;
    }
    std::string text;
    for (std::int32_t i = 0; i < count; ++i) {
        std::uintptr_t stage = 0, vtable = 0;
        mem::try_copy(&stage, array + i * sizeof(void*), sizeof(stage));
        mem::try_copy(&vtable, stage, sizeof(vtable));
        const bool valid = mem::in_section(g_module, ".rdata", vtable, sizeof(void*));
        text += std::format(" {:X}{}", valid ? stage_id(stage) : -1, valid ? "" : "!");
    }
    log::info("views:   {} 0x{:X}: {} stage(s) at 0x{:X}, ids:{}", label, view, count, array, text);
}

// RenderFrame (rdi at Present) view list +0x42E0 / count +0x42EC; its stage resource manager +0x4250,
// whose per-view records are at +0x58 / count +0x64 (first word of a record = the view). 0x4073230.
bool frame_has_view(std::uintptr_t frame, std::uintptr_t view) {
    std::uintptr_t list = 0;
    std::uint32_t count = 0;
    if (!mem::try_copy(&list, frame + 0x42E0, 8) || !mem::try_copy(&count, frame + 0x42EC, 4) || count > 64) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uintptr_t v = 0;
        if (mem::try_copy(&v, list + i * 8, 8) && v == view) {
            return true;
        }
    }
    return false;
}

bool manager_has_record(std::uintptr_t manager, std::uintptr_t view) {
    std::uintptr_t list = 0;
    std::int32_t count = 0;
    if (!mem::try_copy(&list, manager + 0x58, 8) || !mem::try_copy(&count, manager + 0x64, 4) || count < 0 ||
        count > 64) {
        return false;
    }
    for (std::int32_t i = 0; i < count; ++i) {
        std::uintptr_t record = 0, v = 0;
        if (mem::try_copy(&record, list + i * 8, 8) && mem::try_copy(&v, record, 8) && v == view) {
            return true;
        }
    }
    return false;
}

using AddViewFn = void (*)(std::uintptr_t frame, std::uintptr_t view, char special);
bool call_add_view(AddViewFn add_view, std::uintptr_t frame, std::uintptr_t view) {
    __try {
        add_view(frame, view, 0);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

void log_frame_facts(const char* when, std::uintptr_t frame, std::uintptr_t main_view, std::uintptr_t special_view,
                     std::uintptr_t target) {
    std::uintptr_t manager = 0, group = 0;
    mem::try_copy(&manager, frame + 0x4250, 8);
    mem::try_copy(&group, main_view + 0x18, 8);
    log::info("views:   {}: render frame 0x{:X} lists main {} special {} target {}; manager 0x{:X} (main view's "
              "[view+0x18] 0x{:X}) has records for main {} special {} target {}",
              when, frame, frame_has_view(frame, main_view), frame_has_view(frame, special_view),
              frame_has_view(frame, target), manager, group, manager_has_record(manager, main_view),
              manager_has_record(manager, special_view), manager_has_record(manager, target));
}

// The resource manager a stage uses is [[stage+0x10]] (stage 0x0B's init, 0x3E4DBA0). Its key table
// (0x40956C0 / 0x409DB80): buckets at +0x78, bucket count +0x80, next +0x88, keys (u32) +0x98.
bool manager_has_key(std::uintptr_t manager, std::uint32_t key) {
    std::uintptr_t buckets = 0, next = 0, keys = 0;
    std::int32_t count = 0;
    if (!mem::try_copy(&buckets, manager + 0x78, 8) || !mem::try_copy(&count, manager + 0x80, 4) ||
        !mem::try_copy(&next, manager + 0x88, 8) || !mem::try_copy(&keys, manager + 0x98, 8) || count <= 0) {
        return false;
    }
    std::int32_t index = -1;
    mem::try_copy(&index, buckets + (key % static_cast<std::uint32_t>(count)) * 4, 4);
    for (int guard = 0; index >= 0 && guard < 100000; ++guard) {
        std::uint32_t k = 0;
        if (!mem::try_copy(&k, keys + static_cast<std::uintptr_t>(index) * 4, 4)) {
            return false;
        }
        if (k == key) {
            return true;
        }
        if (!mem::try_copy(&index, next + static_cast<std::uintptr_t>(index) * 4, 4)) {
            return false;
        }
    }
    return false;
}

// The manager [[stage+0x10]] of the view's stage with `id`, or 0.
std::uintptr_t stage_manager(std::uintptr_t view, int id) {
    std::uintptr_t array = 0;
    std::int32_t count = 0;
    mem::try_copy(&array, view + 0x28, 8);
    mem::try_copy(&count, view + 0x34, 4);
    for (std::int32_t i = 0; i < count && i < 64; ++i) {
        std::uintptr_t stage = 0, group = 0, manager = 0;
        mem::try_copy(&stage, array + i * 8, 8);
        if (stage && stage_id(stage) == id && mem::try_copy(&group, stage + 0x10, 8) &&
            mem::try_copy(&manager, group, 8)) {
            return manager;
        }
    }
    return 0;
}

// A stage 0x0B's state after its init (0x3E4DBA0): ok flag +0x19, the five loads +0x2140..+0x2160,
// the resource lookups +0x2168 / +0x2170 / +0x2178, +0x2190, +0x1260, and the flag the init checks
// first, [[[stage+8]+0x10]+0x40].
void log_stage_0b(const char* label, std::uintptr_t stage) {
    if (!stage) {
        log::info("views:   {}: no stage", label);
        return;
    }
    std::uint8_t ok = 0;
    mem::try_copy(&ok, stage + 0x19, 1);
    std::string fields;
    for (const std::size_t offset : {0x2140u, 0x2148u, 0x2150u, 0x2158u, 0x2160u, 0x2168u, 0x2170u, 0x2178u, 0x2190u,
                                     0x1260u}) {
        std::uintptr_t value = 0;
        mem::try_copy(&value, stage + offset, 8);
        fields += std::format(" +0x{:X}={:X}", offset, value);
    }
    std::uintptr_t view = 0, frame = 0;
    std::int32_t frame_flag = -1;
    mem::try_copy(&view, stage + 8, 8);
    if (mem::try_copy(&frame, view + 0x10, 8)) {
        mem::try_copy(&frame_flag, frame + 0x40, 4);
    }
    log::info("views:   {} 0x{:X}: ok {}{}; [[view+0x10]+0x40] = {}", label, stage, ok, fields, frame_flag);
}

// The view's stage with `id`, or 0.
std::uintptr_t find_stage(std::uintptr_t view, int id) {
    std::uintptr_t array = 0;
    std::int32_t count = 0;
    mem::try_copy(&array, view + 0x28, 8);
    mem::try_copy(&count, view + 0x34, 4);
    for (std::int32_t i = 0; i < count && i < 64; ++i) {
        std::uintptr_t stage = 0;
        mem::try_copy(&stage, array + i * 8, 8);
        if (stage && stage_id(stage) == id) {
            return stage;
        }
    }
    return 0;
}

// Facts for the stage 0x0B failure: which manager each view's stages use and whether the key stage
// 0x0B binds (0x361F94A4) is registered in it.
void log_resource_facts(const char* when, std::uintptr_t main_view, std::uintptr_t target) {
    constexpr std::uint32_t kKey = 0x361F94A4;
    const std::uintptr_t main_manager = stage_manager(main_view, 0x0B);
    std::uintptr_t target_group = 0, target_manager = 0;
    mem::try_copy(&target_group, target + 0x18, 8); // what [[stage+0x10]] is for a stage of `target`
    target_manager = target_group;
    log::info("views:   {}: main stage 0x0B manager 0x{:X} (key 0x{:X} {}); target view's [view+0x18] 0x{:X} (key "
              "{})",
              when, main_manager, kKey, main_manager ? (manager_has_key(main_manager, kKey) ? "present" : "absent") : "?",
              target_manager, target_manager ? (manager_has_key(target_manager, kKey) ? "present" : "absent") : "?");
}

std::uint32_t stage_count(std::uintptr_t view) {
    std::uint32_t count = 0;
    mem::try_copy(&count, view + 0x34, sizeof(count));
    return count;
}

} // namespace

void build(const ModuleInfo& module, std::uintptr_t controller_in, std::size_t index, bool compositing) {
    if (compositing) {
        log::warn("views: build full is disabled: the split view's own stage 0x45 (DownsampleResolver) hangs the "
                  "render thread (2026-10-19); use !vr_views build");
        return;
    }
    if (controller_in) {
        g_module = module;
        g_controller = controller_in;
    }
    // Automatic: the split view appears some seconds after a load with ForceSplitscreen on. Scan
    // until there's a view besides the main and special ones (3 or more), for up to a minute.
    if (index == kAutomatic && found().size() < 3) {
        if (g_running.exchange(true)) {
            log::info("views: a scan is already running; try build again when it's done");
            return;
        }
        const std::uintptr_t controller = g_controller;
        std::thread([controller, compositing] {
            for (int attempt = 0; attempt < 12 && controller; ++attempt) {
                if (scan_now(controller).size() >= 3) {
                    break;
                }
                log::info("views: no split view yet (attempt {} of 12); scanning again in 5 s", attempt + 1);
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
            g_running.store(false);
            if (found().size() >= 3) {
                build(g_module, 0, kAutomatic, compositing);
            } else {
                log::warn("views: no split view appeared within a minute (!vr_split on, then reload)");
            }
        }).detach();
        log::info("views: looking for the split view before building");
        return;
    }
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
    std::thread([views, index, compositing, setup = *setup] {
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
            // A scan may have finished while the pipeline was being looked for: use the latest views.
            std::vector<std::uintptr_t> latest;
            for (const std::uintptr_t read_state : found()) {
                latest.push_back(read_state - kReadStateInView);
            }
            for (const std::uintptr_t base : latest) {
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
        const auto choose_stages = sig::lookup("pipeline_choose_stages");
        if (!compositing && (!choose_stages || *choose_stages - g_module.base != kChooseStagesRva)) {
            log::warn("views: pipeline_choose_stages isn't at exe+0x{:X} (another game build?); not building",
                      kChooseStagesRva);
            return;
        }
        // A baseline of the renderer's events before the build runs; the build re-arms it for the frames after.
        events::arm(g_module, 3, 6000);
        render::run_on_render_thread([pipeline, target, main_view, special_view, compositing, build = *build_stages,
                                      choose = choose_stages.value_or(0), setup, driven = g_controller] {
            log_stages("before: main   ", main_view);
            log_stages("before: special", special_view);
            log_stages("before: target ", target);
            // The split view starts as a shallow copy of the special view: same stage array buffer
            // (StageGroup at view+0x18: scene graph +0x18, owning view +0x20, stages +0x28, capacity
            // +0x30, count +0x34). Growing it reallocated the shared buffer and left the special view
            // pointing at freed memory (crash, 2026-10-13). Give it its own, empty array first; the
            // game allocates a fresh one on the first stage added.
            // The split view is a shallow copy of another view: of the special view (3 stages) in most runs,
            // of the main view (29 stages, the main view's own array and job context) in the 12:04 run on
            // 2026-10-19, where building into the shared array freed the main view's and crashed the next
            // frame. Whichever it copied, it starts from an empty array of its own.
            std::uintptr_t target_stages = 0, special_stages = 0, main_stages = 0, owner = 0;
            mem::try_copy(&target_stages, target + 0x28, sizeof(target_stages));
            mem::try_copy(&special_stages, special_view + 0x28, sizeof(special_stages));
            mem::try_copy(&main_stages, main_view + 0x28, sizeof(main_stages));
            mem::try_copy(&owner, target + 0x20, sizeof(owner));
            log::info("views:   target stage group: owner view 0x{:X}, stages 0x{:X} (special's 0x{:X}, main's 0x{:X})",
                      owner, target_stages, special_stages, main_stages);
            if (target_stages && (target_stages == special_stages || target_stages == main_stages)) {
                *reinterpret_cast<std::uintptr_t*>(target + 0x28) = 0;
                *reinterpret_cast<std::uint32_t*>(target + 0x30) = 0;
                *reinterpret_cast<std::uint32_t*>(target + 0x34) = 0;
                log::info("views:   detached the target's stage array from the {} view's",
                          target_stages == main_stages ? "main" : "special");
            } else if (target_stages && stage_count(target) != 0) {
                log::warn("views:   the target has {} stage(s) of its own in an unshared array; not building over them",
                          stage_count(target));
                return;
            }
            // Give the split view what the game gives a view it draws: a place in the RenderFrame's view
            // list and a record in the stage resource manager, both done by the game's 0x4073230 (it
            // inserts before the special view). Only when the view isn't in the list already.
            const std::uintptr_t frame = render::render_frame();
            if (frame) {
                log_frame_facts("before add", frame, main_view, special_view, target);
                const auto add_view = sig::lookup("render_frame_add_view");
                if (!frame_has_view(frame, target) && add_view && *add_view - g_module.base == 0x4073230) {
                    const bool added = call_add_view(reinterpret_cast<AddViewFn>(*add_view), frame, target);
                    log::info("views:   0x4073230(frame, split view, 0) {}", added ? "returned" : "FAULTED (caught)");
                    log_frame_facts("after add", frame, main_view, special_view, target);
                }
                std::uintptr_t manager = 0;
                mem::try_copy(&manager, frame + 0x4250, 8);
                if (manager_has_record(manager, target)) {
                    *reinterpret_cast<std::uintptr_t*>(target + 0x20) = target;
                    log::info("views:   the split view has a manager record: it now owns its stage group");
                }
            }
            // Otherwise the stage group's owner (+0x20) stays the special view. Adding a stage registers it with
            // the manager under the owner's record (0x409A4C0: the manager's list at +0x58 holds one
            // record per view; no record for the owner -> the stage isn't registered), and stage
            // 0x0B's key registrations (0x40956C0) fail for unregistered stages, clearing its ok flag
            // and leaving +0x2168 null (the crash at 0x4096593). The split view has no record of its
            // own, so pointing the owner at it broke that (attempts 4-6, 2026-10-13). Giving it its own
            // record needs the function that creates them (not found yet).
            // The stage group's scene graph (+0x18): the split view has the special view's, which only
            // composites; scene stages (0x0B first) then hit a null object while being added (fault at
            // 0x4096593 under 0x407A440, 2026-10-13). Give it the main view's, the world.
            std::uintptr_t graphs[3]{};
            mem::try_copy(&graphs[0], main_view + 0x18, sizeof(std::uintptr_t));
            mem::try_copy(&graphs[1], special_view + 0x18, sizeof(std::uintptr_t));
            mem::try_copy(&graphs[2], target + 0x18, sizeof(std::uintptr_t));
            std::uintptr_t owners[2]{};
            mem::try_copy(&owners[0], main_view + 0x20, sizeof(std::uintptr_t));
            mem::try_copy(&owners[1], special_view + 0x20, sizeof(std::uintptr_t));
            log::info("views:   scene graphs: main 0x{:X}, special 0x{:X}, target 0x{:X}; owners: main 0x{:X}, special "
                      "0x{:X}",
                      graphs[0], graphs[1], graphs[2], owners[0], owners[1]);
            if (graphs[0] && graphs[2] != graphs[0]) {
                *reinterpret_cast<std::uintptr_t*>(target + 0x18) = graphs[0];
                log::info("views:   target now uses the main view's scene graph");
            }
            // Pointer fields the main view has and the target lacks: candidates for what a stage's setup
            // needs (the split view starts as a copy of the compositing-only special view).
            {
                constexpr std::size_t kBytes = 0x3400;
                std::vector<std::uintptr_t> main_words(kBytes / 8), target_words(kBytes / 8);
                if (mem::try_copy(main_words.data(), main_view, kBytes) &&
                    mem::try_copy(target_words.data(), target, kBytes)) {
                    std::string text;
                    int shown = 0;
                    for (std::size_t i = 0; i < main_words.size() && shown < 48; ++i) {
                        if (target_words[i] == 0 && main_words[i] > 0x10000 && main_words[i] < 0x7FFFFFFFFFFF) {
                            text += std::format(" +0x{:X}", i * 8);
                            ++shown;
                        }
                    }
                    log::info("views:   pointer fields set in main, null in target:{}", text.empty() ? " none" : text);
                }
            }
            log_resource_facts("before build", main_view, target);
            {
                std::scoped_lock lock(g_mutex);
                g_special_view = special_view;
                g_eye_views = std::array<std::uintptr_t, 2>{main_view, target}; // for the dump's labels
            }
            dump_jobs("before build");
            // Its own job context (see new_job_context), before the stage setup links it into the frame.
            {
                std::uintptr_t contexts[3]{};
                mem::try_copy(&contexts[0], main_view + 0x40, sizeof(std::uintptr_t));
                mem::try_copy(&contexts[1], special_view + 0x40, sizeof(std::uintptr_t));
                mem::try_copy(&contexts[2], target + 0x40, sizeof(std::uintptr_t));
                log::info("views:   job contexts: main 0x{:X}, special 0x{:X}, split 0x{:X}", contexts[0], contexts[1],
                          contexts[2]);
                if (!contexts[2] || contexts[2] == contexts[0] || contexts[2] == contexts[1]) {
                    const auto functions = job_context_functions();
                    if (!functions) {
                        log::warn("views:   the split view shares another view's job context and the allocator / "
                                  "context functions aren't where this build has them; not building (it would hang)");
                        return;
                    }
                    const std::uintptr_t context =
                        new_job_context(reinterpret_cast<GameAllocatorFn>((*functions)[0]),
                                        reinterpret_cast<AllocateFn>((*functions)[1]),
                                        reinterpret_cast<InitJobContextFn>((*functions)[2]));
                    if (!context) {
                        log::warn("views:   creating a job context for the split view FAULTED or failed; not building");
                        return;
                    }
                    *reinterpret_cast<std::uintptr_t*>(target + 0x40) = context;
                    log::info("views:   the split view shared job context 0x{:X}; it now has its own, 0x{:X}:", contexts[2],
                              context);
                    for (std::size_t i = 0; i < 17; ++i) {
                        std::uintptr_t counter = 0;
                        mem::try_copy(&counter, context + 8 * i, sizeof(counter));
                        log::info("views:     ctx+0x{:02X} {}", 8 * i, describe_counter(counter));
                    }
                }
            }
            const std::uint32_t before = stage_count(target);
            bool ok = true;
            Fault fault;
            if (compositing) {
                // The old hand-built variant, kept only for comparison: it hangs (see ChooseStagesFn).
                for (const auto& range : kStageLists) {
                    const std::uintptr_t list[2] = {g_module.base + range.begin, g_module.base + range.end};
                    ok = call_build_stages(reinterpret_cast<BuildStagesFn>(build), pipeline, target, list, fault);
                    if (!ok) {
                        break;
                    }
                }
            } else {
                std::uintptr_t frame_of_view = 0;
                std::int32_t frame_mode = -1;
                mem::try_copy(&frame_of_view, target + 0x10, sizeof(frame_of_view));
                mem::try_copy(&frame_mode, frame_of_view + 0x40, sizeof(frame_mode));
                log::info("views:   [[view+0x10]+0x40] = {} ({})", frame_mode,
                          frame_mode == 0 ? "no compositing stages; job counters linked into the frame's graph"
                                          : "the view gets its own compositing stages");
                if (g_full_setup.load() && setup - g_module.base == kFullSetupRva) {
                    // The game's whole setup for a new view (0x588E50, game pipeline vtable slot 88): 0x3AC4EA0,
                    // then the stages it creates itself (0x3D, 0x3C, 0x33 and others) and the two output
                    // objects it hooks into the view's stage 0x21. Stage lists alone leave the view black
                    // (2026-10-19). It crashed on 2026-10-13, before the split view had its own stage array,
                    // frame record and job context.
                    // The game's own add-player-view (0x3BAA1A0) creates the view, sets its split-screen index
                    // ([view+0x2268], a byte after WriteState) to 1, then runs this same setup (pipeline slot 88).
                    // The index is read by about 60 places in the stage code and decides stage 0x44 (built for
                    // the main view or index 1, 0x3AC51C0); the special view's copy has 0.
                    std::uint8_t split_index = 0xFF;
                    mem::try_copy(&split_index, target + kSplitScreenIndexInView, 1);
                    *reinterpret_cast<std::uint8_t*>(target + kSplitScreenIndexInView) = 1;
                    log::info("views:   split-screen index of the split view {} -> 1 (as 0x3BAA1A0 does)", split_index);
                    log::info("views:   calling the game's full view setup 0x588E50(pipeline, split view)");
                    ok = call_choose_stages(reinterpret_cast<ChooseStagesFn>(setup), pipeline, target, fault);
                } else {
                    log::info("views:   calling 0x3AC4EA0(pipeline, split view) (stage lists only)");
                    ok = call_choose_stages(reinterpret_cast<ChooseStagesFn>(choose), pipeline, target, fault);
                }
            }
            log::info("views: stage lists{} for view 0x{:X} {}: {} stage(s) before, {} after",
                      compositing ? " (with 0x45/0x07)" : "", target, ok ? "built" : "FAULTED (access violation caught)",
                      before, stage_count(target));
            log_resource_facts("after build", main_view, target);
            if (!ok) {
                log::info("views:   fault at {} accessing 0x{:X}; rcx 0x{:X} rdx 0x{:X} r8 0x{:X}; stack:{}",
                          mem::describe(g_module, fault.code_address), fault.data_address, fault.rcx, fault.rdx,
                          fault.r8, stack_trace(fault));
                log_stage_0b("failing stage 0x0B (split view)", fault.init_stage);
                log_stage_0b("working stage 0x0B (main view) ", find_stage(main_view, 0x0B));
            }
            log_stages("after:  main   ", main_view);
            log_stages("after:  special", special_view);
            log_stages("after:  target ", target);
            dump_jobs("after build");
            events::arm(g_module, 10, 20000); // every event of the next frames, up to a hang
            // A view needs a camera: without one the begin-recording camera builder (0x3CC7490) skips the
            // PerCamera blocks. Give it ours, the game's way, on the game thread (apply_pending_camera).
            // Until 2026-10-14 both states' pointers were written directly, which skipped the
            // controller's Enter.
            std::scoped_lock lock(g_mutex);
            if (!ok) {
                g_eye_views.reset();
            }
            if (driven) {
                g_pending_camera_view = target;
                g_pending_camera = driven;
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

// Job counters (0x4107090 creates them; 4.1.1.7631656): +0x00 pending count (int), +0x04 flags (1 armed,
// 2 released, 4 signalled), +0x40 SRW lock and pending-job ring (capacity +0x80, buffer +0x88, read
// index +0x100), +0x140 a second ring (capacity +0x180, buffer +0x188, read index +0x200) the waiter
// helps run, +0x240 epoch, +0x248 scheduler queue, +0x250 successors (count +0x25C), +0x260 more flags,
// +0x264 queue id, +0x268 predecessors (up to 3, null-terminated).
std::string describe_counter(std::uintptr_t counter) {
    if (!counter) {
        return "null";
    }
    std::uint8_t raw[0x290]{};
    if (!mem::try_copy(raw, counter, sizeof(raw))) {
        return std::format("0x{:X} unreadable", counter);
    }
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v;
        std::memcpy(&v, raw + at, 4);
        return v;
    };
    const auto u64 = [&](std::size_t at) {
        std::uint64_t v;
        std::memcpy(&v, raw + at, 8);
        return v;
    };
    std::string preds;
    for (std::size_t i = 0; i < 4; ++i) {
        const std::uint64_t p = u64(0x268 + 8 * i);
        if (!p) {
            break;
        }
        std::uint32_t pflags = 0;
        std::int32_t pcount = 0;
        mem::try_copy(&pcount, p, 4);
        mem::try_copy(&pflags, p + 4, 4);
        preds += std::format(" 0x{:X}(n{} f{:X})", p, pcount, pflags);
    }
    std::string succs;
    const std::uint32_t succ_count = u32(0x25C);
    for (std::uint32_t i = 0; i < succ_count && i < 6; ++i) {
        std::uint64_t s = 0;
        mem::try_copy(&s, u64(0x250) + 8 * i, 8);
        succs += std::format(" 0x{:X}", s);
    }
    return std::format("0x{:X} n{} f{:X} f260 {:X} epoch {} q1 cap {} rd {} q2 cap {} rd {} sched 0x{:X} qid {} preds[{} ] "
                       "succs {}[{} ]",
                       counter, static_cast<std::int32_t>(u32(0)), u32(4), u32(0x260), u64(0x240), u64(0x80), u64(0x100),
                       u64(0x180), u64(0x200), u64(0x248), u32(0x264), preds, succ_count, succs);
}

void dump_jobs(const char* why, const std::vector<Hint>& hints) {
    const auto mark = [&](std::uintptr_t value) {
        std::string text;
        for (const Hint& hint : hints) {
            if (value && hint.value == value) {
                text += " <== " + hint.where;
            }
        }
        return text;
    };
    const std::uintptr_t frame = render::render_frame();
    std::uintptr_t list = 0;
    std::uint32_t count = 0;
    mem::try_copy(&list, frame + 0x42E0, sizeof(list));
    mem::try_copy(&count, frame + 0x42EC, sizeof(count));
    std::optional<std::array<std::uintptr_t, 2>> eyes;
    std::uintptr_t special = 0;
    {
        std::scoped_lock lock(g_mutex);
        eyes = g_eye_views;
        special = g_special_view;
    }
    std::uintptr_t global = 0;
    mem::try_copy(&global, g_module.base + 0x6048680, sizeof(global)); // DAT_146048680, the frame counter
    std::int32_t frame_mode = -1;
    mem::try_copy(&frame_mode, frame + 0x40, sizeof(frame_mode));
    log::info("jobs [{}]: RenderFrame 0x{:X}, {} view(s) in its list; [frame+0x40] = {}; frame counter {}{}", why, frame,
              count, frame_mode, describe_counter(global), mark(global));
    // Every view's context, for naming counters: (context slot address -> "view N ctx+0xNN").
    std::vector<std::uintptr_t> views(std::min<std::uint32_t>(count, 16));
    if (!views.empty()) {
        mem::try_copy(views.data(), list, views.size() * sizeof(std::uintptr_t));
    }
    std::vector<std::pair<std::uintptr_t, std::string>> names;
    for (std::size_t v = 0; v < views.size(); ++v) {
        std::uintptr_t ctx = 0;
        mem::try_copy(&ctx, views[v] + 0x40, sizeof(ctx));
        for (std::size_t i = 0; i < 17; ++i) {
            std::uintptr_t counter = 0;
            mem::try_copy(&counter, ctx + 8 * i, sizeof(counter));
            names.emplace_back(counter, std::format("view {} ctx+0x{:X}", v, 8 * i));
        }
    }
    const auto name_of = [&](std::uintptr_t counter) {
        for (const auto& [address, name] : names) {
            if (address == counter && counter) {
                return name;
            }
        }
        return counter == global ? std::string("frame counter") : std::string("not a view context slot");
    };
    for (std::size_t v = 0; v < views.size(); ++v) {
        const std::uintptr_t view = views[v];
        const char* role = eyes && view == (*eyes)[0] ? "main"
                           : eyes && view == (*eyes)[1] ? "SPLIT"
                           : view == special             ? "special"
                                                         : "?";
        std::uint8_t recording = 0, enabled = 0;
        std::uintptr_t vtable = 0, ctx = 0, controller = 0, owner = 0;
        mem::try_copy(&vtable, view, sizeof(vtable));
        mem::try_copy(&recording, view + 0x2150, 1);
        mem::try_copy(&enabled, view + kReadStateInView, 1);
        mem::try_copy(&ctx, view + 0x40, sizeof(ctx));
        mem::try_copy(&controller, view + kReadStateInView + state::kController, sizeof(controller));
        mem::try_copy(&owner, view + 0x20, sizeof(owner));
        std::uint8_t split_index = 0xFF;
        mem::try_copy(&split_index, view + kSplitScreenIndexInView, 1);
        log::info("jobs [{}]: view {} 0x{:X} ({}) vtable {} recording {} enabled {} split index {} camera 0x{:X} group owner "
                  "0x{:X} context 0x{:X}{}",
                  why, v, view, role, mem::describe(g_module, vtable), recording, enabled, split_index, controller, owner,
                  ctx, mark(view));
        for (std::size_t i = 0; i < 17; ++i) {
            std::uintptr_t counter = 0;
            mem::try_copy(&counter, ctx + 8 * i, sizeof(counter));
            log::info("jobs [{}]:   ctx+0x{:02X} {}{}", why, 8 * i, describe_counter(counter), mark(counter));
        }
        std::uintptr_t stages = 0;
        std::int32_t stage_count = 0;
        mem::try_copy(&stages, view + 0x28, sizeof(stages));
        mem::try_copy(&stage_count, view + 0x34, sizeof(stage_count));
        for (std::int32_t s = 0; s < stage_count && s < 64; ++s) {
            std::uintptr_t stage = 0, stage_vtable = 0, wait = 0, execute = 0, counter = 0, stage_view = 0;
            std::uint8_t async = 0, ok = 0;
            mem::try_copy(&stage, stages + 8 * s, sizeof(stage));
            mem::try_copy(&stage_vtable, stage, sizeof(stage_vtable));
            mem::try_copy(&execute, stage_vtable + 6 * 8, sizeof(execute));
            mem::try_copy(&wait, stage_vtable + 8 * 8, sizeof(wait));
            mem::try_copy(&stage_view, stage + 0x08, sizeof(stage_view));
            mem::try_copy(&async, stage + 0x18, 1);
            mem::try_copy(&ok, stage + 0x19, 1);
            mem::try_copy(&counter, stage + 0x2138, sizeof(counter));
            log::info("jobs [{}]:   stage {:2} id 0x{:02X} 0x{:X} vtable {} execute {} wait {} view 0x{:X} async {} ok {} "
                      "counter [{}] {}{}{}",
                      why, s, stage_id(stage), stage, mem::describe(g_module, stage_vtable),
                      mem::describe(g_module, execute), mem::describe(g_module, wait), stage_view, async, ok,
                      name_of(counter), describe_counter(counter), mark(stage), mark(counter));
        }
    }
}

const char* view_role(std::uintptr_t view) {
    std::scoped_lock lock(g_mutex);
    if (g_eye_views && view == (*g_eye_views)[0]) {
        return "main";
    }
    if (g_eye_views && view == (*g_eye_views)[1]) {
        return "SPLIT";
    }
    return view && view == g_special_view ? "special" : "view";
}

std::string counter_name(std::uintptr_t counter) {
    if (!counter) {
        return "null";
    }
    const std::uintptr_t frame = render::render_frame();
    std::uintptr_t list = 0;
    std::uint32_t count = 0;
    mem::try_copy(&list, frame + 0x42E0, sizeof(list));
    mem::try_copy(&count, frame + 0x42EC, sizeof(count));
    for (std::uint32_t v = 0; v < count && v < 16; ++v) {
        std::uintptr_t view = 0, ctx = 0;
        mem::try_copy(&view, list + 8 * v, sizeof(view));
        mem::try_copy(&ctx, view + 0x40, sizeof(ctx));
        for (std::size_t i = 0; i < 17; ++i) {
            std::uintptr_t slot = 0;
            mem::try_copy(&slot, ctx + 8 * i, sizeof(slot));
            if (slot == counter) {
                return std::format("{} view ctx+0x{:X}", view_role(view), 8 * i);
            }
        }
    }
    std::uintptr_t global = 0;
    mem::try_copy(&global, g_module.base + 0x6048680, sizeof(global));
    return counter == global ? "frame counter" : "other";
}

int stage_id_of(std::uintptr_t stage) {
    return stage_id(stage);
}

void set_full_setup(bool on) {
    g_full_setup.store(on);
    log::info("views: build uses {}", on ? "the game's full view setup (0x588E50)" : "the stage lists only (0x3AC4EA0)");
}

std::optional<std::array<std::uintptr_t, 2>> eye_views() {
    std::scoped_lock lock(g_mutex);
    return g_eye_views;
}

bool set_stereo_layout(bool on) {
    std::scoped_lock lock(g_mutex);
    if (!g_eye_views) {
        log::warn("views: no built split view yet (!vr_split on, reload, !vr_views build)");
        return false;
    }
    // Overrides are indexed by place in found(), which holds ReadStates.
    std::array<std::optional<std::size_t>, 2> index;
    for (std::size_t eye = 0; eye < 2; ++eye) {
        const auto at = std::find(g_found.begin(), g_found.end(), (*g_eye_views)[eye] + kReadStateInView);
        if (at != g_found.end() && static_cast<std::size_t>(at - g_found.begin()) < g_overrides.size()) {
            index[eye] = static_cast<std::size_t>(at - g_found.begin());
        }
    }
    if (!index[0] || !index[1]) {
        log::warn("views: the main or split view isn't among the found views (!vr_views find)");
        return false;
    }
    for (std::size_t eye = 0; eye < 2; ++eye) {
        g_overrides[*index[eye]] = on ? Override{true, false, {0.5f, 1.0f, eye == 0 ? 0.0f : 0.5f, 0.0f}, true}
                                      : Override{true, true, Placement{}, true};
    }
    log::info("views: stereo layout {} (main view {} {}, split view {} {})", on ? "on" : "off", *index[0],
              on ? "left" : "full", *index[1], on ? "right" : "full");
    return true;
}

void on_frame() {
    apply_pending_camera();
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
