// Phase 0 camera experiment: Lua reports the active rf::CameraController's address through a
// bridge file; we check it against the Script Extender's layout, inline-hook its Update, and after
// each update overwrite the view matrices with a requested pose. Inline (not vtable) hooking
// because the game is built with Control Flow Guard.

#include "bg3vr/camera.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/win_util.hpp"

#include <safetyhook.hpp>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numbers>
#include <sstream>

namespace bg3vr::camera {

namespace {

struct Vec3 {
    float x, y, z;
};

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 normalize(const Vec3& v) {
    const float length = std::sqrt(dot(v, v));
    return {v.x / length, v.y / length, v.z / length};
}

// State shared between the watcher thread and the game's update thread.
std::mutex g_mutex;
std::uintptr_t g_controller = 0;
std::optional<Pose> g_pose;
std::atomic<std::uint64_t> g_update_calls{0};

safetyhook::InlineHook g_update_hook;
std::uintptr_t g_hooked_function = 0;

ModuleInfo g_module;

bool in_section(const char* name, std::uintptr_t address, std::size_t size = 1) {
    const Section* section = find_section(g_module, name);
    return section && address >= section->start && address + size <= section->start + section->size;
}

bool readable(std::uintptr_t address, std::size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) {
        return false;
    }
    const DWORD readable_flags = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return info.State == MEM_COMMIT && (info.Protect & readable_flags) && !(info.Protect & PAGE_GUARD) &&
           address + size <= region_end;
}

template <class T>
T read(std::uintptr_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

void write_matrix(std::uintptr_t address, const Mat4& m) {
    std::memcpy(reinterpret_cast<void*>(address), m.data(), sizeof(float) * 16);
}

void update_detour(void* self, const void* game_time) {
    g_update_hook.call<void>(self, game_time);
    g_update_calls.fetch_add(1, std::memory_order_relaxed);

    Pose pose;
    {
        std::scoped_lock lock(g_mutex);
        if (!g_pose || reinterpret_cast<std::uintptr_t>(self) != g_controller) {
            return;
        }
        pose = *g_pose;
    }
    Mat4 world, view;
    pose_matrices(pose, world, view);
    const auto base = reinterpret_cast<std::uintptr_t>(self);
    write_matrix(base + layout::kViewMatrix, view);
    write_matrix(base + layout::kInvViewMatrix, world);
}

// Checks that `controller` looks like an rf::CameraController and hooks its class's Update.
bool attach(std::uintptr_t controller) {
    if (!readable(controller, layout::kSize)) {
        log::warn("camera: 0x{:X} is not readable memory", controller);
        return false;
    }
    const auto vtable = read<std::uintptr_t>(controller);
    if (!in_section(".rdata", vtable, layout::kVtableSlots * sizeof(void*))) {
        log::warn("camera: 0x{:X} has vtable 0x{:X} outside .rdata; not a controller", controller, vtable);
        return false;
    }
    const auto update = read<std::uintptr_t>(vtable + layout::kUpdateSlot * sizeof(void*));
    if (!in_section(".text", update)) {
        log::warn("camera: vtable slot {} = 0x{:X} is outside .text", layout::kUpdateSlot, update);
        return false;
    }

    const float fov = read<float>(controller + layout::kFov);
    const float near_plane = read<float>(controller + layout::kNearPlane);
    const float aspect = read<float>(controller + layout::kAspectRatio);
    const auto inv_view = read<Mat4>(controller + layout::kInvViewMatrix);
    log::info("camera: controller 0x{:X} vtable rva 0x{:X} Update rva 0x{:X}", controller, vtable - g_module.base,
              update - g_module.base);
    log::info("camera: fov {:.4f} rad ({:.1f} deg) near {:.3f} aspect {:.3f} pos ({:.3f}, {:.3f}, {:.3f})", fov,
              fov * 180.0f / std::numbers::pi_v<float>, near_plane, aspect, inv_view[12], inv_view[13], inv_view[14]);

    if (!(fov > 0.1f && fov < 3.2f) || !(near_plane > 0.0f && near_plane < 10.0f) || !(aspect > 0.5f && aspect < 5.0f)) {
        log::warn("camera: field values don't match the expected layout; not hooking");
        return false;
    }

    if (g_hooked_function != update) {
        g_update_hook = {};
        auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(update), reinterpret_cast<void*>(&update_detour));
        if (!hook) {
            log::error("camera: hooking Update at rva 0x{:X} failed (error type {})", update - g_module.base,
                       static_cast<int>(hook.error().type));
            g_hooked_function = 0;
            return false;
        }
        g_update_hook = std::move(*hook);
        g_hooked_function = update;
        log::info("camera: hooked Update at rva 0x{:X}", update - g_module.base);
    }
    return true;
}

std::filesystem::path default_bridge_path() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    // Ext.IO.SaveFile writes under the profile's "Script Extender" folder.
    return std::filesystem::path(std::wstring(buffer, length)) / L"Larian Studios" / L"Baldur's Gate 3" /
           L"Script Extender" / L"BG3VR" / L"camera_bridge.txt";
}

DWORD WINAPI watch_bridge(void* param) {
    const std::filesystem::path path = *static_cast<std::filesystem::path*>(param);
    delete static_cast<std::filesystem::path*>(param);
    log::info("camera: watching bridge file {}", to_utf8(path.wstring()));

    std::filesystem::file_time_type last_write{};
    std::uint64_t last_reported_calls = 0;
    for (;;) {
        Sleep(250);

        std::error_code ec;
        const auto write_time = std::filesystem::last_write_time(path, ec);
        if (ec || write_time == last_write) {
            continue;
        }
        last_write = write_time;

        std::ifstream in(path, std::ios::binary);
        std::stringstream content;
        content << in.rdbuf();
        const BridgeRequest request = parse_bridge(content.str());

        if (request.controller) {
            const bool attached = attach(*request.controller);
            std::scoped_lock lock(g_mutex);
            g_controller = attached ? *request.controller : 0;
        }
        {
            std::scoped_lock lock(g_mutex);
            if (request.pose_off) {
                g_pose.reset();
                log::info("camera: pose override off");
            } else if (request.pose) {
                g_pose = request.pose;
                log::info("camera: pose override ({:.2f}, {:.2f}, {:.2f}) yaw {:.1f} pitch {:.1f}{}", request.pose->x,
                          request.pose->y, request.pose->z, request.pose->yaw_deg, request.pose->pitch_deg,
                          g_controller ? "" : " (no controller attached yet)");
            }
        }

        const std::uint64_t calls = g_update_calls.load(std::memory_order_relaxed);
        log::info("camera: Update called {} times since the last bridge change", calls - last_reported_calls);
        last_reported_calls = calls;
    }
}

} // namespace

void pose_matrices(const Pose& pose, Mat4& world, Mat4& view) {
    const float yaw = pose.yaw_deg * std::numbers::pi_v<float> / 180.0f;
    const float pitch = pose.pitch_deg * std::numbers::pi_v<float> / 180.0f;
    const Vec3 forward = normalize({std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)});
    const Vec3 right = normalize(cross({0.0f, 1.0f, 0.0f}, forward));
    const Vec3 up = cross(forward, right);
    const Vec3 p{pose.x, pose.y, pose.z};

    world = {right.x, right.y, right.z, 0.0f, up.x, up.y, up.z, 0.0f,
             forward.x, forward.y, forward.z, 0.0f, p.x, p.y, p.z, 1.0f};
    view = {right.x, up.x, forward.x, 0.0f, right.y, up.y, forward.y, 0.0f,
            right.z, up.z, forward.z, 0.0f, -dot(right, p), -dot(up, p), -dot(forward, p), 1.0f};
}

BridgeRequest parse_bridge(std::string_view content) {
    BridgeRequest request;
    while (!content.empty()) {
        const auto eol = content.find('\n');
        const std::string_view line = text::trim(content.substr(0, eol));
        content = eol == std::string_view::npos ? std::string_view{} : content.substr(eol + 1);

        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string key = text::to_lower(text::trim(line.substr(0, eq)));
        const std::string_view value = text::trim(line.substr(eq + 1));

        if (key == "controller") {
            std::string_view hex = value;
            if (hex.starts_with("0x") || hex.starts_with("0X")) {
                hex.remove_prefix(2);
            }
            if (const auto address = text::parse_int(std::string("0x") + std::string(hex))) {
                request.controller = static_cast<std::uintptr_t>(*address);
            }
        } else if (key == "pose") {
            if (text::to_lower(value) == "off") {
                request.pose_off = true;
                continue;
            }
            const auto words = text::split_whitespace(value);
            if (words.size() != 5) {
                continue;
            }
            float numbers[5];
            bool ok = true;
            for (std::size_t i = 0; i < 5 && ok; ++i) {
                const std::string word(words[i]);
                char* end = nullptr;
                numbers[i] = std::strtof(word.c_str(), &end);
                ok = end == word.c_str() + word.size();
            }
            if (ok) {
                request.pose = Pose{numbers[0], numbers[1], numbers[2], numbers[3], numbers[4]};
            }
        }
    }
    return request;
}

void start(const Config& config, const ModuleInfo& module) {
    if (!config.get_bool("camera", "bridge", true)) {
        log::info("camera: bridge disabled in bg3vr.ini");
        return;
    }
    g_module = module;
    const std::string configured = config.get("camera", "bridge_file");
    auto* path = new std::filesystem::path(configured.empty() ? default_bridge_path()
                                                              : std::filesystem::path(configured));
    if (path->empty()) {
        log::error("camera: no bridge file path (LOCALAPPDATA unset?)");
        delete path;
        return;
    }
    if (HANDLE thread = CreateThread(nullptr, 0, watch_bridge, path, 0, nullptr)) {
        CloseHandle(thread);
    } else {
        delete path;
    }
}

} // namespace bg3vr::camera
