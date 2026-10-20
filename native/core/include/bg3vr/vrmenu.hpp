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
// settings a player changes while playing: first / third person, where the game UI sits, turning and comfort, which
// hand features are on, eye height, recentring. Point at it with the right hand's laser (or touch it) and pull the
// trigger; the right stick left / right changes the row under the laser.
//
// Menu holds the rows and their current values and turns pointer input into Actions (pure, unit-tested). Renderer
// draws a Menu's Snapshot into a premultiplied BGRA image with Direct2D on its own thread, so redrawing never costs
// the presenting thread a frame. Settings changes are applied by the core (bridge set=<section>.<key> <value>) and
// saved to menu.ini next to bg3vr.ini, which overrides it on the next start.
namespace bg3vr::vrmenu {

inline constexpr int kWidth = 1024;
inline constexpr int kHeight = 1280;

// State owned elsewhere that the menu shows and can ask to toggle.
struct Live {
    bool third_person = false;
    bool ui_wrist = false;
};

struct Actions {
    std::vector<std::pair<std::string, std::string>> settings; // "section.key", value
    bool toggle_view = false;
    bool toggle_ui = false;
    bool recenter = false;
    bool recalibrate = false;
    bool close = false;
    float haptic = 0.0f; // a pulse on the pointing hand (0 = none)
};

// What to draw (copied to the render thread).
struct Snapshot {
    struct Row {
        enum class Kind { Section, Choice, Buttons } kind = Kind::Choice;
        std::string label;
        std::string value;     // Choice: the current option's text
        bool off = false;      // Choice: the current option means "off" (drawn dimmer)
        bool hovered = false;
        int hovered_button = -1;           // Buttons: which one is under the laser
        std::vector<std::string> buttons;  // Buttons: their captions
        int top = 0, height = 0;           // pixels
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
    Actions update(std::optional<std::array<float, 2>> pointer, bool click, float stick_x, const Live& live);

    // Increases whenever what update() would draw changes.
    std::uint64_t revision() const { return revision_; }
    Snapshot snapshot() const;

    // For tests and the log: the current value text of the row labelled `label` ("" if none).
    std::string value_of(const std::string& label) const;
    // The row index under v (or -1), and for a Buttons row which button is under u (or -1).
    int row_at(float v) const;
    int button_at(int row, float u) const;

private:
    struct Option {
        std::string text;
        std::string value;
        bool off = false;
    };
    struct Item {
        Snapshot::Row::Kind kind = Snapshot::Row::Kind::Choice;
        std::string label;
        std::string setting;            // "section.key"; empty for live rows and buttons
        enum class Live { None, View, Ui } live = Live::None;
        std::vector<Option> options;
        int current = 0;
        std::vector<std::string> buttons;
        int fallback = 0; // the option used when the setting isn't configured (the module's own default)
        int top = 0, height = 0;
    };
    std::vector<Item> items_;
    int hovered_ = -1;
    int hovered_button_ = -1;
    Live live_{};
    bool stick_armed_ = true;
    std::uint64_t revision_ = 1;

    void layout();
    int current_index(const Item& item) const;
    void step(int index, int direction, Actions& actions);
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
