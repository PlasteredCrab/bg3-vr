// Camera override at the source. The controller's view update (exe 4.1.1.7631656: 0x4085150)
// builds the view from GetWorldTransform / GetWorldTranslate (vtable slots 13 / 9), which return the
// camera entity's ECS Transform component; every other system that asks the camera where it is
// reads the same component. At the start of the view update (signature camera_view_update) we
// write the requested pose into that Transform, so the view, its inverse, the frustum, culling and
// all other readers agree. The hook on the matrix setter call (camera_set_view_call) only checks
// that the matrix the game built matches the one we expect (or, with [camera] force_view, also
// overwrites it). Lua reports which controller to drive, and the pose, through a bridge file.

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
std::atomic<std::uint64_t> g_transform_writes{0};
std::atomic<std::uint64_t> g_view_checks{0};
std::atomic<float> g_view_max_error{0.0f};
bool g_force_view = false;

safetyhook::MidHook g_view_update_hook;
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

// Every controller that passes through the view setter, with call counts (fixed table, no locks).
constexpr std::size_t kTrackedControllers = 16;
std::array<std::atomic<std::uintptr_t>, kTrackedControllers> g_seen_controllers{};
std::array<std::atomic<std::uint64_t>, kTrackedControllers> g_seen_counts{};

void track_controller(std::uintptr_t controller) {
    for (std::size_t i = 0; i < kTrackedControllers; ++i) {
        std::uintptr_t current = g_seen_controllers[i].load(std::memory_order_acquire);
        if (current == 0) {
            std::uintptr_t expected = 0;
            if (g_seen_controllers[i].compare_exchange_strong(expected, controller)) {
                current = controller;
            } else {
                current = expected;
            }
        }
        if (current == controller) {
            g_seen_counts[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

std::optional<Pose> pose_for(std::uintptr_t controller) {
    std::scoped_lock lock(g_mutex);
    if (!g_pose || controller != g_controller) {
        return std::nullopt;
    }
    return g_pose;
}

// ---- hiding visuals from the main view (first-person head) ----------------------------------
// The game's own MoveableObject::ToggleCullFlag600(bool set) sets/clears the main-view bits (0x600,
// an inclusion mask). Calls are made on the game thread (from the view update hook): each frame we
// check the flags and clear them again only if the game restored them. Objects are re-validated
// every time (readable, same vtable) so a destroyed visual is dropped, not called.

struct CullTarget {
    std::uintptr_t object = 0;
    std::uintptr_t vtable = 0;
};
std::vector<CullTarget> g_cull_targets; // guarded by g_mutex
std::vector<CullTarget> g_cull_restore; // shown again once, then dropped
std::atomic<std::uint64_t> g_cull_frames{0};
std::atomic<std::uint64_t> g_cull_calls{0};
std::atomic<bool> g_cull_pending{false};       // a hide/show request is waiting
std::atomic<std::uint64_t> g_cull_updates{0};  // view updates seen, for throttling
std::atomic<std::uint64_t> g_cull_ticks{0};    // QPC ticks spent in process_cull
constexpr std::uint64_t kCullCheckInterval = 32; // the game hasn't been seen to restore the flags

using ToggleCullFn = void (*)(std::uintptr_t, bool);

bool still_valid(const CullTarget& target) {
    return readable(target.object, layout::kCullFlags + sizeof(std::uint16_t)) &&
           read<std::uintptr_t>(target.object) == target.vtable;
}

void set_main_view(const CullTarget& target, bool visible) {
    const auto toggle = read<ToggleCullFn>(target.vtable + layout::kToggleCullFlag600Slot * sizeof(void*));
    toggle(target.object, visible);
    g_cull_calls.fetch_add(1, std::memory_order_relaxed);
}

// Runs on every view update (~5 per frame); does work only for pending requests or every
// kCullCheckInterval-th update, since each check costs a lock and a VirtualQuery per object.
void process_cull() {
    const bool due = g_cull_updates.fetch_add(1, std::memory_order_relaxed) % kCullCheckInterval == 0;
    if (!due && !g_cull_pending.load(std::memory_order_acquire)) {
        return;
    }
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    std::scoped_lock lock(g_mutex);
    g_cull_pending.store(false, std::memory_order_release);
    for (const CullTarget& target : g_cull_restore) {
        if (still_valid(target)) {
            set_main_view(target, true);
        }
    }
    g_cull_restore.clear();
    std::erase_if(g_cull_targets, [](const CullTarget& target) { return !still_valid(target); });
    for (const CullTarget& target : g_cull_targets) {
        const auto flags = read<std::uint16_t>(target.object + layout::kCullFlags);
        if (flags & layout::kMainViewBits) {
            set_main_view(target, false);
        }
    }
    if (!g_cull_targets.empty()) {
        g_cull_frames.fetch_add(1, std::memory_order_relaxed);
    }
    QueryPerformanceCounter(&end);
    g_cull_ticks.fetch_add(static_cast<std::uint64_t>(end.QuadPart - start.QuadPart), std::memory_order_relaxed);
}

// Entry of the view update: rcx = controller. Write the pose into the camera's Transform.
void on_view_update(safetyhook::Context& ctx) {
    process_cull();
    const auto pose = pose_for(ctx.rcx);
    if (!pose) {
        return;
    }
    using GetTransformFn = float* (*)(std::uintptr_t);
    const auto vtable = read<std::uintptr_t>(ctx.rcx);
    const auto get_transform = read<GetTransformFn>(vtable + layout::kGetWorldTransformSlot * sizeof(void*));
    float* transform = get_transform(ctx.rcx);
    if (!transform) {
        return;
    }
    const auto q = pose_quaternion(*pose);
    std::memcpy(transform, q.data(), sizeof(q));
    const float translate[3] = {pose->x, pose->y, pose->z};
    std::memcpy(reinterpret_cast<std::uint8_t*>(transform) + layout::kTransformTranslate, translate, sizeof(translate));
    g_transform_writes.fetch_add(1, std::memory_order_relaxed);
}

// At the call to the matrix setter: rbx = controller, rdx = the view matrix the game just built.
void on_set_view(safetyhook::Context& ctx) {
    g_view_calls.fetch_add(1, std::memory_order_relaxed);
    track_controller(ctx.rbx);
    const auto pose = pose_for(ctx.rbx);
    if (!pose) {
        return;
    }
    Mat4 world, expected;
    pose_matrices(*pose, world, expected);
    float* built = reinterpret_cast<float*>(ctx.rdx);
    float error = 0.0f;
    for (std::size_t i = 0; i < 16; ++i) {
        error = std::max(error, std::abs(built[i] - expected[i]));
    }
    g_view_checks.fetch_add(1, std::memory_order_relaxed);
    if (error > g_view_max_error.load(std::memory_order_relaxed)) {
        g_view_max_error.store(error, std::memory_order_relaxed);
    }
    if (g_force_view) {
        std::memcpy(built, expected.data(), sizeof(expected));
    }
}

bool install_mid_hook(const char* signature, safetyhook::MidHookFn callback, safetyhook::MidHook& out) {
    const auto site = sig::lookup(signature);
    if (!site) {
        log::error("camera: signature {} not resolved", signature);
        return false;
    }
    auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(*site), callback);
    if (!hook) {
        log::error("camera: mid-hook {} at exe+0x{:X} failed (error type {})", signature, *site - g_module.base,
                   static_cast<int>(hook.error().type));
        return false;
    }
    out = std::move(*hook);
    log::info("camera: hook {} installed at exe+0x{:X}", signature, *site - g_module.base);
    return true;
}

void install_view_hooks() {
    install_mid_hook("camera_view_update", &on_view_update, g_view_update_hook);
    install_mid_hook("camera_set_view_call", &on_set_view, g_set_view_hook);
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

std::string describe(std::uintptr_t address);

// Checks `object` before we ever call into it: a vtable in .rdata, slot functions in .text, and a
// ToggleCullFlag600 whose first bytes contain the 0x600 or 0xF9FF constant it must use.
std::optional<CullTarget> check_cull_target(std::uintptr_t object) {
    if (!readable(object, 0x40)) {
        log::warn("hide: 0x{:X} is not readable", object);
        return std::nullopt;
    }
    const auto vtable = read<std::uintptr_t>(object);
    if (!in_section(".rdata", vtable, (layout::kToggleCullFlag600Slot + 1) * sizeof(void*))) {
        log::warn("hide: 0x{:X} has vtable 0x{:X} outside .rdata", object, vtable);
        return std::nullopt;
    }
    const auto toggle = read<std::uintptr_t>(vtable + layout::kToggleCullFlag600Slot * sizeof(void*));
    const auto get_flags = read<std::uintptr_t>(vtable + layout::kGetCullFlagsSlot * sizeof(void*));
    if (!in_section(".text", toggle, 64)) {
        log::warn("hide: slot {} of 0x{:X} is outside .text", layout::kToggleCullFlag600Slot, object);
        return std::nullopt;
    }
    const auto code = read<std::array<std::uint8_t, 64>>(toggle);
    bool has_constant = false;
    for (std::size_t i = 0; i + 1 < code.size(); ++i) {
        has_constant = has_constant || (code[i] == 0x00 && code[i + 1] == 0x06) || (code[i] == 0xFF && code[i + 1] == 0xF9);
    }
    log::info("hide: object 0x{:X} vtable {} ToggleCullFlag600 {} GetCullFlags {} flags 0x{:X} check {}", object,
              describe(vtable), describe(toggle), describe(get_flags),
              read<std::uint16_t>(object + layout::kCullFlags), has_constant ? "passed" : "FAILED");
    if (!has_constant) {
        return std::nullopt;
    }
    return CullTarget{object, vtable};
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

        if (request.hide) {
            std::vector<CullTarget> validated;
            for (const std::uintptr_t object : *request.hide) {
                if (const auto target = check_cull_target(object)) {
                    validated.push_back(*target);
                }
            }
            std::scoped_lock lock(g_mutex);
            g_cull_restore.insert(g_cull_restore.end(), g_cull_targets.begin(), g_cull_targets.end());
            g_cull_targets = std::move(validated);
            g_cull_pending.store(true, std::memory_order_release);
            log::info("hide: {} object(s) hidden from the main view, {} to restore", g_cull_targets.size(),
                      g_cull_restore.size());
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
        log::info("camera: view set {} times since the last bridge change; transform written {} times; "
                  "built view checked {} times, max error {:.6f}{}; hide applied in {} frames",
                  calls - last_reported_calls, g_transform_writes.load(std::memory_order_relaxed),
                  g_view_checks.load(std::memory_order_relaxed), g_view_max_error.load(std::memory_order_relaxed),
                  g_force_view ? " (force_view on)" : "", g_cull_frames.load(std::memory_order_relaxed));
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const std::uint64_t checks = g_cull_frames.load(std::memory_order_relaxed);
        const double total_us = 1e6 * static_cast<double>(g_cull_ticks.load(std::memory_order_relaxed)) /
                                static_cast<double>(frequency.QuadPart);
        log::info("hide: ToggleCullFlag600 called {} times so far (once per hide/show, plus each time the game "
                  "restored the flags); {} checks, {:.1f} us total, {:.2f} us each",
                  g_cull_calls.load(std::memory_order_relaxed), checks, total_us,
                  checks ? total_us / static_cast<double>(checks) : 0.0);
        g_view_max_error.store(0.0f, std::memory_order_relaxed);
        last_reported_calls = calls;
        for (std::size_t i = 0; i < kTrackedControllers; ++i) {
            const std::uintptr_t seen = g_seen_controllers[i].load(std::memory_order_acquire);
            if (seen == 0) {
                break;
            }
            if (!readable(seen, layout::kSize)) {
                log::info("camera:   controller 0x{:X} (gone) views set {}", seen,
                          g_seen_counts[i].load(std::memory_order_relaxed));
                continue;
            }
            const auto vtable = read<std::uintptr_t>(seen);
            const auto inv_view = read<Mat4>(seen + layout::kInvViewMatrix);
            log::info("camera:   controller 0x{:X} vtable {} views set {} ortho {} fov {:.3f} near {:.3f} pos "
                      "({:.2f}, {:.2f}, {:.2f}) fwd ({:.2f}, {:.2f}, {:.2f}){}",
                      seen, describe(vtable), g_seen_counts[i].load(std::memory_order_relaxed),
                      read<std::uint8_t>(seen + layout::kIsOrthographic), read<float>(seen + layout::kFov),
                      read<float>(seen + layout::kNearPlane), inv_view[12], inv_view[13], inv_view[14], inv_view[8],
                      inv_view[9], inv_view[10], seen == g_controller ? "  <- driven" : "");
        }
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

std::array<float, 4> pose_quaternion(const Pose& pose) {
    Mat4 world, view;
    pose_matrices(pose, world, view);
    // Rotation matrix m[row][col] with columns right, up, forward.
    const float m00 = world[0], m10 = world[1], m20 = world[2];
    const float m01 = world[4], m11 = world[5], m21 = world[6];
    const float m02 = world[8], m12 = world[9], m22 = world[10];
    const float trace = m00 + m11 + m22;
    float x, y, z, w;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        w = 0.25f * s;
        x = (m21 - m12) / s;
        y = (m02 - m20) / s;
        z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        w = (m21 - m12) / s;
        x = 0.25f * s;
        y = (m01 + m10) / s;
        z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        w = (m02 - m20) / s;
        x = (m01 + m10) / s;
        y = 0.25f * s;
        z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        w = (m10 - m01) / s;
        x = (m02 + m20) / s;
        y = (m12 + m21) / s;
        z = 0.25f * s;
    }
    return {x, y, z, w};
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
        } else if (key == "hide") {
            std::vector<std::uintptr_t> objects;
            if (text::to_lower(value) != "off") {
                std::string_view rest = value;
                while (!rest.empty()) {
                    const auto comma = rest.find(',');
                    std::string_view item = text::trim(rest.substr(0, comma));
                    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                    if (item.starts_with("0x") || item.starts_with("0X")) {
                        item.remove_prefix(2);
                    }
                    if (const auto address = text::parse_int(std::string("0x") + std::string(item))) {
                        objects.push_back(static_cast<std::uintptr_t>(*address));
                    }
                }
            }
            request.hide = std::move(objects);
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
    g_force_view = config.get_bool("camera", "force_view", false);
    install_view_hooks();
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
