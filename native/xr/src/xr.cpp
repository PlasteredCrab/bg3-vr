#include "bg3vr/xr.hpp"

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
    for (const auto& extension : extensions) {
        has_d3d11 = has_d3d11 || std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    }
    if (!has_d3d11) {
        error = std::string("runtime lacks ") + XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
        return Status::Failed;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(create.applicationInfo.applicationName, "BG3 VR");
    create.applicationInfo.applicationVersion = 1;
    strcpy_s(create.applicationInfo.engineName, "BG3VR");
    create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    create.enabledExtensionCount = 1;
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

    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(system.instance, system.id, &system_properties);
    system.headset = system_properties.systemName;

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

bool Session::create(const System& system, ID3D11Device* device, std::string& error, DXGI_FORMAT preferred) {
    instance_ = system.instance;
    device_ = device;
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
        eyes_[i].width = static_cast<int32_t>(system.views[i].recommendedImageRectWidth);
        eyes_[i].height = static_cast<int32_t>(system.views[i].recommendedImageRectHeight);
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

bool Session::end_frame_quad(const Frame& frame, float width_m, float distance_m) {
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.space = space_;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain = quad_.swapchain;
    layer.subImage.imageRect = {{0, 0}, {quad_.width, quad_.height}};
    layer.pose.orientation.w = 1.0f;
    layer.pose.position = {0.0f, 0.0f, -distance_m};
    layer.size = {width_m, width_m * static_cast<float>(quad_.height) / static_cast<float>(quad_.width)};
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};

    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = frame.state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = frame.state.shouldRender && quad_.swapchain != XR_NULL_HANDLE ? 1 : 0;
    end.layers = layers;
    return XR_SUCCEEDED(xrEndFrame(session_, &end));
}

void Session::destroy() {
    eyes_.push_back(quad_); // destroyed with the rest
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
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = frame.state.predictedDisplayTime;
    locate.space = space_;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    frame.views.assign(eyes_.size(), {XR_TYPE_VIEW});
    uint32_t located = 0;
    const XrResult result = xrLocateViews(session_, &locate, &view_state, static_cast<uint32_t>(frame.views.size()),
                                          &located, frame.views.data());
    frame.views_valid = XR_SUCCEEDED(result) && (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    return true;
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
    std::vector<XrCompositionLayerProjectionView> projection_views(eyes_.size(),
                                                                   {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    for (std::size_t i = 0; i < eyes_.size() && i < frame.views.size(); ++i) {
        projection_views[i].pose = frame.views[i].pose;
        projection_views[i].fov = frame.views[i].fov;
        projection_views[i].subImage.swapchain = eyes_[i].swapchain;
        projection_views[i].subImage.imageRect = {{0, 0}, {eyes_[i].width, eyes_[i].height}};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = space_;
    layer.viewCount = static_cast<uint32_t>(projection_views.size());
    layer.views = projection_views.data();
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};

    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = frame.state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = frame.state.shouldRender && frame.views_valid ? 1 : 0;
    end.layers = layers;
    return XR_SUCCEEDED(xrEndFrame(session_, &end));
}

} // namespace bg3vr::xr
