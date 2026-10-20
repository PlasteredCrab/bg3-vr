#include "bg3vr/vrmenu.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/text.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite_1.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <format>
#include <mutex>
#include <numbers>
#include <thread>

namespace bg3vr::vrmenu {

namespace {

using Kind = Snapshot::Row::Kind;

// Layout (pixels of the kWidth x kHeight image).
constexpr int kHeaderBottom = 184;
constexpr int kTabsHeight = 78;
constexpr int kChoiceHeight = 62;
constexpr int kCompactHeight = 44;
constexpr int kHeadingHeight = 40;
constexpr int kButtonsHeight = 110;
constexpr int kFooterTop = kHeight - 62;
constexpr int kButtonsTop = kFooterTop - kButtonsHeight - 8;
constexpr int kGuideNavHeight = 64; // the Previous / Next strip at the bottom of the Guide
constexpr float kTabGap = 12.0f;
constexpr float kArrowLeft = 600.0f;   // "<" of a choice row
constexpr float kArrowRight = 916.0f;  // ">" of a choice row
constexpr float kArrowWidth = 40.0f;
constexpr float kButtonsLeft = 70.0f;
constexpr float kButtonsRight = kWidth - 70.0f;
constexpr float kButtonGap = 24.0f;

// Controller inputs, in vrplay's [bindings] order. 0-5 belong to the other hand, 6-10 to the pointing hand.
enum class Input { Trigger, Grip, Primary, Secondary, Menu, Stick };
struct Control {
    const char* key; // [bindings] key
    bool pointing;   // on the pointing (dominant) hand
    Input input;
    const char* fallback; // vrplay's default
};
constexpr Control kControls[] = {
    {"left_trigger", false, Input::Trigger, "key:lalt"}, {"left_grip", false, Input::Grip, "none"},
    {"left_x", false, Input::Primary, "key:m"},          {"left_y", false, Input::Secondary, "view"},
    {"left_menu", false, Input::Menu, "vrmenu"},         {"left_stick_click", false, Input::Stick, "panel"},
    {"right_trigger", true, Input::Trigger, "mouse_left"}, {"right_grip", true, Input::Grip, "mouse_right"},
    {"right_a", true, Input::Primary, "key:i"},          {"right_b", true, Input::Secondary, "key:escape"},
    {"right_stick_click", true, Input::Stick, "recenter"},
};

// What a button can do (BG3's default PC keys: Engine.pak PlayerProfiles/Eoc/keyboard_1.json).
struct ActionName {
    const char* value;
    const char* text;
};
constexpr ActionName kActions[] = {
    {"mouse_left", "Click / select"},  {"mouse_right", "Right click / cancel"}, {"key:escape", "Back / game menu"},
    {"key:i", "Inventory"},            {"key:m", "Map"},                        {"key:j", "Journal"},
    {"key:k", "Spellbook"},            {"key:n", "Character sheet"},            {"key:lalt", "Show item labels"},
    {"key:space", "End turn / skip"},  {"key:c", "Sneak"},                      {"key:z", "Jump"},
    {"key:v", "Shove"},                {"key:f", "Switch weapon set"},          {"key:u", "Draw / sheathe"},
    {"key:tab", "Party view"},         {"key:o", "Tactical camera"},            {"view", "First / third person"},
    {"panel", "Wrist UI ↔ in front"}, {"vrmenu", "VR menu"},               {"recenter", "Recentre (hold)"},
    {"none", "Nothing"},
};

bool parse_float(const std::string& s, float& out) {
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    out = std::strtof(s.c_str(), &end);
    return end && *end == '\0';
}

bool truthy(const std::string& s) {
    const std::string v = text::to_lower(s);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

std::string capitalised(std::string s) {
    if (!s.empty()) {
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    }
    return s;
}

} // namespace

Menu::Menu() {
    // Each section is a page, shown by its tab.
    const auto section = [&](const char* label) { pages_.push_back(label); };
    const auto page = [&] { return static_cast<int>(pages_.size()) - 1; };
    const auto choice = [&](const char* label, const char* setting, std::vector<Option> options, int fallback) {
        Item item;
        item.page = page();
        item.label = label;
        item.setting = setting;
        item.options = std::move(options);
        item.fallback = fallback;
        item.current = fallback;
        items_.push_back(std::move(item));
    };
    const auto on_off = [&](const char* label, const char* setting, bool fallback) {
        choice(label, setting, {{"On", "true", false}, {"Off", "false", true}}, fallback ? 0 : 1);
    };
    const auto live = [&](const char* label, Item::Live which, std::vector<Option> options) {
        Item item;
        item.page = page();
        item.label = label;
        item.live = which;
        item.options = std::move(options);
        items_.push_back(std::move(item));
    };

    section("View");
    live("Perspective", Item::Live::View, {{"First person", "first"}, {"Third person", "third"}});
    live("Game interface", Item::Live::Ui, {{"In front of you", "world"}, {"On your wrist", "wrist"}});
    choice("Render resolution", "render.vr_resolution",
           {{"Game's own", "0", true}, {"60%", "60"}, {"70%", "70"}, {"80%", "80"}, {"90%", "90"}, {"100%", "100"},
            {"120%", "120"}},
           3);
    choice("Wrist interface size", "ui.wrist_width",
           {{"Small", "0.24"}, {"Medium", "0.3"}, {"Large", "0.36"}, {"Huge", "0.44"}}, 1);
    section("Comfort");
    choice("Turning", "vr.turn", {{"Snap", "snap"}, {"Smooth", "smooth"}}, 0);
    choice("Snap angle", "vr.snap_degrees",
           {{"15°", "15"}, {"30°", "30"}, {"45°", "45"}, {"60°", "60"}, {"90°", "90"}}, 1);
    choice("Smooth turn speed", "vr.smooth_degrees_per_second",
           {{"60°/s", "60"}, {"90°/s", "90"}, {"120°/s", "120"}, {"180°/s", "180"}, {"240°/s", "240"}},
           2);
    choice("Comfort vignette", "vr.comfort_vignette",
           {{"Off", "0", true}, {"Light", "0.25"}, {"Medium", "0.5"}, {"Strong", "0.8"}}, 2);
    on_off("Stick movement", "vr.stick_locomotion", true);
    section("Body");
    choice("Dominant hand", "vr.dominant_hand", {{"Right", "right"}, {"Left", "left"}}, 0);
    on_off("Arms follow controllers", "ik.arms", true);
    choice("Weapon angle", "ik.weapon_pitch",
           {{"−60°", "-60"}, {"−45°", "-45"}, {"−30°", "-30"}, {"−15°", "-15"},
            {"Straight", "0"}, {"+15°", "15"}, {"+30°", "30"}, {"+45°", "45"}, {"+60°", "60"}},
           4);
    on_off("Point to select", "vr.hand_pointing", true);
    on_off("Grab objects", "grab.hold", false);
    on_off("Physical melee", "combat.physical", false);
    on_off("Holsters", "holster.enabled", true);
    section("Scale");
    choice("IPD (eye distance)", "xr.ipd_mm",
           {{"Headset", "0"}, {"56 mm", "56"}, {"58 mm", "58"}, {"60 mm", "60"}, {"62 mm", "62"}, {"64 mm", "64"},
            {"66 mm", "66"}, {"68 mm", "68"}, {"70 mm", "70"}, {"72 mm", "72"}},
           0);
    choice("World scale", "xr.world_scale",
           {{"80%", "0.8"}, {"90%", "0.9"}, {"95%", "0.95"}, {"100% (true)", "1"}, {"105%", "1.05"}, {"110%", "1.1"},
            {"120%", "1.2"}},
           3);
    choice("Eye height", "vr.eye_offset",
           {{"−10 cm", "-0.1"}, {"−5 cm", "-0.05"}, {"At the eyes", "0"}, {"+5 cm", "0.05"}, {"+10 cm", "0.1"},
            {"+15 cm", "0.15"}, {"+20 cm", "0.2"}},
           2);
    section("Controls");
    std::vector<Option> actions;
    for (const ActionName& a : kActions) {
        actions.push_back({a.text, a.value, std::string(a.value) == "none"});
    }
    for (int pass = 0; pass < 2; ++pass) {
        // The pointing hand first, then the other hand; headings name the physical hand.
        const bool pointing = pass == 0;
        Item heading;
        heading.kind = Kind::Section;
        heading.page = page();
        heading.control = pointing ? -2 : -3;
        items_.push_back(std::move(heading));
        for (int c = 0; c < static_cast<int>(std::size(kControls)); ++c) {
            if (kControls[c].pointing != pointing) {
                continue;
            }
            Item item;
            item.page = page();
            item.compact = true;
            item.control = c;
            item.setting = std::string("bindings.") + kControls[c].key;
            item.options = actions;
            for (int a = 0; a < static_cast<int>(std::size(kActions)); ++a) {
                if (std::string(kActions[a].value) == kControls[c].fallback) {
                    item.fallback = item.current = a;
                }
            }
            items_.push_back(std::move(item));
        }
    }
    section("Guide");
    Item guide;
    guide.kind = Kind::Guide;
    guide.page = page();
    guide.buttons = {"‹  Previous", "Next  ›"};
    items_.push_back(std::move(guide));

    Item buttons;
    buttons.kind = Kind::Buttons;
    buttons.buttons = {"Recentre", "Calibrate height", "Close"};
    items_.push_back(std::move(buttons));
    Item tabs;
    tabs.kind = Kind::Tabs;
    tabs.buttons = pages_;
    items_.insert(items_.begin(), std::move(tabs));
    layout();
}

void Menu::layout() {
    int y = kHeaderBottom;
    for (Item& item : items_) {
        if (item.kind == Kind::Tabs) {
            item.top = kHeaderBottom;
            item.height = kTabsHeight;
            y = kHeaderBottom + kTabsHeight + 8;
        } else if (item.kind == Kind::Buttons) {
            item.top = kButtonsTop;
            item.height = kButtonsHeight;
        } else if (item.page == page_) {
            item.top = y;
            item.height = item.kind == Kind::Guide     ? kButtonsTop - 8 - y
                          : item.kind == Kind::Section ? kHeadingHeight
                          : item.compact               ? kCompactHeight
                                                       : kChoiceHeight;
            y += item.height;
        } else {
            item.top = -10000; // on another page
            item.height = 0;
        }
    }
}

void Menu::show_page(int page) {
    if (page >= 0 && page < static_cast<int>(pages_.size()) && page != page_) {
        page_ = page;
        layout();
        ++revision_;
    }
}

int Menu::page_of(const std::string& label) const {
    for (const Item& item : items_) {
        if (item.label == label || (item.control >= 0 && label_of(item) == label)) {
            return item.page;
        }
    }
    return -1;
}

void Menu::load(const Config& config) {
    for (Item& item : items_) {
        if (item.setting.empty()) {
            continue;
        }
        const auto dot = item.setting.find('.');
        const std::string raw = config.get(item.setting.substr(0, dot), item.setting.substr(dot + 1));
        item.current = item.fallback;
        if (raw.empty()) {
            continue;
        }
        float number = 0.0f;
        if (item.options.front().value == "true") {
            item.current = truthy(raw) ? 0 : 1;
        } else if (item.control < 0 && parse_float(raw, number)) {
            float best = 1e30f;
            for (int i = 0; i < static_cast<int>(item.options.size()); ++i) {
                float option = 0.0f;
                if (parse_float(item.options[i].value, option) && std::abs(option - number) < best) {
                    best = std::abs(option - number);
                    item.current = i;
                }
            }
        } else {
            for (int i = 0; i < static_cast<int>(item.options.size()); ++i) {
                if (text::to_lower(item.options[i].value) == text::to_lower(raw)) {
                    item.current = i;
                }
            }
        }
    }
    ++revision_;
}

int Menu::current_index(const Item& item) const {
    switch (item.live) {
    case Item::Live::View:
        return live_.third_person ? 1 : 0;
    case Item::Live::Ui:
        return live_.ui_wrist ? 1 : 0;
    default:
        return item.current;
    }
}

std::string Menu::setting(const std::string& name) const {
    for (const Item& item : items_) {
        if (item.setting == name && !item.options.empty()) {
            return item.options[static_cast<std::size_t>(item.current)].value;
        }
    }
    return {};
}

bool Menu::guide_shown() const {
    return page_ == static_cast<int>(pages_.size()) - 1;
}

// The physical name of a controller input for the current controller and dominant hand ("right trigger", "A").
std::string Menu::control_name(int control) const {
    const Control& c = kControls[control];
    const bool left_handed = setting("vr.dominant_hand") == "left";
    const bool physical_right = c.pointing != left_handed;
    const std::string hand = physical_right ? "right" : "left";
    const std::string& p = live_.profile;
    if (p == "hands") {
        switch (c.input) {
        case Input::Trigger:
            return hand + " fist";
        case Input::Grip:
            return hand + " fist (grab)";
        case Input::Menu:
            return hand + " palm pinch";
        default:
            return {};
        }
    }
    switch (c.input) {
    case Input::Trigger:
        return hand + " trigger";
    case Input::Grip:
        return hand + " grip";
    case Input::Stick:
        return hand + " stick click";
    case Input::Primary:
        return p == "touch" ? (physical_right ? "A" : "X") : p == "index" ? hand + " A" : hand + " primary button";
    case Input::Secondary:
        return p == "touch" ? (physical_right ? "B" : "Y") : p == "index" ? hand + " B" : hand + " secondary button";
    case Input::Menu:
        return p == "touch" && physical_right ? std::string() : hand + " menu button";
    }
    return {};
}

std::string Menu::label_of(const Item& item) const {
    const bool left_handed = setting("vr.dominant_hand") == "left";
    if (item.control == -2) {
        return std::format("Pointing hand ({})", left_handed ? "left" : "right");
    }
    if (item.control == -3) {
        return std::format("Other hand ({})", left_handed ? "right" : "left");
    }
    if (item.control >= 0) {
        std::string name = control_name(item.control);
        // On the Controls page the heading says which hand: drop it from the row.
        for (const char* prefix : {"right ", "left "}) {
            if (name.rfind(prefix, 0) == 0) {
                name = name.substr(std::strlen(prefix));
            }
        }
        return name.empty() ? std::string("(no such button)") : capitalised(name);
    }
    return item.label;
}

std::string Menu::control_for(const std::string& action) const {
    for (const Item& item : items_) {
        if (item.control >= 0 && !item.options.empty() && item.options[static_cast<std::size_t>(item.current)].value == action) {
            const std::string name = control_name(item.control);
            if (!name.empty()) {
                return name;
            }
        }
    }
    return {};
}

std::vector<Menu::Lesson> Menu::lessons() const {
    std::vector<Lesson> out;
    const bool hands = live_.profile == "hands";
    const std::string dom = setting("vr.dominant_hand") == "left" ? "left" : "right";
    const std::string other = dom == "left" ? "right" : "left";
    const auto bound = [&](const char* action, const std::string& fallback) {
        const std::string c = control_for(action);
        return c.empty() ? fallback : (c.size() <= 2 ? c : "the " + c);
    };
    const std::string click = bound("mouse_left", hands ? "closing your hand" : "the trigger");
    const std::string squeeze = hands ? "close your hand" : "squeeze the grip";

    out.push_back({"Point and select",
                   hands ? std::format("Point with your {} index finger: a laser shows where it points and moves the cursor. "
                                       "Close your hand into a fist to click: walk, talk, loot, cast, pick a line of "
                                       "dialogue.", dom)
                         : std::format("Point with your {} hand: the laser shows where it points and moves the cursor. "
                                       "Press {} to click: walk, talk, loot, attack, pick a line of dialogue.", dom, click),
                   "point"});
    if (setting("vr.stick_locomotion") != "false" && !hands) {
        out.push_back({"Walking",
                       std::format("Push the {} stick to walk where you look; a half push strolls. You can also point at "
                                   "the ground and click: your character walks there and the party follows.", other),
                       "walk"});
    } else {
        out.push_back({"Walking", std::format("Point at the ground and {} to walk there; the party follows.",
                                              hands ? "close your hand" : "click"),
                       "walk"});
    }
    const std::string recentre = bound("recenter", "the Recentre button in this menu");
    if (hands) {
        out.push_back({"Turning", "Turn your body to turn. Recentre from this menu if forward drifts.", "turn"});
    } else if (setting("vr.turn") == "smooth") {
        out.push_back({"Turning", std::format("Push the {} stick left or right to turn smoothly. Hold {} to face "
                                              "forward again.", dom, recentre),
                       "turn"});
    } else {
        out.push_back({"Turning", std::format("Flick the {} stick left or right to turn {} degrees. Hold {} to face "
                                              "forward again.", dom, setting("vr.snap_degrees"), recentre),
                       "turn"});
    }
    out.push_back({"Your wrist",
                   std::format("Raise your {} wrist like reading a watch: the game's interface sits on it (spells, "
                               "hotbar, party, portraits). Touch it with your {} {}, or point at it and {}.{}",
                               other, dom, hands ? "index finger" : "controller's tip", hands ? "close your hand" : "click",
                               control_for("panel").empty() ? std::string()
                                                            : std::format(" {} moves it in front of you and back.",
                                                                          capitalised(bound("panel", "")))),
                   "wrist"});
    if (setting("holster.enabled") != "false") {
        out.push_back({"Weapons",
                       std::format("Reach over your right shoulder or to a hip, {} and pull your hand away to draw your "
                                   "melee weapons. Over your left shoulder: your bow or crossbow. Bring your hand back to "
                                   "a holster and let go to put it away.", squeeze),
                       "holster"});
        out.push_back({"Light and bags",
                       std::format("{} at the middle of your chest: a flame lights in that hand (again to put it "
                                   "out). {} low behind your back to open the inventory.", capitalised(squeeze),
                                   capitalised(squeeze)),
                       "chest"});
    }
    if (setting("grab.hold") == "true") {
        out.push_back({"Grabbing",
                       std::format("Reach for an item and {} to pick it up; let go to put it down, or let go mid-swing "
                                   "to throw it. Aim at something far away, {}, and flick your hand back to pull it to "
                                   "you.", squeeze, squeeze),
                       "grab"});
    }
    if (setting("combat.physical") == "true") {
        out.push_back({"Fighting", "With a weapon in hand, swing it through an enemy to make your normal attack: the "
                                   "dice still decide whether it hits.",
                       "swing"});
    }
    std::string buttons;
    for (const Item& item : items_) {
        if (item.control < 0 || item.options.empty()) {
            continue;
        }
        const Option& o = item.options[static_cast<std::size_t>(item.current)];
        const std::string name = control_name(item.control);
        if (o.value == "none" || name.empty()) {
            continue;
        }
        buttons += std::format("{}{}: {}", buttons.empty() ? "" : "   ·   ", capitalised(name), o.text);
    }
    out.push_back({"Your buttons", (buttons.empty() ? std::string("No buttons are bound.") : buttons) +
                                       ". Change any of them on the Controls page.",
                   "buttons"});
    out.push_back({"This menu",
                   hands ? std::format("Turn your {} palm toward your face and pinch to open or close this menu. Every "
                                       "change applies at once and is kept for next time.", other)
                         : std::format("{} opens and closes this menu. Every change applies at once and is kept for next "
                                       "time.", capitalised(bound("vrmenu", "The menu button"))),
                   "menu"});
    return out;
}

int Menu::row_at(float v) const {
    const float y = v * static_cast<float>(kHeight);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Item& item = items_[i];
        if (item.kind != Kind::Section && item.height > 0 && y >= static_cast<float>(item.top) &&
            y < static_cast<float>(item.top + item.height)) {
            return i;
        }
    }
    return -1;
}

int Menu::button_at(int row, float u) const {
    if (row < 0 || row >= static_cast<int>(items_.size()) ||
        (items_[row].kind != Kind::Buttons && items_[row].kind != Kind::Tabs && items_[row].kind != Kind::Guide)) {
        return -1;
    }
    const int count = static_cast<int>(items_[row].buttons.size());
    const float gap = items_[row].kind == Kind::Tabs ? kTabGap : kButtonGap;
    const float x = u * static_cast<float>(kWidth);
    const float width = (kButtonsRight - kButtonsLeft - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
    for (int b = 0; b < count; ++b) {
        const float left = kButtonsLeft + static_cast<float>(b) * (width + gap);
        if (x >= left && x < left + width) {
            return b;
        }
    }
    return -1;
}

void Menu::step(int index, int direction, Actions& actions) {
    Item& item = items_[index];
    if (item.kind == Kind::Guide) {
        const int count = std::max(1, static_cast<int>(lessons().size()));
        lesson_ = (lesson_ + direction + count) % count;
        anim_time_ = 0.0f;
        ++revision_;
        return;
    }
    if (item.kind != Kind::Choice || item.options.empty()) {
        return;
    }
    if (item.live == Item::Live::View) {
        actions.toggle_view = true;
        return;
    }
    if (item.live == Item::Live::Ui) {
        actions.toggle_ui = true;
        return;
    }
    const int count = static_cast<int>(item.options.size());
    item.current = (item.current + direction + count) % count;
    actions.settings.emplace_back(item.setting, item.options[item.current].value);
    if (item.setting == "vr.eye_offset") {
        actions.recalibrate = true;
    }
    ++revision_;
}

Actions Menu::update(std::optional<std::array<float, 2>> pointer, bool click, float stick_x, const Live& live) {
    Actions actions;
    if (live.third_person != live_.third_person || live.ui_wrist != live_.ui_wrist || live.profile != live_.profile) {
        live_ = live;
        ++revision_;
    }
    const int row = pointer ? row_at((*pointer)[1]) : -1;
    int button = pointer ? button_at(row, (*pointer)[0]) : -1;
    if (row >= 0 && items_[row].kind == Kind::Guide) {
        // Only the navigation strip at the bottom of the Guide is a button.
        const float y = (*pointer)[1] * static_cast<float>(kHeight);
        if (y < static_cast<float>(items_[row].top + items_[row].height - kGuideNavHeight)) {
            button = -1;
        }
    }
    if (row != hovered_ || button != hovered_button_) {
        const Kind kind = row >= 0 ? items_[row].kind : Kind::Section;
        const bool buttons = kind == Kind::Buttons || kind == Kind::Tabs || kind == Kind::Guide;
        if (row >= 0 && (!buttons || button >= 0)) {
            actions.haptic = 0.08f;
        }
        hovered_ = row;
        hovered_button_ = button;
        ++revision_;
    }
    if (click && row >= 0) {
        const Item& item = items_[row];
        if (item.kind == Kind::Tabs) {
            if (button >= 0) {
                show_page(button);
                actions.haptic = 0.25f;
            }
        } else if (item.kind == Kind::Guide) {
            if (button >= 0) {
                step(row, button == 0 ? -1 : 1, actions);
                actions.haptic = 0.25f;
            }
        } else if (item.kind == Kind::Buttons) {
            switch (button) {
            case 0:
                actions.recenter = true;
                break;
            case 1:
                actions.recalibrate = true;
                break;
            case 2:
                actions.close = true;
                break;
            default:
                break;
            }
            if (button >= 0) {
                actions.haptic = 0.25f;
            }
        } else if (item.kind == Kind::Choice) {
            const float x = (*pointer)[0] * static_cast<float>(kWidth);
            const bool back = x >= kArrowLeft - 20.0f && x < kArrowLeft + kArrowWidth + 20.0f;
            step(row, back ? -1 : 1, actions);
            actions.haptic = 0.25f;
        }
    }
    // The stick changes the row under the laser (on the tabs: the page; on the Guide: the lesson).
    if (row >= 0 && (items_[row].kind == Kind::Choice || items_[row].kind == Kind::Tabs || items_[row].kind == Kind::Guide)) {
        if (stick_armed_ && std::abs(stick_x) > 0.7f) {
            if (items_[row].kind == Kind::Tabs) {
                const int count = static_cast<int>(pages_.size());
                show_page((page_ + (stick_x > 0.0f ? 1 : -1) + count) % count);
            } else {
                step(row, stick_x > 0.0f ? 1 : -1, actions);
            }
            actions.haptic = 0.2f;
            stick_armed_ = false;
        }
    }
    if (std::abs(stick_x) < 0.3f) {
        stick_armed_ = true;
    }
    return actions;
}

void Menu::show_lesson(int lesson, float anim_time) {
    lesson_ = lesson;
    anim_time_ = anim_time;
    ++revision_;
}

void Menu::tick(float seconds) {
    if (!guide_shown()) {
        return;
    }
    anim_time_ += seconds;
    anim_since_draw_ += seconds;
    if (anim_since_draw_ >= 1.0f / 20.0f) { // the animation redraws at 20 Hz
        anim_since_draw_ = 0.0f;
        ++revision_;
    }
}

Snapshot Menu::snapshot() const {
    Snapshot s;
    const std::vector<Lesson> lessons_now = lessons();
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Item& item = items_[i];
        if (item.height <= 0) {
            continue; // on another page
        }
        Snapshot::Row row;
        row.kind = item.kind;
        row.label = label_of(item);
        row.top = item.top;
        row.height = item.height;
        row.compact = item.compact;
        row.hovered = i == hovered_;
        if (item.kind == Kind::Choice && !item.options.empty()) {
            const Option& option = item.options[static_cast<std::size_t>(current_index(item))];
            row.value = option.text;
            row.off = option.off;
        }
        if (item.kind == Kind::Buttons || item.kind == Kind::Tabs || item.kind == Kind::Guide) {
            row.buttons = item.buttons;
            row.hovered_button = i == hovered_ ? hovered_button_ : -1;
            row.selected_button = item.kind == Kind::Tabs ? page_ : -1;
        }
        if (item.kind == Kind::Guide && !lessons_now.empty()) {
            const int n = static_cast<int>(lessons_now.size());
            const Lesson& lesson = lessons_now[static_cast<std::size_t>(((lesson_ % n) + n) % n)];
            row.label = lesson.title;
            row.value = lesson.text;
            row.anim = lesson.anim;
            row.anim_time = anim_time_;
            row.lesson = ((lesson_ % n) + n) % n;
            row.lessons = n;
            row.pointing_hand = setting("vr.dominant_hand") == "left" ? "left" : "right";
        }
        s.rows.push_back(std::move(row));
    }
    s.footer = "Point and pull the trigger  ·  stick ◂ ▸ changes a row  ·  menu button closes";
    return s;
}

std::string Menu::value_of(const std::string& label) const {
    for (const Item& item : items_) {
        if ((item.label == label || (item.control >= 0 && label_of(item) == label)) && item.kind == Kind::Choice &&
            !item.options.empty()) {
            return item.options[static_cast<std::size_t>(current_index(item))].text;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------------------------
// Renderer

namespace {

template <typename T> void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

D2D1_COLOR_F rgb(std::uint32_t hex, float alpha = 1.0f) {
    return {static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f, alpha};
}

// The palette: dark leather and old gold.
constexpr std::uint32_t kGold = 0xD9B46C;
constexpr std::uint32_t kGoldBright = 0xF2D58F;
constexpr std::uint32_t kGoldDim = 0x8C6D3C;
constexpr std::uint32_t kText = 0xECE2CC;
constexpr std::uint32_t kTextDim = 0x9E9078;
constexpr std::uint32_t kSection = 0xB8975A;

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

} // namespace

struct Renderer::Impl {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Snapshot> pending;
    bool quit = false;
    std::vector<std::uint32_t> done;
    std::uint64_t done_seq = 0;
    std::uint64_t taken_seq = 0;
    int init = 0; // 0 waiting, 1 ready, -1 failed

    // Owned by the thread.
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* dwrite = nullptr;
    ID2D1DCRenderTarget* target = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1LinearGradientBrush* background = nullptr;
    IDWriteTextFormat* title = nullptr;
    IDWriteTextFormat* subtitle = nullptr;
    IDWriteTextFormat* section = nullptr;
    IDWriteTextFormat* label = nullptr;
    IDWriteTextFormat* value = nullptr;
    IDWriteTextFormat* arrow = nullptr;
    IDWriteTextFormat* button = nullptr;
    IDWriteTextFormat* footer = nullptr;
    IDWriteTextFormat* tab = nullptr;
    IDWriteTextFormat* small_label = nullptr;
    IDWriteTextFormat* small_value = nullptr;
    IDWriteTextFormat* guide_title = nullptr;
    IDWriteTextFormat* guide_text = nullptr;
    IDWriteTextFormat* guide_count = nullptr;
    IDWriteTextFormat* glyph = nullptr;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    void* bits = nullptr;

    bool create(std::string& error);
    void destroy();
    void draw(const Snapshot& s);
    void run();
    void draw_guide(const Snapshot::Row& row, float top, float bottom);
    void draw_anim(const std::string& anim, float t, const D2D1_RECT_F& area, bool mirror);

    IDWriteTextFormat* format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                              DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        IDWriteTextFormat* f = nullptr;
        if (FAILED(dwrite->CreateTextFormat(family, nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f))) {
            return nullptr;
        }
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return f;
    }

    void text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& rect, D2D1_COLOR_F color) {
        brush->SetColor(color);
        target->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, rect, brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // Spaced capitals (the section headings); returns the right edge of the text.
    float spaced(const std::wstring& s, IDWriteTextFormat* f, float x, float top, float bottom, D2D1_COLOR_F color) {
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 900.0f, bottom - top, &layout))) {
            return x;
        }
        IDWriteTextLayout1* layout1 = nullptr;
        if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&layout1)))) {
            layout1->SetCharacterSpacing(0.0f, 4.0f, 0.0f, {0, static_cast<UINT32>(s.size())});
            layout1->Release();
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        brush->SetColor(color);
        target->DrawTextLayout({x, top}, layout, brush);
        layout->Release();
        return x + metrics.widthIncludingTrailingWhitespace;
    }

    void diamond(float x, float y, float r, D2D1_COLOR_F color) {
        ID2D1PathGeometry* path = nullptr;
        if (FAILED(factory->CreatePathGeometry(&path))) {
            return;
        }
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(path->Open(&sink))) {
            sink->BeginFigure({x, y - r}, D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F points[] = {{x + r, y}, {x, y + r}, {x - r, y}};
            sink->AddLines(points, 3);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            brush->SetColor(color);
            target->FillGeometry(path, brush);
        }
        path->Release();
    }

    void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F color, float width) {
        brush->SetColor(color);
        target->DrawLine({x0, y0}, {x1, y1}, brush, width);
    }
};

bool Renderer::Impl::create(std::string& error) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory))) {
        error = "D2D1CreateFactory failed";
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite)))) {
        error = "DWriteCreateFactory failed";
        return false;
    }
    D2D1_RENDER_TARGET_PROPERTIES props{};
    props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    if (FAILED(factory->CreateDCRenderTarget(&props, &target))) {
        error = "CreateDCRenderTarget failed";
        return false;
    }
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // ClearType needs an opaque target
    dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kWidth;
    info.bmiHeader.biHeight = -kHeight; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bitmap || !bits) {
        error = "CreateDIBSection failed";
        return false;
    }
    old_bitmap = SelectObject(dc, bitmap);
    const RECT rect{0, 0, kWidth, kHeight};
    if (FAILED(target->BindDC(dc, &rect))) {
        error = "BindDC failed";
        return false;
    }
    if (FAILED(target->CreateSolidColorBrush(rgb(kText), &brush))) {
        error = "CreateSolidColorBrush failed";
        return false;
    }
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, rgb(0x2B2016, 0.97f)}, {0.45f, rgb(0x1A130D, 0.97f)}, {1.0f, rgb(0x0C0906, 0.97f)}};
    ID2D1GradientStopCollection* collection = nullptr;
    if (FAILED(target->CreateGradientStopCollection(stops, 3, &collection))) {
        error = "CreateGradientStopCollection failed";
        return false;
    }
    const HRESULT hr = target->CreateLinearGradientBrush({{0.0f, 0.0f}, {0.0f, static_cast<float>(kHeight)}}, collection, &background);
    collection->Release();
    if (FAILED(hr)) {
        error = "CreateLinearGradientBrush failed";
        return false;
    }
    // Palatino for the old-book headings (DirectWrite falls back to a system font if it is missing), Segoe UI for
    // what must be read at a glance.
    title = format(L"Palatino Linotype", 60.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    subtitle = format(L"Palatino Linotype", 23.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC, DWRITE_TEXT_ALIGNMENT_CENTER);
    section = format(L"Palatino Linotype", 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    label = format(L"Segoe UI", 29.0f, DWRITE_FONT_WEIGHT_NORMAL);
    value = format(L"Segoe UI", 28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    arrow = format(L"Palatino Linotype", 46.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    button = format(L"Palatino Linotype", 28.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    footer = format(L"Segoe UI", 19.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    tab = format(L"Palatino Linotype", 21.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    small_label = format(L"Segoe UI", 24.0f, DWRITE_FONT_WEIGHT_NORMAL);
    small_value = format(L"Segoe UI", 23.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    guide_title = format(L"Palatino Linotype", 34.0f, DWRITE_FONT_WEIGHT_BOLD);
    guide_text = format(L"Segoe UI", 24.0f, DWRITE_FONT_WEIGHT_NORMAL);
    guide_count = format(L"Segoe UI", 21.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    glyph = format(L"Segoe UI", 18.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (guide_text) {
        guide_text->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        guide_text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        guide_text->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 33.0f, 26.0f);
    }
    if (!title || !subtitle || !section || !label || !value || !arrow || !button || !footer || !tab || !small_label ||
        !small_value || !guide_title || !guide_text || !guide_count || !glyph) {
        error = "CreateTextFormat failed";
        return false;
    }
    return true;
}

void Renderer::Impl::destroy() {
    for (IDWriteTextFormat** f : {&title, &subtitle, &section, &label, &value, &arrow, &button, &footer, &tab,
                                  &small_label, &small_value, &guide_title, &guide_text, &guide_count, &glyph}) {
        release(*f);
    }
    release(background);
    release(brush);
    release(target);
    release(dwrite);
    release(factory);
    if (dc) {
        if (old_bitmap) {
            SelectObject(dc, old_bitmap);
        }
        DeleteDC(dc);
        dc = nullptr;
    }
    if (bitmap) {
        DeleteObject(bitmap);
        bitmap = nullptr;
    }
}

void Renderer::Impl::draw(const Snapshot& s) {
    const float w = static_cast<float>(kWidth);
    const float h = static_cast<float>(kHeight);
    target->BeginDraw();
    target->SetTransform(D2D1::IdentityMatrix());
    target->Clear(D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.0f});

    // The frame: leather, a gold rule, a thin inner rule with diamonds at its corners.
    const D2D1_ROUNDED_RECT outer{{8.0f, 8.0f, w - 8.0f, h - 8.0f}, 30.0f, 30.0f};
    target->FillRoundedRectangle(outer, background);
    brush->SetColor(rgb(kGold));
    target->DrawRoundedRectangle(outer, brush, 3.0f);
    brush->SetColor(rgb(kGoldDim, 0.8f));
    target->DrawRoundedRectangle({{24.0f, 24.0f, w - 24.0f, h - 24.0f}, 20.0f, 20.0f}, brush, 1.5f);
    for (const auto& [x, y] : {std::pair{40.0f, 40.0f}, {w - 40.0f, 40.0f}, {40.0f, h - 40.0f}, {w - 40.0f, h - 40.0f}}) {
        diamond(x, y, 8.0f, rgb(kGold));
    }

    // Title.
    text(L"VR SETTINGS", title, {0.0f, 46.0f, w, 122.0f}, rgb(kGoldBright));
    text(L"Baldur’s Gate 3  ·  Virtual Reality", subtitle, {0.0f, 116.0f, w, 150.0f}, rgb(kTextDim));
    line(130.0f, 170.0f, w * 0.5f - 22.0f, 170.0f, rgb(kGoldDim), 1.5f);
    line(w * 0.5f + 22.0f, 170.0f, w - 130.0f, 170.0f, rgb(kGoldDim), 1.5f);
    diamond(w * 0.5f, 170.0f, 9.0f, rgb(kGold));
    diamond(124.0f, 170.0f, 3.5f, rgb(kGoldDim));
    diamond(w - 124.0f, 170.0f, 3.5f, rgb(kGoldDim));

    for (const Snapshot::Row& row : s.rows) {
        const float top = static_cast<float>(row.top);
        const float bottom = top + static_cast<float>(row.height);
        const float mid = (top + bottom) * 0.5f;
        switch (row.kind) {
        case Kind::Section: {
            std::wstring caption = widen(row.label);
            std::transform(caption.begin(), caption.end(), caption.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
            const float right = spaced(caption, section, 64.0f, top + 8.0f, bottom, rgb(kSection));
            line(right + 14.0f, mid + 4.0f, w - 64.0f, mid + 4.0f, rgb(kGoldDim, 0.55f), 1.0f);
            break;
        }
        case Kind::Choice: {
            if (row.hovered) {
                brush->SetColor(rgb(kGold, 0.14f));
                target->FillRoundedRectangle({{48.0f, top + 3.0f, w - 48.0f, bottom - 3.0f}, 10.0f, 10.0f}, brush);
                brush->SetColor(rgb(kGold));
                target->FillRectangle({48.0f, top + 10.0f, 53.0f, bottom - 10.0f}, brush);
            } else {
                line(78.0f, bottom, w - 78.0f, bottom, rgb(kGoldDim, 0.18f), 1.0f);
            }
            text(widen(row.label), row.compact ? small_label : label, {78.0f, top, kArrowLeft - 10.0f, bottom},
                 rgb(row.hovered ? kGoldBright : kText));
            const D2D1_COLOR_F arrows = rgb(row.hovered ? kGold : kGoldDim);
            text(L"\u2039", arrow, {kArrowLeft, top - 4.0f, kArrowLeft + kArrowWidth, bottom - 4.0f}, arrows);
            text(L"\u203A", arrow, {kArrowRight, top - 4.0f, kArrowRight + kArrowWidth, bottom - 4.0f}, arrows);
            text(widen(row.value), row.compact ? small_value : value, {kArrowLeft + kArrowWidth, top, kArrowRight, bottom},
                 row.off ? rgb(kTextDim) : rgb(kGoldBright));
            break;
        }
        case Kind::Tabs: {
            const int count = static_cast<int>(row.buttons.size());
            const float bw = (kButtonsRight - kButtonsLeft - kTabGap * static_cast<float>(count - 1)) / static_cast<float>(count);
            for (int b = 0; b < count; ++b) {
                const float left = kButtonsLeft + static_cast<float>(b) * (bw + kTabGap);
                const D2D1_RECT_F r{left, top + 10.0f, left + bw, bottom - 12.0f};
                const bool selected = row.selected_button == b;
                const bool hot = row.hovered_button == b;
                if (selected || hot) {
                    brush->SetColor(rgb(kGold, selected ? 0.20f : 0.10f));
                    target->FillRoundedRectangle({r, 10.0f, 10.0f}, brush);
                }
                if (selected) {
                    brush->SetColor(rgb(kGoldBright));
                    target->FillRectangle({left + 18.0f, r.bottom - 4.0f, left + bw - 18.0f, r.bottom - 1.0f}, brush);
                }
                std::wstring caption = widen(row.buttons[static_cast<std::size_t>(b)]);
                std::transform(caption.begin(), caption.end(), caption.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
                text(caption, tab, r, rgb(selected ? kGoldBright : hot ? kGold : kSection));
            }
            line(64.0f, bottom, w - 64.0f, bottom, rgb(kGoldDim, 0.55f), 1.0f);
            break;
        }
        case Kind::Guide:
            draw_guide(row, top, bottom);
            break;
        case Kind::Buttons: {
            const int count = static_cast<int>(row.buttons.size());
            const float bw = (kButtonsRight - kButtonsLeft - kButtonGap * static_cast<float>(count - 1)) / static_cast<float>(count);
            for (int b = 0; b < count; ++b) {
                const float left = kButtonsLeft + static_cast<float>(b) * (bw + kButtonGap);
                const D2D1_ROUNDED_RECT r{{left, top + 26.0f, left + bw, bottom - 12.0f}, 12.0f, 12.0f};
                const bool hot = row.hovered_button == b;
                brush->SetColor(hot ? rgb(kGold, 0.24f) : rgb(0x1F1710, 0.9f));
                target->FillRoundedRectangle(r, brush);
                brush->SetColor(rgb(hot ? kGoldBright : kGoldDim));
                target->DrawRoundedRectangle(r, brush, hot ? 2.5f : 1.5f);
                text(widen(row.buttons[static_cast<std::size_t>(b)]), button, r.rect, rgb(hot ? kGoldBright : kGold));
            }
            break;
        }
        }
    }

    line(160.0f, static_cast<float>(kFooterTop) - 6.0f, w - 160.0f, static_cast<float>(kFooterTop) - 6.0f, rgb(kGoldDim, 0.4f), 1.0f);
    text(widen(s.footer), footer, {0.0f, static_cast<float>(kFooterTop), w, static_cast<float>(kFooterTop) + 34.0f}, rgb(kTextDim));
    const HRESULT hr = target->EndDraw();
    if (FAILED(hr)) {
        log::warn("vr menu: drawing failed (0x{:08X})", static_cast<unsigned>(hr));
        return;
    }
    GdiFlush();
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(kWidth) * kHeight);
    std::memcpy(pixels.data(), bits, pixels.size() * sizeof(std::uint32_t));
    std::scoped_lock lock(mutex);
    done = std::move(pixels);
    ++done_seq;
}

// The Guide: lesson title and number, an animation in a framed box, the text, and Previous / Next.
void Renderer::Impl::draw_guide(const Snapshot::Row& row, float top, float bottom) {
    const float w = static_cast<float>(kWidth);
    text(widen(row.label), guide_title, {78.0f, top + 2.0f, w - 220.0f, top + 50.0f}, rgb(kGoldBright));
    text(widen(std::format("{} / {}", row.lesson + 1, row.lessons)), guide_count, {w - 260.0f, top + 2.0f, w - 78.0f, top + 50.0f},
         rgb(kTextDim));
    const D2D1_RECT_F area{w * 0.5f - 250.0f, top + 58.0f, w * 0.5f + 250.0f, top + 298.0f};
    brush->SetColor(rgb(0x0E0A07, 0.85f));
    target->FillRoundedRectangle({area, 14.0f, 14.0f}, brush);
    brush->SetColor(rgb(kGoldDim, 0.7f));
    target->DrawRoundedRectangle({area, 14.0f, 14.0f}, brush, 1.5f);
    draw_anim(row.anim, row.anim_time, area, row.pointing_hand == "left");
    const float nav_top = bottom - static_cast<float>(kGuideNavHeight);
    text(widen(row.value), guide_text, {78.0f, top + 312.0f, w - 78.0f, nav_top - 4.0f}, rgb(kText));
    const int count = static_cast<int>(row.buttons.size());
    const float bw = (kButtonsRight - kButtonsLeft - kButtonGap * static_cast<float>(count - 1)) / static_cast<float>(count);
    for (int b = 0; b < count; ++b) {
        const float left = kButtonsLeft + static_cast<float>(b) * (bw + kButtonGap);
        const D2D1_ROUNDED_RECT r{{left, nav_top + 8.0f, left + bw, bottom - 6.0f}, 10.0f, 10.0f};
        const bool hot = row.hovered_button == b;
        brush->SetColor(hot ? rgb(kGold, 0.22f) : rgb(0x1F1710, 0.9f));
        target->FillRoundedRectangle(r, brush);
        brush->SetColor(rgb(hot ? kGoldBright : kGoldDim));
        target->DrawRoundedRectangle(r, brush, hot ? 2.0f : 1.2f);
        text(widen(row.buttons[static_cast<std::size_t>(b)]), tab, r.rect, rgb(hot ? kGoldBright : kGold));
    }
}

// Gold-line animations of a stylised player (or hands and controllers), looping every few seconds. Coordinates are in
// the box's unit square (x right, y down); `mirror` swaps left and right for a left-handed player.
void Renderer::Impl::draw_anim(const std::string& anim, float t, const D2D1_RECT_F& area, bool mirror) {
    const float aw = area.right - area.left, ah = area.bottom - area.top;
    const auto P = [&](float x, float y) {
        return D2D1_POINT_2F{area.left + (mirror ? 1.0f - x : x) * aw, area.top + y * ah};
    };
    const auto seg = [&](float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width) {
        brush->SetColor(c);
        target->DrawLine(P(x0, y0), P(x1, y1), brush, width, nullptr);
    };
    const auto ring = [&](float x, float y, float r, D2D1_COLOR_F c, float width) {
        brush->SetColor(c);
        target->DrawEllipse({P(x, y), r * ah, r * ah}, brush, width);
    };
    const auto dot = [&](float x, float y, float r, D2D1_COLOR_F c) {
        brush->SetColor(c);
        target->FillEllipse({P(x, y), r * ah, r * ah}, brush);
    };
    const auto label_at = [&](float x, float y, const wchar_t* s, D2D1_COLOR_F c) {
        const D2D1_POINT_2F p = P(x, y);
        text(s, glyph, {p.x - 60.0f, p.y - 14.0f, p.x + 60.0f, p.y + 14.0f}, c);
    };
    const auto lerp = [](float a, float b, float k) { return a + (b - a) * k; };
    const auto ease = [](float k) { k = std::clamp(k, 0.0f, 1.0f); return k * k * (3.0f - 2.0f * k); };
    const D2D1_COLOR_F gold = rgb(kGold), bright = rgb(kGoldBright), dim = rgb(kGoldDim, 0.8f), faint = rgb(kGoldDim, 0.35f);
    const float period = 3.0f;
    const float p = std::fmod(std::max(t, 0.0f), period) / period; // 0..1 through the loop

    // A front-facing upper body, the pointing (right) arm drawn by each animation.
    const auto body = [&](float cx) {
        ring(cx, 0.22f, 0.085f, gold, 2.5f);                      // head
        seg(cx, 0.31f, cx, 0.36f, gold, 2.5f);                   // neck
        seg(cx - 0.13f, 0.38f, cx + 0.13f, 0.38f, gold, 3.0f);   // shoulders
        seg(cx - 0.13f, 0.38f, cx - 0.09f, 0.92f, dim, 2.0f);    // torso sides
        seg(cx + 0.13f, 0.38f, cx + 0.09f, 0.92f, dim, 2.0f);
        seg(cx - 0.09f, 0.92f, cx + 0.09f, 0.92f, dim, 2.0f);
    };
    const auto arm = [&](float sx, float sy, float hx, float hy, D2D1_COLOR_F c) {
        const float ex = (sx + hx) * 0.5f + (hx > sx ? 0.03f : -0.03f), ey = (sy + hy) * 0.5f + 0.08f;
        seg(sx, sy, ex, ey, c, 3.0f);
        seg(ex, ey, hx, hy, c, 3.0f);
    };
    const auto controller = [&](float x, float y, bool pressed) {
        ring(x, y, 0.035f, pressed ? bright : gold, pressed ? 3.0f : 2.0f);
        if (pressed) {
            dot(x, y, 0.02f, rgb(kGold, 0.6f));
        }
    };

    if (anim == "point") {
        body(0.22f);
        const float aim = std::sin(p * 6.2831853f) * 0.08f;
        const float hx = 0.48f, hy = 0.48f + aim * 0.3f;
        arm(0.35f, 0.38f, hx, hy, gold);
        const bool click = p > 0.55f && p < 0.68f;
        controller(hx, hy, click);
        const float tx = 0.88f, ty = 0.42f + aim;
        seg(hx + 0.03f, hy, tx, ty, rgb(kGoldBright, 0.7f), 2.0f); // the laser
        ring(tx, ty, click ? 0.06f + (p - 0.55f) * 0.5f : 0.04f, click ? bright : gold, 2.0f);
        if (click) {
            label_at(0.62f, 0.85f, L"CLICK", bright);
        }
    } else if (anim == "walk") {
        // The walking stick, and the ground moving towards you.
        ring(0.22f, 0.55f, 0.13f, gold, 2.0f);
        const float push = ease(p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f);
        dot(0.22f, 0.55f - push * 0.09f, 0.05f, bright);
        for (int i = 0; i < 6; ++i) {
            const float y = std::fmod(0.15f + static_cast<float>(i) * 0.16f + p * 0.16f * 3.0f, 0.96f) + 0.02f;
            const float half = 0.06f + y * 0.22f;
            seg(0.72f - half, y, 0.72f + half, y, rgb(kGoldDim, 0.25f + y * 0.6f), 1.5f);
        }
        seg(0.72f, 0.80f, 0.72f, 0.30f, bright, 2.5f);
        seg(0.72f, 0.30f, 0.69f, 0.36f, bright, 2.5f);
        seg(0.72f, 0.30f, 0.75f, 0.36f, bright, 2.5f);
    } else if (anim == "turn") {
        // Seen from above: the head turning in steps.
        const float steps = std::floor(p * 4.0f);
        const float angle = (steps * 30.0f + 30.0f * ease((p * 4.0f - steps) * 3.0f)) * 3.14159265f / 180.0f;
        const float cx = 0.5f, cy = 0.55f;
        ring(cx, cy, 0.16f, gold, 2.5f);
        const float nx = cx + std::sin(angle) * 0.22f * ah / aw, ny = cy - std::cos(angle) * 0.22f;
        seg(cx, cy, nx, ny, bright, 3.0f);
        for (int i = 0; i < 12; ++i) {
            const float a = static_cast<float>(i) * 0.5235988f;
            dot(cx + std::sin(a) * 0.32f * ah / aw, cy - std::cos(a) * 0.32f, 0.008f, faint);
        }
        ring(0.85f, 0.75f, 0.06f, gold, 2.0f); // the turning stick
        dot(0.85f + 0.04f * ah / aw * (std::fmod(p * 4.0f, 1.0f) < 0.3f ? 1.0f : 0.0f), 0.75f, 0.03f, bright);
    } else if (anim == "wrist") {
        // The other forearm raised across the body with the panel on it; the pointing finger touches it.
        body(0.5f);
        seg(0.37f, 0.38f, 0.30f, 0.62f, gold, 3.0f);       // the wrist arm: upper arm
        seg(0.30f, 0.62f, 0.55f, 0.55f, gold, 3.0f);       // forearm raised across
        brush->SetColor(rgb(kGold, 0.18f));
        target->FillRectangle({P(0.40f, 0.44f).x, P(0.40f, 0.44f).y, P(0.53f, 0.54f).x, P(0.53f, 0.54f).y}, brush);
        brush->SetColor(bright);
        target->DrawRectangle({P(0.40f, 0.44f).x, P(0.40f, 0.44f).y, P(0.53f, 0.54f).x, P(0.53f, 0.54f).y}, brush, 2.0f);
        const float k = ease(p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f);
        const float fx = lerp(0.80f, 0.47f, k), fy = lerp(0.60f, 0.49f, k);
        arm(0.63f, 0.38f, fx, fy, gold);
        dot(fx, fy, 0.018f, bright);
        if (k > 0.95f) {
            ring(0.47f, 0.49f, 0.05f, bright, 2.0f);
        }
    } else if (anim == "holster") {
        // Reach over the shoulder, close the hand, pull the weapon out in front.
        body(0.42f);
        float hx, hy;
        float blade = 0.0f;
        bool closed = false;
        if (p < 0.35f) {
            const float k = ease(p / 0.35f);
            hx = lerp(0.70f, 0.58f, k);
            hy = lerp(0.70f, 0.24f, k);
        } else if (p < 0.5f) {
            hx = 0.58f;
            hy = 0.24f;
            closed = true;
        } else {
            const float k = ease((p - 0.5f) / 0.4f);
            hx = lerp(0.58f, 0.82f, k);
            hy = lerp(0.24f, 0.55f, k);
            closed = true;
            blade = k;
        }
        seg(0.50f, 0.20f, 0.66f, 0.70f, faint, 6.0f); // the sheathed weapon on the back
        arm(0.55f, 0.38f, hx, hy, gold);
        if (closed) {
            dot(hx, hy, 0.03f, bright);
        } else {
            ring(hx, hy, 0.03f, gold, 2.0f);
        }
        if (blade > 0.0f) {
            seg(hx, hy, hx + 0.14f * blade, hy - 0.30f * blade, bright, 4.0f);
        }
        label_at(0.84f, 0.15f, closed ? L"GRIP" : L"", bright);
    } else if (anim == "chest") {
        body(0.5f);
        const float k = ease(p < 0.4f ? p / 0.4f : 1.0f);
        const float hx = lerp(0.78f, 0.53f, k), hy = lerp(0.75f, 0.52f, k);
        arm(0.63f, 0.38f, hx, hy, gold);
        if (p >= 0.4f) {
            dot(hx, hy, 0.03f, bright);
            const float flicker = 0.03f * std::sin(t * 23.0f) + 0.02f * std::sin(t * 37.0f);
            brush->SetColor(rgb(0xFFB040, 0.85f));
            target->FillEllipse({P(hx, hy - 0.10f), 0.035f * ah, (0.08f + flicker) * ah}, brush);
            brush->SetColor(rgb(0xFFE8A0, 0.9f));
            target->FillEllipse({P(hx, hy - 0.08f), 0.015f * ah, (0.04f + flicker * 0.5f) * ah}, brush);
        } else {
            ring(hx, hy, 0.03f, gold, 2.0f);
        }
    } else if (anim == "grab") {
        const float k = p < 0.4f ? ease(p / 0.4f) : 1.0f;
        const float lift = p > 0.55f ? ease((p - 0.55f) / 0.35f) : 0.0f;
        const float hx = lerp(0.25f, 0.62f, k), hy = lerp(0.40f, 0.62f, k) - lift * 0.3f;
        const bool closed = p > 0.45f;
        brush->SetColor(rgb(kGold, 0.25f));
        const float bx = 0.62f, by = 0.70f - lift * 0.3f;
        target->FillRectangle({P(bx - 0.05f, by - 0.05f).x, P(bx - 0.05f, by - 0.05f).y, P(bx + 0.05f, by + 0.05f).x,
                               P(bx + 0.05f, by + 0.05f).y},
                              brush);
        seg(0.10f, 0.20f, hx, hy, gold, 3.0f);
        if (closed) {
            dot(hx, hy, 0.035f, bright);
        } else {
            ring(hx, hy, 0.035f, gold, 2.0f);
        }
        seg(0.05f, 0.80f, 0.95f, 0.80f, faint, 1.5f); // the floor
    } else if (anim == "swing") {
        body(0.30f);
        const float k = ease(std::fmod(p * 1.5f, 1.0f) * 1.4f);
        const float a = lerp(-1.2f, 1.0f, k);
        const float hx = 0.50f + std::cos(a) * 0.18f, hy = 0.45f + std::sin(a) * 0.18f;
        arm(0.43f, 0.38f, hx, hy, gold);
        seg(hx, hy, hx + std::cos(a - 1.2f) * 0.25f, hy + std::sin(a - 1.2f) * 0.25f, bright, 4.0f);
        for (int i = 1; i < 5; ++i) {
            const float b = a - static_cast<float>(i) * 0.15f;
            const float x = 0.50f + std::cos(b) * 0.18f, y = 0.45f + std::sin(b) * 0.18f;
            seg(x, y, x + std::cos(b - 1.2f) * 0.25f, y + std::sin(b - 1.2f) * 0.25f, rgb(kGold, 0.25f - 0.05f * static_cast<float>(i)), 3.0f);
        }
        ring(0.85f, 0.40f, 0.08f, faint, 2.0f); // a target
    } else if (anim == "buttons") {
        // Both controllers, buttons lighting in turn.
        const int lit = static_cast<int>(p * 6.0f);
        for (int h = 0; h < 2; ++h) {
            const float cx = h ? 0.70f : 0.30f;
            ring(cx, 0.50f, 0.22f, gold, 2.5f);
            const float bx = cx + (h ? 0.05f : -0.05f) * ah / aw;
            dot(bx, 0.42f, 0.035f, lit == h * 3 ? bright : dim);
            dot(bx + (h ? 0.06f : -0.06f) * ah / aw, 0.55f, 0.035f, lit == h * 3 + 1 ? bright : dim);
            ring(cx - (h ? 0.07f : -0.07f) * ah / aw, 0.52f, 0.05f, lit == h * 3 + 2 ? bright : dim, 2.0f);
        }
    } else if (anim == "menu") {
        ring(0.30f, 0.55f, 0.20f, gold, 2.5f);
        const bool press = p > 0.2f && p < 0.35f;
        dot(0.26f, 0.45f, 0.03f, press ? bright : dim);
        const float k = ease((p - 0.3f) / 0.3f);
        if (k > 0.0f) {
            brush->SetColor(rgb(kGold, 0.15f * k));
            const D2D1_POINT_2F a = P(0.55f, 0.55f - 0.30f * k), b = P(0.90f, 0.55f + 0.30f * k);
            target->FillRoundedRectangle({{std::min(a.x, b.x), a.y, std::max(a.x, b.x), b.y}, 8.0f, 8.0f}, brush);
            brush->SetColor(rgb(kGoldBright, k));
            target->DrawRoundedRectangle({{std::min(a.x, b.x), a.y, std::max(a.x, b.x), b.y}, 8.0f, 8.0f}, brush, 2.0f);
        }
    }
}

void Renderer::Impl::run() {
    std::string error;
    const bool ok = create(error);
    {
        std::scoped_lock lock(mutex);
        init = ok ? 1 : -1;
    }
    wake.notify_all();
    if (!ok) {
        log::error("vr menu: can't draw the menu: {}", error);
        destroy();
        return;
    }
    for (;;) {
        Snapshot snapshot;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return quit || pending.has_value(); });
            if (quit) {
                break;
            }
            snapshot = std::move(*pending);
            pending.reset();
        }
        draw(snapshot);
    }
    destroy();
}

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}

Renderer::~Renderer() {
    if (impl_->thread.joinable()) {
        {
            std::scoped_lock lock(impl_->mutex);
            impl_->quit = true;
        }
        impl_->wake.notify_all();
        impl_->thread.join();
    }
}

bool Renderer::start() {
    if (!impl_->thread.joinable()) {
        impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    }
    std::unique_lock lock(impl_->mutex);
    impl_->wake.wait(lock, [&] { return impl_->init != 0; });
    return impl_->init > 0;
}

void Renderer::submit(Snapshot snapshot) {
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->pending = std::move(snapshot);
    }
    impl_->wake.notify_all();
}

bool Renderer::take(std::vector<std::uint32_t>& pixels) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->done_seq == impl_->taken_seq || impl_->done.empty()) {
        return false;
    }
    impl_->taken_seq = impl_->done_seq;
    pixels = impl_->done;
    return true;
}

} // namespace bg3vr::vrmenu
