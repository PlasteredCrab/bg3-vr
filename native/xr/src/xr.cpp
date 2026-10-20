#include "bg3vr/xr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bg3vr::xr {

namespace {

std::string describe(XrInstance instance, const char* what, XrResult result) {
    return std::string(what) + " failed: " + result_name(instance, result);
}

// The sRGB / linear sibling of an 8-bit colour format (same bits, so copies between them work).
DXGI_FORMAT sibling(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return format;
    }
}

bool is_srgb(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

} // namespace

const char* result_name(XrInstance instance, XrResult result) {
    thread_local char buffer[XR_MAX_RESULT_STRING_SIZE];
    if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, result, buffer))) {
        std::snprintf(buffer, sizeof(buffer), "XrResult %d", static_cast<int>(result));
    }
    return buffer;
}

Status open(System& system, std::string& error) {
    uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result)) {
        error = describe(XR_NULL_HANDLE, "enumerating instance extensions (is an OpenXR runtime installed?)", result);
        return Status::Failed;
    }
    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
    bool has_d3d11 = false;
    bool has_hands = false;
    for (const auto& extension : extensions) {
        has_d3d11 = has_d3d11 || std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
        has_hands = has_hands || std::strcmp(extension.extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == 0;
    }
    if (!has_d3d11) {
        error = std::string("runtime lacks ") + XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
        return Status::Failed;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_EXT_HAND_TRACKING_EXTENSION_NAME};
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(create.applicationInfo.applicationName, "BG3 VR");
    create.applicationInfo.applicationVersion = 1;
    strcpy_s(create.applicationInfo.engineName, "BG3VR");
    create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    create.enabledExtensionCount = has_hands ? 2 : 1;
    create.enabledExtensionNames = enabled;
    result = xrCreateInstance(&create, &system.instance);
    if (XR_FAILED(result)) {
        error = describe(XR_NULL_HANDLE, "xrCreateInstance", result);
        return Status::Failed;
    }

    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(system.instance, &properties);
    char runtime[XR_MAX_RUNTIME_NAME_SIZE + 32];
    std::snprintf(runtime, sizeof(runtime), "%s %u.%u.%u", properties.runtimeName,
                  XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion),
                  XR_VERSION_PATCH(properties.runtimeVersion));
    system.runtime = runtime;

    XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystem(system.instance, &info, &system.id);
    if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        error = "no headset available (is it connected and SteamVR running?)";
        return Status::NoHeadset;
    }
    if (XR_FAILED(result)) {
        error = describe(system.instance, "xrGetSystem", result);
        return Status::Failed;
    }

    XrSystemHandTrackingPropertiesEXT hand_properties{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES, has_hands ? &hand_properties : nullptr};
    xrGetSystemProperties(system.instance, system.id, &system_properties);
    system.headset = system_properties.systemName;
    system.hand_tracking = has_hands && hand_properties.supportsHandTracking == XR_TRUE;

    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(system.instance, system.id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                      &view_count, nullptr);
    system.views.assign(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(system.instance, system.id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      view_count, &view_count, system.views.data());

    PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
    xrGetInstanceProcAddr(system.instance, "xrGetD3D11GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!get_requirements) {
        error = "xrGetD3D11GraphicsRequirementsKHR unavailable";
        return Status::Failed;
    }
    result = get_requirements(system.instance, system.id, &requirements);
    if (XR_FAILED(result)) {
        error = describe(system.instance, "xrGetD3D11GraphicsRequirementsKHR", result);
        return Status::Failed;
    }
    system.adapter = requirements.adapterLuid;
    system.min_feature_level = requirements.minFeatureLevel;
    return Status::Ok;
}

void close(System& system) {
    if (system.instance != XR_NULL_HANDLE) {
        xrDestroyInstance(system.instance);
        system.instance = XR_NULL_HANDLE;
    }
}

Session::~Session() {
    destroy();
}

bool Session::create(const System& system, ID3D11Device* device, std::string& error, DXGI_FORMAT preferred,
                     int32_t eye_width, int32_t eye_height) {
    instance_ = system.instance;
    device_ = device;
    hand_tracking_supported_ = system.hand_tracking;
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = system.id;
    XrResult result = xrCreateSession(instance_, &session_info, &session_);
    if (XR_FAILED(result)) {
        error = describe(instance_, "xrCreateSession", result);
        return false;
    }

    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    result = xrCreateReferenceSpace(session_, &space_info, &space_);
    if (XR_FAILED(result)) {
        error = describe(instance_, "xrCreateReferenceSpace", result);
        return false;
    }

    uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(session_, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(session_, format_count, &format_count, formats.data());
    const auto offered = [&formats](DXGI_FORMAT format) {
        for (const int64_t candidate : formats) {
            if (candidate == static_cast<int64_t>(format)) {
                return true;
            }
        }
        return false;
    };
    format_ = formats.empty() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : static_cast<DXGI_FORMAT>(formats.front());
    // A copied desktop image holds gamma-encoded bytes whatever its format says, so the runtime must
    // read them as sRGB: the sRGB form of `preferred` first, then `preferred` itself.
    const DXGI_FORMAT srgb = is_srgb(preferred) ? preferred : sibling(preferred);
    if (preferred != DXGI_FORMAT_UNKNOWN && srgb != DXGI_FORMAT_UNKNOWN && offered(srgb)) {
        format_ = srgb;
    } else if (preferred != DXGI_FORMAT_UNKNOWN && offered(preferred)) {
        format_ = preferred;
    } else {
        for (const DXGI_FORMAT candidate : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
            if (offered(candidate)) {
                format_ = candidate;
                break;
            }
        }
    }

    eyes_.resize(system.views.size());
    for (std::size_t i = 0; i < eyes_.size(); ++i) {
        eyes_[i].width = eye_width > 0 ? eye_width : static_cast<int32_t>(system.views[i].recommendedImageRectWidth);
        eyes_[i].height = eye_height > 0 ? eye_height : static_cast<int32_t>(system.views[i].recommendedImageRectHeight);
        if (!create_swapchain(eyes_[i], error)) {
            return false;
        }
    }
    return true;
}

bool Session::create_swapchain(Eye& eye, std::string& error) {
    XrSwapchainCreateInfo swapchain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchain_info.format = format_;
    swapchain_info.sampleCount = 1;
    swapchain_info.width = static_cast<uint32_t>(eye.width);
    swapchain_info.height = static_cast<uint32_t>(eye.height);
    swapchain_info.faceCount = 1;
    swapchain_info.arraySize = 1;
    swapchain_info.mipCount = 1;
    const XrResult result = xrCreateSwapchain(session_, &swapchain_info, &eye.swapchain);
    if (XR_FAILED(result)) {
        error = describe(instance_, "xrCreateSwapchain", result);
        return false;
    }
    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(eye.swapchain, 0, &image_count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(eye.swapchain, image_count, &image_count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    for (const auto& image : images) {
        D3D11_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = format_;
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        EyeImage entry{image.texture, nullptr};
        device_->CreateRenderTargetView(image.texture, &rtv, &entry.view);
        eye.images.push_back(entry);
    }
    return true;
}

bool Session::create_quad(int32_t width, int32_t height, std::string& error) {
    quad_.width = width;
    quad_.height = height;
    return create_swapchain(quad_, error);
}

EyeImage Session::acquire_quad() {
    xrAcquireSwapchainImage(quad_.swapchain, nullptr, &quad_.acquired);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(quad_.swapchain, &wait);
    return quad_.images[quad_.acquired];
}

void Session::release_quad() {
    xrReleaseSwapchainImage(quad_.swapchain, nullptr);
}

bool Session::create_panel(int32_t width, int32_t height, std::string& error, std::size_t index) {
    Eye& eye = panels_[index].eye;
    eye.width = width;
    eye.height = height;
    return create_swapchain(eye, error);
}

EyeImage Session::acquire_panel(std::size_t index) {
    Eye& eye = panels_[index].eye;
    xrAcquireSwapchainImage(eye.swapchain, nullptr, &eye.acquired);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(eye.swapchain, &wait);
    return eye.images[eye.acquired];
}

void Session::release_panel(std::size_t index) {
    xrReleaseSwapchainImage(panels_[index].eye.swapchain, nullptr);
    panels_[index].ready = true;
}

bool Session::panel_layer(std::size_t index, XrCompositionLayerQuad& layer) const {
    const Panel& p = panels_[index];
    if (!p.placement || !p.ready || p.eye.swapchain == XR_NULL_HANDLE || p.eye.width <= 0) {
        return false;
    }
    XrSpace space = space_;
    if (p.placement->anchor == PanelPlacement::Anchor::LeftGrip) {
        space = grip_spaces_[kLeft];
    } else if (p.placement->anchor == PanelPlacement::Anchor::RightGrip) {
        space = grip_spaces_[kRight];
    }
    if (space == XR_NULL_HANDLE) {
        return false;
    }
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = space;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain = p.eye.swapchain;
    layer.subImage.imageRect = {{0, 0}, {p.eye.width, p.eye.height}};
    layer.pose = p.placement->pose;
    layer.size = {p.placement->width_m,
                  p.placement->width_m * static_cast<float>(p.eye.height) / static_cast<float>(p.eye.width)};
    return true;
}

bool Session::end_frame_quad(const Frame& frame, float width_m, float distance_m) {
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.space = space_;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain = quad_.swapchain;
    layer.subImage.imageRect = {{0, 0}, {quad_.width, quad_.height}};
    layer.pose.orientation.w = 1.0f;
    layer.pose.position = {0.0f, 0.0f, -distance_m};
    layer.size = {width_m, width_m * static_cast<float>(quad_.height) / static_cast<float>(quad_.width)};
    XrCompositionLayerQuad panels[kPanelCount]{};
    const XrCompositionLayerBaseHeader* layers[1 + kPanelCount] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    uint32_t count = 1;
    for (std::size_t i = 0; i < kPanelCount; ++i) {
        if (panel_layer(i, panels[i])) {
            layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panels[i]);
        }
    }

    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = frame.state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = frame.state.shouldRender && quad_.swapchain != XR_NULL_HANDLE ? count : 0;
    end.layers = layers;
    return XR_SUCCEEDED(xrEndFrame(session_, &end));
}

bool Session::create_input(std::string& error) {
    if (session_ == XR_NULL_HANDLE) {
        error = "create_input: no session";
        return false;
    }
    if (action_set_ != XR_NULL_HANDLE) {
        return true;
    }
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(set_info.actionSetName, "bg3vr");
    strcpy_s(set_info.localizedActionSetName, "BG3 VR");
    XrResult result = xrCreateActionSet(instance_, &set_info, &action_set_);
    if (XR_FAILED(result)) {
        error = describe(instance_, "xrCreateActionSet", result);
        return false;
    }
    xrStringToPath(instance_, "/user/hand/left", &hand_paths_[kLeft]);
    xrStringToPath(instance_, "/user/hand/right", &hand_paths_[kRight]);

    const auto make = [&](XrAction& action, const char* name, const char* localized, XrActionType type) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(info.actionName, name);
        strcpy_s(info.localizedActionName, localized);
        info.actionType = type;
        info.countSubactionPaths = 2;
        info.subactionPaths = hand_paths_;
        const XrResult r = xrCreateAction(action_set_, &info, &action);
        if (XR_FAILED(r)) {
            error = describe(instance_, "xrCreateAction", r) + " (" + name + ")";
            return false;
        }
        return true;
    };
    if (!make(grip_pose_, "grip_pose", "Hand grip pose", XR_ACTION_TYPE_POSE_INPUT) ||
        !make(aim_pose_, "aim_pose", "Hand aim pose", XR_ACTION_TYPE_POSE_INPUT) ||
        !make(trigger_, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !make(squeeze_, "squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !make(thumbstick_, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
        !make(thumbstick_click_, "thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(primary_, "primary", "Primary button", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(secondary_, "secondary", "Secondary button", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(menu_, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(haptic_, "haptic", "Haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT)) {
        destroy_input();
        return false;
    }

    // Suggested bindings per controller profile; a profile the runtime rejects is skipped.
    struct Binding {
        XrAction action;
        std::string path;
    };
    const auto suggest = [&](const char* profile, const std::vector<Binding>& bindings) {
        XrPath profile_path = XR_NULL_PATH;
        xrStringToPath(instance_, profile, &profile_path);
        std::vector<XrActionSuggestedBinding> list;
        for (const Binding& b : bindings) {
            XrPath path = XR_NULL_PATH;
            if (XR_SUCCEEDED(xrStringToPath(instance_, b.path.c_str(), &path))) {
                list.push_back({b.action, path});
            }
        }
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = profile_path;
        suggested.countSuggestedBindings = static_cast<uint32_t>(list.size());
        suggested.suggestedBindings = list.data();
        return XR_SUCCEEDED(xrSuggestInteractionProfileBindings(instance_, &suggested));
    };
    const auto both = [](XrAction action, const char* suffix) {
        return std::vector<Binding>{{action, std::string("/user/hand/left") + suffix},
                                    {action, std::string("/user/hand/right") + suffix}};
    };
    const auto join = [](std::initializer_list<std::vector<Binding>> parts) {
        std::vector<Binding> all;
        for (const auto& part : parts) {
            all.insert(all.end(), part.begin(), part.end());
        }
        return all;
    };
    const auto poses = [&] {
        return join({both(grip_pose_, "/input/grip/pose"), both(aim_pose_, "/input/aim/pose"),
                     both(haptic_, "/output/haptic")});
    };
    bool any = false;
    any |= suggest("/interaction_profiles/oculus/touch_controller",
                   join({poses(), both(trigger_, "/input/trigger/value"), both(squeeze_, "/input/squeeze/value"),
                         both(thumbstick_, "/input/thumbstick"), both(thumbstick_click_, "/input/thumbstick/click"),
                         {{primary_, "/user/hand/left/input/x/click"}, {primary_, "/user/hand/right/input/a/click"},
                          {secondary_, "/user/hand/left/input/y/click"}, {secondary_, "/user/hand/right/input/b/click"},
                          {menu_, "/user/hand/left/input/menu/click"}}}));
    any |= suggest("/interaction_profiles/valve/index_controller",
                   join({poses(), both(trigger_, "/input/trigger/value"), both(squeeze_, "/input/squeeze/value"),
                         both(thumbstick_, "/input/thumbstick"), both(thumbstick_click_, "/input/thumbstick/click"),
                         both(primary_, "/input/a/click"), both(secondary_, "/input/b/click")}));
    any |= suggest("/interaction_profiles/htc/vive_controller",
                   join({poses(), both(trigger_, "/input/trigger/value"), both(squeeze_, "/input/squeeze/click"),
                         both(thumbstick_, "/input/trackpad"), both(thumbstick_click_, "/input/trackpad/click"),
                         both(menu_, "/input/menu/click")}));
    any |= suggest("/interaction_profiles/khr/simple_controller",
                   join({poses(), both(trigger_, "/input/select/click"), both(menu_, "/input/menu/click")}));
    if (!any) {
        error = "no controller profile's bindings were accepted";
        destroy_input();
        return false;
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &action_set_;
    result = xrAttachSessionActionSets(session_, &attach);
    if (XR_FAILED(result)) {
        error = describe(instance_, "xrAttachSessionActionSets", result);
        destroy_input();
        return false;
    }
    for (std::size_t h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space_info.subactionPath = hand_paths_[h];
        space_info.poseInActionSpace.orientation.w = 1.0f;
        space_info.action = grip_pose_;
        xrCreateActionSpace(session_, &space_info, &grip_spaces_[h]);
        space_info.action = aim_pose_;
        xrCreateActionSpace(session_, &space_info, &aim_spaces_[h]);
    }
    create_hand_trackers();
    return true;
}

void Session::create_hand_trackers() {
    if (!hand_tracking_supported_ || hand_trackers_[0] != XR_NULL_HANDLE) {
        return;
    }
    xrGetInstanceProcAddr(instance_, "xrCreateHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&create_hand_tracker_));
    xrGetInstanceProcAddr(instance_, "xrDestroyHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&destroy_hand_tracker_));
    xrGetInstanceProcAddr(instance_, "xrLocateHandJointsEXT", reinterpret_cast<PFN_xrVoidFunction*>(&locate_hand_joints_));
    if (!create_hand_tracker_ || !destroy_hand_tracker_ || !locate_hand_joints_) {
        return;
    }
    for (std::size_t h = 0; h < 2; ++h) {
        XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        info.hand = h == kLeft ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (XR_FAILED(create_hand_tracker_(session_, &info, &hand_trackers_[h]))) {
            hand_trackers_[h] = XR_NULL_HANDLE;
        }
    }
    if (hand_trackers_[0] == XR_NULL_HANDLE || hand_trackers_[1] == XR_NULL_HANDLE) {
        for (XrHandTrackerEXT& t : hand_trackers_) {
            if (t != XR_NULL_HANDLE) {
                destroy_hand_tracker_(t);
                t = XR_NULL_HANDLE;
            }
        }
    }
}

namespace {

XrVector3f sub(const XrVector3f& a, const XrVector3f& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
float dot3(const XrVector3f& a, const XrVector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
XrVector3f cross3(const XrVector3f& a, const XrVector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
XrVector3f unit(const XrVector3f& v) {
    const float n = std::sqrt(dot3(v, v));
    return n > 1e-6f ? XrVector3f{v.x / n, v.y / n, v.z / n} : XrVector3f{0.0f, 0.0f, 0.0f};
}
float distance3(const XrVector3f& a, const XrVector3f& b) {
    const XrVector3f d = sub(a, b);
    return std::sqrt(dot3(d, d));
}

// A rotation whose -Z axis is `forward` and +Y is `up` (made perpendicular), as a quaternion.
XrQuaternionf look(const XrVector3f& forward, const XrVector3f& up_hint) {
    const XrVector3f z{-forward.x, -forward.y, -forward.z};
    const float along = dot3(up_hint, z);
    const XrVector3f y = unit({up_hint.x - along * z.x, up_hint.y - along * z.y, up_hint.z - along * z.z});
    const XrVector3f x = cross3(y, z);
    // Matrix columns x, y, z -> quaternion.
    const float m00 = x.x, m11 = y.y, m22 = z.z;
    const float trace = m00 + m11 + m22;
    XrQuaternionf q{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (y.z - z.y) / s;
        q.y = (z.x - x.z) / s;
        q.z = (x.y - y.x) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (y.z - z.y) / s;
        q.x = 0.25f * s;
        q.y = (y.x + x.y) / s;
        q.z = (z.x + x.z) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (z.x - x.z) / s;
        q.x = (y.x + x.y) / s;
        q.y = 0.25f * s;
        q.z = (z.y + y.z) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (x.y - y.x) / s;
        q.x = (z.x + x.z) / s;
        q.y = (z.y + y.z) / s;
        q.z = 0.25f * s;
    }
    return q;
}

} // namespace

void Session::sample_hand(std::size_t index, XrTime time, Hand& hand) {
    if (hand_trackers_[index] == XR_NULL_HANDLE) {
        return;
    }
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT]{};
    XrHandJointVelocityEXT velocities[XR_HAND_JOINT_COUNT_EXT]{};
    XrHandJointVelocitiesEXT velocity_list{XR_TYPE_HAND_JOINT_VELOCITIES_EXT};
    velocity_list.jointCount = XR_HAND_JOINT_COUNT_EXT;
    velocity_list.jointVelocities = velocities;
    XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, &velocity_list};
    locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
    locations.jointLocations = joints;
    XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
    info.baseSpace = space_;
    info.time = time;
    if (XR_FAILED(locate_hand_joints_(hand_trackers_[index], &info, &locations)) || locations.isActive != XR_TRUE) {
        return;
    }
    const auto valid = [&](XrHandJointEXT j) {
        return (joints[j].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    };
    for (XrHandJointEXT j : {XR_HAND_JOINT_PALM_EXT, XR_HAND_JOINT_WRIST_EXT, XR_HAND_JOINT_INDEX_PROXIMAL_EXT,
                             XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, XR_HAND_JOINT_LITTLE_PROXIMAL_EXT,
                             XR_HAND_JOINT_INDEX_TIP_EXT, XR_HAND_JOINT_THUMB_TIP_EXT}) {
        if (!valid(j)) {
            return;
        }
    }
    const XrVector3f palm = joints[XR_HAND_JOINT_PALM_EXT].pose.position;
    const XrVector3f wrist = joints[XR_HAND_JOINT_WRIST_EXT].pose.position;
    const XrVector3f index_knuckle = joints[XR_HAND_JOINT_INDEX_PROXIMAL_EXT].pose.position;
    const XrVector3f middle_knuckle = joints[XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT].pose.position;
    const XrVector3f little_knuckle = joints[XR_HAND_JOINT_LITTLE_PROXIMAL_EXT].pose.position;
    const XrVector3f fingers = unit(sub(middle_knuckle, wrist));
    const XrVector3f up = sub(index_knuckle, little_knuckle); // up the fist, for both hands
    hand.hand_tracked = true;
    hand.grip = {look(fingers, up), palm};
    hand.grip_valid = true;
    hand.aim = {look(unit(sub(index_knuckle, wrist)), up), index_knuckle};
    hand.aim_valid = true;
    hand.index_tip = joints[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position;
    // Out of the palm: the right palm faces -X when the hand is held like a controller (+Y up, -Z forward), the left +X.
    const XrVector3f back = cross3(fingers, unit(up)); // for the right hand this points out of its back
    hand.palm_normal = index == kRight ? XrVector3f{-back.x, -back.y, -back.z} : back;
    hand.palm_normal = unit(hand.palm_normal);
    float curl = 0.0f;
    int counted = 0;
    for (XrHandJointEXT tip : {XR_HAND_JOINT_MIDDLE_TIP_EXT, XR_HAND_JOINT_RING_TIP_EXT, XR_HAND_JOINT_LITTLE_TIP_EXT}) {
        if (valid(tip)) {
            curl += std::clamp((0.09f - distance3(joints[tip].pose.position, palm)) / 0.045f, 0.0f, 1.0f);
            ++counted;
        }
    }
    hand.fist = counted ? curl / static_cast<float>(counted) : 0.0f;
    hand.pinch = std::clamp((0.035f - distance3(joints[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position, hand.index_tip)) / 0.02f,
                            0.0f, 1.0f);
    hand.index_extended = distance3(hand.index_tip, palm) > 0.075f;
    hand.trigger = hand.fist; // the core decides between click and grab (headset.cpp)
    hand.squeeze = hand.fist;
    const XrHandJointVelocityEXT& v = velocities[XR_HAND_JOINT_PALM_EXT];
    hand.velocity_valid = (v.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
    hand.linear_velocity = v.linearVelocity;
    hand.angular_velocity = v.angularVelocity;
}

bool Session::sync_input(XrTime time, Hand (&hands)[2]) {
    hands[kLeft] = {};
    hands[kRight] = {};
    if (action_set_ == XR_NULL_HANDLE || !running_) {
        return false;
    }
    XrActiveActionSet active{action_set_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    const XrResult result = xrSyncActions(session_, &sync);
    if (result != XR_SUCCESS) {
        return false; // XR_SESSION_NOT_FOCUSED: no input this frame
    }
    for (std::size_t h = 0; h < 2; ++h) {
        Hand& hand = hands[h];
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.subactionPath = hand_paths_[h];

        XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};
        get.action = grip_pose_;
        xrGetActionStatePose(session_, &get, &pose_state);
        hand.active = pose_state.isActive == XR_TRUE;

        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, &velocity};
        if (grip_spaces_[h] != XR_NULL_HANDLE && XR_SUCCEEDED(xrLocateSpace(grip_spaces_[h], space_, time, &location))) {
            const XrSpaceLocationFlags needed =
                XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            hand.grip = location.pose;
            hand.grip_valid = (location.locationFlags & needed) == needed;
            hand.velocity_valid = (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
            hand.linear_velocity = velocity.linearVelocity;
            hand.angular_velocity = velocity.angularVelocity;
        }
        XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
        if (aim_spaces_[h] != XR_NULL_HANDLE && XR_SUCCEEDED(xrLocateSpace(aim_spaces_[h], space_, time, &aim))) {
            hand.aim = aim.pose;
            hand.aim_valid = (aim.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        }

        XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
        get.action = trigger_;
        if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &get, &value)) && value.isActive) {
            hand.trigger = value.currentState;
        }
        value = {XR_TYPE_ACTION_STATE_FLOAT};
        get.action = squeeze_;
        if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &get, &value)) && value.isActive) {
            hand.squeeze = value.currentState;
        }
        XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
        get.action = thumbstick_;
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &get, &stick)) && stick.isActive) {
            hand.thumbstick = stick.currentState;
        }
        const auto button = [&](XrAction action) {
            XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
            get.action = action;
            return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &get, &state)) && state.isActive &&
                   state.currentState == XR_TRUE;
        };
        hand.thumbstick_click = button(thumbstick_click_);
        hand.primary = button(primary_);
        hand.secondary = button(secondary_);
        hand.menu = button(menu_);
        if (!hand.active) {
            sample_hand(h, time, hand); // no controller in this hand: the tracked hand, if any
        }
    }
    return true;
}

void Session::haptic(std::size_t hand, float amplitude, float seconds, float frequency) {
    if (action_set_ == XR_NULL_HANDLE || hand > 1) {
        return;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    vibration.duration = static_cast<XrDuration>(seconds * 1e9);
    vibration.frequency = frequency > 0.0f ? frequency : XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = haptic_;
    info.subactionPath = hand_paths_[hand];
    xrApplyHapticFeedback(session_, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

std::string Session::interaction_profile(std::size_t hand) const {
    if (action_set_ == XR_NULL_HANDLE || hand > 1) {
        return {};
    }
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (XR_FAILED(xrGetCurrentInteractionProfile(session_, hand_paths_[hand], &state)) ||
        state.interactionProfile == XR_NULL_PATH) {
        return {};
    }
    char buffer[XR_MAX_PATH_LENGTH]{};
    uint32_t length = 0;
    if (XR_FAILED(xrPathToString(instance_, state.interactionProfile, sizeof(buffer), &length, buffer))) {
        return {};
    }
    return buffer;
}

void Session::destroy_input() {
    for (XrHandTrackerEXT& t : hand_trackers_) {
        if (t != XR_NULL_HANDLE && destroy_hand_tracker_) {
            destroy_hand_tracker_(t);
        }
        t = XR_NULL_HANDLE;
    }
    for (std::size_t h = 0; h < 2; ++h) {
        for (XrSpace* space : {&grip_spaces_[h], &aim_spaces_[h]}) {
            if (*space != XR_NULL_HANDLE) {
                xrDestroySpace(*space);
                *space = XR_NULL_HANDLE;
            }
        }
    }
    if (action_set_ != XR_NULL_HANDLE) {
        xrDestroyActionSet(action_set_); // destroys its actions too
        action_set_ = XR_NULL_HANDLE;
    }
    grip_pose_ = aim_pose_ = trigger_ = squeeze_ = thumbstick_ = thumbstick_click_ = primary_ = secondary_ = menu_ =
        haptic_ = XR_NULL_HANDLE;
}

void Session::destroy() {
    destroy_input();
    eyes_.push_back(quad_); // destroyed with the rest
    for (Panel& p : panels_) {
        eyes_.push_back(p.eye);
        p = {};
    }
    quad_ = {};
    for (Eye& eye : eyes_) {
        for (EyeImage& image : eye.images) {
            if (image.view) {
                image.view->Release();
            }
        }
        if (eye.swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(eye.swapchain);
        }
    }
    eyes_.clear();
    if (space_ != XR_NULL_HANDLE) {
        xrDestroySpace(space_);
        space_ = XR_NULL_HANDLE;
    }
    if (session_ != XR_NULL_HANDLE) {
        xrDestroySession(session_);
        session_ = XR_NULL_HANDLE;
    }
    running_ = false;
}

bool Session::poll() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running_ = XR_SUCCEEDED(xrBeginSession(session_, &begin));
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(session_);
                running_ = false;
            } else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                quit_ = true;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            quit_ = true;
        } else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            profile_changed_ = true;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return !quit_;
}

void Session::request_exit() {
    if (running_) {
        xrRequestExitSession(session_);
    } else {
        quit_ = true;
    }
}

bool Session::begin_frame(Frame& frame) {
    if (!running_) {
        return false;
    }
    frame.state = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(session_, nullptr, &frame.state)) || XR_FAILED(xrBeginFrame(session_, nullptr))) {
        return false;
    }
    frame.views_valid = locate(frame.state.predictedDisplayTime, frame.views);
    return true;
}

bool Session::locate(XrTime time, std::vector<XrView>& views) {
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = time;
    info.space = space_;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    views.assign(eyes_.size(), {XR_TYPE_VIEW});
    uint32_t located = 0;
    const XrResult result =
        xrLocateViews(session_, &info, &view_state, static_cast<uint32_t>(views.size()), &located, views.data());
    return XR_SUCCEEDED(result) && (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
}

EyeImage Session::acquire(std::size_t index) {
    Eye& eye = eyes_[index];
    xrAcquireSwapchainImage(eye.swapchain, nullptr, &eye.acquired);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(eye.swapchain, &wait);
    return eye.images[eye.acquired];
}

void Session::release(std::size_t index) {
    xrReleaseSwapchainImage(eyes_[index].swapchain, nullptr);
}

bool Session::end_frame(const Frame& frame) {
    static const std::vector<XrView> kNone;
    return end_frame(frame, frame.views_valid ? frame.views : kNone);
}

bool Session::end_frame(const Frame& frame, const std::vector<XrView>& rendered) {
    std::vector<XrCompositionLayerProjectionView> projection_views(eyes_.size(),
                                                                   {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    for (std::size_t i = 0; i < eyes_.size() && i < rendered.size(); ++i) {
        projection_views[i].pose = rendered[i].pose;
        projection_views[i].fov = rendered[i].fov;
        projection_views[i].subImage.swapchain = eyes_[i].swapchain;
        projection_views[i].subImage.imageRect = {{0, 0}, {eyes_[i].width, eyes_[i].height}};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = space_;
    layer.viewCount = static_cast<uint32_t>(projection_views.size());
    layer.views = projection_views.data();
    XrCompositionLayerQuad panels[kPanelCount]{};
    const bool has_projection = rendered.size() == eyes_.size() && !eyes_.empty();
    const XrCompositionLayerBaseHeader* layers[1 + kPanelCount];
    uint32_t count = 0;
    if (has_projection) {
        layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    }
    for (std::size_t i = 0; i < kPanelCount; ++i) {
        if (panel_layer(i, panels[i])) {
            layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panels[i]);
        }
    }

    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = frame.state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = frame.state.shouldRender ? count : 0;
    end.layers = layers;
    return XR_SUCCEEDED(xrEndFrame(session_, &end));
}

} // namespace bg3vr::xr
