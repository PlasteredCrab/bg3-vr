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
// Button combos ([combos]): hold `modifier`, press `button` -> `action` (tapped). The button's own binding doesn't fire;
// a modifier with a key or mouse binding of its own fires it on release instead, unless a combo was used meanwhile.
struct Combo {
    int modifier = -1;
    int button = -1;
    Binding action;
};
struct ComboDefault {
    Control modifier;
    Control button;
    const char* action;
};
constexpr ComboDefault kComboDefaults[] = {
    {LeftGrip, RightA, "key:n"},          // character sheet
    {LeftGrip, RightB, "key:j"},          // journal
    {LeftGrip, LeftX, "key:k"},           // spellbook
    {LeftGrip, LeftY, "key:tab"},         // party view
    {LeftGrip, RightTrigger, "key:space"}, // end turn
    {LeftGrip, RightStickClick, "key:c"}, // sneak
    {LeftGrip, LeftStickClick, "vrmenu"}, // the VR menu (controllers without a menu button: Index)
};
std::vector<Combo> g_combos;
std::array<bool, kControlCount> g_swallow{};    // pressed as a combo's button: ignore until released
std::array<bool, kControlCount> g_deferred{};   // a modifier with its own binding, held: fire on release
std::array<bool, kControlCount> g_combo_used{}; // a combo fired while this modifier was held
std::uint64_t g_combo_taps = 0;

// New bindings from the VR menu (any thread), taken by on_controllers (held inputs are released first).
std::mutex g_pending_mutex;
std::optional<std::array<Binding, kControlCount>> g_pending_bindings;
std::optional<std::vector<Combo>> g_pending_combos;

std::array<Binding, kControlCount> parse_bindings(const Config& config, std::string& bound);
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
std::atomic<float> g_eye_offset{0.0f};
constexpr float kHeadBoneToEyes = 0.10f; // used only when the skeleton has no eye bones

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

// Releases every key / button this module holds down. `forget_edges`: also forget which controls are down (VR play
// off); otherwise a control still held doesn't show a new press next frame (opening the menu, new bindings).
void release_all(bool forget_edges = true) {
    for (int c = 0; c < kControlCount; ++c) {
        if (g_injected[c]) {
            inject(g_bindings[c].device, g_bindings[c].id, false);
            g_injected[c] = false;
        }
        if (forget_edges) {
            g_down[c] = false;
        }
    }
}

// Analog controls press above 0.6 and release below 0.4.
bool analog(float value, bool was) {
    return was ? value > 0.4f : value > 0.6f;
}

std::array<Binding, kControlCount> parse_bindings(const Config& config, std::string& bound) {
    std::array<Binding, kControlCount> bindings{};
    for (int c = 0; c < kControlCount; ++c) {
        const std::string value = config.get("bindings", kControlNames[c], kDefaults[c]);
        if (const auto b = parse_binding(value)) {
            bindings[c] = *b;
            if (b->bound) {
                bound += std::format("{}{}={}", bound.empty() ? "" : ", ", kControlNames[c], b->text);
            }
        } else {
            log::warn("vr: [bindings] {} = '{}' is not mouse_left/right/middle, key:<name>, recenter, panel, view or "
                      "vrmenu; unbound",
                      kControlNames[c], value);
        }
    }
    return bindings;
}

std::vector<Combo> parse_combos(const Config& config, std::string& listed) {
    std::vector<Combo> combos;
    for (const ComboDefault& d : kComboDefaults) {
        const std::string key = std::string(kControlNames[d.modifier]) + "+" + kControlNames[d.button];
        const std::string value = config.get("combos", key, d.action);
        if (const auto b = parse_binding(value); b && b->bound) {
            combos.push_back({d.modifier, d.button, *b});
            listed += std::format("{}{}={}", listed.empty() ? "" : ", ", key, b->text);
        }
    }
    return combos;
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
    g_bindings = parse_bindings(config, bound);
    std::string combos;
    g_combos = parse_combos(config, combos);
    log::info("vr: button combos {}", combos.empty() ? "none" : combos);
    configure(config);
    health::set("controller bindings", g_inject ? health::State::Ready : health::State::Unavailable, g_inject ? "" : "signature input_inject_raw");
    log::info("vr: bindings {}; turn {} ({}), right stick scroll {}, eye height {}", bound.empty() ? "none" : bound,
              g_snap ? "snap" : "smooth",
              g_snap ? std::format("{:.0f} deg", g_snap_degrees.load()) : std::format("{:.0f} deg/s", g_smooth_degrees_per_s.load()),
              g_wheel ? "on" : "off",
              g_auto_eye_height ? std::format("from the character's eyes {:+.2f} m", g_eye_offset.load()) : std::string("fixed"));
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
    g_eye_offset = number("eye_offset", 0.0f, -0.5f, 0.5f);
}

void mouse_left(bool down) {
    if (g_active.load(std::memory_order_relaxed) || !down) {
        inject(layout::kDeviceMouse, layout::kMouseLeft, down);
    }
}

bool tap_key(std::string_view name) {
    if (!g_active.load(std::memory_order_relaxed)) {
        return false;
    }
    for (const KeyName& k : kKeys) {
        if (name == k.name) {
            return inject(layout::kDeviceKeyboard, k.id, true) && inject(layout::kDeviceKeyboard, k.id, false);
        }
    }
    return false;
}

void configure_bindings(const Config& config) {
    std::string bound, listed;
    auto bindings = parse_bindings(config, bound);
    auto combos = parse_combos(config, listed);
    {
        std::scoped_lock lock(g_pending_mutex);
        g_pending_bindings = bindings;
        g_pending_combos = combos;
    }
    log::info("vr: bindings now {}; combos {}", bound.empty() ? "none" : bound, listed.empty() ? "none" : listed);
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
    {
        std::scoped_lock lock(g_pending_mutex);
        if (g_pending_bindings) {
            release_all(false); // with the old bindings
            g_bindings = *g_pending_bindings;
            g_pending_bindings.reset();
        }
        if (g_pending_combos) {
            g_combos = *g_pending_combos;
            g_pending_combos.reset();
            g_swallow = {};
            g_deferred = {};
        }
    }
    const Hand& l = hands[0];
    const Hand& r = hands[1];
    const std::array<bool, kControlCount> now{
        l.active && analog(l.trigger, g_down[LeftTrigger]), l.active && analog(l.squeeze, g_down[LeftGrip]),
        // The menu button is whichever controller has one (Touch: the left), so a left-handed layout keeps it.
        l.active && l.primary, l.active && l.secondary, (l.active && l.menu) || (r.active && r.menu), l.active && l.stick_click,
        r.active && analog(r.trigger, g_down[RightTrigger]), r.active && analog(r.squeeze, g_down[RightGrip]),
        r.active && r.primary, r.active && r.secondary, r.active && r.stick_click};
    const bool grips_grab = hold::enabled(); // the grips pick things up instead
    const bool menu_open = g_menu_open.load(std::memory_order_relaxed);
    if (menu_open && std::any_of(g_injected.begin(), g_injected.end(), [](bool b) { return b; })) {
        release_all(false);
    }
    // A tap of a binding: keys and mouse buttons pressed and released, the VR actions as feedback.
    const auto tap = [&](const Binding& action, int c) {
        if (action.view) {
            feedback.toggle_view = true;
        } else if (action.panel) {
            feedback.toggle_panel = true;
        } else if (action.menu) {
            feedback.toggle_menu = true;
        } else if (action.recenter) {
            feedback.recenter = true;
        } else if (action.bound) {
            inject(action.device, action.id, true);
            inject(action.device, action.id, false);
        }
        feedback.haptic[c < RightTrigger ? 0 : 1] = 0.3f;
    };
    // Combos first: a button pressed while its combo's modifier is held.
    if (!menu_open) {
        for (const Combo& combo : g_combos) {
            // A grip that is holding something (a holster, a body, a drawn bow) isn't a combo modifier.
            const bool busy = (combo.modifier == LeftGrip && l.grip_taken) || (combo.modifier == RightGrip && r.grip_taken);
            if (now[combo.button] && !g_down[combo.button] && now[combo.modifier] && !busy) {
                tap(combo.action, combo.button);
                g_swallow[combo.button] = true;
                g_combo_used[combo.modifier] = true;
                ++g_combo_taps;
                log::info("vr: combo {} + {} -> {}", kControlNames[combo.modifier], kControlNames[combo.button],
                          combo.action.text);
            }
        }
    }
    for (int c = 0; c < kControlCount; ++c) {
        const Binding& b = g_bindings[c];
        const bool grip = c == LeftGrip || c == RightGrip;
        if (g_swallow[c]) {
            if (!now[c]) {
                g_swallow[c] = false;
            }
            g_down[c] = now[c];
            continue;
        }
        // A modifier with a key / mouse binding of its own: that binding fires on release, unless a combo was used.
        const bool device_binding = b.bound && !b.recenter && !b.panel && !b.view && !b.menu;
        const bool modifier = device_binding && !menu_open &&
                              std::any_of(g_combos.begin(), g_combos.end(), [c](const Combo& k) { return k.modifier == c; });
        if (modifier || g_deferred[c]) {
            if (now[c] && !g_down[c]) {
                g_deferred[c] = true;
                g_combo_used[c] = false;
            } else if (!now[c] && g_down[c] && g_deferred[c]) {
                g_deferred[c] = false;
                if (!g_combo_used[c]) {
                    tap(b, c);
                }
            }
            g_down[c] = now[c];
            continue;
        }
        if (now[c] && !g_down[c]) {
            g_combo_used[c] = false;
        }
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
    // The character's eyes (Dummy_EyeFX bones) when the skeleton has them, else the head bone plus a typical offset.
    // Eye bones that don't sit near the head bone (the 2026-10-20 run read 19 m) aren't trusted: head bone + 0.10 m.
    const bool eyes_ok = head->has_eyes && std::abs(head->eyes_world[1] - head->world[1]) < 0.3f;
    if (head->has_eyes && !eyes_ok && g_samples.empty()) {
        log::warn("vr: the eye bones are {:.2f} m from the head bone (model ({:.3f}, {:.3f}, {:.3f})); using the head bone",
                  head->eyes_world[1] - head->world[1], head->eyes_model[0], head->eyes_model[1], head->eyes_model[2]);
    }
    g_samples.push_back((eyes_ok ? head->eyes_world[1] : head->world[1] + kHeadBoneToEyes) - (*feet)[1]);
    if (g_samples.size() < 45) {
        return;
    }
    std::vector<float> sorted = g_samples;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float median = sorted[sorted.size() / 2];
    g_samples.clear();
    g_calibrated_generation = generation;
    if (median < 0.3f || median > 3.0f) {
        log::warn("vr: eyes {:.2f} m above the character's origin looks wrong; eye height left as it is", median);
        return;
    }
    const float offset = g_eye_offset.load();
    const float height = median + offset;
    camera::set_follow_height(height);
    log::info("vr: eye height {:.2f} m ({} {:.2f} m above the character's origin {:+.2f} m; head bone {:.2f} m; the "
              "character's eyes are {:.3f} m apart)",
              height, head->has_eyes && std::abs(head->eyes_world[1] - head->world[1]) < 0.3f ? "eye bones" : "head bone + 0.10 m",
              median, offset,
              head->world[1] - (*feet)[1], head->eye_separation);
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
