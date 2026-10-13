// xr_test: standalone OpenXR + D3D11 check, outside the game (Phase 1 step 1).
//
//   bg3vr_xr_test --probe          runtime, headset and recommended eye sizes; no session
//   bg3vr_xr_test [--seconds N]    open a session and show a test pattern in each eye (default 15 s):
//                                  a checkerboard tinted red (left eye) / blue (right eye), pulsing so
//                                  dropped frames are visible; prints the head pose once a second
//
// Exit codes: 0 ok, 1 OpenXR/D3D11 error, 2 no headset (form factor unavailable).

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

XrInstance g_instance = XR_NULL_HANDLE;

const char* result_name(XrResult result) {
    static char buffer[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(g_instance, result, buffer))) {
        std::snprintf(buffer, sizeof(buffer), "XrResult %d", static_cast<int>(result));
    }
    return buffer;
}

bool check(XrResult result, const char* what) {
    if (XR_FAILED(result)) {
        std::fprintf(stderr, "%s failed: %s\n", what, result_name(result));
        return false;
    }
    return true;
}

template <class T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<ID3D11RenderTargetView*> views;
};

int run(bool probe_only, double seconds) {
    // ---- instance ----------------------------------------------------------------------------
    uint32_t extension_count = 0;
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr), "enumerate extensions")) {
        return 1;
    }
    std::vector<XrExtensionProperties> extensions(extension_count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, extensions.data());
    bool has_d3d11 = false;
    for (const auto& e : extensions) {
        has_d3d11 = has_d3d11 || std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    }
    if (!has_d3d11) {
        std::fprintf(stderr, "runtime lacks %s\n", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        return 1;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(create.applicationInfo.applicationName, "BG3VR xr_test");
    create.applicationInfo.applicationVersion = 1;
    strcpy_s(create.applicationInfo.engineName, "BG3VR");
    create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    create.enabledExtensionCount = 1;
    create.enabledExtensionNames = enabled;
    if (!check(xrCreateInstance(&create, &g_instance), "xrCreateInstance")) {
        return 1;
    }

    XrInstanceProperties instance_props{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &instance_props);
    std::printf("runtime: %s %u.%u.%u\n", instance_props.runtimeName, XR_VERSION_MAJOR(instance_props.runtimeVersion),
                XR_VERSION_MINOR(instance_props.runtimeVersion), XR_VERSION_PATCH(instance_props.runtimeVersion));

    // ---- system ------------------------------------------------------------------------------
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    const XrResult system_result = xrGetSystem(g_instance, &system_info, &system);
    if (system_result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        std::printf("no headset available (is it connected and SteamVR running?)\n");
        xrDestroyInstance(g_instance);
        return 2;
    }
    if (!check(system_result, "xrGetSystem")) {
        return 1;
    }
    XrSystemProperties system_props{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_instance, system, &system_props);
    std::printf("headset: %s (vendor 0x%X), max swapchain %ux%u, orientation tracking %d, position tracking %d\n",
                system_props.systemName, system_props.vendorId, system_props.graphicsProperties.maxSwapchainImageWidth,
                system_props.graphicsProperties.maxSwapchainImageHeight,
                system_props.trackingProperties.orientationTracking, system_props.trackingProperties.positionTracking);

    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(g_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count,
                                      nullptr);
    std::vector<XrViewConfigurationView> config_views(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(g_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count,
                                      &view_count, config_views.data());
    for (uint32_t i = 0; i < view_count; ++i) {
        std::printf("view %u: recommended %ux%u (max %ux%u), %u samples\n", i, config_views[i].recommendedImageRectWidth,
                    config_views[i].recommendedImageRectHeight, config_views[i].maxImageRectWidth,
                    config_views[i].maxImageRectHeight, config_views[i].recommendedSwapchainSampleCount);
    }

    PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!get_requirements || !check(get_requirements(g_instance, system, &requirements), "D3D11 requirements")) {
        return 1;
    }
    std::printf("D3D11 adapter LUID %08lX:%08lX, min feature level 0x%X\n", requirements.adapterLuid.HighPart,
                requirements.adapterLuid.LowPart, requirements.minFeatureLevel);

    if (probe_only) {
        xrDestroyInstance(g_instance);
        return 0;
    }

    // ---- D3D11 device on the adapter OpenXR wants -----------------------------------------------
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (std::memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)) == 0) {
            std::printf("using adapter: %ls\n", desc.Description);
            break;
        }
        release(adapter);
    }
    release(factory);
    if (!adapter) {
        std::fprintf(stderr, "no DXGI adapter matches the runtime's LUID\n");
        return 1;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   2, D3D11_SDK_VERSION, &device, nullptr, &context);
    release(adapter);
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11CreateDevice failed: 0x%08lX\n", hr);
        return 1;
    }
    ID3D11DeviceContext1* context1 = nullptr;
    context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1));

    // ---- session, space, swapchains ------------------------------------------------------------
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    if (!check(xrCreateSession(g_instance, &session_info, &session), "xrCreateSession")) {
        return 1;
    }

    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    if (!check(xrCreateReferenceSpace(session, &space_info, &space), "xrCreateReferenceSpace")) {
        return 1;
    }

    uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data());
    int64_t format = formats.empty() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : formats.front();
    for (int64_t f : formats) {
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            format = f;
            break;
        }
    }
    std::printf("swapchain format: %lld\n", static_cast<long long>(format));

    std::vector<Eye> eyes(view_count);
    for (uint32_t i = 0; i < view_count; ++i) {
        Eye& eye = eyes[i];
        eye.width = static_cast<int32_t>(config_views[i].recommendedImageRectWidth);
        eye.height = static_cast<int32_t>(config_views[i].recommendedImageRectHeight);
        XrSwapchainCreateInfo swapchain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        swapchain_info.format = format;
        swapchain_info.sampleCount = 1;
        swapchain_info.width = static_cast<uint32_t>(eye.width);
        swapchain_info.height = static_cast<uint32_t>(eye.height);
        swapchain_info.faceCount = 1;
        swapchain_info.arraySize = 1;
        swapchain_info.mipCount = 1;
        if (!check(xrCreateSwapchain(session, &swapchain_info, &eye.swapchain), "xrCreateSwapchain")) {
            return 1;
        }
        uint32_t image_count = 0;
        xrEnumerateSwapchainImages(eye.swapchain, 0, &image_count, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(eye.swapchain, image_count, &image_count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        for (const auto& image : images) {
            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
            rtv_desc.Format = static_cast<DXGI_FORMAT>(format);
            rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView* rtv = nullptr;
            device->CreateRenderTargetView(image.texture, &rtv_desc, &rtv);
            eye.views.push_back(rtv);
        }
    }

    // ---- frame loop --------------------------------------------------------------------------
    bool running = false;
    bool quit = false;
    uint64_t frames = 0, frames_this_second = 0;
    const auto start = std::chrono::steady_clock::now();
    auto last_report = start;

    while (!quit) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                std::printf("session state %d\n", static_cast<int>(changed.state));
                if (changed.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = check(xrBeginSession(session, &begin), "xrBeginSession");
                } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                } else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                quit = true;
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }

        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed > seconds) {
            if (running) {
                xrRequestExitSession(session);
            } else {
                break;
            }
        }
        if (!running) {
            Sleep(10);
            continue;
        }

        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        if (!check(xrWaitFrame(session, nullptr, &frame_state), "xrWaitFrame") ||
            !check(xrBeginFrame(session, nullptr), "xrBeginFrame")) {
            break;
        }

        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = frame_state.predictedDisplayTime;
        locate.space = space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        std::vector<XrView> views(view_count, {XR_TYPE_VIEW});
        uint32_t located = 0;
        xrLocateViews(session, &locate, &view_state, view_count, &located, views.data());

        std::vector<XrCompositionLayerProjectionView> projection_views(view_count,
                                                                       {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(elapsed * 3.0));
        for (uint32_t i = 0; i < view_count && frame_state.shouldRender; ++i) {
            Eye& eye = eyes[i];
            uint32_t index = 0;
            xrAcquireSwapchainImage(eye.swapchain, nullptr, &index);
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(eye.swapchain, &wait);

            ID3D11RenderTargetView* rtv = eye.views[index];
            const float base[4] = {i == 0 ? 0.35f * pulse : 0.05f, 0.05f, i == 0 ? 0.05f : 0.35f * pulse, 1.0f};
            context->ClearRenderTargetView(rtv, base);
            if (context1) {
                // 8x8 checkerboard: bright squares mark the image's orientation and the eye.
                const float bright[4] = {i == 0 ? 0.9f : 0.3f, 0.3f, i == 0 ? 0.3f : 0.9f, 1.0f};
                std::vector<D3D11_RECT> rects;
                const LONG cw = eye.width / 8, ch = eye.height / 8;
                for (LONG y = 0; y < 8; ++y) {
                    for (LONG x = 0; x < 8; ++x) {
                        if ((x + y) % 2 == 0) {
                            rects.push_back({x * cw, y * ch, (x + 1) * cw, (y + 1) * ch});
                        }
                    }
                }
                context1->ClearView(rtv, bright, rects.data(), static_cast<UINT>(rects.size()));
            }
            xrReleaseSwapchainImage(eye.swapchain, nullptr);

            projection_views[i].pose = views[i].pose;
            projection_views[i].fov = views[i].fov;
            projection_views[i].subImage.swapchain = eye.swapchain;
            projection_views[i].subImage.imageRect = {{0, 0}, {eye.width, eye.height}};
        }

        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = space;
        layer.viewCount = view_count;
        layer.views = projection_views.data();
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frame_state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = frame_state.shouldRender ? 1 : 0;
        end.layers = layers;
        if (!check(xrEndFrame(session, &end), "xrEndFrame")) {
            break;
        }
        ++frames;
        ++frames_this_second;

        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            const auto& p = views.empty() ? XrPosef{} : views[0].pose;
            std::printf("%.0f s: %llu frames/s, display period %.2f ms, left eye pos (%.3f, %.3f, %.3f) quat (%.3f, "
                        "%.3f, %.3f, %.3f)\n",
                        elapsed, static_cast<unsigned long long>(frames_this_second),
                        static_cast<double>(frame_state.predictedDisplayPeriod) / 1e6, p.position.x, p.position.y,
                        p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
            frames_this_second = 0;
            last_report = now;
        }
    }
    std::printf("done: %llu frames\n", static_cast<unsigned long long>(frames));

    for (Eye& eye : eyes) {
        for (auto*& rtv : eye.views) {
            release(rtv);
        }
        if (eye.swapchain) {
            xrDestroySwapchain(eye.swapchain);
        }
    }
    xrDestroySpace(space);
    xrDestroySession(session);
    xrDestroyInstance(g_instance);
    release(context1);
    release(context);
    release(device);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool probe_only = false;
    double seconds = 15.0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--probe") {
            probe_only = true;
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: bg3vr_xr_test [--probe] [--seconds N]\n");
            return 1;
        }
    }
    return run(probe_only, seconds);
}
