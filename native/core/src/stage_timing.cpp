#include "bg3vr/stage_timing.hpp"

#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/views.hpp"

#include <windows.h>
#include <d3d11.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace bg3vr::stage_timing {

namespace {

constexpr std::uintptr_t kViewExecuteRva = 0x3CC7010; // (view): the view's own buffer, then every stage's slot 8
constexpr std::uintptr_t kCommandExecuteRva = 0x407F240; // (cmd): runs a command buffer against D3D11
// 0x3CC7010..0x3CC7061 as read (the call's rel32 included): the mod runs these steps itself, so all of it must match.
constexpr std::uint8_t kViewExecuteBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x80, 0xB9, 0x50, 0x21, 0x00, 0x00, 0x00, 0x74,
    0x09, 0x48, 0x83, 0xC1, 0x50, 0xE8, 0x11, 0x82, 0x3B, 0x00, 0xC6, 0x87, 0x50, 0x21, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x5F, 0x28,
    0x48, 0x85, 0xDB, 0x74, 0x23, 0x48, 0x63, 0x47, 0x34, 0x48, 0x8D, 0x3C, 0xC3, 0x48, 0x3B, 0xDF, 0x74, 0x16, 0x0F, 0x1F, 0x40,
    0x00, 0x48, 0x8B, 0x0B, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x40, 0x48, 0x83, 0xC3, 0x08, 0x48, 0x3B, 0xDF, 0x75, 0xEE};
constexpr std::uint8_t kCommandExecuteBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x4C, 0x89,
                                                 0x44, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xF9, 0x80,
                                                 0xB9, 0x2D, 0x12, 0x00, 0x00, 0x00};

constexpr std::size_t kViewOwnBuffer = 0x50, kViewOwnPending = 0x2150, kViewStages = 0x28, kViewStageCount = 0x34;
constexpr std::size_t kCmdRenderer = 0x38, kRendererLock = 0x78, kCmdExecutes = 0x122D, kCmdContext = 0x48;

// Keys: view slot (0 left eye, 1 right eye, 2 any other view, 3 outside every view's window) x stage (0..0xFF, 0x100
// the view's own buffer, 0x101 a buffer outside every stage's window).
constexpr int kViewSlots = 4, kStageKeys = 0x102, kOwnBuffer = 0x100, kNoStage = 0x101;
constexpr int kKeys = kViewSlots * kStageKeys;
constexpr const char* kSlotNames[kViewSlots] = {"left", "right", "other views", "outside views"};

constexpr int kRing = 6;      // frames in flight before a result is read (no stall)
constexpr int kPairs = 384;   // timed executes per frame

struct Pair {
    int key = 0;
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
};
struct FrameQueries {
    ID3D11Query* disjoint = nullptr;
    std::array<Pair, kPairs> pairs{};
    int used = 0;
    bool open = false;    // disjoint begun, not ended
    bool pending = false; // ended, results not read
    bool overflow = false;
};

ModuleInfo g_module;
safetyhook::InlineHook g_view_hook, g_command_hook;
std::atomic<bool> g_enabled{true};
std::atomic<int> g_report_frames{600};
std::atomic<bool> g_installed{false};

// Under the renderer's critical section (taken in the executor hook): queries and the ring.
ID3D11DeviceContext* g_context = nullptr;
std::array<FrameQueries, kRing> g_ring{};
int g_current = 0;
std::uint64_t g_frame = ~0ull;
bool g_queries_failed = false;
bool g_context_checked = false;

// Accumulated since the last report (g_acc).
std::mutex g_acc;
struct Totals {
    double gpu_ms = 0.0;       // GPU time of the buffers executed with this key
    double exec_cpu_ms = 0.0;  // render-thread time inside the executor
    double window_cpu_ms = 0.0; // the stage's slot 8 (wait + execute), from the view loop
    std::uint64_t executes = 0;
    std::uint64_t windows = 0;
};
std::array<Totals, kKeys> g_totals{};
std::array<double, kViewSlots> g_view_window_ms{}; // whole 0x3CC7010 per view slot
std::uint64_t g_frames_collected = 0, g_frames_dropped = 0, g_frames_disjoint = 0, g_frames_overflow = 0;
double g_gpu_span_ms = 0.0; // first timestamp to last, per collected frame
int g_max_executes = 0;
std::set<DWORD> g_exec_threads, g_view_threads;
std::uint64_t g_report_at = 0;
std::atomic<bool> g_report_due{false};

// Which window the current thread is in.
thread_local int t_view_slot = 3;
thread_local int t_stage = kNoStage;
thread_local int t_exec_depth = 0;
thread_local std::uint64_t t_eyes_frame = ~0ull;
thread_local std::array<std::uintptr_t, 2> t_eyes{};

double g_qpc_ms = 0.0; // ms per QPC tick

std::int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// A stage's id, cached by vtable (slot 1 returns a constant).
std::mutex g_id_mutex;
std::vector<std::pair<std::uintptr_t, int>> g_ids;
int stage_key(std::uintptr_t stage) {
    std::uintptr_t vtable = 0;
    if (!stage || !mem::try_copy(&vtable, stage, sizeof(vtable))) {
        return kNoStage;
    }
    std::scoped_lock lock(g_id_mutex);
    for (const auto& [v, id] : g_ids) {
        if (v == vtable) {
            return id;
        }
    }
    const int id = views::stage_id_of(stage);
    const int key = id >= 0 && id <= 0xFF ? id : kNoStage;
    g_ids.emplace_back(vtable, key);
    return key;
}

int view_slot(std::uintptr_t view) {
    const std::uint64_t frame = render::frame_count();
    if (frame != t_eyes_frame) {
        t_eyes_frame = frame;
        const auto eyes = views::eye_views();
        t_eyes = eyes ? *eyes : std::array<std::uintptr_t, 2>{};
    }
    return view && view == t_eyes[0] ? 0 : view && view == t_eyes[1] ? 1 : 2;
}

using ViewExecuteFn = void (*)(std::uintptr_t view);
using CommandExecuteFn = void (*)(std::uintptr_t cmd, std::uintptr_t a, std::uintptr_t b);
using StageWaitFn = void (*)(std::uintptr_t stage);

// The game's 0x3CC7010, step for step, with a window around each part.
void on_view_execute(std::uintptr_t view) {
    const int slot = view_slot(view);
    const int saved_slot = t_view_slot, saved_stage = t_stage;
    t_view_slot = slot;
    const std::int64_t view_start = qpc();
    auto* bytes = reinterpret_cast<std::uint8_t*>(view);
    if (bytes[kViewOwnPending]) {
        t_stage = kOwnBuffer;
        reinterpret_cast<CommandExecuteFn>(g_module.base + kCommandExecuteRva)(view + kViewOwnBuffer, 0, 0);
    }
    bytes[kViewOwnPending] = 0;
    const auto* stages = *reinterpret_cast<std::uintptr_t* const*>(view + kViewStages);
    std::array<std::pair<int, double>, 64> windows{};
    int window_count = 0;
    if (stages) {
        const std::int32_t count = *reinterpret_cast<const std::int32_t*>(view + kViewStageCount);
        for (const std::uintptr_t* s = stages; s != stages + count; ++s) {
            const std::uintptr_t stage = *s;
            const int key = stage_key(stage);
            t_stage = key;
            const std::int64_t t0 = qpc();
            const auto vtable = *reinterpret_cast<const std::uintptr_t*>(stage);
            reinterpret_cast<StageWaitFn>(*reinterpret_cast<const std::uintptr_t*>(vtable + 0x40))(stage);
            if (window_count < static_cast<int>(windows.size())) {
                windows[static_cast<std::size_t>(window_count++)] = {key, static_cast<double>(qpc() - t0) * g_qpc_ms};
            }
        }
    }
    const double view_ms = static_cast<double>(qpc() - view_start) * g_qpc_ms;
    t_view_slot = saved_slot;
    t_stage = saved_stage;
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock(g_acc);
    for (int i = 0; i < window_count; ++i) {
        Totals& t = g_totals[static_cast<std::size_t>(slot * kStageKeys + windows[static_cast<std::size_t>(i)].first)];
        t.window_cpu_ms += windows[static_cast<std::size_t>(i)].second;
        ++t.windows;
    }
    g_view_window_ms[static_cast<std::size_t>(slot)] += view_ms;
    if (g_view_threads.size() < 16) {
        g_view_threads.insert(GetCurrentThreadId());
    }
}

ID3D11Query* make_query(ID3D11Device* device, D3D11_QUERY type) {
    D3D11_QUERY_DESC desc{type, 0};
    ID3D11Query* q = nullptr;
    if (FAILED(device->CreateQuery(&desc, &q))) {
        return nullptr;
    }
    return q;
}

bool make_queries() {
    ID3D11Device* device = render::device();
    if (!device) {
        return false;
    }
    device->GetImmediateContext(&g_context);
    for (FrameQueries& f : g_ring) {
        f.disjoint = make_query(device, D3D11_QUERY_TIMESTAMP_DISJOINT);
        if (!f.disjoint) {
            return false;
        }
        for (Pair& p : f.pairs) {
            p.begin = make_query(device, D3D11_QUERY_TIMESTAMP);
            p.end = make_query(device, D3D11_QUERY_TIMESTAMP);
            if (!p.begin || !p.end) {
                return false;
            }
        }
    }
    log::info("perf: stage timing made {} x {} timestamp pairs on the game's device", kRing, kPairs);
    return true;
}

// Reads a finished frame's queries (no flush, no wait: not ready means dropped).
void collect(FrameQueries& f) {
    f.pending = false;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (g_context->GetData(f.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        std::scoped_lock lock(g_acc);
        ++g_frames_dropped;
        return;
    }
    if (dj.Disjoint || !dj.Frequency) {
        std::scoped_lock lock(g_acc);
        ++g_frames_disjoint;
        return;
    }
    std::array<std::pair<int, double>, kPairs> results{};
    int n = 0;
    std::uint64_t first = ~0ull, last = 0;
    for (int i = 0; i < f.used; ++i) {
        std::uint64_t a = 0, b = 0;
        if (g_context->GetData(f.pairs[static_cast<std::size_t>(i)].begin, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            g_context->GetData(f.pairs[static_cast<std::size_t>(i)].end, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            std::scoped_lock lock(g_acc);
            ++g_frames_dropped;
            return;
        }
        first = std::min(first, a);
        last = std::max(last, b);
        results[static_cast<std::size_t>(n++)] = {f.pairs[static_cast<std::size_t>(i)].key,
                                                  b > a ? static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency) : 0.0};
    }
    std::scoped_lock lock(g_acc);
    for (int i = 0; i < n; ++i) {
        g_totals[static_cast<std::size_t>(results[static_cast<std::size_t>(i)].first)].gpu_ms += results[static_cast<std::size_t>(i)].second;
    }
    if (n && last > first) {
        g_gpu_span_ms += static_cast<double>(last - first) * 1000.0 / static_cast<double>(dj.Frequency);
    }
    ++g_frames_collected;
    if (f.overflow) {
        ++g_frames_overflow;
    }
}

// A new frame reached the executor: close the last one, read the oldest, open this one.
void turn_frame(std::uint64_t frame) {
    FrameQueries& done = g_ring[static_cast<std::size_t>(g_current)];
    if (done.open) {
        g_context->End(done.disjoint);
        done.open = false;
        done.pending = true;
    }
    g_current = (g_current + 1) % kRing;
    FrameQueries& next = g_ring[static_cast<std::size_t>(g_current)];
    if (next.pending) {
        collect(next);
    }
    next.used = 0;
    next.overflow = false;
    g_context->Begin(next.disjoint);
    next.open = true;
    if (g_report_at == 0) {
        g_report_at = frame + static_cast<std::uint64_t>(g_report_frames.load(std::memory_order_relaxed));
    } else if (frame >= g_report_at) {
        g_report_at = frame + static_cast<std::uint64_t>(g_report_frames.load(std::memory_order_relaxed));
        g_report_due.store(true, std::memory_order_relaxed);
    }
}

void on_command_execute(std::uintptr_t cmd, std::uintptr_t a, std::uintptr_t b) {
    if (!g_enabled.load(std::memory_order_relaxed) || t_exec_depth > 0 || !cmd ||
        !reinterpret_cast<const std::uint8_t*>(cmd)[kCmdExecutes]) {
        g_command_hook.call<void>(cmd, a, b);
        return;
    }
    const std::uintptr_t renderer = *reinterpret_cast<const std::uintptr_t*>(cmd + kCmdRenderer);
    auto* section = reinterpret_cast<CRITICAL_SECTION*>(renderer + kRendererLock);
    const int key = t_view_slot * kStageKeys + t_stage;
    EnterCriticalSection(section); // the executor's own lock (re-entered by it): the context is ours meanwhile
    ++t_exec_depth;
    Pair* pair = nullptr;
    if (!g_queries_failed) {
        if (!g_context && !make_queries()) {
            g_queries_failed = true;
            log::warn("perf: stage timing can't make its GPU queries; CPU times only");
        }
        if (g_context && !g_context_checked) {
            g_context_checked = true;
            std::uintptr_t game_context = 0;
            mem::try_copy(&game_context, cmd + kCmdContext, sizeof(game_context));
            log::info("perf: the executor's context [cmd+0x48] 0x{:X}, the device's immediate context 0x{:X} ({}); "
                      "executor thread {}",
                      game_context, reinterpret_cast<std::uintptr_t>(g_context),
                      game_context == reinterpret_cast<std::uintptr_t>(g_context) ? "the same" : "DIFFERENT",
                      GetCurrentThreadId());
        }
    }
    if (g_context && !g_queries_failed) {
        const std::uint64_t frame = render::frame_count();
        if (frame != g_frame) {
            g_frame = frame;
            turn_frame(frame);
        }
        FrameQueries& f = g_ring[static_cast<std::size_t>(g_current)];
        if (f.used < kPairs) {
            pair = &f.pairs[static_cast<std::size_t>(f.used++)];
            pair->key = key;
            g_context->End(pair->begin);
        } else {
            f.overflow = true;
        }
    }
    const std::int64_t t0 = qpc();
    g_command_hook.call<void>(cmd, a, b);
    const double ms = static_cast<double>(qpc() - t0) * g_qpc_ms;
    if (pair) {
        g_context->End(pair->end);
    }
    const int used = g_ring[static_cast<std::size_t>(g_current)].used;
    --t_exec_depth;
    LeaveCriticalSection(section);
    {
        std::scoped_lock lock(g_acc);
        Totals& t = g_totals[static_cast<std::size_t>(key)];
        t.exec_cpu_ms += ms;
        ++t.executes;
        g_max_executes = std::max(g_max_executes, used);
        if (g_exec_threads.size() < 16) {
            g_exec_threads.insert(GetCurrentThreadId());
        }
    }
    if (g_report_due.exchange(false, std::memory_order_relaxed)) {
        report();
    }
}

bool bytes_match(std::uintptr_t address, const std::uint8_t* expected, std::size_t size) {
    std::array<std::uint8_t, 128> actual{};
    return size <= actual.size() && mem::try_copy(actual.data(), address, size) && std::memcmp(actual.data(), expected, size) == 0;
}

std::string stage_name(int stage) {
    return stage == kOwnBuffer ? std::string("own buffer") : stage == kNoStage ? std::string("no stage") : std::format("0x{:02X}", stage);
}

} // namespace

void configure(const Config& config) {
    g_enabled.store(config.get_bool("perf", "stage_timing", true), std::memory_order_relaxed);
    g_report_frames.store(static_cast<int>(std::clamp<long long>(config.get_int("perf", "stage_timing_frames", 600), 60, 100000)),
                          std::memory_order_relaxed);
}

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    configure(config);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_ms = 1000.0 / static_cast<double>(f.QuadPart);
    const bool view_ok = bytes_match(module.base + kViewExecuteRva, kViewExecuteBytes, sizeof(kViewExecuteBytes));
    const bool command_ok = bytes_match(module.base + kCommandExecuteRva, kCommandExecuteBytes, sizeof(kCommandExecuteBytes));
    if (!view_ok || !command_ok) {
        health::set("Stage timing", health::State::Unavailable, "the executors differ in this build");
        log::warn("perf: stage timing off: the view executor ({}) / command executor ({}) differ in this build",
                  view_ok ? "matches" : "differs", command_ok ? "matches" : "differs");
        return;
    }
    auto view_hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(module.base + kViewExecuteRva),
                                                    reinterpret_cast<void*>(&on_view_execute));
    auto command_hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(module.base + kCommandExecuteRva),
                                                       reinterpret_cast<void*>(&on_command_execute));
    if (!view_hook || !command_hook) {
        health::set("Stage timing", health::State::Unavailable, "hooks failed");
        log::error("perf: stage timing hooks failed");
        return;
    }
    g_view_hook = std::move(*view_hook);
    g_command_hook = std::move(*command_hook);
    g_installed.store(true);
    health::set("Stage timing", g_enabled.load() ? health::State::Ready : health::State::Off, "[perf] stage_timing");
    log::info("perf: stage timing hooked (view executor exe+0x{:X}, command executor exe+0x{:X}); a report every {} frames",
              kViewExecuteRva, kCommandExecuteRva, g_report_frames.load());
}

Window window() {
    return {t_view_slot <= 1 ? t_view_slot : -1, t_stage == kNoStage ? -1 : t_stage};
}

void report() {
    if (!g_installed.load()) {
        return;
    }
    std::array<Totals, kKeys> totals;
    std::array<double, kViewSlots> view_ms;
    std::uint64_t collected, dropped, disjoint, overflow;
    double span;
    int max_executes;
    std::string threads;
    {
        std::scoped_lock lock(g_acc);
        totals = g_totals;
        view_ms = g_view_window_ms;
        collected = g_frames_collected;
        dropped = g_frames_dropped;
        disjoint = g_frames_disjoint;
        overflow = g_frames_overflow;
        span = g_gpu_span_ms;
        max_executes = g_max_executes;
        threads = "executor threads";
        for (DWORD t : g_exec_threads) {
            threads += std::format(" {}", t);
        }
        threads += "; view threads";
        for (DWORD t : g_view_threads) {
            threads += std::format(" {}", t);
        }
        g_totals = {};
        g_view_window_ms = {};
        g_frames_collected = g_frames_dropped = g_frames_disjoint = g_frames_overflow = 0;
        g_gpu_span_ms = 0.0;
        g_max_executes = 0;
        g_exec_threads.clear();
        g_view_threads.clear();
    }
    // CPU numbers are per frame of the report window; GPU numbers per collected frame.
    const double frames = static_cast<double>(std::max(1, g_report_frames.load()));
    const double gpu_frames = static_cast<double>(std::max<std::uint64_t>(1, collected));
    std::array<double, kViewSlots> gpu{}, exec{};
    for (int s = 0; s < kViewSlots; ++s) {
        for (int k = 0; k < kStageKeys; ++k) {
            const Totals& t = totals[static_cast<std::size_t>(s * kStageKeys + k)];
            gpu[static_cast<std::size_t>(s)] += t.gpu_ms;
            exec[static_cast<std::size_t>(s)] += t.exec_cpu_ms;
        }
    }
    log::info("perf: stage timing, {} GPU frames read ({} not ready, {} disjoint, {} over {} buffers); GPU per frame: "
              "busy span {:.2f} ms; left {:.2f}, right {:.2f}, other views {:.2f}, outside views {:.2f} ms. Render-thread "
              "execute per frame: left {:.2f}, right {:.2f}, other {:.2f}, outside {:.2f} ms; view windows (waits "
              "included): left {:.2f}, right {:.2f}, other {:.2f} ms; up to {} buffers a frame; {}",
              collected, dropped, disjoint, overflow, kPairs, span / gpu_frames, gpu[0] / gpu_frames, gpu[1] / gpu_frames,
              gpu[2] / gpu_frames, gpu[3] / gpu_frames, exec[0] / frames, exec[1] / frames, exec[2] / frames, exec[3] / frames,
              view_ms[0] / frames, view_ms[1] / frames, view_ms[2] / frames, max_executes, threads);
    for (int s = 0; s < kViewSlots; ++s) {
        std::vector<std::pair<double, int>> order;
        for (int k = 0; k < kStageKeys; ++k) {
            const Totals& t = totals[static_cast<std::size_t>(s * kStageKeys + k)];
            if (t.executes || t.windows) {
                order.emplace_back(t.gpu_ms / gpu_frames + t.window_cpu_ms / frames * 1e-3, k);
            }
        }
        if (order.empty()) {
            continue;
        }
        std::sort(order.begin(), order.end(), std::greater<>());
        std::string text;
        for (std::size_t i = 0; i < order.size() && i < 15; ++i) {
            const int k = order[i].second;
            const Totals& t = totals[static_cast<std::size_t>(s * kStageKeys + k)];
            text += std::format("\n    {:<10} GPU {:6.3f} ms, execute {:6.3f} ms, window {:6.3f} ms, {:.1f} buffers", stage_name(k),
                                t.gpu_ms / gpu_frames, t.exec_cpu_ms / frames, t.window_cpu_ms / frames,
                                static_cast<double>(t.executes) / frames);
        }
        log::info("perf: {} per frame, by stage (GPU first):{}", kSlotNames[s], text);
    }
    // What the right eye costs on top of the left, per stage (the candidates for sharing, agent AT).
    std::vector<std::pair<double, int>> extra;
    for (int k = 0; k < kStageKeys; ++k) {
        const double r = totals[static_cast<std::size_t>(kStageKeys + k)].gpu_ms / gpu_frames;
        if (r > 0.05) {
            extra.emplace_back(r, k);
        }
    }
    std::sort(extra.begin(), extra.end(), std::greater<>());
    std::string text;
    for (std::size_t i = 0; i < extra.size() && i < 10; ++i) {
        const int k = extra[i].second;
        text += std::format(" {} {:.2f}/{:.2f}", stage_name(k), extra[i].first, totals[static_cast<std::size_t>(k)].gpu_ms / gpu_frames);
    }
    if (!text.empty()) {
        log::info("perf: right / left GPU ms per stage:{}", text);
    }
    // Agent AT 2.4: both eyes' skinning keys its per-mesh offsets by [view+0x226C]; equal values would race.
    if (const auto eyes = views::eye_views()) {
        std::int32_t sides[2] = {-1, -1};
        mem::try_copy(&sides[0], (*eyes)[0] + 0x226C, sizeof(sides[0]));
        mem::try_copy(&sides[1], (*eyes)[1] + 0x226C, sizeof(sides[1]));
        log::info("perf: skinning slots [view+0x226C]: left {}, right {}{}", sides[0], sides[1],
                  sides[0] == sides[1] ? " (THE SAME: both eyes write one slot)" : "");
    }
}

} // namespace bg3vr::stage_timing
