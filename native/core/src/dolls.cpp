#include "bg3vr/dolls.hpp"

#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"

#include <safetyhook.hpp>
#include <psapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace bg3vr::dolls {

namespace {

// bg3_dx11.exe 4.1.1.7631656 (agent V's recipe; every call's prologue is checked at install).
constexpr std::uintptr_t kRendererGlobal = 0x6046F98;  // [renderer]
constexpr std::uintptr_t kFrameManagerIndex = 0x604C6FC; // u32
constexpr std::uintptr_t kQueueManager = 0x604B1C0;    // the job-queue manager
constexpr std::uintptr_t kAllocTracking = 0x604B304;   // byte: the game registers allocations when set
constexpr std::uintptr_t kRenderScaleProperty = 0x6005DE8; // Noesis DependencyProperty* RenderScale

constexpr std::uintptr_t kReadyRva = 0x1B72D00;        // bool (renderer): camera, scene and setup done
constexpr std::uintptr_t kCreateFrameRva = 0x3BB1880;  // index (frameManager, desc*), -1 on failure
constexpr std::uintptr_t kAllocRva = 0x4175960;        // void* (size, align)
constexpr std::uintptr_t kViewCtorRva = 0x17F1E60;     // (view, frame, sx, sy, ox, oy, cfg*)
constexpr std::uintptr_t kAddViewRva = 0x4073230;      // (frame, view, 0)
constexpr std::uintptr_t kBuildStagesRva = 0x3AC4EA0;  // (renderer, view)
constexpr std::uintptr_t kSetSceneRva = 0x3CCC550;     // (view, scene)
constexpr std::uintptr_t kSetAtmosphereRva = 0x3CC9700; // (view, atmosphere, const FixedString* id, float blend, bool)
constexpr std::uintptr_t kSetLightingRva = 0x3CCAAC0;   // same shape
constexpr std::uintptr_t kSetCameraRva = 0x17F2B30;    // (view, controller, 0)
constexpr std::uintptr_t kDetachCameraRva = 0x3CC90B0; // (view, 0, 0)
constexpr std::uintptr_t kFreeViewFrameRva = 0x1B76620; // (frame index, view): self-deferring remove, delete, destroy
constexpr std::uintptr_t kEnableFrameRva = 0x1B79150;  // (frame index, bool): self-deferring
constexpr std::uintptr_t kGetQueueRva = 0x40F8B20;     // queue* (manager, n)
constexpr std::uintptr_t kQueueAllocRva = 0x40F9B30;   // record* (allocator, size, align)
constexpr std::uintptr_t kRendererDtorRva = 0x1B76070; // (renderer): destroying a doll
constexpr std::uintptr_t kRebindAtmosphereRva = 0x1B76990; // (renderer, ...): new atmosphere for view 1
constexpr std::uintptr_t kRebindLightingRva = 0x1B77150;   // (renderer, ...): new lighting for view 1
constexpr std::uintptr_t kConnectRva = 0x155DA40;      // LSPaperdoll::OnConnectToView(this, ...)
constexpr std::uintptr_t kDisconnectRva = 0x155DAC0;   // LSPaperdoll::OnDisconnect(this)
constexpr std::uintptr_t kPointToScreenRva = 0x45FC6B0; // Point* (visual, Point* out, const Point* in)
constexpr std::uintptr_t kGetValueRva = 0x45EE7B0;     // const void* (element, property, 4)

constexpr std::uintptr_t kEntryVtable = 0x55EE6F0;
constexpr std::uintptr_t kRendererVtable = 0x5646D78;
constexpr std::size_t kEntryCount = 0x26C, kEntries = 0x270;
constexpr std::size_t kEntryUri = 0x08, kEntryRenderer = 0x70;
constexpr std::size_t kRendererFrame = 0x3A0, kRendererView = 0x3A8;
constexpr std::size_t kRendererAtmosphere = 0x3C0, kRendererAtmosphereId = 0x3C8;
constexpr std::size_t kRendererLighting = 0x3D0, kRendererLightingId = 0x3D8;
constexpr std::size_t kRendererEye = 0x478, kRendererTarget = 0x484;
constexpr std::size_t kViewWriteEnabled = 0x21E0, kViewReadEnabled = 0x2158;
constexpr std::size_t kViewController = 0x2260, kViewScene = 0x2B10, kViewConfig = 0x2B70, kViewConfigSize = 0x64;
constexpr std::size_t kView2274 = 0x2274, kViewAtmosphere = 0x28A0;
constexpr std::size_t kViewCameraGuard = 0x2C98, kViewCameraFlag = 0x2CA0;
constexpr std::size_t kFrameDisplaySize = 0x30, kFrameEnabled = 0x42F0;
constexpr std::size_t kFrameOutput = 0x42D8, kFrameTargets = 0x42C8, kFrameTargetIndex = 0x42C0;
constexpr std::size_t kElementUri = 0x298, kElementPixels = 0x2D0;
constexpr std::size_t kControllerProjection = 0x110;

using ReadyFn = std::uint64_t (*)(std::uintptr_t);
using CreateFrameFn = std::uint64_t (*)(std::uintptr_t, const void*);
using AllocFn = std::uintptr_t (*)(std::uint64_t, std::uint64_t);
using ViewCtorFn = void (*)(std::uintptr_t, std::uintptr_t, float, float, float, float, const void*);
using AddViewFn = void (*)(std::uintptr_t, std::uintptr_t, std::uint64_t);
using BuildStagesFn = void (*)(std::uintptr_t, std::uintptr_t);
using SetSceneFn = void (*)(std::uintptr_t, std::uintptr_t);
using SetEnvironmentFn = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, float, bool);
using SetCameraFn = void (*)(std::uintptr_t, std::uintptr_t, std::uint64_t);
using DetachFn = void (*)(std::uintptr_t, std::uint64_t, std::uint64_t);
using FreeViewFrameFn = void (*)(std::int32_t, std::uintptr_t);
using EnableFrameFn = void (*)(std::int32_t, bool);
using GetQueueFn = std::uintptr_t (*)(std::uintptr_t, std::int32_t);
using QueueAllocFn = std::uintptr_t (*)(std::uintptr_t, std::uint64_t, std::uint64_t);
using QueueGateFn = bool (*)(std::uintptr_t);
using JobFn = std::uint64_t (*)(void*);
using ViewSlotFn = void (*)(std::uintptr_t);
using PointToScreenFn = float* (*)(std::uintptr_t, float*, const float*);
using GetValueFn = const void* (*)(std::uintptr_t, std::uintptr_t, std::int32_t);

ModuleInfo g_module;
bool g_ready = false;
std::atomic<bool> g_enabled{true};

enum class State { Creating, Created, Bound, Failed, Gone };

struct Doll {
    // Fixed when found (game thread).
    std::uintptr_t renderer = 0;
    std::int32_t uri = 0;
    std::int32_t f1 = -1;
    std::uintptr_t v1 = 0;
    std::int32_t width = 0, height = 0;
    std::array<std::uint8_t, kViewConfigSize> config{};
    std::uint32_t v1_2274 = 0;
    // The mod's eye-2 frame and view (written once by the create job, read after state >= Created).
    std::atomic<State> state{State::Creating};
    std::atomic<bool> cancelled{false};
    std::int32_t f2 = -1;
    std::uintptr_t frame2 = 0;
    std::uintptr_t v2 = 0;
    // Game thread.
    std::uint8_t enabled_seen = 0xFF;
    std::uint64_t frames = 0;
    // Under g_mutex: what the other threads need.
    std::uintptr_t element = 0;
    std::array<float, 4> rect{}; // UI pixels
    bool rect_valid = false;
    float distance = 0.0f;       // doll camera to its target (doll units)
    std::optional<std::array<vr::Vec3, 2>> eyes_m; // relative to the window, metres
    float window_m = 0.0f;
    std::uint64_t camera_logs = 0;
};

std::mutex g_mutex;                         // guards the shared Doll fields, g_elements, g_dolls' membership
std::deque<std::unique_ptr<Doll>> g_dolls;  // never freed (a queued job may still point at one)
std::set<std::uintptr_t> g_elements;        // connected LSPaperdoll elements

safetyhook::InlineHook g_dtor_hook, g_atmosphere_hook, g_lighting_hook, g_connect_hook, g_disconnect_hook;
std::atomic<std::uint64_t> g_created{0}, g_bound{0}, g_destroyed{0}, g_failed{0}, g_queued{0};

template <class Fn>
Fn game(std::uintptr_t rva) {
    return reinterpret_cast<Fn>(g_module.base + rva);
}

std::uintptr_t peek(std::uintptr_t address) {
    std::uintptr_t value = 0;
    mem::try_copy(&value, address, sizeof(value));
    return value;
}

std::uintptr_t frame_manager() {
    const std::uintptr_t renderer = peek(g_module.base + kRendererGlobal);
    const std::uintptr_t a = renderer ? peek(renderer + 0x1D8) : 0;
    const std::uintptr_t b = a ? peek(a + 0x30) : 0;
    std::uint32_t index = 0;
    mem::try_copy(&index, g_module.base + kFrameManagerIndex, sizeof(index));
    return b ? peek(b + static_cast<std::uintptr_t>(index) * 0xF8) : 0;
}

std::uintptr_t frame_at(std::int32_t index) {
    const std::uintptr_t manager = frame_manager();
    const std::uintptr_t frames = manager ? peek(manager + 0x18) : 0;
    return frames && index >= 0 ? peek(frames + static_cast<std::uintptr_t>(index) * 8) : 0;
}

// The frame's final image object (as 0x1B77C80 picks it): its first qword is the ID3D11Texture2D.
std::uintptr_t output_object(std::uintptr_t frame) {
    if (!frame) {
        return 0;
    }
    if (const std::uintptr_t out = peek(frame + kFrameOutput)) {
        return out;
    }
    std::int32_t index = 0;
    mem::try_copy(&index, frame + kFrameTargetIndex, sizeof(index));
    const std::uintptr_t targets = peek(frame + kFrameTargets);
    return targets ? peek(targets + static_cast<std::uintptr_t>(index) * 8) : 0;
}

ID3D11Texture2D* texture_of(std::uintptr_t object) {
    const std::uintptr_t candidate = object ? peek(object) : 0;
    if (!candidate) {
        return nullptr;
    }
    ID3D11Texture2D* texture = nullptr;
    __try {
        if (FAILED(reinterpret_cast<IUnknown*>(candidate)->QueryInterface(__uuidof(ID3D11Texture2D),
                                                                         reinterpret_cast<void**>(&texture)))) {
            texture = nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        texture = nullptr;
    }
    return texture;
}

// Runs `job(&payload)` now if the game's doll work may run here, else queues it on queue 2 exactly as the game queues
// its own doll frame work (0x1B75A90 / 0x1B79390).
void run_q2(JobFn job, Doll* payload) {
    const std::uintptr_t manager = peek(g_module.base + kQueueManager);
    if (!manager) {
        return;
    }
    auto* lock = reinterpret_cast<PSRWLOCK>(manager + 0x138);
    AcquireSRWLockShared(lock);
    const std::uintptr_t gate = peek(peek(manager + 0x148) + 0x10);
    ReleaseSRWLockShared(lock);
    const auto must_defer = gate ? reinterpret_cast<QueueGateFn>(peek(peek(gate) + 8))(gate) : true;
    if (!must_defer) {
        job(&payload);
        return;
    }
    const std::uintptr_t queue = game<GetQueueFn>(kGetQueueRva)(manager, 2);
    auto* section = reinterpret_cast<LPCRITICAL_SECTION>(queue + 0x40);
    EnterCriticalSection(section);
    auto* record = reinterpret_cast<std::uintptr_t*>(game<QueueAllocFn>(kQueueAllocRva)(peek(queue + 0x10), 0x10, 8));
    record[0] = reinterpret_cast<std::uintptr_t>(job);
    record[1] = reinterpret_cast<std::uintptr_t>(payload);
    LeaveCriticalSection(section);
    g_queued.fetch_add(1, std::memory_order_relaxed);
}

void destroy_eye2(Doll& d) {
    if (!d.v2 || d.f2 < 0) {
        return;
    }
    *reinterpret_cast<std::uint8_t*>(d.v2 + kViewWriteEnabled) = 0;
    if (peek(d.v2 + kViewCameraGuard) == 0) {
        game<DetachFn>(kDetachCameraRva)(d.v2, 0, 0);
    }
    *reinterpret_cast<std::uint64_t*>(d.v2 + kViewCameraFlag) = 0;
    game<SetSceneFn>(kSetSceneRva)(d.v2, 0);
    game<FreeViewFrameFn>(kFreeViewFrameRva)(d.f2, d.v2); // defers itself; removes, deletes the view, destroys the frame
    g_destroyed.fetch_add(1, std::memory_order_relaxed);
}

// Queue 2 (or right away): the right eye's frame and view, as 0x1B75D50 makes the doll's own (agent V 2.2).
std::uint64_t create_job(void* payload) {
    Doll& d = **static_cast<Doll**>(payload);
    if (d.cancelled.load()) {
        d.state = State::Gone;
        return 8;
    }
    const std::uintptr_t manager = frame_manager();
    struct Desc {
        std::uint64_t zero = 0;
        std::int32_t sizes[6]{};
        std::uint64_t zero2 = 0;
        std::int32_t type = 2;
        std::uint8_t flag = 0;
        std::uint8_t pad[3]{};
    } desc;
    static_assert(sizeof(Desc) == 0x30);
    for (int i = 0; i < 3; ++i) {
        desc.sizes[2 * i] = d.width;
        desc.sizes[2 * i + 1] = d.height;
    }
    const std::int32_t index = manager ? static_cast<std::int32_t>(game<CreateFrameFn>(kCreateFrameRva)(manager, &desc)) : -1;
    const std::uintptr_t frame = frame_at(index);
    if (index < 0 || !frame) {
        log::warn("dolls: renderer 0x{:X}: the right eye's frame couldn't be made (index {})", d.renderer, index);
        g_failed.fetch_add(1, std::memory_order_relaxed);
        d.state = State::Failed;
        return 8;
    }
    *reinterpret_cast<std::uint8_t*>(frame + kFrameEnabled) = 0;
    const std::uintptr_t view = game<AllocFn>(kAllocRva)(0x2D90, 0x10);
    if (!view) {
        log::warn("dolls: renderer 0x{:X}: no memory for the right eye's view", d.renderer);
        g_failed.fetch_add(1, std::memory_order_relaxed);
        d.state = State::Failed;
        return 8;
    }
    game<ViewCtorFn>(kViewCtorRva)(view, frame, 1.0f, 1.0f, 0.0f, 0.0f, d.config.data());
    game<AddViewFn>(kAddViewRva)(frame, view, 0);
    *reinterpret_cast<std::uint32_t*>(view + kView2274) = d.v1_2274;
    game<BuildStagesFn>(kBuildStagesRva)(peek(g_module.base + kRendererGlobal), view);
    *reinterpret_cast<std::uint8_t*>(view + kViewWriteEnabled) = 0; // until bound
    *reinterpret_cast<std::uint8_t*>(view + kViewReadEnabled) = 0;
    d.f2 = index;
    d.frame2 = frame;
    d.v2 = view;
    g_created.fetch_add(1, std::memory_order_relaxed);
    log::info("dolls: renderer 0x{:X}: right eye frame {} (0x{:X}, {}x{}) and view 0x{:X} made on thread {}", d.renderer,
              index, frame, d.width, d.height, view, GetCurrentThreadId());
    if (d.cancelled.load()) {
        destroy_eye2(d);
        d.state = State::Gone;
        return 8;
    }
    d.state = State::Created;
    return 8;
}

// Game thread: the right eye's view gets the doll's scene, atmosphere, lighting and camera (as 0x1B740F0, 0x1B75FC0
// and 0x1B76330 give them to view 1) and is switched on.
void bind(Doll& d) {
    const std::uintptr_t r = d.renderer;
    if (const std::uintptr_t a = peek(d.v2 + kViewAtmosphere)) {
        *reinterpret_cast<std::uint64_t*>(a + 0x364) = 0;
        *reinterpret_cast<std::uint32_t*>(a + 0x36C) = 0;
        *reinterpret_cast<std::uint8_t*>(a + 0x370) = 1;
    }
    game<SetSceneFn>(kSetSceneRva)(d.v2, peek(d.v1 + kViewScene));
    if (const std::uintptr_t atmosphere = peek(r + kRendererAtmosphere)) {
        game<SetEnvironmentFn>(kSetAtmosphereRva)(d.v2, atmosphere, r + kRendererAtmosphereId, 0.0f, false);
    }
    if (const std::uintptr_t lighting = peek(r + kRendererLighting)) {
        game<SetEnvironmentFn>(kSetLightingRva)(d.v2, lighting, r + kRendererLightingId, 0.0f, false);
    }
    reinterpret_cast<ViewSlotFn>(peek(peek(d.v2) + 0x30))(d.v2);
    game<SetCameraFn>(kSetCameraRva)(d.v2, peek(d.v1 + kViewController), 0);
    *reinterpret_cast<std::uint8_t*>(d.v2 + kViewWriteEnabled) = 1;
    const std::uintptr_t frame1 = frame_at(d.f1);
    d.enabled_seen = frame1 ? *reinterpret_cast<const std::uint8_t*>(frame1 + kFrameEnabled) : 0;
    game<EnableFrameFn>(kEnableFrameRva)(d.f2, d.enabled_seen != 0);
    d.state = State::Bound;
    g_bound.fetch_add(1, std::memory_order_relaxed);
    log::info("dolls: renderer 0x{:X}: right eye bound (scene 0x{:X}, camera controller 0x{:X}, frame on {})", r,
              peek(d.v1 + kViewScene), peek(d.v1 + kViewController), d.enabled_seen);
}

Doll* find_renderer(std::uintptr_t renderer) {
    for (const auto& d : g_dolls) {
        if (d->renderer == renderer && d->state.load() != State::Gone && d->state.load() != State::Failed) {
            return d.get();
        }
    }
    return nullptr;
}

// A renderer whose right eye couldn't be made: not tried again until it closes (its record turns Gone then).
bool failed_before(std::uintptr_t renderer) {
    for (const auto& d : g_dolls) {
        if (d->renderer == renderer && d->state.load() == State::Failed) {
            return true;
        }
    }
    return false;
}

// The picture's rectangle on the UI, in UI pixels (Noesis PointToScreen of the element's corners).
std::optional<std::array<float, 4>> element_rect(std::uintptr_t element) {
    __try {
        const auto* pixels = reinterpret_cast<const std::int32_t*>(element + kElementPixels);
        float scale = 1.0f;
        if (const auto* value = static_cast<const float*>(
                game<GetValueFn>(kGetValueRva)(element, peek(g_module.base + kRenderScaleProperty), 4))) {
            scale = std::clamp(*value, 0.05f, 1.0f);
        }
        const float in0[2] = {0.0f, 0.0f};
        const float in1[2] = {static_cast<float>(pixels[0]) / scale, static_cast<float>(pixels[1]) / scale};
        float a[2]{}, b[2]{};
        game<PointToScreenFn>(kPointToScreenRva)(element, a, in0);
        game<PointToScreenFn>(kPointToScreenRva)(element, b, in1);
        if (!(b[0] > a[0]) || !(b[1] > a[1])) {
            return std::nullopt;
        }
        return std::array<float, 4>{a[0], a[1], b[0], b[1]};
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return std::nullopt;
    }
}

std::uintptr_t on_renderer_dtor(std::uintptr_t r, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    Doll* d = nullptr;
    {
        std::scoped_lock lock(g_mutex);
        d = find_renderer(r);
        for (const auto& other : g_dolls) {
            if (other->renderer == r && other->state.load() == State::Failed) {
                other->state = State::Gone;
            }
        }
    }
    if (d) {
        const State state = d->state.load();
        if (state == State::Creating) {
            d->cancelled = true; // the queued job destroys what it makes
        } else if (state == State::Created || state == State::Bound) {
            d->state = State::Gone;
            destroy_eye2(*d);
        }
        log::info("dolls: renderer 0x{:X} closes: its right eye is released", r);
    }
    return g_dtor_hook.call<std::uintptr_t>(r, a, b, c);
}

std::uintptr_t on_rebind_atmosphere(std::uintptr_t r, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    const std::uintptr_t result = g_atmosphere_hook.call<std::uintptr_t>(r, a, b, c);
    Doll* d = nullptr;
    {
        std::scoped_lock lock(g_mutex);
        d = find_renderer(r);
    }
    if (d && d->state.load() == State::Bound) {
        if (const std::uintptr_t atmosphere = peek(r + kRendererAtmosphere)) {
            game<SetEnvironmentFn>(kSetAtmosphereRva)(d->v2, atmosphere, r + kRendererAtmosphereId, 0.0f, false);
        }
    }
    return result;
}

std::uintptr_t on_rebind_lighting(std::uintptr_t r, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    const std::uintptr_t result = g_lighting_hook.call<std::uintptr_t>(r, a, b, c);
    Doll* d = nullptr;
    {
        std::scoped_lock lock(g_mutex);
        d = find_renderer(r);
    }
    if (d && d->state.load() == State::Bound) {
        if (const std::uintptr_t lighting = peek(r + kRendererLighting)) {
            game<SetEnvironmentFn>(kSetLightingRva)(d->v2, lighting, r + kRendererLightingId, 0.0f, false);
        }
    }
    return result;
}

std::uintptr_t on_connect(std::uintptr_t element, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    {
        std::scoped_lock lock(g_mutex);
        g_elements.insert(element);
    }
    return g_connect_hook.call<std::uintptr_t>(element, a, b, c);
}

std::uintptr_t on_disconnect(std::uintptr_t element, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    {
        std::scoped_lock lock(g_mutex);
        g_elements.erase(element);
        for (const auto& d : g_dolls) {
            if (d->element == element) {
                d->element = 0;
                d->rect_valid = false;
            }
        }
    }
    return g_disconnect_hook.call<std::uintptr_t>(element, a, b, c);
}

bool prologue(std::uintptr_t rva, std::initializer_list<std::uint8_t> expected) {
    std::size_t i = 0;
    for (const std::uint8_t byte : expected) {
        std::uint8_t actual = 0;
        if (!mem::try_copy(&actual, g_module.base + rva + i++, 1) || actual != byte) {
            log::warn("dolls: exe+0x{:X} doesn't start as expected (byte {} is 0x{:02X}, not 0x{:02X})", rva, i - 1, actual,
                      byte);
            return false;
        }
    }
    return true;
}

template <class Fn>
bool hook(safetyhook::InlineHook& out, std::uintptr_t rva, Fn* detour) {
    auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(g_module.base + rva), reinterpret_cast<void*>(detour));
    if (!h) {
        log::warn("dolls: hooking exe+0x{:X} failed", rva);
        return false;
    }
    out = std::move(*h);
    return true;
}

} // namespace

void configure(const Config& config) {
    const bool on = config.get_bool("ui", "stereo_dolls", true);
    if (g_enabled.exchange(on) != on) {
        log::info("dolls: 3D characters in UI screens {}", on ? "on (for screens opened from now on)" : "off");
    }
}

bool enabled() {
    return g_ready && g_enabled.load(std::memory_order_relaxed);
}

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    configure(config);
    const bool code =
        prologue(kReadyRva, {0x48, 0x8B, 0x91, 0xB0, 0x03, 0x00, 0x00}) &&
        prologue(kCreateFrameRva, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55}) &&
        prologue(kAllocRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10}) &&
        prologue(kViewCtorRva, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08}) &&
        prologue(kAddViewRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10}) &&
        prologue(kBuildStagesRva, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55}) &&
        prologue(kSetSceneRva, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10}) &&
        prologue(kSetAtmosphereRva, {0x48, 0x8B, 0xC4, 0x55}) && prologue(kSetLightingRva, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55}) &&
        prologue(kSetCameraRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57}) &&
        prologue(kDetachCameraRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10}) &&
        prologue(kFreeViewFrameRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10}) &&
        prologue(kEnableFrameRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55}) &&
        prologue(kGetQueueRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10}) &&
        prologue(kQueueAllocRva, {0x40, 0x55, 0x56}) &&
        prologue(kRendererDtorRva, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18}) &&
        prologue(kRebindAtmosphereRva, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x20}) &&
        prologue(kRebindLightingRva, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x20}) &&
        prologue(kConnectRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57}) && prologue(kDisconnectRva, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20}) &&
        prologue(kPointToScreenRva, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57}) &&
        prologue(kGetValueRva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18});
    std::uint8_t tracking = 0;
    mem::try_copy(&tracking, module.base + kAllocTracking, 1);
    if (!code || tracking) {
        health::set("3D UI characters", health::State::Unavailable,
                    tracking ? "the game tracks allocations in this build" : "a function didn't match");
        log::warn("dolls: 3D UI characters unavailable ({})", tracking ? "allocation tracking is on" : "this build differs");
        return;
    }
    g_ready = hook(g_dtor_hook, kRendererDtorRva, &on_renderer_dtor) &&
              hook(g_atmosphere_hook, kRebindAtmosphereRva, &on_rebind_atmosphere) &&
              hook(g_lighting_hook, kRebindLightingRva, &on_rebind_lighting) && hook(g_connect_hook, kConnectRva, &on_connect) &&
              hook(g_disconnect_hook, kDisconnectRva, &on_disconnect);
    health::set("3D UI characters", g_ready ? (g_enabled ? health::State::Ready : health::State::Off) : health::State::Unavailable,
                "[ui] stereo_dolls");
    log::info("dolls: 3D UI characters {}", g_ready ? "ready (a right-eye render is added when a screen shows a character)"
                                                    : "unavailable (a hook failed)");
}

void on_manager_update(std::uintptr_t manager) {
    if (!enabled() || !manager) {
        return;
    }
    const auto count = mem::read<std::int32_t>(manager + kEntryCount);
    const std::uintptr_t entries = peek(manager + kEntries);
    if (count <= 0 || count > 64 || !entries) {
        return;
    }
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uintptr_t entry = peek(entries + 8 * static_cast<std::uintptr_t>(i));
        if (!entry || peek(entry) != g_module.base + kEntryVtable) {
            continue;
        }
        const std::uintptr_t r = peek(entry + kEntryRenderer);
        if (!r || peek(r) != g_module.base + kRendererVtable) {
            continue;
        }
        Doll* d = nullptr;
        bool failed = false;
        {
            std::scoped_lock lock(g_mutex);
            d = find_renderer(r);
            failed = !d && failed_before(r);
        }
        if (failed) {
            continue;
        }
        if (!d) {
            // A new doll, once the game has finished setting it up.
            const std::int32_t f1 = mem::read<std::int32_t>(r + kRendererFrame);
            const std::uintptr_t v1 = peek(r + kRendererView);
            const std::uintptr_t frame1 = frame_at(f1);
            if (!(game<ReadyFn>(kReadyRva)(r) & 0xFF) || !v1 || f1 < 0 || !frame1) {
                continue;
            }
            auto doll = std::make_unique<Doll>();
            doll->renderer = r;
            doll->uri = mem::read<std::int32_t>(entry + kEntryUri);
            doll->f1 = f1;
            doll->v1 = v1;
            mem::try_copy(&doll->width, frame1 + kFrameDisplaySize, sizeof(doll->width));
            mem::try_copy(&doll->height, frame1 + kFrameDisplaySize + 4, sizeof(doll->height));
            mem::try_copy(doll->config.data(), v1 + kViewConfig, kViewConfigSize);
            mem::try_copy(&doll->v1_2274, v1 + kView2274, sizeof(doll->v1_2274));
            if (doll->width < 16 || doll->height < 16 || doll->width > 4096 || doll->height > 4096) {
                continue;
            }
            log::info("dolls: renderer 0x{:X} (frame {}, view 0x{:X}, {}x{}, uri {}) is ready: making its right eye", r, f1,
                      v1, doll->width, doll->height, doll->uri);
            {
                std::scoped_lock lock(g_mutex);
                g_dolls.push_back(std::move(doll));
                d = g_dolls.back().get();
            }
            run_q2(&create_job, d);
            continue;
        }
        if (d->state.load() == State::Created) {
            bind(*d);
        }
        if (d->state.load() != State::Bound) {
            continue;
        }
        ++d->frames;
        // Keep the right eye with the left one: camera, on/off.
        const std::uintptr_t controller = peek(d->v1 + kViewController);
        if (controller && controller != peek(d->v2 + kViewController)) {
            game<SetCameraFn>(kSetCameraRva)(d->v2, controller, 0);
        }
        const std::uintptr_t frame1 = frame_at(d->f1);
        const std::uint8_t on = frame1 ? *reinterpret_cast<const std::uint8_t*>(frame1 + kFrameEnabled) : 0;
        if (on != d->enabled_seen) {
            d->enabled_seen = on;
            game<EnableFrameFn>(kEnableFrameRva)(d->f2, on != 0);
        }
        // The doll camera's distance to what it looks at: the window plane.
        float eye[3]{}, target[3]{};
        mem::try_copy(eye, r + kRendererEye, sizeof(eye));
        mem::try_copy(target, r + kRendererTarget, sizeof(target));
        const float distance = std::sqrt((target[0] - eye[0]) * (target[0] - eye[0]) + (target[1] - eye[1]) * (target[1] - eye[1]) +
                                         (target[2] - eye[2]) * (target[2] - eye[2]));
        // Where its picture is on the UI.
        std::uintptr_t element = 0;
        {
            std::scoped_lock lock(g_mutex);
            for (const std::uintptr_t e : g_elements) {
                if (mem::read<std::int32_t>(e + kElementUri) == d->uri) {
                    element = e;
                    break;
                }
            }
        }
        const auto rect = element ? element_rect(element) : std::nullopt;
        {
            std::scoped_lock lock(g_mutex);
            d->distance = std::isfinite(distance) ? distance : 0.0f;
            d->element = element;
            d->rect_valid = rect.has_value();
            if (rect) {
                d->rect = *rect;
            }
        }
        if (d->frames == 1 || d->frames % 600 == 0) {
            log::info("dolls: renderer 0x{:X}: element 0x{:X}, rect {}, camera distance {:.3f}, frame on {}", r, element,
                      rect ? std::format("({:.0f}, {:.0f})-({:.0f}, {:.0f})", (*rect)[0], (*rect)[1], (*rect)[2], (*rect)[3])
                           : std::string("none"),
                      distance, on);
        }
    }
}

bool replaces(std::uintptr_t element) {
    if (!enabled()) {
        return false;
    }
    std::scoped_lock lock(g_mutex);
    for (const auto& d : g_dolls) {
        if (d->element == element && d->state.load() == State::Bound && d->rect_valid && d->eyes_m) {
            return true;
        }
    }
    return false;
}

std::optional<EyeCamera> eye_camera(std::uintptr_t view, std::uintptr_t controller) {
    if (!enabled() || !view) {
        return std::nullopt;
    }
    std::scoped_lock lock(g_mutex);
    for (const auto& d : g_dolls) {
        if (d->state.load() != State::Bound || (view != d->v1 && view != d->v2)) {
            continue;
        }
        const int eye = view == d->v1 ? 0 : 1;
        if (!d->eyes_m || d->window_m <= 0.0f || d->distance <= 0.0f) {
            return std::nullopt;
        }
        // The doll camera's frustum at the target distance is the window (its projection's diagonal: 1 / tan).
        float projection[16]{};
        if (!mem::try_copy(projection, controller + kControllerProjection, sizeof(projection)) || projection[0] == 0.0f ||
            projection[5] == 0.0f) {
            return std::nullopt;
        }
        const float half_w = d->distance / std::abs(projection[0]);
        const float half_h = d->distance / std::abs(projection[5]);
        const float scale = 2.0f * half_w / d->window_m; // doll units per metre
        const vr::Vec3 e{(*d->eyes_m)[eye][0] * scale, (*d->eyes_m)[eye][1] * scale,
                         std::max((*d->eyes_m)[eye][2] * scale, 0.05f * d->distance)};
        constexpr float kDegrees = 57.2957795f;
        EyeCamera out;
        out.eye = eye;
        out.offset = {e[0], e[1], e[2] - d->distance};
        out.fov.left_deg = std::atan((-half_w - e[0]) / e[2]) * kDegrees;
        out.fov.right_deg = std::atan((half_w - e[0]) / e[2]) * kDegrees;
        out.fov.down_deg = std::atan((-half_h - e[1]) / e[2]) * kDegrees;
        out.fov.up_deg = std::atan((half_h - e[1]) / e[2]) * kDegrees;
        if (d->camera_logs++ % 900 == 0) {
            log::info("dolls: renderer 0x{:X} {} eye: offset ({:.3f}, {:.3f}, {:.3f}) doll units, fov l {:.1f} r {:.1f} d {:.1f} "
                      "u {:.1f} deg (window {:.3f} m, {:.3f} x {:.3f} doll units at {:.3f})",
                      d->renderer, eye ? "right" : "left", out.offset[0], out.offset[1], out.offset[2], out.fov.left_deg,
                      out.fov.right_deg, out.fov.down_deg, out.fov.up_deg, d->window_m, 2.0f * half_w, 2.0f * half_h,
                      d->distance);
        }
        return out;
    }
    return std::nullopt;
}

std::vector<Shown> shown() {
    std::vector<Shown> out;
    if (!enabled()) {
        return out;
    }
    std::scoped_lock lock(g_mutex);
    for (const auto& d : g_dolls) {
        if (d->state.load() != State::Bound || !d->rect_valid || d->enabled_seen == 0) {
            continue;
        }
        Shown s;
        s.id = d->renderer;
        s.rect = d->rect;
        s.texture[0] = texture_of(output_object(frame_at(d->f1)));
        s.texture[1] = texture_of(output_object(d->frame2));
        if (!s.texture[0] || !s.texture[1]) {
            for (ID3D11Texture2D* t : s.texture) {
                if (t) {
                    t->Release();
                }
            }
            continue;
        }
        out.push_back(s);
    }
    return out;
}

void set_window(std::uintptr_t id, std::optional<std::array<vr::Vec3, 2>> eyes_m, float width_m) {
    std::scoped_lock lock(g_mutex);
    for (const auto& d : g_dolls) {
        if (d->renderer == id && d->state.load() == State::Bound) {
            d->eyes_m = eyes_m;
            d->window_m = width_m;
        }
    }
}

void report() {
    log::info("dolls: {}; right eyes made {}, bound {}, released {}, failed {} ({} made through the game's queue)",
              !g_ready ? "unavailable" : g_enabled.load() ? "on" : "off", g_created.load(), g_bound.load(),
              g_destroyed.load(), g_failed.load(), g_queued.load());
}

} // namespace bg3vr::dolls
