// Dependency-free unit tests for the engine-agnostic core utilities.

#include "bg3vr/camera.hpp"
#include "bg3vr/config.hpp"
#include "bg3vr/hooks.hpp"
#include "bg3vr/pattern.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/watch.hpp"

#include <atomic>
#include <chrono>
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

    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(&g_watched_value);
    const auto hits = watch::watch_writes(std::span(&address, 1), std::chrono::milliseconds(300));
    CHECK(!hits.empty());
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

void test_parse_bridge() {
    const auto request = camera::parse_bridge("controller=000001F2AB34CD50\r\npose=1.5 -2 3 90 -10\n");
    CHECK(request.controller == 0x000001F2AB34CD50ull);
    CHECK(request.pose && near_eq(request.pose->x, 1.5f) && near_eq(request.pose->yaw_deg, 90.0f) &&
          near_eq(request.pose->pitch_deg, -10.0f));
    CHECK(!request.pose_off);

    const auto off = camera::parse_bridge("controller=0x10\npose=off\n");
    CHECK(off.controller == 0x10u && off.pose_off && !off.pose && !off.probe);
    CHECK(camera::parse_bridge("probe=1\n").probe);

    const auto hide = camera::parse_bridge("hide=0000019AA2C20280, 0x10,zz\n");
    CHECK(hide.hide && hide.hide->size() == 2 && hide.hide->at(0) == 0x0000019AA2C20280ull && hide.hide->at(1) == 0x10u);
    const auto show = camera::parse_bridge("hide=off\n");
    CHECK(show.hide && show.hide->empty());
    CHECK(!camera::parse_bridge("pose=off\n").hide);

    const auto bad = camera::parse_bridge("pose=1 2 three 4 5\ncontroller=zz\n");
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
    test_parse_bridge();
    test_watch_probes();
    test_config();
    if (g_failures == 0) {
        std::printf("all tests passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
