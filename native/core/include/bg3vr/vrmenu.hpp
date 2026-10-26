#pragma once

#include "bg3vr/config.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The mod's own VR menu: a panel in the headset (opened with the left menu button, the "vrmenu" binding) for the
// settings a player changes while playing, in tabs: View, Comfort, Body, Scale, Controls (every controller button's
// action) and Guide (animated lessons that follow the current settings, bindings and controller). Point at it with
// the pointing hand's laser (or touch it) and pull the trigger; the stick left / right changes the row under the
// laser.
//
// Menu holds the rows and their current values and turns pointer input into Actions (pure, unit-tested). Renderer
// draws a Menu's Snapshot into a premultiplied BGRA image with Direct2D on its own thread, so redrawing never costs
// the presenting thread a frame. Settings changes are applied by the core (bridge set=<section>.<key> <value>) and
// saved to menu.ini next to bg3vr.ini, which overrides it on the next start.
namespace bg3vr::vrmenu {

inline constexpr int kWidth = 1024;
inline constexpr int kHeight = 1024;

// State owned elsewhere that the menu shows and can ask to toggle.
struct Live {
    bool third_person = false;
    bool ui_wrist = false;
    // The controllers: "touch", "index", "vive", "hands" (hand tracking, no controllers) or "other".
    std::string profile = "touch";
};

struct Actions {
    std::vector<std::pair<std::string, std::string>> settings; // "section.key", value
    bool toggle_view = false;
    bool toggle_ui = false;
    bool unlock_travel = false; // testing: unlock every waypoint and the map in the loaded game (Lua, Server/Testing.lua)
    bool recenter = false;
    bool recalibrate = false;
    bool close = false;
    float haptic = 0.0f; // a pulse on the pointing hand (0 = none)
};

// What to draw (copied to the render thread).
struct Snapshot {
    struct Row {
        enum class Kind { Section, Choice, Buttons, Tabs, Guide } kind = Kind::Choice;
        std::string label;     // Guide: the lesson title
        std::string value;     // Choice: the current option's text; Guide: the lesson text
        bool off = false;      // Choice: the current option means "off" (drawn dimmer)
        bool hovered = false;
        bool compact = false;              // a smaller row (the Controls page)
        int hovered_button = -1;           // Buttons / Tabs / Guide: which one is under the laser
        int selected_button = -1;          // Tabs: the page shown
        std::vector<std::string> buttons;  // Buttons / Tabs / Guide (navigation): their captions
        int top = 0, height = 0;           // pixels
        // Guide: which animation, its time (seconds, loops), and the lesson number / count.
        std::string anim;
        float anim_time = 0.0f;
        int lesson = 0, lessons = 0;
        std::string pointing_hand; // "right" | "left": the animations mirror for a left-handed player
    };
    std::vector<Row> rows;
    std::string footer;
};

class Menu {
public:
    Menu();

    // Current values from the configuration (bg3vr.ini with menu.ini applied).
    void load(const Config& config);

    // One frame of input: `pointer` is the laser (or fingertip) on the panel in uv (v down), nullopt when off it;
    // `click` is a trigger press (or a touch) this frame; `stick_x` the pointing hand's stick (-1..1).
    // Controller navigation without the laser: a stick up / down moves the highlight between rows, left / right changes
    // the highlighted row (or the page on the tabs, the button on the button strip), A activates it, B closes the menu.
    struct Nav {
        float stick_y = 0.0f; // up positive
        bool accept = false;  // A pressed this frame
        bool back = false;    // B pressed this frame
    };
    Actions update(std::optional<std::array<float, 2>> pointer, bool click, float stick_x, const Live& live, Nav nav = {});
    // Time passing (seconds): animates the Guide page while it is shown.
    void tick(float seconds);

    // Increases whenever what update() would draw changes.
    std::uint64_t revision() const { return revision_; }
    Snapshot snapshot() const;

    // For tests and the log: the current value text of the row labelled `label` ("" if none).
    std::string value_of(const std::string& label) const;
    // The label of the highlighted row (laser or controller navigation), for the log.
    std::string hovered_label() const;
    // The current value of a setting ("section.key") as the menu holds it ("" if no row has it).
    std::string setting(const std::string& name) const;
    // The row index under v (or -1), and for a Buttons / Tabs row which button is under u (or -1).
    int row_at(float v) const;
    int button_at(int row, float u) const;
    // Pages (the tabs along the top): which is shown, show another, and the page a row is on (-1 if none).
    int page() const { return page_; }
    void show_page(int page);
    int page_of(const std::string& label) const;
    // The Guide's lessons for the current settings: title and text (for tests).
    struct Lesson {
        std::string title;
        std::string text;
        std::string anim;
    };
    std::vector<Lesson> lessons() const;
    int lesson() const { return lesson_; }
    void show_lesson(int lesson, float anim_time = 0.0f);
    void show_guide() { show_page(static_cast<int>(pages_.size()) - 1); }

private:
    struct Option {
        std::string text;
        std::string value;
        bool off = false;
        bool custom = false; // a value from the ini that no option matches, shown as it is
    };
    struct Item {
        Snapshot::Row::Kind kind = Snapshot::Row::Kind::Choice;
        std::string label;
        std::string setting;            // "section.key"; empty for live rows and buttons
        enum class Live { None, View, Ui, UnlockTravel } live = Live::None;
        int control = -1;               // Controls page: which controller input (labels follow the controller)
        std::vector<Option> options;
        int current = 0;
        std::vector<std::string> buttons;
        int page = -1;    // the tab it is on; -1: always shown (the tabs, the buttons)
        bool compact = false;
        int fallback = 0; // the option used when the setting isn't configured (the module's own default)
        int top = 0, height = 0;
    };
    std::vector<Item> items_;
    std::vector<std::string> pages_;
    int page_ = 0;
    int hovered_ = -1;
    int hovered_button_ = -1;
    Live live_{};
    bool stick_armed_ = true;
    bool stick_y_armed_ = true;
    int focus_ = -1;        // the row the controller navigation is on (-1: none yet)
    int focus_button_ = 0;  // on the button strip
    int next_row(int from, int direction) const; // the next row that can be navigated to on this page
    int lesson_ = 0;
    float anim_time_ = 0.0f;
    float anim_since_draw_ = 0.0f;
    std::uint64_t revision_ = 1;

    void layout();
    int current_index(const Item& item) const;
    void step(int index, int direction, Actions& actions);
    std::string label_of(const Item& item) const;
    std::string control_name(int control) const; // e.g. "right trigger", "A"
    std::string control_for(const std::string& action) const; // the control bound to an action, "" if none
    bool guide_shown() const;
};

// Draws Snapshots on its own thread (Direct2D + DirectWrite into a DIB section; no COM, no GPU device).
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Starts the thread (once). False if Direct2D or DirectWrite can't be created (the reason is logged).
    bool start();
    // Asks for a drawing of `snapshot` (replaces one still waiting).
    void submit(Snapshot snapshot);
    // The latest finished image (kWidth x kHeight premultiplied BGRA, rows top-down) if it is newer than the one
    // taken before; false otherwise.
    bool take(std::vector<std::uint32_t>& pixels);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bg3vr::vrmenu
