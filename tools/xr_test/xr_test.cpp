// xr_test: standalone OpenXR + D3D11 check, outside the game (Phase 1 step 1), using the same
// bg3vr_xr session code the core will use.
//
//   bg3vr_xr_test --probe          runtime, headset and recommended eye sizes; no session
//   bg3vr_xr_test [--seconds N]    open a session and show a test pattern in each eye (default 15 s):
//                                  a checkerboard tinted red (left eye) / blue (right eye), pulsing so
//                                  dropped frames are visible. Prints each eye's field of view and the
//                                  eye separation once, then frame rate and head pose every second.
//   bg3vr_xr_test --quad [--seconds N]
//                                  the in-game mirror's path without the game: draws the pattern into a
//                                  1920x1080 R8G8B8A8_UNORM texture (standing in for the back buffer),
//                                  copies it into a quad swapchain and shows it as a 3 m screen 2.5 m
//                                  ahead, as headset.cpp does with [xr] enabled = true.
//   --input                        (with either session mode) also reads the motion controllers through the
//                                  same actions the game uses: prints the bound profile, every button press
//                                  and release, and both hands' grip poses each second; a grip press buzzes
//                                  that hand.
//
// Exit codes: 0 ok, 1 OpenXR/D3D11 error, 2 no headset (form factor unavailable).

#include "bg3vr/xr.hpp"

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

namespace {

template <class T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

double degrees(float radians) {
    return static_cast<double>(radians) * 180.0 / std::numbers::pi;
}

void draw_pattern(ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, ID3D11RenderTargetView* rtv,
                  std::size_t eye, int32_t width, int32_t height, float pulse) {
    const float base[4] = {eye == 0 ? 0.35f * pulse : 0.05f, 0.05f, eye == 0 ? 0.05f : 0.35f * pulse, 1.0f};
    context->ClearRenderTargetView(rtv, base);
    if (!context1) {
        return;
    }
    // 8x8 checkerboard: bright squares mark the image's orientation and the eye.
    const float bright[4] = {eye == 0 ? 0.9f : 0.3f, 0.3f, eye == 0 ? 0.3f : 0.9f, 1.0f};
    std::vector<D3D11_RECT> rects;
    const LONG cw = width / 8, ch = height / 8;
    for (LONG y = 0; y < 8; ++y) {
        for (LONG x = 0; x < 8; ++x) {
            if ((x + y) % 2 == 0) {
                rects.push_back({x * cw, y * ch, (x + 1) * cw, (y + 1) * ch});
            }
        }
    }
    context1->ClearView(rtv, bright, rects.data(), static_cast<UINT>(rects.size()));
}

// The game's back buffer stand-in for --quad.
constexpr int32_t kQuadWidth = 1920;
constexpr int32_t kQuadHeight = 1080;
constexpr DXGI_FORMAT kQuadFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

int run(bool probe_only, bool quad, bool input, double seconds) {
    bg3vr::xr::System system;
    std::string error;
    const auto status = bg3vr::xr::open(system, error);
    if (!system.runtime.empty()) {
        std::printf("runtime: %s\n", system.runtime.c_str());
    }
    if (status != bg3vr::xr::Status::Ok) {
        std::printf("%s\n", error.c_str());
        bg3vr::xr::close(system);
        return status == bg3vr::xr::Status::NoHeadset ? 2 : 1;
    }
    std::printf("headset: %s\n", system.headset.c_str());
    for (std::size_t i = 0; i < system.views.size(); ++i) {
        const auto& view = system.views[i];
        std::printf("view %zu: recommended %ux%u (max %ux%u), %u samples\n", i, view.recommendedImageRectWidth,
                    view.recommendedImageRectHeight, view.maxImageRectWidth, view.maxImageRectHeight,
                    view.recommendedSwapchainSampleCount);
    }
    std::printf("D3D11 adapter LUID %08lX:%08lX, min feature level 0x%X\n", system.adapter.HighPart,
                system.adapter.LowPart, system.min_feature_level);
    if (probe_only) {
        bg3vr::xr::close(system);
        return 0;
    }

    // A D3D11 device on the adapter the runtime wants (in the game we'll use the game's device).
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (std::memcmp(&desc.AdapterLuid, &system.adapter, sizeof(LUID)) == 0) {
            std::printf("using adapter: %ls\n", desc.Description);
            break;
        }
        release(adapter);
    }
    release(factory);
    if (!adapter) {
        std::printf("no DXGI adapter matches the runtime's LUID\n");
        bg3vr::xr::close(system);
        return 1;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context);
    release(adapter);
    if (FAILED(hr)) {
        std::printf("D3D11CreateDevice failed: 0x%08lX\n", hr);
        bg3vr::xr::close(system);
        return 1;
    }
    ID3D11DeviceContext1* context1 = nullptr;
    context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1));

    ID3D11Texture2D* source = nullptr;
    ID3D11RenderTargetView* source_view = nullptr;
    if (quad) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kQuadWidth;
        desc.Height = kQuadHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = kQuadFormat;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        device->CreateTexture2D(&desc, nullptr, &source);
        if (source) {
            device->CreateRenderTargetView(source, nullptr, &source_view);
        }
    }

    int exit_code = 0;
    {
        bg3vr::xr::Session session;
        const bool created = quad ? session.create(system, device, error, kQuadFormat) &&
                                        session.create_quad(kQuadWidth, kQuadHeight, error)
                                  : session.create(system, device, error);
        if (!created || (quad && !source_view)) {
            std::printf("%s\n", created ? "can't create the stand-in back buffer" : error.c_str());
            exit_code = 1;
        } else {
            std::printf("swapchain format: %d%s\n", static_cast<int>(session.format()),
                        quad ? " (quad mode: copying a 1920x1080 R8G8B8A8_UNORM image)" : "");
            if (input && !session.create_input(error)) {
                std::printf("input: %s\n", error.c_str());
                input = false;
            }
            bg3vr::xr::Hand hands[2]{}, last_hands[2]{};
            const auto start = std::chrono::steady_clock::now();
            auto last_report = start;
            uint64_t frames = 0, frames_this_second = 0;
            bool reported_eyes = false;

            while (session.poll()) {
                const double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (elapsed > seconds) {
                    session.request_exit();
                }
                bg3vr::xr::Frame frame;
                if (!session.running() || !session.begin_frame(frame)) {
                    Sleep(10);
                    continue;
                }
                if (frame.views_valid && !reported_eyes && frame.views.size() == 2) {
                    for (std::size_t i = 0; i < 2; ++i) {
                        const auto& fov = frame.views[i].fov;
                        std::printf("eye %zu fov: left %.1f right %.1f up %.1f down %.1f deg\n", i,
                                    degrees(fov.angleLeft), degrees(fov.angleRight), degrees(fov.angleUp),
                                    degrees(fov.angleDown));
                    }
                    const auto& a = frame.views[0].pose.position;
                    const auto& b = frame.views[1].pose.position;
                    std::printf("eye separation (IPD): %.1f mm\n",
                                1000.0 * std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                                                   (a.z - b.z) * (a.z - b.z)));
                    reported_eyes = true;
                }

                if (input) {
                    if (session.take_profile_changed()) {
                        for (std::size_t h = 0; h < 2; ++h) {
                            const std::string profile = session.interaction_profile(h);
                            std::printf("%s hand profile: %s\n", h == 0 ? "left" : "right",
                                        profile.empty() ? "(none)" : profile.c_str());
                        }
                    }
                    session.sync_input(frame.state.predictedDisplayTime, hands);
                    for (std::size_t h = 0; h < 2; ++h) {
                        const char* name = h == 0 ? "left" : "right";
                        const auto edge = [&](const char* what, bool was, bool is) {
                            if (was != is) {
                                std::printf("%s %s %s\n", name, what, is ? "down" : "up");
                            }
                        };
                        edge("trigger", last_hands[h].trigger > 0.5f, hands[h].trigger > 0.5f);
                        edge("squeeze", last_hands[h].squeeze > 0.5f, hands[h].squeeze > 0.5f);
                        edge("stick-click", last_hands[h].thumbstick_click, hands[h].thumbstick_click);
                        edge("primary", last_hands[h].primary, hands[h].primary);
                        edge("secondary", last_hands[h].secondary, hands[h].secondary);
                        edge("menu", last_hands[h].menu, hands[h].menu);
                        if (last_hands[h].squeeze <= 0.5f && hands[h].squeeze > 0.5f) {
                            session.haptic(h, 0.5f, 0.1f);
                        }
                        last_hands[h] = hands[h];
                    }
                }

                const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(elapsed * 3.0));
                if (quad) {
                    if (frame.state.shouldRender) {
                        draw_pattern(context, context1, source_view, 0, kQuadWidth, kQuadHeight, pulse);
                        const auto image = session.acquire_quad();
                        context->CopyResource(image.texture, source);
                        session.release_quad();
                    }
                    if (!session.end_frame_quad(frame, 3.0f, 2.5f)) {
                        std::printf("xrEndFrame failed\n");
                        exit_code = 1;
                        break;
                    }
                } else if (frame.state.shouldRender) {
                    for (std::size_t i = 0; i < session.view_count(); ++i) {
                        const auto image = session.acquire(i);
                        draw_pattern(context, context1, image.view, i, session.width(i), session.height(i), pulse);
                        session.release(i);
                    }
                }
                if (!quad && !session.end_frame(frame)) {
                    std::printf("xrEndFrame failed\n");
                    exit_code = 1;
                    break;
                }
                ++frames;
                ++frames_this_second;

                const auto now = std::chrono::steady_clock::now();
                if (now - last_report >= std::chrono::seconds(1)) {
                    const XrPosef pose = frame.views.empty() ? XrPosef{} : frame.views[0].pose;
                    std::printf("%.0f s: %llu frames/s, display period %.2f ms, left eye pos (%.3f, %.3f, %.3f) "
                                "quat (%.3f, %.3f, %.3f, %.3f)\n",
                                elapsed, static_cast<unsigned long long>(frames_this_second),
                                static_cast<double>(frame.state.predictedDisplayPeriod) / 1e6, pose.position.x,
                                pose.position.y, pose.position.z, pose.orientation.x, pose.orientation.y,
                                pose.orientation.z, pose.orientation.w);
                    for (std::size_t h = 0; input && h < 2; ++h) {
                        const auto& hand = hands[h];
                        std::printf("  %s: %s%s grip (%.3f, %.3f, %.3f) vel (%.2f, %.2f, %.2f) trigger %.2f squeeze "
                                    "%.2f stick (%.2f, %.2f)\n",
                                    h == 0 ? "left" : "right", hand.active ? "active" : "inactive",
                                    hand.grip_valid ? " tracked" : " untracked", hand.grip.position.x,
                                    hand.grip.position.y, hand.grip.position.z, hand.linear_velocity.x,
                                    hand.linear_velocity.y, hand.linear_velocity.z, hand.trigger, hand.squeeze,
                                    hand.thumbstick.x, hand.thumbstick.y);
                    }
                    frames_this_second = 0;
                    last_report = now;
                }
            }
            std::printf("done: %llu frames\n", static_cast<unsigned long long>(frames));
        }
    }
    bg3vr::xr::close(system);
    release(source_view);
    release(source);
    release(context1);
    release(context);
    release(device);
    return exit_code;
}

} // namespace

int main(int argc, char** argv) {
    bool probe_only = false;
    bool quad = false;
    bool input = false;
    double seconds = 15.0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--probe") {
            probe_only = true;
        } else if (arg == "--quad") {
            quad = true;
        } else if (arg == "--input") {
            input = true;
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: bg3vr_xr_test [--probe] [--quad] [--input] [--seconds N]\n");
            return 1;
        }
    }
    return run(probe_only, quad, input, seconds);
}
