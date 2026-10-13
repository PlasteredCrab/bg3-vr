#include "bg3vr/render.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/signatures.hpp"

#include <safetyhook.hpp>

#include <atomic>
#include <chrono>

namespace bg3vr::render {

namespace {

safetyhook::MidHook g_present_hook;
std::atomic<IDXGISwapChain*> g_swapchain{nullptr};
std::atomic<ID3D11Device*> g_device{nullptr};
std::atomic<PresentCallback> g_callback{nullptr};
std::atomic<std::uint64_t> g_frames{0};
std::uint64_t g_last_frames = 0;
std::chrono::steady_clock::time_point g_last_report = std::chrono::steady_clock::now();
ModuleInfo g_module;

void describe_swapchain(IDXGISwapChain* swapchain) {
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
        g_device.store(device, std::memory_order_release);
        device->Release(); // we don't own it; the renderer keeps it alive
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    log::info("render: swapchain 0x{:X} device 0x{:X}: {}x{} format {} buffers {} swap effect {} windowed {} flags 0x{:X}",
              reinterpret_cast<std::uintptr_t>(swapchain), reinterpret_cast<std::uintptr_t>(device),
              desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<int>(desc.BufferDesc.Format),
              desc.BufferCount, static_cast<int>(desc.SwapEffect), desc.Windowed, desc.Flags);
}

// Just before the game's Present call: rcx = the IDXGISwapChain*, rdi = the RenderFrame.
void on_present(safetyhook::Context& ctx) {
    auto* swapchain = reinterpret_cast<IDXGISwapChain*>(ctx.rcx);
    if (!swapchain) {
        return;
    }
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (g_swapchain.exchange(swapchain, std::memory_order_acq_rel) != swapchain) {
        describe_swapchain(swapchain); // first frame, or the swapchain was recreated
    }
    if (const PresentCallback callback = g_callback.load(std::memory_order_acquire)) {
        callback(swapchain, ctx.rdi);
    }
}

} // namespace

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

IDXGISwapChain* swapchain() {
    return g_swapchain.load(std::memory_order_acquire);
}

ID3D11Device* device() {
    return g_device.load(std::memory_order_acquire);
}

void set_present_callback(PresentCallback callback) {
    g_callback.store(callback, std::memory_order_release);
}

void report() {
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t frames = g_frames.load(std::memory_order_relaxed);
    const double seconds = std::chrono::duration<double>(now - g_last_report).count();
    log::info("render: {} frames presented since the last report ({:.1f} fps over {:.0f} s)", frames - g_last_frames,
              seconds > 0 ? static_cast<double>(frames - g_last_frames) / seconds : 0.0, seconds);
    g_last_frames = frames;
    g_last_report = now;
}

} // namespace bg3vr::render
