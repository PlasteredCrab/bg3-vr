// Phase 0 camera experiment. The controller's view update (exe 4.1.1.7631656: function 0x4085150)
// builds a view matrix on the stack and passes it to the matrix setter (0x408BD60), which stores it
// at controller+0x90, computes the inverse, and is followed by the frustum rebuild. A mid-hook on
// that call (signature camera_set_view_call) swaps in our view matrix, so the inverse, frustum and
// culling all follow the requested pose. Lua reports which controller to drive (and the pose)
// through a bridge file; other cameras pass through untouched.

#include "bg3vr/camera.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/watch.hpp"
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

// State shared between the watcher thread and the game's camera update.
std::mutex g_mutex;
std::uintptr_t g_controller = 0;
std::optional<Pose> g_pose;
std::atomic<std::uint64_t> g_view_calls{0};
std::atomic<std::uint64_t> g_view_overrides{0};

safetyhook::MidHook g_set_view_hook;

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

// At the call to the matrix setter: rbx = controller, rdx = the freshly built view matrix.
void on_set_view(safetyhook::Context& ctx) {
    g_view_calls.fetch_add(1, std::memory_order_relaxed);
    Pose pose;
    {
        std::scoped_lock lock(g_mutex);
        if (!g_pose || ctx.rbx != g_controller) {
            return;
        }
        pose = *g_pose;
    }
    Mat4 world, view;
    pose_matrices(pose, world, view);
    std::memcpy(reinterpret_cast<void*>(ctx.rdx), view.data(), sizeof(view));
    g_view_overrides.fetch_add(1, std::memory_order_relaxed);
}

void install_view_hook() {
    const auto site = sig::lookup("camera_set_view_call");
    if (!site) {
        log::error("camera: signature camera_set_view_call not resolved; pose override unavailable");
        return;
    }
    auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(*site), &on_set_view);
    if (!hook) {
        log::error("camera: mid-hook at exe+0x{:X} failed (error type {})", *site - g_module.base,
                   static_cast<int>(hook.error().type));
        return;
    }
    g_set_view_hook = std::move(*hook);
    log::info("camera: view hook installed at exe+0x{:X}", *site - g_module.base);
}

// Checks that `controller` looks like an rf::CameraController before we act on it.
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
        log::warn("camera: field values don't match the expected layout; ignoring this controller");
        return false;
    }
    return true;
}

// "bg3_dx11+0x1234" for the game exe, "module.dll+0x1234" for others.
std::string describe(std::uintptr_t address) {
    if (address >= g_module.base && address < g_module.base + g_module.image_size) {
        return std::format("exe+0x{:X}", address - g_module.base);
    }
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module)) {
        wchar_t name[MAX_PATH];
        const DWORD length = GetModuleFileNameW(module, name, MAX_PATH);
        const std::wstring file = std::filesystem::path(std::wstring(name, length)).filename().wstring();
        return std::format("{}+0x{:X}", to_utf8(file), address - reinterpret_cast<std::uintptr_t>(module));
    }
    return std::format("0x{:X}", address);
}

// Which vtable slots run, and which instructions write the camera's pose and matrices.
void run_probe(std::uintptr_t controller) {
    const auto vtable = read<std::uintptr_t>(controller);
    std::vector<std::uintptr_t> slots(layout::kVtableSlots);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        slots[i] = read<std::uintptr_t>(vtable + i * sizeof(void*));
    }

    log::info("probe: counting calls to {} vtable slots for 3 s", slots.size());
    const auto counts = watch::count_calls(slots, std::chrono::milliseconds(3000));
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (counts[i] == UINT64_MAX) {
            log::info("probe:   slot {:2} {} not hookable (too short or shared)", i, describe(slots[i]));
        } else if (counts[i] > 0) {
            log::info("probe:   slot {:2} {} called {} times", i, describe(slots[i]), counts[i]);
        }
    }

    const std::array<std::uintptr_t, 3> watched = {
        controller + layout::kWorldTranslate,
        controller + layout::kViewMatrix + 12 * sizeof(float),
        controller + layout::kInvViewMatrix + 12 * sizeof(float),
    };
    constexpr std::array<const char*, 3> kNames = {"WorldTransform.Translate", "ViewMatrix[12..13]",
                                                   "InvViewMatrix[12..13]"};
    log::info("probe: watching writes to the controller's pose and matrices for 2 s");
    const auto hits = watch::watch_writes(watched, std::chrono::milliseconds(2000));
    if (hits.empty()) {
        log::info("probe:   no writes seen");
    }
    for (const auto& hit : hits) {
        log::info("probe:   {} written at {} x{}", kNames[hit.watch_index], describe(hit.rip), hit.count);
        std::string stack;
        for (std::uintptr_t caller : hit.callers) {
            if (caller == 0) {
                break;
            }
            stack += "  <- " + describe(caller);
        }
        log::info("probe:     stack{}", stack);
    }
    log::info("probe: done");
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

        if (request.probe) {
            std::uintptr_t controller = 0;
            {
                std::scoped_lock lock(g_mutex);
                controller = g_controller;
            }
            if (controller) {
                run_probe(controller);
            } else {
                log::warn("probe: no validated controller to probe");
            }
        }

        const std::uint64_t calls = g_view_calls.load(std::memory_order_relaxed);
        log::info("camera: view set {} times since the last bridge change ({} overridden in total)",
                  calls - last_reported_calls, g_view_overrides.load(std::memory_order_relaxed));
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
        } else if (key == "probe") {
            const std::string flag = text::to_lower(value);
            request.probe = flag == "1" || flag == "true" || flag == "yes";
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
    install_view_hook();
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
