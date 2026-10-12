// Dependency-free unit tests for the engine-agnostic core utilities.

#include "bg3vr/config.hpp"
#include "bg3vr/hooks.hpp"
#include "bg3vr/pattern.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/text.hpp"

#include <cstdio>
#include <cstring>
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
    test_config();
    if (g_failures == 0) {
        std::printf("all tests passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
