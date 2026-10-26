#include "bg3vr/render.hpp"

#include "bg3vr/watchdog.hpp"

#include "bg3vr/config.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/win_util.hpp"

#include <safetyhook.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace bg3vr::render {

namespace {

safetyhook::MidHook g_present_hook;
std::atomic<IDXGISwapChain*> g_swapchain{nullptr};
std::atomic<std::uintptr_t> g_render_frame{0};
std::atomic<ID3D11Device*> g_device{nullptr};
std::atomic<PresentCallback> g_callback{nullptr};
std::mutex g_tasks_mutex;
std::vector<std::function<void()>> g_tasks;
std::atomic<bool> g_have_tasks{false};
std::atomic<int> g_sync_override{-1};
// The game's own Present arguments (before any override), for the report.
std::atomic<std::uint32_t> g_game_sync_interval{0};
std::atomic<std::uint32_t> g_game_present_flags{0};
std::atomic<std::uint64_t> g_sync_overrides{0};
std::atomic<std::uint64_t> g_frames{0};
std::uint64_t g_last_frames = 0;
std::chrono::steady_clock::time_point g_last_report = std::chrono::steady_clock::now();
ModuleInfo g_module;
std::atomic<int> g_max_latency{1}; // [render] max_frame_latency (0 = leave DXGI's default)

void describe_swapchain(IDXGISwapChain* swapchain) {
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
        g_device.store(device, std::memory_order_release);
        device->Release(); // we don't own it; the renderer keeps it alive
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    // Which GPU the game renders on: the headset needs the same one, and an integrated GPU would
    // explain a low frame rate.
    IDXGIDevice* dxgi = nullptr;
    if (device && SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) {
        IDXGIAdapter* adapter = nullptr;
        DXGI_ADAPTER_DESC adapter_desc{};
        if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&adapter_desc))) {
            log::info("render: GPU '{}' (vendor 0x{:04X}, {} MB dedicated), feature level 0x{:X}",
                      to_utf8(adapter_desc.Description), adapter_desc.VendorId,
                      adapter_desc.DedicatedVideoMemory >> 20, static_cast<unsigned>(device->GetFeatureLevel()));
        }
        if (adapter) {
            adapter->Release();
        }
        dxgi->Release();
    }
    // The GPU frame queue (agent W 3.3): the game never sets it, so DXGI's default (3) lets the CPU run up to 3
    // frames ahead of the GPU: in VR that is up to 3 frames between a head pose and its image.
    IDXGIDevice1* dxgi1 = nullptr;
    if (device && SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgi1)))) {
        UINT before = 0;
        dxgi1->GetMaximumFrameLatency(&before);
        const int wanted = g_max_latency.load(std::memory_order_relaxed);
        if (wanted > 0 && static_cast<UINT>(wanted) != before) {
            const HRESULT hr = dxgi1->SetMaximumFrameLatency(static_cast<UINT>(wanted));
            log::info("render: GPU frame queue {} -> {}{}", before, wanted, SUCCEEDED(hr) ? "" : " (failed)");
        } else {
            log::info("render: GPU frame queue {} ([render] max_frame_latency {})", before, wanted);
        }
        dxgi1->Release();
    }
    log::info("render: swapchain 0x{:X} device 0x{:X}: {}x{} format {} buffers {} swap effect {} windowed {} flags 0x{:X}",
              reinterpret_cast<std::uintptr_t>(swapchain), reinterpret_cast<std::uintptr_t>(device),
              desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<int>(desc.BufferDesc.Format),
              desc.BufferCount, static_cast<int>(desc.SwapEffect), desc.Windowed, desc.Flags);
}

// Just before the game's Present call: rcx = the IDXGISwapChain*, rdi = the RenderFrame.
std::atomic<std::uint32_t> g_presenting_thread{0};
std::atomic<double> g_last_frame_ms{0.0}, g_last_callback_ms{0.0};
LARGE_INTEGER g_last_present{};

double qpc_ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    static const double per_ms = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    return static_cast<double>(b.QuadPart - a.QuadPart) / per_ms;
}

void on_present(safetyhook::Context& ctx) {
    auto* swapchain = reinterpret_cast<IDXGISwapChain*>(ctx.rcx);
    if (!swapchain) {
        return;
    }
    g_presenting_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    LARGE_INTEGER entered;
    QueryPerformanceCounter(&entered);
    if (g_last_present.QuadPart) {
        g_last_frame_ms.store(qpc_ms(g_last_present, entered), std::memory_order_relaxed);
    }
    g_last_present = entered;
    watchdog::mark("render: present hook entered");
    g_frames.fetch_add(1, std::memory_order_relaxed);
    g_render_frame.store(ctx.rdi, std::memory_order_release);
    g_game_sync_interval.store(static_cast<std::uint32_t>(ctx.rdx), std::memory_order_relaxed);
    g_game_present_flags.store(static_cast<std::uint32_t>(ctx.r8), std::memory_order_relaxed);
    if (g_swapchain.exchange(swapchain, std::memory_order_acq_rel) != swapchain) {
        describe_swapchain(swapchain); // first frame, or the swapchain was recreated
    }
    if (g_have_tasks.load(std::memory_order_acquire)) {
        std::vector<std::function<void()>> tasks;
        {
            std::scoped_lock lock(g_tasks_mutex);
            tasks.swap(g_tasks);
            g_have_tasks.store(false, std::memory_order_release);
        }
        watchdog::mark("render: tasks queued for the render thread");
        for (auto& task : tasks) {
            task();
        }
    }
    if (const PresentCallback callback = g_callback.load(std::memory_order_acquire)) {
        watchdog::mark("render: present callback (headset)");
        LARGE_INTEGER before, after;
        QueryPerformanceCounter(&before);
        callback(swapchain, ctx.rdi);
        QueryPerformanceCounter(&after);
        g_last_callback_ms.store(qpc_ms(before, after), std::memory_order_relaxed);
    }
    watchdog::mark("render: back in the game's Present call (DXGI Present, then the game's next frame)");
    // Present(SyncInterval = edx, Flags = r8d). The tearing flag is only valid with interval 0 and the
    // game sets it itself when it may, so only the interval is replaced.
    if (const int interval = g_sync_override.load(std::memory_order_relaxed); interval >= 0) {
        if (static_cast<int>(ctx.rdx & 0xFFFFFFFF) != interval) {
            g_sync_overrides.fetch_add(1, std::memory_order_relaxed);
        }
        ctx.rdx = static_cast<std::uintptr_t>(interval);
    }
}

} // namespace

void configure(const Config& config) {
    g_max_latency = static_cast<int>(std::clamp(config.get_int("render", "max_frame_latency", 1), 0LL, 16LL));
}

void install(const ModuleInfo& module) {
    g_module = module;
    const auto site = sig::lookup("dx11_present_call");
    if (!site) {
        log::warn("render: signature dx11_present_call not resolved (Vulkan build, or the game changed)");
        return;
    }
    auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(*site), &on_present);
    if (!hook) {
        log::error("render: mid-hook at exe+0x{:X} failed (error type {})", *site - module.base,
                   static_cast<int>(hook.error().type));
        return;
    }
    g_present_hook = std::move(*hook);
    log::info("render: present hook installed at exe+0x{:X}", *site - module.base);
}

double last_frame_ms() {
    return g_last_frame_ms.load(std::memory_order_relaxed);
}

double last_callback_ms() {
    return g_last_callback_ms.load(std::memory_order_relaxed);
}

std::uint32_t presenting_thread() {
    return g_presenting_thread.load(std::memory_order_relaxed);
}

std::uint64_t frame_count() {
    return g_frames.load(std::memory_order_relaxed);
}

std::uintptr_t render_frame() {
    return g_render_frame.load(std::memory_order_acquire);
}

IDXGISwapChain* swapchain() {
    return g_swapchain.load(std::memory_order_acquire);
}

ID3D11Device* device() {
    return g_device.load(std::memory_order_acquire);
}

void run_on_render_thread(std::function<void()> task) {
    std::scoped_lock lock(g_tasks_mutex);
    g_tasks.push_back(std::move(task));
    g_have_tasks.store(true, std::memory_order_release);
}

void set_present_callback(PresentCallback callback) {
    g_callback.store(callback, std::memory_order_release);
}

void set_sync_interval_override(int interval) {
    g_sync_override.store(interval, std::memory_order_relaxed);
    if (interval >= 0) {
        log::info("render: Present sync interval forced to {}", interval);
    } else {
        log::info("render: Present sync interval back to the game's");
    }
}

void report() {
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t frames = g_frames.load(std::memory_order_relaxed);
    const double seconds = std::chrono::duration<double>(now - g_last_report).count();
    log::info("render: {} frames presented since the last report ({:.1f} fps over {:.0f} s)", frames - g_last_frames,
              seconds > 0 ? static_cast<double>(frames - g_last_frames) / seconds : 0.0, seconds);
    log::info("render: the game presents with sync interval {} (1 = vsync on), flags 0x{:X}",
              g_game_sync_interval.load(std::memory_order_relaxed), g_game_present_flags.load(std::memory_order_relaxed));
    if (g_sync_override.load(std::memory_order_relaxed) >= 0) {
        log::info("render: sync interval overridden on {} presents in total", g_sync_overrides.load(std::memory_order_relaxed));
    }
    g_last_frames = frames;
    g_last_report = now;
}

} // namespace bg3vr::render
