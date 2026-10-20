#include "bg3vr/vrplay.hpp"

#include "bg3vr/camera.hpp"
#include "bg3vr/hold.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/skeleton.hpp"
#include "bg3vr/text.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bg3vr::vrplay {

namespace {

ModuleInfo g_module;
using InjectRawFn = bool (*)(std::uintptr_t manager, const void* change, bool flag);
InjectRawFn g_inject = nullptr;
std::atomic<bool> g_active{false};

// Raw key names (SE InputRawType, keyboard part) for the [bindings] values "key:<name>".
struct KeyName {
    const char* name;
    std::uint16_t id;
};
constexpr KeyName kKeys[] = {
    {"enter", 0}, {"escape", 1}, {"backspace", 2}, {"tab", 3}, {"capslock", 4}, {"space", 5}, {"insert", 9},
    {"home", 10}, {"pageup", 11}, {"del", 12}, {"end", 13}, {"pagedown", 14}, {"comma", 15}, {"hyphen", 16},
    {"dot", 17}, {"slash", 18}, {"equals", 20}, {"leftbracket", 21}, {"backslash", 22}, {"rightbracket", 23},
    {"backtick", 24}, {"apostrophe", 25}, {"num_0", 26}, {"num_1", 27}, {"num_2", 28}, {"num_3", 29}, {"num_4", 30},
    {"num_5", 31}, {"num_6", 32}, {"num_7", 33}, {"num_8", 34}, {"num_9", 35}, {"a", 36}, {"b", 37}, {"c", 38},
    {"d", 39}, {"e", 40}, {"f", 41}, {"g", 42}, {"h", 43}, {"i", 44}, {"j", 45}, {"k", 46}, {"l", 47}, {"m", 48},
    {"n", 49}, {"o", 50}, {"p", 51}, {"q", 52}, {"r", 53}, {"s", 54}, {"t", 55}, {"u", 56}, {"v", 57}, {"w", 58},
    {"x", 59}, {"y", 60}, {"z", 61}, {"f1", 62}, {"f2", 63}, {"f3", 64}, {"f4", 65}, {"f5", 66}, {"f6", 67},
    {"f7", 68}, {"f8", 69}, {"f9", 70}, {"f10", 71}, {"f11", 72}, {"f12", 73}, {"right", 86}, {"left", 87},
    {"down", 88}, {"up", 89}, {"lctrl", 107}, {"lshift", 108}, {"lalt", 109}, {"rctrl", 111}, {"rshift", 112},
    {"ralt", 113},
};

// The controls that can be bound, in a fixed order.
enum Control : int {
    LeftTrigger, LeftGrip, LeftX, LeftY, LeftMenu, LeftStickClick,
    RightTrigger, RightGrip, RightA, RightB, RightStickClick, kControlCount
};
constexpr const char* kControlNames[kControlCount] = {"left_trigger",  "left_grip",    "left_x",      "left_y",
                                                       "left_menu",     "left_stick_click", "right_trigger",
                                                       "right_grip",    "right_a",     "right_b",
                                                       "right_stick_click"};

struct Binding {
    bool bound = false;
    bool recenter = false; // hold to recentre
    bool panel = false;    // press to move the UI panel between the world and the wrist
    bool view = false;     // press to switch first / third person
    bool menu = false;     // press to open / close the VR menu
    std::uint16_t device = 0;
    std::uint16_t id = 0;
    std::string text;      // as configured, for the log
};
std::array<Binding, kControlCount> g_bindings{};
// Defaults (BG3's default PC key bindings for the keys; every one is overridable in [bindings]).
constexpr const char* kDefaults[kControlCount] = {
    "key:lalt",     // left trigger: hold to highlight items / show tooltips
    "",             // left grip: reserved for grabbing
    "key:m",        // X: map
    "view",         // Y: first / third person
    "vrmenu",       // menu: the VR menu (B is the game's Escape)
    "panel",        // left stick click: UI panel to the wrist and back
    "mouse_left",   // right trigger: click (move, interact, attack, UI)
    "mouse_right",  // right grip: context menu
    "key:i",        // A: inventory
    "key:escape",   // B: cancel / back
    "recenter",     // right stick click: hold to recentre
};

// Comfort.
std::atomic<bool> g_snap{true};
std::atomic<float> g_snap_degrees{30.0f};
std::atomic<float> g_smooth_degrees_per_s{120.0f};
std::atomic<bool> g_wheel{true};
std::atomic<float> g_vignette{0.5f};
std::atomic<bool> g_menu_open{false};
std::atomic<bool> g_recalibrate{false};
std::atomic<float> g_turn_rate{0.0f};
float g_recenter_hold_s = 0.8f;

// Eye height.
std::atomic<bool> g_auto_eye_height{true};
std::atomic<float> g_eye_offset{0.10f};

// Presenting thread state.
std::array<bool, kControlCount> g_down{};
std::array<bool, kControlCount> g_injected{}; // a press was sent and needs its release
bool g_snap_armed = true;
double g_last_time = 0.0;
double g_recenter_since = -1.0;
bool g_recenter_fired = false;
double g_next_wheel = 0.0;
std::uint64_t g_injections = 0;
std::uint64_t g_failures = 0;

// Game thread state.
std::uint64_t g_calibrated_generation = 0;
std::vector<float> g_samples;

std::optional<Binding> parse_binding(std::string value) {
    value = text::to_lower(std::string(text::trim(value)));
    Binding b;
    b.text = value;
    if (value.empty() || value == "none") {
        return b;
    }
    b.bound = true;
    if (value == "recenter") {
        b.recenter = true;
        return b;
    }
    if (value == "panel") {
        b.panel = true;
        return b;
    }
    if (value == "view") {
        b.view = true;
        return b;
    }
    if (value == "vrmenu") {
        b.menu = true;
        return b;
    }
    if (value == "mouse_left" || value == "mouse_right" || value == "mouse_middle") {
        b.device = layout::kDeviceMouse;
        b.id = value == "mouse_left" ? layout::kMouseLeft : value == "mouse_right" ? layout::kMouseRight
                                                                                  : layout::kMouseMiddle;
        return b;
    }
    if (value.rfind("key:", 0) == 0) {
        const std::string name = value.substr(4);
        for (const KeyName& key : kKeys) {
            if (name == key.name) {
                b.device = layout::kDeviceKeyboard;
                b.id = key.id;
                return b;
            }
        }
    }
    return std::nullopt;
}

bool inject(std::uint16_t device, std::uint16_t id, bool pressed) {
    std::uintptr_t manager = 0;
    if (!g_inject || !mem::try_copy(&manager, g_module.base + layout::kInputManager, sizeof(manager)) || !manager) {
        ++g_failures;
        return false;
    }
    // InputRawChange as the game's own SDL handlers build it (0x57DC00): u16 id, u16 device, float value,
    // float value2, u32 state.
    struct alignas(16) Change {
        std::uint16_t id;
        std::uint16_t device;
        float value;
        float value2;
        std::uint32_t state;
    } change{id, device, pressed ? 1.0f : 0.0f, pressed ? 1.0f : 0.0f, pressed ? 1u : 0u};
    static_assert(sizeof(Change) == 16);
    g_inject(manager, &change, false);
    ++g_injections;
    return true;
}

void release_all() {
    for (int c = 0; c < kControlCount; ++c) {
        if (g_injected[c]) {
            inject(g_bindings[c].device, g_bindings[c].id, false);
            g_injected[c] = false;
        }
        g_down[c] = false;
    }
}

// Analog controls press above 0.6 and release below 0.4.
bool analog(float value, bool was) {
    return was ? value > 0.4f : value > 0.6f;
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    const auto site = sig::lookup("input_inject_raw");
    if (site) {
        g_inject = reinterpret_cast<InjectRawFn>(*site);
    } else {
        log::warn("vr: signature input_inject_raw not resolved; controller buttons won't reach the game");
    }
    std::string bound;
    for (int c = 0; c < kControlCount; ++c) {
        const std::string value = config.get("bindings", kControlNames[c], kDefaults[c]);
        if (const auto b = parse_binding(value)) {
            g_bindings[c] = *b;
            if (b->bound) {
                bound += std::format("{}{}={}", bound.empty() ? "" : ", ", kControlNames[c], b->text);
            }
        } else {
            log::warn("vr: [bindings] {} = '{}' is not mouse_left/right/middle, key:<name>, recenter, panel or view; unbound",
                      kControlNames[c], value);
        }
    }
    configure(config);
    health::set("controller bindings", g_inject ? health::State::Ready : health::State::Unavailable, g_inject ? "" : "signature input_inject_raw");
    log::info("vr: bindings {}; turn {} ({}), right stick scroll {}, eye height {}", bound.empty() ? "none" : bound,
              g_snap ? "snap" : "smooth",
              g_snap ? std::format("{:.0f} deg", g_snap_degrees.load()) : std::format("{:.0f} deg/s", g_smooth_degrees_per_s.load()),
              g_wheel ? "on" : "off",
              g_auto_eye_height ? std::format("from the head bone {:+.2f} m", g_eye_offset.load()) : std::string("fixed"));
}

void configure(const Config& config) {
    g_snap = text::to_lower(config.get("vr", "turn", "snap")) != "smooth";
    const auto number = [&](const char* key, float fallback, float lo, float hi) {
        const std::string v = config.get("vr", key);
        return v.empty() ? fallback : std::clamp(std::strtof(v.c_str(), nullptr), lo, hi);
    };
    g_snap_degrees = number("snap_degrees", 30.0f, 5.0f, 90.0f);
    g_smooth_degrees_per_s = number("smooth_degrees_per_second", 120.0f, 10.0f, 720.0f);
    g_wheel = config.get_bool("vr", "stick_scroll", true);
    g_vignette = number("comfort_vignette", 0.5f, 0.0f, 1.0f);
    g_auto_eye_height = config.get_bool("vr", "auto_eye_height", true);
    g_eye_offset = number("eye_offset", 0.10f, -0.5f, 0.5f);
}

void mouse_left(bool down) {
    if (g_active.load(std::memory_order_relaxed) || !down) {
        inject(layout::kDeviceMouse, layout::kMouseLeft, down);
    }
}

void recalibrate() {
    g_recalibrate.store(true, std::memory_order_relaxed);
}

void set_menu_open(bool open) {
    if (g_menu_open.exchange(open) != open) {
        log::info("vr: menu {}", open ? "open (the controllers drive it)" : "closed");
    }
}

void set_active(bool on) {
    const bool was = g_active.exchange(on, std::memory_order_relaxed);
    if (was == on) {
        return;
    }
    if (on) {
        g_calibrated_generation = 0; // calibrate the eye height again
    }
    log::info("vr: play {}", on ? "on (controller bindings, turning, recentre)" : "off");
}

bool active() {
    return g_active.load(std::memory_order_relaxed);
}

Feedback on_controllers(const Hand (&hands)[2], double time_s) {
    Feedback feedback;
    const double dt = g_last_time > 0.0 ? std::clamp(time_s - g_last_time, 0.0, 0.1) : 0.0;
    g_last_time = time_s;
    if (!g_active.load(std::memory_order_relaxed)) {
        if (std::any_of(g_injected.begin(), g_injected.end(), [](bool b) { return b; })) {
            release_all();
        }
        return feedback;
    }
    const Hand& l = hands[0];
    const Hand& r = hands[1];
    const std::array<bool, kControlCount> now{
        l.active && analog(l.trigger, g_down[LeftTrigger]), l.active && analog(l.squeeze, g_down[LeftGrip]),
        l.active && l.primary, l.active && l.secondary, l.active && l.menu, l.active && l.stick_click,
        r.active && analog(r.trigger, g_down[RightTrigger]), r.active && analog(r.squeeze, g_down[RightGrip]),
        r.active && r.primary, r.active && r.secondary, r.active && r.stick_click};
    const bool grips_grab = hold::enabled(); // the grips pick things up instead
    const bool menu_open = g_menu_open.load(std::memory_order_relaxed);
    if (menu_open && std::any_of(g_injected.begin(), g_injected.end(), [](bool b) { return b; })) {
        release_all();
    }
    for (int c = 0; c < kControlCount; ++c) {
        const Binding& b = g_bindings[c];
        const bool grip = c == LeftGrip || c == RightGrip;
        if (b.menu && now[c] && !g_down[c]) {
            feedback.toggle_menu = true;
            feedback.haptic[c < RightTrigger ? 0 : 1] = 0.3f;
        }
        if (menu_open) {
            g_down[c] = now[c];
            continue;
        }
        if (b.panel && now[c] && !g_down[c]) {
            feedback.toggle_panel = true;
            feedback.haptic[c < RightTrigger ? 0 : 1] = 0.3f;
        }
        if (b.view && now[c] && !g_down[c]) {
            feedback.toggle_view = true;
            feedback.haptic[c < RightTrigger ? 0 : 1] = 0.3f;
        }
        const bool holster_grip = grip && (c == LeftGrip ? l.grip_taken : r.grip_taken);
        if (now[c] != g_down[c] && b.bound && !b.recenter && !b.panel && !b.view && !b.menu &&
            !((grips_grab || holster_grip) && grip && !g_injected[c])) {
            if (inject(b.device, b.id, now[c])) {
                g_injected[c] = now[c];
                if (now[c]) {
                    feedback.haptic[c < RightTrigger ? 0 : 1] = 0.15f;
                }
            }
        }
        if (b.recenter) {
            if (now[c] && !g_down[c]) {
                g_recenter_since = time_s;
                g_recenter_fired = false;
            } else if (now[c] && !g_recenter_fired && time_s - g_recenter_since >= g_recenter_hold_s) {
                g_recenter_fired = true;
                feedback.recenter = true;
                feedback.haptic[c < RightTrigger ? 0 : 1] = 0.5f;
                log::info("vr: recentre (held {})", kControlNames[c]);
            }
        }
        g_down[c] = now[c];
    }

    // Right stick: x turns, y scrolls.
    g_turn_rate.store(0.0f, std::memory_order_relaxed);
    if (r.active && !menu_open) {
        const float x = r.stick_x;
        if (g_snap) {
            if (g_snap_armed && std::abs(x) > 0.7f) {
                camera::add_yaw(x > 0.0f ? g_snap_degrees.load() : -g_snap_degrees.load());
                g_snap_armed = false;
                feedback.haptic[1] = std::max(feedback.haptic[1], 0.1f);
            } else if (std::abs(x) < 0.3f) {
                g_snap_armed = true;
            }
        } else if (std::abs(x) > 0.2f) {
            const float t = (std::abs(x) - 0.2f) / 0.8f;
            g_turn_rate.store(t, std::memory_order_relaxed);
            camera::add_yaw((x > 0.0f ? 1.0f : -1.0f) * t * g_smooth_degrees_per_s * static_cast<float>(dt));
        }
        const float y = r.stick_y;
        if (g_wheel && std::abs(y) > 0.6f && std::abs(y) > std::abs(x)) {
            if (time_s >= g_next_wheel) {
                const std::uint16_t id = y > 0.0f ? layout::kWheelUp : layout::kWheelDown;
                inject(layout::kDeviceMouse, id, true);
                inject(layout::kDeviceMouse, id, false);
                g_next_wheel = time_s + 0.12;
            }
        } else {
            g_next_wheel = 0.0;
        }
    }
    return feedback;
}

void on_frame() {
    if (!g_active.load(std::memory_order_relaxed) || !g_auto_eye_height) {
        return;
    }
    if (g_recalibrate.exchange(false, std::memory_order_relaxed)) {
        g_calibrated_generation = 0;
        g_samples.clear();
    }
    const std::uint64_t generation = skeleton::generation();
    if (generation == 0 || generation == g_calibrated_generation) {
        return;
    }
    const auto head = skeleton::head();
    const auto feet = camera::follow_position();
    if (!head || !head->fresh || !feet) {
        return;
    }
    g_samples.push_back(head->world[1] - (*feet)[1]);
    if (g_samples.size() < 45) {
        return;
    }
    std::vector<float> sorted = g_samples;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float median = sorted[sorted.size() / 2];
    g_samples.clear();
    g_calibrated_generation = generation;
    if (median < 0.3f || median > 3.0f) {
        log::warn("vr: head bone {:.2f} m above the character's origin looks wrong; eye height left as it is", median);
        return;
    }
    const float offset = g_eye_offset.load();
    const float height = median + offset;
    camera::set_follow_height(height);
    log::info("vr: eye height {:.2f} m (head bone {:.2f} m above the character's origin {:+.2f} m)", height, median,
              offset);
}

std::atomic<bool> g_third{false};

void set_third_person(bool on) {
    if (g_third.exchange(on) != on) {
        log::info("vr: {} person", on ? "third (the game's camera on a screen)" : "first");
    }
}

bool third_person() {
    return g_third.load(std::memory_order_relaxed);
}

float turn_rate() {
    return g_turn_rate.load(std::memory_order_relaxed);
}

float comfort_vignette() {
    return g_vignette;
}

void report() {
    log::info("vr: play {}, {} inputs injected, {} failed", g_active.load(std::memory_order_relaxed) ? "on" : "off",
              g_injections, g_failures);
}

} // namespace bg3vr::vrplay
