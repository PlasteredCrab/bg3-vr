// Dependency-free unit tests for the engine-agnostic core utilities.

#include "bg3vr/bridge.hpp"
#include "bg3vr/pose.hpp"
#include "bg3vr/config.hpp"
#include "bg3vr/hooks.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/pattern.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/views.hpp"
#include "bg3vr/vr_math.hpp"
#include "bg3vr/watch.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <cstdio>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

using namespace bg3vr;

void test_parse_int() {
    CHECK(text::parse_int("42") == 42);
    CHECK(text::parse_int(" -0x10 ") == -16);
    CHECK(!text::parse_int("0x"));
    CHECK(!text::parse_int("12abc"));
}

void test_parse_pattern() {
    const auto p = parse_pattern("48 8B ?? ? C3");
    CHECK(p.has_value());
    CHECK(p->size() == 5);
    CHECK(p->bytes[1] == 0x8B && p->mask[1]);
    CHECK(!p->mask[2] && !p->mask[3]);
    CHECK(!parse_pattern("48 8"));
    CHECK(!parse_pattern("48 ZZ"));
    CHECK(!parse_pattern("?? ??"));
    CHECK(!parse_pattern(""));
}

void test_find() {
    const std::vector<std::uint8_t> hay = {0x00, 0x48, 0x8B, 0x05, 0x11, 0x48, 0x8B, 0x07, 0x22, 0x48, 0x8B};
    const auto p = parse_pattern("48 8B ??");
    const auto hits = find_all(hay, *p);
    CHECK(hits.size() == 2); // the trailing "48 8B" has no room for the wildcard
    CHECK(hits.size() == 2 && hits[0] == 1 && hits[1] == 5);
    CHECK(find_all(hay, *p, 1).size() == 1);

    const auto leading_wildcard = parse_pattern("?? 8B 07");
    CHECK(find_first(hay, *leading_wildcard) == 5);
    CHECK(!find_first(hay, *parse_pattern("C3")));
}

std::vector<std::uint8_t> code_with_rip(std::size_t at, std::int32_t disp) {
    std::vector<std::uint8_t> buffer(64, 0x90);
    const std::uint8_t mov[] = {0x48, 0x8B, 0x05};
    std::memcpy(buffer.data() + at, mov, sizeof(mov));
    std::memcpy(buffer.data() + at + 3, &disp, sizeof(disp));
    return buffer;
}

void test_resolve_rip() {
    const auto buffer = code_with_rip(8, 0x20);
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.data());
    CHECK(resolve_rip(base + 8, 3, 7) == base + 8 + 7 + 0x20);

    const auto negative = code_with_rip(16, -12);
    const auto nbase = reinterpret_cast<std::uintptr_t>(negative.data());
    CHECK(resolve_rip(nbase + 16, 3, 7) == nbase + 16 + 7 - 12);
}

void test_signature_lines() {
    std::string error;
    CHECK(!sig::parse_line("# comment", &error) && error.empty());
    CHECK(!sig::parse_line("   ", &error) && error.empty());

    const auto plain = sig::parse_line("camera = 48 8B 05 ?? ?? ?? ??", &error);
    CHECK(plain && plain->name == "camera" && plain->pattern.size() == 7 && plain->offset == 0 && !plain->rip);

    const auto full = sig::parse_line("cam_global = 90 48 8B 05 ?? ?? ?? ?? | offset 1 | rip 3 7", &error);
    CHECK(full && full->offset == 1 && full->rip && full->rip->disp_offset == 3 && full->rip->instr_len == 7);

    error.clear();
    CHECK(!sig::parse_line("broken 48 8B", &error) && !error.empty());
    error.clear();
    CHECK(!sig::parse_line("x = 48 | rip 3", &error) && !error.empty());
    error.clear();
    CHECK(!sig::parse_line("x = 48 | wobble 2", &error) && !error.empty());
}

void test_scan() {
    auto buffer = code_with_rip(8, 0x20);
    buffer[7] = 0xC3; // unique byte before the mov
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.data());

    std::string error;
    const auto spec = sig::parse_line("g = C3 48 8B 05 ?? ?? ?? ?? | offset 1 | rip 3 7", &error);
    CHECK(spec.has_value());
    const auto result = sig::scan(*spec, buffer, base);
    CHECK(result.hit_count == 1);
    CHECK(result.address == base + 8 + 7 + 0x20);

    const auto ambiguous = sig::parse_line("a = 90 90", &error);
    const auto many = sig::scan(*ambiguous, buffer, base);
    CHECK(many.hit_count == sig::kMaxHits && !many.address);

    sig::publish("g", *result.address);
    CHECK(sig::lookup("g") == result.address);
    CHECK(!sig::lookup("missing"));
}

__declspec(noinline) int add_numbers(int a, int b) {
    return a + b;
}

safetyhook::InlineHook g_add_hook;

int add_detour(int a, int b) {
    return g_add_hook.call<int>(a, b) * 10;
}

void test_inline_hook() {
    int (*volatile fn)(int, int) = &add_numbers; // volatile: keep calls going through the hooked code
    CHECK(fn(2, 3) == 5);

    CHECK(!hooks::install("not_published", reinterpret_cast<void*>(&add_detour)));

    sig::publish("test_add", reinterpret_cast<std::uintptr_t>(fn));
    g_add_hook = hooks::install("test_add", reinterpret_cast<void*>(&add_detour));
    CHECK(static_cast<bool>(g_add_hook));
    CHECK(fn(2, 3) == 50);

    g_add_hook = {};
    CHECK(fn(2, 3) == 5);
}

alignas(8) volatile std::uint64_t g_watched_value = 0;

__declspec(noinline) void write_watched(std::uint64_t value) {
    g_watched_value = value;
}

__declspec(noinline) int counted_function(int x) {
    return x * 3 + 1;
}

void test_watch_probes() {
    std::atomic<bool> running{true};
    std::thread worker([&] {
        std::uint64_t i = 0;
        int (*volatile fn)(int) = &counted_function;
        while (running.load()) {
            write_watched(++i);
            fn(static_cast<int>(i));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // let the worker start

    // Setting debug registers trips endpoint protection (Sophos Intercept X logs an "HWBGuard"
    // mitigation each time), so the hardware write watch only runs when asked for.
    char flag[8] = {};
    const bool watch_enabled = GetEnvironmentVariableA("BG3VR_TEST_HWBP", flag, sizeof(flag)) > 0 && flag[0] == '1';
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(&g_watched_value);
    const auto hits = watch_enabled ? watch::watch_writes(std::span(&address, 1), std::chrono::milliseconds(300))
                                    : std::vector<watch::WriteHit>{};
    if (!watch_enabled) {
        std::printf("skipped hardware write watch test (set BG3VR_TEST_HWBP=1 to run it)\n");
    }
    CHECK(!watch_enabled || !hits.empty());
    if (!hits.empty()) {
        CHECK(hits[0].watch_index == 0 && hits[0].count > 10);
        // The instruction after the write sits inside write_watched (or its incremental-link body).
        bool in_writer = false;
        for (const auto& hit : hits) {
            const auto start = reinterpret_cast<std::uintptr_t>(&write_watched);
            in_writer = in_writer || (hit.rip > start - 0x100000 && hit.rip < start + 0x100000);
        }
        CHECK(in_writer);
    }

    const std::uintptr_t function = reinterpret_cast<std::uintptr_t>(&counted_function);
    const auto counts = watch::count_calls(std::span(&function, 1), std::chrono::milliseconds(300));
    CHECK(counts.size() == 1 && counts[0] != UINT64_MAX && counts[0] > 10);

    running = false;
    worker.join();
    CHECK(counted_function(2) == 7); // still works after unhooking
}

void test_memory() {
    const int local = 42;
    const auto address = reinterpret_cast<std::uintptr_t>(&local);
    CHECK(mem::readable(address, sizeof(local)));
    CHECK(mem::read<int>(address) == 42);
    CHECK(!mem::readable(0, 8));
    CHECK(!mem::readable(0x10, 8));

    // The test exe itself stands in for the game module.
    const ModuleInfo module = query_main_module();
    const auto function = reinterpret_cast<std::uintptr_t>(&test_memory);
    CHECK(mem::in_section(module, ".text", function));
    CHECK(!mem::in_section(module, ".rdata", function));
    CHECK(!mem::in_section(module, ".text", address));
    CHECK(mem::describe(module, function).starts_with("exe+0x"));
    CHECK(mem::describe(module, reinterpret_cast<std::uintptr_t>(&GetTickCount64)).find("+0x") != std::string::npos);

    std::uint64_t copied = 0;
    CHECK(mem::try_copy(&copied, address, sizeof(int)) && static_cast<int>(copied) == 42);
    CHECK(!mem::try_copy(&copied, 0x10, sizeof(copied)));

    // Cost of the two ways to check an address before reading it (endpoint protection can make
    // VirtualQuery expensive).
    LARGE_INTEGER frequency, t0, t1, t2;
    QueryPerformanceFrequency(&frequency);
    bool all = true;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 1000; ++i) {
        all = mem::readable(address, sizeof(local)) && all;
    }
    QueryPerformanceCounter(&t1);
    for (int i = 0; i < 1000; ++i) {
        all = mem::try_copy(&copied, address, sizeof(int)) && all;
    }
    QueryPerformanceCounter(&t2);
    CHECK(all);
    const auto us = [&](LARGE_INTEGER a, LARGE_INTEGER b) {
        return 1e6 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(frequency.QuadPart) / 1000.0;
    };
    std::printf("mem::readable %.3f us per call, mem::try_copy %.3f us per call\n", us(t0, t1), us(t1, t2));
}

bool near_eq(float a, float b) {
    return a - b < 1e-4f && b - a < 1e-4f;
}

void test_pose_matrices() {
    camera::Mat4 world, view;

    camera::pose_matrices({1.0f, 2.0f, 3.0f, 0.0f, 0.0f}, world, view);
    CHECK(near_eq(world[8], 0.0f) && near_eq(world[9], 0.0f) && near_eq(world[10], 1.0f)); // looks along +Z
    CHECK(near_eq(world[0], 1.0f) && near_eq(world[5], 1.0f));                             // right +X, up +Y
    CHECK(near_eq(world[12], 1.0f) && near_eq(world[13], 2.0f) && near_eq(world[14], 3.0f));

    camera::pose_matrices({0.0f, 0.0f, 0.0f, 90.0f, 0.0f}, world, view);
    CHECK(near_eq(world[8], 1.0f) && near_eq(world[10], 0.0f)); // yaw 90 looks along +X

    camera::pose_matrices({0.0f, 0.0f, 0.0f, 0.0f, 30.0f}, world, view);
    CHECK(world[9] > 0.49f); // positive pitch looks up

    // view is the inverse of world
    camera::pose_matrices({5.0f, -2.0f, 7.5f, 37.0f, -12.0f}, world, view);
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += view[k * 4 + row] * world[col * 4 + k];
            }
            CHECK(near_eq(sum, row == col ? 1.0f : 0.0f));
        }
    }
}

// v' = q v q* for a unit quaternion (x, y, z, w).
std::array<float, 3> rotate(const std::array<float, 4>& q, std::array<float, 3> v) {
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const float tx = 2.0f * (qy * v[2] - qz * v[1]);
    const float ty = 2.0f * (qz * v[0] - qx * v[2]);
    const float tz = 2.0f * (qx * v[1] - qy * v[0]);
    return {v[0] + qw * tx + (qy * tz - qz * ty), v[1] + qw * ty + (qz * tx - qx * tz),
            v[2] + qw * tz + (qx * ty - qy * tx)};
}

void test_pose_quaternion() {
    const camera::Pose poses[] = {{0, 0, 0, 0, 0}, {0, 0, 0, 90, 0}, {0, 0, 0, 180, 0}, {0, 0, 0, -135, 0},
                                  {0, 0, 0, 37, -25}, {0, 0, 0, 200, 60}, {0, 0, 0, 0, 89}};
    for (const auto& pose : poses) {
        camera::Mat4 world, view;
        camera::pose_matrices(pose, world, view);
        const auto q = camera::pose_quaternion(pose);
        CHECK(near_eq(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0f));
        for (int axis = 0; axis < 3; ++axis) {
            std::array<float, 3> unit{0, 0, 0};
            unit[axis] = 1.0f;
            const auto r = rotate(q, unit);
            CHECK(near_eq(r[0], world[axis * 4]) && near_eq(r[1], world[axis * 4 + 1]) &&
                  near_eq(r[2], world[axis * 4 + 2]));
        }
    }
}

std::array<float, 4> transform(const camera::Mat4& m, const std::array<float, 4>& v) {
    std::array<float, 4> out{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            out[row] += m[col * 4 + row] * v[col];
        }
    }
    return out;
}

bool vec_near(const vr::Vec3& a, const vr::Vec3& b) {
    return near_eq(a[0], b[0]) && near_eq(a[1], b[1]) && near_eq(a[2], b[2]);
}

void test_vr_math() {
    // yaw_quat agrees with the yaw/pitch pose math, and quaternion matrices with pose_matrices.
    for (const float yaw : {0.0f, 45.0f, 180.0f, -120.0f}) {
        const auto q = camera::pose_quaternion({0, 0, 0, yaw, 0});
        const auto y = vr::yaw_quat(yaw);
        CHECK(near_eq(std::abs(q[0] * y[0] + q[1] * y[1] + q[2] * y[2] + q[3] * y[3]), 1.0f));
    }
    for (const camera::Pose pose : {camera::Pose{1, 2, 3, 30, -20}, camera::Pose{-5, 0.5f, 9, 250, 60}}) {
        camera::Mat4 world_a, view_a, world_b, view_b;
        camera::pose_matrices(pose, world_a, view_a);
        vr::camera_matrices({{pose.x, pose.y, pose.z}, camera::pose_quaternion(pose)}, world_b, view_b);
        bool same = true;
        for (std::size_t i = 0; i < 16; ++i) {
            same = same && near_eq(world_a[i], world_b[i]) && near_eq(view_a[i], view_b[i]);
        }
        CHECK(same);
    }

    // from_openxr is the Z mirror: mirror(rotate(q, v)) == rotate(from_openxr(q), mirror(v)).
    const auto mirror = [](const vr::Vec3& v) { return vr::Vec3{v[0], v[1], -v[2]}; };
    const vr::Quat xr_rotations[] = {
        {0, 0, 0, 1},
        {0, 0.258819f, 0, 0.965926f},                // 30 deg yaw
        {0.130526f, 0, 0, 0.991445f},                // 15 deg pitch
        {0, 0, 0.173648f, 0.984808f},                // 20 deg roll
        {0.2f, -0.3f, 0.1f, 0.926013f},              // mixed (normalised)
    };
    for (const auto& q : xr_rotations) {
        const auto game = vr::from_openxr({{0.1f, 1.6f, -0.2f}, q});
        CHECK(vec_near(game.position, {0.1f, 1.6f, 0.2f}));
        for (const vr::Vec3& v : {vr::Vec3{1, 0, 0}, vr::Vec3{0, 1, 0}, vr::Vec3{0, 0, 1}, vr::Vec3{0.3f, -0.4f, 0.8f}}) {
            CHECK(vec_near(mirror(vr::rotate(q, v)), vr::rotate(game.orientation, mirror(v))));
        }
    }
    // Turning the head left in OpenXR (+30 deg about +Y) looks toward -X in the game.
    const auto left = vr::from_openxr({{0, 0, 0}, {0, 0.258819f, 0, 0.965926f}});
    const auto forward = vr::rotate(left.orientation, {0, 0, 1});
    CHECK(forward[0] < -0.49f && forward[2] > 0.86f);

    // Anchoring: the local forward offset follows the anchor's yaw; the anchor's pitch is ignored.
    const auto placed = vr::anchor({10, 2, -3, 90, 45}, {{0, 0, 1}, {0, 0, 0, 1}});
    CHECK(vec_near(placed.position, {11, 2, -3}));
    CHECK(vec_near(vr::rotate(placed.orientation, {0, 0, 1}), {1, 0, 0}));

    // Projection: the symmetric case reproduces the game's own matrix (logged 2026-10-12: vertical
    // FOV 55 deg, aspect 1.6, near 0.1, far 10000 -> [0]=1.201 [5]=1.921 [10]~-1e-5 [11]=1 [14]~0.1).
    const float half_v = 27.5f * std::numbers::pi_v<float> / 180.0f;
    const float half_h = std::atan(std::tan(half_v) * 1.6f);
    const auto p = vr::projection(-half_h, half_h, half_v, -half_v, 0.1f, 10000.0f);
    CHECK(std::abs(p[0] - 1.2006f) < 1e-3f && std::abs(p[5] - 1.9210f) < 1e-3f);
    CHECK(std::abs(p[10]) < 1e-4f && near_eq(p[11], 1.0f) && std::abs(p[14] - 0.1f) < 1e-4f);
    CHECK(near_eq(p[8], 0.0f) && near_eq(p[9], 0.0f));

    // An asymmetric eye frustum (Quest-2-like left eye): its edges map to NDC -1/+1, the near plane
    // to depth 1 and the far plane to depth 0 (reversed Z).
    const float l = -0.942478f, r = 0.698132f, u = 0.733038f, d = -0.837758f; // -54, 40, 42, -48 deg
    const auto eye = vr::projection(l, r, u, d, 0.1f, 10000.0f);
    const auto ndc = [&](float x, float y, float z) {
        const auto c = transform(eye, {x, y, z, 1.0f});
        return std::array<float, 3>{c[0] / c[3], c[1] / c[3], c[2] / c[3]};
    };
    const float z = 5.0f;
    CHECK(near_eq(ndc(std::tan(r) * z, 0, z)[0], 1.0f) && near_eq(ndc(std::tan(l) * z, 0, z)[0], -1.0f));
    CHECK(near_eq(ndc(0, std::tan(u) * z, z)[1], 1.0f) && near_eq(ndc(0, std::tan(d) * z, z)[1], -1.0f));
    CHECK(near_eq(ndc(0, 0, 0.1f)[2], 1.0f) && std::abs(ndc(0, 0, 10000.0f)[2]) < 1e-4f);
}

void test_parse_bridge() {
    const auto request = bridge::parse("controller=000001F2AB34CD50\r\npose=1.5 -2 3 90 -10\n");
    CHECK(request.controller == 0x000001F2AB34CD50ull);
    CHECK(request.pose && near_eq(request.pose->x, 1.5f) && near_eq(request.pose->yaw_deg, 90.0f) &&
          near_eq(request.pose->pitch_deg, -10.0f));
    CHECK(!request.pose_off);

    const auto off = bridge::parse("controller=0x10\npose=off\n");
    CHECK(off.controller == 0x10u && off.pose_off && !off.pose && !off.probe);
    CHECK(bridge::parse("probe=1\n").probe);

    const auto hide = bridge::parse("hide=0000019AA2C20280, 0x10,zz\n");
    CHECK(hide.hide && hide.hide->size() == 2 && hide.hide->at(0) == 0x0000019AA2C20280ull && hide.hide->at(1) == 0x10u);
    const auto show = bridge::parse("hide=off\n");
    CHECK(show.hide && show.hide->empty());
    CHECK(!bridge::parse("pose=off\n").hide);
    CHECK(bridge::parse("mouselook=on\n").mouselook == true);
    CHECK(bridge::parse("mouselook=0\n").mouselook == false);
    CHECK(!bridge::parse("mouselook=maybe\n").mouselook);

    const auto sweep_on = bridge::parse("sweep=on\n");
    CHECK(sweep_on.sweep && *sweep_on.sweep && near_eq((*sweep_on.sweep)->yaw_deg, 60.0f) &&
          near_eq((*sweep_on.sweep)->period_s, 8.0f));
    const auto sweep_custom = bridge::parse("sweep=90 10\n");
    CHECK(sweep_custom.sweep && *sweep_custom.sweep && near_eq((*sweep_custom.sweep)->yaw_deg, 90.0f) &&
          near_eq((*sweep_custom.sweep)->pitch_deg, 10.0f) && near_eq((*sweep_custom.sweep)->period_s, 8.0f));
    const auto sweep_off = bridge::parse("sweep=off\n");
    CHECK(sweep_off.sweep && !*sweep_off.sweep);
    CHECK(!bridge::parse("sweep=fast\n").sweep);
    CHECK(!bridge::parse("pose=off\n").sweep);

    const auto eye = bridge::parse("fov=-52 44 47 -51\n");
    CHECK(eye.eye_fov && *eye.eye_fov && near_eq((*eye.eye_fov)->left_deg, -52.0f) &&
          near_eq((*eye.eye_fov)->down_deg, -51.0f));
    const auto eye_off = bridge::parse("fov=off\n");
    CHECK(eye_off.eye_fov && !*eye_off.eye_fov);
    CHECK(!bridge::parse("fov=10 -10 40 -40\n").eye_fov); // right < left

    CHECK(bridge::parse("xr_lag=2\n").xr_lag == 2);
    CHECK(bridge::parse("xr_lag=AUTO\n").xr_lag == -1);
    CHECK(!bridge::parse("xr_lag=8\n").xr_lag && !bridge::parse("xr_lag=-3\n").xr_lag && !bridge::parse("xr_lag=x\n").xr_lag);
    CHECK(!bridge::parse("pose=off\n").xr_lag);
    CHECK(bridge::parse("views=find\n").views_find == true && bridge::parse("views=DUMP\n").views_find == false);
    CHECK(!bridge::parse("views=x\n").views_find);
    CHECK(!bridge::parse("fov=-45 45 90 -45\n").eye_fov); // 90 deg is not a frustum
    CHECK(!bridge::parse("fov=-45 45 45\n").eye_fov);

    const auto bad = bridge::parse("pose=1 2 three 4 5\ncontroller=zz\n");
    CHECK(!bad.pose && !bad.controller);
}

void test_config() {
    Config config;
    config.load_string("# top\n[Dev]\nConsole = yes\r\nlog_level=debug\n\n[scan]\nlimit = 0x20\nbad\n");
    CHECK(config.get_bool("dev", "console", false));
    CHECK(config.get("DEV", "LOG_LEVEL") == "debug");
    CHECK(config.get_int("scan", "limit", 0) == 32);
    CHECK(config.get("scan", "missing", "fallback") == "fallback");
    CHECK(!config.get_bool("scan", "limit", false));
}

// The render-view scan finds a planted ReadState/WriteState pair pointing at a "controller" exactly
// once (not again in its own copy buffer), and ignores a lone pointer that isn't in a state.
void test_views_scan() {
    auto* controller = new std::uint64_t(0);
    const auto controller_address = reinterpret_cast<std::uintptr_t>(controller);
    auto* view = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(view != nullptr);
    if (!view) {
        return;
    }
    const auto plant_state = [&](std::size_t at) {
        view[at] = 1;                                     // Enabled
        const float scale[2] = {1.0f, 1.0f};
        std::memcpy(view + at + 0x08, scale, sizeof(scale));
        const int viewport[4] = {1920, 1080, 0, 0};       // Width, Height, OffsetX, OffsetY
        for (const std::size_t viewport_at : {0x18u, 0x38u, 0x58u}) {
            std::memcpy(view + at + viewport_at + 0x10, viewport, sizeof(viewport));
        }
        std::memcpy(view + at + 0x80, &controller_address, sizeof(controller_address));
    };
    plant_state(0x2148);
    plant_state(0x2148 + 0x88);
    std::memcpy(view + 0x100, &controller_address, sizeof(controller_address)); // a stray reference

    const auto found = views::scan_now(controller_address);
    CHECK(found.size() == 1 && found[0] == reinterpret_cast<std::uintptr_t>(view) + 0x2148);
    VirtualFree(view, 0, MEM_RELEASE);
    delete controller;
}

} // namespace

int main() {
    test_parse_int();
    test_parse_pattern();
    test_find();
    test_resolve_rip();
    test_signature_lines();
    test_scan();
    test_inline_hook();
    test_pose_matrices();
    test_pose_quaternion();
    test_vr_math();
    test_parse_bridge();
    test_watch_probes();
    test_memory();
    test_config();
    test_views_scan();
    if (g_failures == 0) {
        std::printf("all tests passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
