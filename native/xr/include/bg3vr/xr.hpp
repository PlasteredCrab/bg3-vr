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
#include <string>
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
};

enum class Status { Ok, NoHeadset, Failed };

// Creates an instance with XR_KHR_D3D11_enable and finds the head-mounted display.
Status open(System& system, std::string& error);
void close(System& system);

const char* result_name(XrInstance instance, XrResult result);

struct Frame {
    XrFrameState state{XR_TYPE_FRAME_STATE};
    std::vector<XrView> views; // located in the session's LOCAL space for state.predictedDisplayTime
    bool views_valid = false;  // orientation (and position) tracked
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
    // swapchain per view at the recommended size, sRGB where the runtime offers it.
    bool create(const System& system, ID3D11Device* device, std::string& error);
    void destroy();

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
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    bool running_ = false;
    bool quit_ = false;
};

} // namespace bg3vr::xr
