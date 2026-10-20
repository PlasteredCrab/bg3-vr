#pragma once

// OpenXR with D3D11: runtime and headset discovery, and a session on a caller-supplied device
// (in the game this will be the game's own device). Used by tools/xr_test today and the core later.

#include <d3d11.h>
#include <windows.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bg3vr::xr {

struct System {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId id = XR_NULL_SYSTEM_ID;
    std::string runtime; // e.g. "SteamVR/OpenXR 2.17.10"
    std::string headset; // the system name the runtime reports
    std::vector<XrViewConfigurationView> views;
    LUID adapter{};      // the GPU the runtime needs the session's device on
    D3D_FEATURE_LEVEL min_feature_level{};
    bool hand_tracking = false; // XR_EXT_hand_tracking enabled and the system supports it
};

enum class Status { Ok, NoHeadset, Failed };

// Creates an instance with XR_KHR_D3D11_enable (and XR_EXT_hand_tracking when the runtime has it) and finds the
// head-mounted display.
Status open(System& system, std::string& error);
void close(System& system);

const char* result_name(XrInstance instance, XrResult result);

struct Frame {
    XrFrameState state{XR_TYPE_FRAME_STATE};
    std::vector<XrView> views; // located in the session's LOCAL space for state.predictedDisplayTime
    bool views_valid = false;  // orientation (and position) tracked
};

// One motion controller, sampled by Session::sync_input. Poses are in the session's LOCAL space (OpenXR
// convention: right-handed, +Y up, -Z forward), like the views.
struct Hand {
    bool active = false;      // the runtime has a controller bound for this hand
    XrPosef grip{};           // where the hand holds the controller (for held objects)
    XrPosef aim{};            // a pointing ray: origin and -Z direction (for pointing / gravity pull)
    bool grip_valid = false;  // position and orientation tracked
    bool aim_valid = false;
    XrVector3f linear_velocity{};  // of the grip pose, m/s (throwing)
    XrVector3f angular_velocity{}; // rad/s
    bool velocity_valid = false;
    float trigger = 0.0f;     // 0..1
    float squeeze = 0.0f;     // grip button, 0..1
    XrVector2f thumbstick{};  // -1..1 (trackpad on Vive)
    bool thumbstick_click = false;
    bool primary = false;     // A (right) / X (left)
    bool secondary = false;   // B (right) / Y (left)
    bool menu = false;        // menu button (left on Touch)

    // Hand tracking (XR_EXT_hand_tracking), used when no controller is active for this hand. The joints become the
    // same inputs: grip = the palm in the controller grip's frame (-Z where the fingers point, +Y up the fist, from
    // the knuckles), aim = a ray from the index knuckle along wrist -> index knuckle (it stays put when the hand
    // closes), trigger and squeeze = how closed the hand is.
    bool hand_tracked = false;
    XrVector3f index_tip{};   // for touching panels
    XrVector3f palm_normal{}; // out of the palm (unit)
    float fist = 0.0f;        // 0 open .. 1 closed (middle, ring and little fingertips to the palm)
    float pinch = 0.0f;       // 0 apart .. 1 thumb and index tips touching
    bool index_extended = false; // the index finger is straight (pointing)
};
enum HandIndex : std::size_t { kLeft = 0, kRight = 1 };

// Where the panel layer (Session::create_panel) is shown: a pose in the session's LOCAL space, or relative to a
// hand's grip (the runtime then keeps it on the controller with no added latency). Width in metres; the height
// follows the panel image's aspect ratio.
struct PanelPlacement {
    enum class Anchor { Local, LeftGrip, RightGrip } anchor = Anchor::Local;
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}};
    float width_m = 0.3f;
};

struct EyeImage {
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* view = nullptr;
};

class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Opens a session on `device` (which must be on system.adapter), a LOCAL reference space and one
    // swapchain per view at the recommended size. The format is the sRGB form of `preferred` when
    // the runtime offers it (copied game images are gamma-encoded), else `preferred`, else an sRGB
    // format.
    // `eye_width` / `eye_height`, when non-zero, replace the recommended per-eye size (the game's
    // back buffer is copied whole into an eye image, so they must match it).
    bool create(const System& system, ID3D11Device* device, std::string& error,
                DXGI_FORMAT preferred = DXGI_FORMAT_UNKNOWN, int32_t eye_width = 0, int32_t eye_height = 0);
    void destroy();

    // A flat "screen" swapchain of the given size, shown with end_frame_quad (same format as the
    // eye swapchains).
    bool create_quad(int32_t width, int32_t height, std::string& error);
    EyeImage acquire_quad();
    void release_quad();
    // Submits only the quad: a screen `width_m` wide, centred `distance_m` in front of the LOCAL
    // origin, its height from the quad's aspect ratio.
    bool end_frame_quad(const Frame& frame, float width_m, float distance_m);

    // Handles runtime events (begins and ends the session as the runtime asks). Returns false once
    // the application should stop.
    bool poll();
    bool running() const { return running_; }
    void request_exit();

    // xrWaitFrame, xrBeginFrame, xrLocateViews. False when not running or on error.
    bool begin_frame(Frame& frame);
    // The next image of view `index`'s swapchain, valid until release(index).
    EyeImage acquire(std::size_t index);
    void release(std::size_t index);
    // Submits one projection layer built from frame.views (none when the runtime says not to render).
    bool end_frame(const Frame& frame);
    // Submits one projection layer whose views are `rendered`: the pose and field of view each eye
    // image was actually rendered with, which may be older than frame.views. Empty: no layer.
    bool end_frame(const Frame& frame, const std::vector<XrView>& rendered);

    // The views at `time` (e.g. a display time further ahead than the current frame's) in LOCAL
    // space. False when orientation isn't tracked.
    bool locate(XrTime time, std::vector<XrView>& views);

    // Motion controllers: one action set (grip and aim poses, trigger, squeeze, thumbstick and its click,
    // primary / secondary / menu buttons, haptics) with suggested bindings for Oculus Touch, Valve Index,
    // HTC Vive and the Khronos simple controller, attached to the session. Call once after create().
    bool create_input(std::string& error);
    bool has_input() const { return action_set_ != XR_NULL_HANDLE; }
    // Hand trackers for both hands (System::hand_tracking); called by create_input. False if unavailable.
    bool has_hand_tracking() const { return hand_trackers_[0] != XR_NULL_HANDLE; }
    // Syncs the actions and samples both hands, poses located at `time` (use the frame's predicted display
    // time). False when the session isn't focused or input wasn't created (hands then read inactive).
    bool sync_input(XrTime time, Hand (&hands)[2]);
    // A vibration on one hand: amplitude 0..1 for `seconds` (frequency 0 = runtime default).
    void haptic(std::size_t hand, float amplitude, float seconds, float frequency = 0.0f);
    // The controller profile the runtime bound to a hand (e.g. "/interaction_profiles/oculus/touch_controller"),
    // empty when none. The runtime reports changes as events; take_profile_changed() says one arrived.
    std::string interaction_profile(std::size_t hand) const;
    bool take_profile_changed() { return std::exchange(profile_changed_, false); }

    // Quad layers drawn over the eye views with source alpha (premultiplied), e.g. the game's UI on a wrist or
    // world panel (index 0) and the mod's own menu (index 1, drawn on top). Hand anchors need create_input().
    // A panel is shown while set_panel holds a placement and an image has been released at least once.
    static constexpr std::size_t kPanelCount = 2;
    bool create_panel(int32_t width, int32_t height, std::string& error, std::size_t index = 0);
    EyeImage acquire_panel(std::size_t index = 0);
    void release_panel(std::size_t index = 0);
    void set_panel(std::optional<PanelPlacement> placement, std::size_t index = 0) {
        panels_[index].placement = placement;
    }
    bool has_panel(std::size_t index = 0) const { return panels_[index].eye.swapchain != XR_NULL_HANDLE; }
    // Removes a panel's swapchain (to make it again at another size).
    void destroy_panel(std::size_t index = 0);

    std::size_t view_count() const { return eyes_.size(); }
    int32_t width(std::size_t index) const { return eyes_[index].width; }
    int32_t height(std::size_t index) const { return eyes_[index].height; }
    DXGI_FORMAT format() const { return format_; }

private:
    struct Eye {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        int32_t width = 0;
        int32_t height = 0;
        std::vector<EyeImage> images;
        uint32_t acquired = 0;
    };

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE;
    std::vector<Eye> eyes_;
    Eye quad_;
    ID3D11Device* device_ = nullptr;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;

    bool create_swapchain(Eye& eye, std::string& error);
    void destroy_input();
    // Panel `index`'s quad layer, if it should be shown; false otherwise.
    bool panel_layer(std::size_t index, XrCompositionLayerQuad& layer) const;

    struct Panel {
        Eye eye;
        bool ready = false; // an image was released
        std::optional<PanelPlacement> placement;
    };
    Panel panels_[kPanelCount];

    XrActionSet action_set_ = XR_NULL_HANDLE;
    XrPath hand_paths_[2]{};
    XrAction grip_pose_ = XR_NULL_HANDLE, aim_pose_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE,
             squeeze_ = XR_NULL_HANDLE, thumbstick_ = XR_NULL_HANDLE, thumbstick_click_ = XR_NULL_HANDLE,
             primary_ = XR_NULL_HANDLE, secondary_ = XR_NULL_HANDLE, menu_ = XR_NULL_HANDLE,
             haptic_ = XR_NULL_HANDLE;
    XrSpace grip_spaces_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace aim_spaces_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool running_ = false;
    bool quit_ = false;
    bool profile_changed_ = false;

    bool hand_tracking_supported_ = false;
    XrHandTrackerEXT hand_trackers_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    PFN_xrCreateHandTrackerEXT create_hand_tracker_ = nullptr;
    PFN_xrDestroyHandTrackerEXT destroy_hand_tracker_ = nullptr;
    PFN_xrLocateHandJointsEXT locate_hand_joints_ = nullptr;
    void create_hand_trackers();
    void sample_hand(std::size_t index, XrTime time, Hand& hand);
};

} // namespace bg3vr::xr
