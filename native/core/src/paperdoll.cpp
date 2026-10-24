#include "bg3vr/paperdoll.hpp"

#include "bg3vr/dice.hpp"
#include "bg3vr/dolls.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/pattern.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/text.hpp"

#include <safetyhook.hpp>

#include <d3d11.h>
#include <psapi.h>

#include <array>
#include <cstring>
#include <format>
#include <atomic>
#include <mutex>
#include <set>
#include <span>
#include <string>

namespace bg3vr::paperdoll {

namespace {

// 4.1.1.7631656 (agent P; prologues from bg3vr_disasm 2026-10-20).
constexpr std::uintptr_t kManagerUpdate = 0x13E1A40; // (PM, ?, float dt)
constexpr const char* kManagerUpdatePrologue =
    "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 0F 29 74 24 70";
constexpr std::uintptr_t kOnRender = 0x155D9D0;      // LSPaperdoll::OnRender(this, ctx)
constexpr const char* kOnRenderPrologue = "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 89 A0 02 00 00";
constexpr std::uintptr_t kEntryVtable = 0x55EE6F0;    // the 0x90-byte manager entry
constexpr std::uintptr_t kRendererVtable = 0x5646D78; // PaperdollRenderer
constexpr std::size_t kEntryCount = 0x26C, kEntries = 0x270;
constexpr std::size_t kEntryTexture = 0x10, kEntryRenderer = 0x70;

enum class Mode { Normal, Diagnose, Hide };
std::atomic<Mode> g_mode{Mode::Diagnose};
ModuleInfo g_module;
safetyhook::InlineHook g_update_hook;
safetyhook::InlineHook g_render_hook;
std::uintptr_t g_d3d11_begin = 0, g_d3d11_end = 0;

std::mutex g_mutex;
std::set<std::uintptr_t> g_seen; // renderers logged
ID3D11Texture2D* g_readback_texture = nullptr; // AddRef'd, read on the presenting thread
std::uintptr_t g_readback_renderer = 0;
std::atomic<std::uint64_t> g_updates{0}, g_hidden{0}, g_dolls{0};

bool check(std::uintptr_t rva, const char* prologue, const char* name) {
    const auto pattern = parse_pattern(prologue);
    std::array<std::uint8_t, 64> bytes{};
    if (!pattern || pattern->size() > bytes.size() || !mem::try_copy(bytes.data(), g_module.base + rva, pattern->size())) {
        return false;
    }
    const auto at = find_first(std::span<const std::uint8_t>(bytes.data(), pattern->size()), *pattern);
    if (!at || *at != 0) {
        log::error("paperdoll: {} at exe+0x{:X} doesn't have the expected prologue (game updated?)", name, rva);
        return false;
    }
    return true;
}

// A D3D11 COM object: its vtable lies in d3d11.dll.
bool is_d3d11_object(std::uintptr_t p) {
    std::uintptr_t vtable = 0;
    return p && mem::try_copy(&vtable, p, sizeof(vtable)) && vtable >= g_d3d11_begin && vtable < g_d3d11_end;
}

// The ID3D11Texture2D inside the game's render-frame texture object: the first qword (within 0x100 bytes, then one
// pointer deeper) that is a D3D11 object answering QueryInterface for a 2D texture. Returns it AddRef'd.
ID3D11Texture2D* find_texture(std::uintptr_t object, std::string& where) {
    for (int depth = 0; depth < 2; ++depth) {
        for (std::size_t off = 0; off < 0x100; off += 8) {
            std::uintptr_t p = 0;
            if (!mem::try_copy(&p, object + off, sizeof(p))) {
                break;
            }
            std::uintptr_t candidate = p;
            if (depth == 1) {
                if (!p || !mem::try_copy(&candidate, p, sizeof(candidate))) {
                    continue;
                }
            }
            if (!is_d3d11_object(candidate)) {
                continue;
            }
            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(reinterpret_cast<IUnknown*>(candidate)->QueryInterface(__uuidof(ID3D11Texture2D),
                                                                               reinterpret_cast<void**>(&texture)))) {
                where = depth == 0 ? std::format("+0x{:X}", off) : std::format("[+0x{:X}]", off);
                return texture;
            }
        }
    }
    return nullptr;
}

void log_doll(std::uintptr_t entry, std::uintptr_t renderer) {
    const auto frame_index = mem::read<std::uint32_t>(renderer + 0x3A0);
    const auto view = mem::read<std::uintptr_t>(renderer + 0x3A8);
    const auto camera = mem::read<std::uintptr_t>(renderer + 0x3B0);
    const auto scene = mem::read<std::uintptr_t>(renderer + 0x348);
    const auto yaw = mem::read<float>(renderer + 0x404);
    const auto spin = mem::read<float>(renderer + 0x40C);
    const auto clone = mem::read<std::uint64_t>(renderer + 0x08);
    const auto texture_object = mem::read<std::uintptr_t>(entry + kEntryTexture);
    std::string where;
    ID3D11Texture2D* texture = texture_object ? find_texture(texture_object, where) : nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    if (texture) {
        texture->GetDesc(&desc);
    }
    log::info("paperdoll: doll {} entry 0x{:X} renderer 0x{:X}: clone entity 0x{:X}, frame {}, view 0x{:X}, camera 0x{:X}, "
              "scene 0x{:X}, yaw {:.2f} spin {:.2f}; output object 0x{:X} -> {}",
              g_dolls.load(), entry, renderer, clone, frame_index, view, camera, scene, yaw, spin, texture_object,
              texture ? std::format("ID3D11Texture2D 0x{:X} at {} : {}x{} format {} bind 0x{:X} misc 0x{:X} mips {}",
                                    reinterpret_cast<std::uintptr_t>(texture), where, desc.Width, desc.Height,
                                    static_cast<int>(desc.Format), desc.BindFlags, desc.MiscFlags, desc.MipLevels)
                      : std::string("no D3D11 texture found in its first 0x100 bytes (one pointer deep)"));
    if (texture) {
        std::scoped_lock lock(g_mutex);
        if (!g_readback_texture) {
            g_readback_texture = texture; // read its corner on the presenting thread
            g_readback_renderer = renderer;
            texture = nullptr;
        }
    }
    if (texture) {
        texture->Release();
    }
}

std::uintptr_t on_update(std::uintptr_t pm, std::uintptr_t a, float dt) {
    const std::uintptr_t result = g_update_hook.call<std::uintptr_t>(pm, a, dt);
    g_updates.fetch_add(1, std::memory_order_relaxed);
    dolls::on_manager_update(pm); // the 3D versions (game thread, after the game's own update)
    dice::on_manager_update();    // the physical die (made, moved and removed here, as the game does its clones)
    if (g_mode.load(std::memory_order_relaxed) == Mode::Normal || !pm) {
        return result;
    }
    const auto count = mem::read<std::int32_t>(pm + kEntryCount);
    const auto entries = mem::read<std::uintptr_t>(pm + kEntries);
    if (count <= 0 || count > 64 || !entries) {
        return result;
    }
    static std::atomic<bool> dumped{false};
    bool any = false;
    for (std::int32_t i = 0; i < count; ++i) {
        const auto e = mem::read<std::uintptr_t>(entries + 8 * static_cast<std::size_t>(i));
        any = any || (e && mem::read<std::uintptr_t>(e) == g_module.base + kEntryVtable);
    }
    if (!any && !dumped.exchange(true)) {
        log::warn("paperdoll: manager 0x{:X} has {} entries at 0x{:X} but none with the entry vtable exe+0x{:X}; first "
                  "qwords {:X} {:X} {:X} {:X}",
                  pm, count, entries, kEntryVtable, mem::read<std::uintptr_t>(entries), mem::read<std::uintptr_t>(entries + 8),
                  mem::read<std::uintptr_t>(entries + 16), mem::read<std::uintptr_t>(entries + 24));
    }
    for (std::int32_t i = 0; i < count; ++i) {
        const auto entry = mem::read<std::uintptr_t>(entries + 8 * static_cast<std::size_t>(i));
        if (!entry || mem::read<std::uintptr_t>(entry) != g_module.base + kEntryVtable) {
            continue;
        }
        const auto renderer = mem::read<std::uintptr_t>(entry + kEntryRenderer);
        if (!renderer || mem::read<std::uintptr_t>(renderer) != g_module.base + kRendererVtable ||
            !mem::read<std::uintptr_t>(entry + kEntryTexture)) {
            continue;
        }
        bool fresh = false;
        {
            std::scoped_lock lock(g_mutex);
            fresh = g_seen.insert(renderer).second;
        }
        if (fresh) {
            g_dolls.fetch_add(1, std::memory_order_relaxed);
            log_doll(entry, renderer);
        }
    }
    return result;
}

void on_render(std::uintptr_t self, std::uintptr_t context) {
    if (g_mode.load(std::memory_order_relaxed) == Mode::Hide || dolls::replaces(self)) {
        g_hidden.fetch_add(1, std::memory_order_relaxed);
        return; // the flat picture stays out of the UI (the 3D doll replaces it)
    }
    g_render_hook.call<void>(self, context);
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    const std::string mode = text::to_lower(config.get("ui", "paperdoll", "diagnose"));
    g_mode = mode == "normal" ? Mode::Normal : mode == "hide" ? Mode::Hide : Mode::Diagnose;
    if (g_mode == Mode::Normal && !config.get_bool("ui", "stereo_dolls", true)) {
        log::info("paperdoll: off ([ui] paperdoll = normal, stereo_dolls = false)");
        return;
    }
    if (HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll")) {
        MODULEINFO info{};
        if (GetModuleInformation(GetCurrentProcess(), d3d11, &info, sizeof(info))) {
            g_d3d11_begin = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
            g_d3d11_end = g_d3d11_begin + info.SizeOfImage;
        }
    }
    if (!check(kManagerUpdate, kManagerUpdatePrologue, "paperdoll manager update") ||
        !check(kOnRender, kOnRenderPrologue, "LSPaperdoll::OnRender")) {
        health::set("UI paperdolls", health::State::Unavailable, "a function didn't match");
        return;
    }
    auto update = safetyhook::InlineHook::create(reinterpret_cast<void*>(module.base + kManagerUpdate),
                                                 reinterpret_cast<void*>(&on_update));
    auto render = safetyhook::InlineHook::create(reinterpret_cast<void*>(module.base + kOnRender),
                                                 reinterpret_cast<void*>(&on_render));
    if (!update || !render) {
        health::set("UI paperdolls", health::State::Unavailable, "hook failed");
        return;
    }
    g_update_hook = std::move(*update);
    g_render_hook = std::move(*render);
    health::set("UI paperdolls", health::State::Ready, g_mode == Mode::Hide ? "hide (flat picture off)" : "diagnose (logging)");
    log::info("paperdoll: {} (manager update exe+0x{:X}, OnRender exe+0x{:X}; d3d11 0x{:X}..0x{:X})",
              g_mode == Mode::Hide ? "hiding the flat character pictures and logging the dolls" : "logging the dolls",
              kManagerUpdate, kOnRender, g_d3d11_begin, g_d3d11_end);
}

void on_present() {
    ID3D11Texture2D* texture = nullptr;
    std::uintptr_t renderer = 0;
    {
        std::scoped_lock lock(g_mutex);
        texture = g_readback_texture;
        renderer = g_readback_renderer;
        g_readback_texture = nullptr;
    }
    if (!texture) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    ID3D11Device* device = render::device();
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* staging = nullptr;
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Width = 1;
    sd.Height = 1;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    if (device && desc.SampleDesc.Count == 1 && SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging))) {
        device->GetImmediateContext(&context);
        const D3D11_BOX corner{1, 1, 0, 2, 2, 1};
        context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, 0, &corner);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            std::array<std::uint8_t, 16> bytes{};
            std::memcpy(bytes.data(), mapped.pData, 16);
            context->Unmap(staging, 0);
            log::info("paperdoll: renderer 0x{:X} texture corner pixel (1,1) bytes {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
                      "{:02X} {:02X} (format {}): look at the alpha channel for a transparent background",
                      renderer, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                      static_cast<int>(desc.Format));
        }
        context->Release();
        staging->Release();
    } else {
        log::info("paperdoll: renderer 0x{:X}: corner readback not possible (samples {}, format {})", renderer,
                  desc.SampleDesc.Count, static_cast<int>(desc.Format));
    }
    texture->Release();
}

void report() {
    log::info("paperdoll: {} manager updates, {} dolls seen, {} flat pictures hidden", g_updates.load(), g_dolls.load(),
              g_hidden.load());
}

} // namespace bg3vr::paperdoll
