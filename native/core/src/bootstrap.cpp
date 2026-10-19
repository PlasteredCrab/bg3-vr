// Core entry point. Phase 0: identify the game build, report graphics modules as they load,
// and scan signatures.txt so reverse-engineering finds can be checked without recompiling.

#include "bg3vr/bridge.hpp"
#include "bg3vr/camera.hpp"
#include "bg3vr/config.hpp"
#include "bg3vr/cull.hpp"
#include "bg3vr/events.hpp"
#include "bg3vr/gpu_trace.hpp"
#if BG3VR_HAVE_OPENXR
#include "bg3vr/headset.hpp"
#endif
#include "bg3vr/probe.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/dialogue.hpp"
#include "bg3vr/picking.hpp"
#include "bg3vr/quality.hpp"
#include "bg3vr/rayprobe.hpp"
#include "bg3vr/skeleton.hpp"
#include "bg3vr/stereo.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/trace.hpp"
#include "bg3vr/views.hpp"
#include "bg3vr/watchdog.hpp"
#include "bg3vr/win_util.hpp"

#include <windows.h>

#include <array>
#include <filesystem>
#include <mutex>

namespace {

using namespace bg3vr;

std::once_flag g_once;
std::filesystem::path g_root;
Config g_config;

log::Level parse_level(const std::string& name) {
    const std::string value = text::to_lower(name);
    if (value == "debug") return log::Level::Debug;
    if (value == "warn") return log::Level::Warn;
    if (value == "error") return log::Level::Error;
    return log::Level::Info;
}

void log_module(const ModuleInfo& module) {
    log::info("game exe: {}", to_utf8(module.path));
    log::info("version {}, renderer {}", to_utf8(module.file_version), to_string(guess_renderer(module)));
    log::info("image base 0x{:X}, size 0x{:X}", module.base, module.image_size);
    for (const Section& section : module.sections) {
        log::debug("  section {:<8} rva 0x{:08X} size 0x{:X}", section.name, section.start - module.base, section.size);
    }
}

void scan_signatures(const ModuleInfo& module) {
    std::vector<std::string> errors;
    const auto specs = sig::load_file(g_root / "signatures.txt", &errors);
    for (const auto& error : errors) {
        log::warn("signatures.txt: {}", error);
    }
    if (specs.empty()) {
        log::info("no signatures to scan");
        return;
    }

    const Section* code = find_section(module, ".text");
    if (!code) {
        log::error("main module has no .text section; signature scan skipped");
        return;
    }
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(code->start), code->size);

    for (const auto& spec : specs) {
        const auto result = sig::scan(spec, bytes, code->start);
        if (result.hit_count == 0) {
            log::warn("sig {}: not found", spec.name);
        } else if (!result.address) {
            log::warn("sig {}: {}{} hits, must be unique", spec.name, result.hit_count,
                      result.hit_count >= sig::kMaxHits ? "+" : "");
        } else {
            log::info("sig {}: rva 0x{:X}", spec.name, *result.address - module.base);
            sig::publish(spec.name, *result.address);
        }
    }
}

// Logs graphics- and mod-related DLLs as they load, so we learn the startup order.
DWORD WINAPI watch_modules(void*) {
    constexpr std::array<const wchar_t*, 6> kNames = {
        L"dxgi.dll", L"d3d11.dll", L"vulkan-1.dll", L"DWrite.dll", L"openxr_loader.dll", L"NoesisApp.dll",
    };
    std::array<bool, kNames.size()> seen{};
    for (int tick = 0; tick < 240; ++tick) { // 120 s
        for (std::size_t i = 0; i < kNames.size(); ++i) {
            if (seen[i]) {
                continue;
            }
            if (HMODULE module = GetModuleHandleW(kNames[i])) {
                seen[i] = true;
                wchar_t path[MAX_PATH * 2];
                const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
                log::info("module loaded: {}", to_utf8(std::wstring_view(path, length)));
            }
        }
        Sleep(500);
    }
    log::debug("module watch finished");
    return 0;
}

// On the game thread at every camera view update.
void on_game_frame() {
    cull::on_frame();
    views::on_frame();
    skeleton::on_frame();
    rayprobe::on_frame();
    quality::on_frame();
}

// Runs on the bridge thread for every new version of the bridge file; only keys present act.
void handle_bridge(const ModuleInfo& module, const bridge::Request& request) {
    if (request.controller) {
        camera::attach(*request.controller);
    }
    if (request.pose_off) {
        camera::set_pose(std::nullopt);
    } else if (request.pose) {
        camera::set_pose(request.pose);
    }
    if (request.follow) {
        camera::set_follow(*request.follow);
    }
    if (request.mouselook) {
        camera::set_mouselook(*request.mouselook);
    }
    if (request.sweep) {
        camera::set_sweep(*request.sweep);
    }
    if (request.eye_fov) {
        camera::set_eye_fov(*request.eye_fov);
    }
#if BG3VR_HAVE_OPENXR
    if (request.xr_lag) {
        headset::set_lag(*request.xr_lag);
    }
    if (request.recenter) {
        headset::recenter();
    }
    if (request.hands) {
        headset::report_hands(*request.hands == "buzz");
    }
#endif
    if (request.stereo) {
        if (*request.stereo == "on") {
            if (const auto eyes = views::eye_views()) {
                stereo::assign((*eyes)[0], 0);
                stereo::assign((*eyes)[1], 1);
                views::set_stereo_layout(true);
                // Where each view's draws land, and the renderer's events, for the first frames of the layout.
                events::arm(module, 3, 6000);
                gpu_trace::arm(3);
                views::diff_eye_views("stereo on");
#if BG3VR_HAVE_OPENXR
                headset::set_stereo(true);
#endif
            } else {
                log::warn("stereo: no built split view yet (!vr_split on, reload, !vr_views build)");
            }
        } else if (*request.stereo == "off") {
#if BG3VR_HAVE_OPENXR
            headset::set_stereo(false);
#endif
            stereo::clear();
            views::set_stereo_layout(false);
        }
        stereo::report();
    }
    if (request.hide) {
        cull::set_hidden(*request.hide);
    }
    if (request.skeleton) {
        skeleton::attach_visual(*request.skeleton);
    }
    if (request.pick_hand) {
        picking::set_hand(*request.pick_hand);
    }
    if (request.quality_report) {
        quality::request_report();
    }
    if (request.ray) {
        if (*request.ray == "report") {
            rayprobe::request_report();
        } else {
            rayprobe::set_source(*request.ray == "camera" ? rayprobe::Source::Camera
                                 : *request.ray == "hand" ? rayprobe::Source::Hand
                                                          : rayprobe::Source::Off);
        }
    }
    if (request.trace) {
        if (*request.trace == "on") {
            trace::start(module);
        } else if (*request.trace == "off") {
            trace::stop();
            trace::report();
        } else {
            trace::report();
        }
    }
    if (request.views_build) {
        views::build(module, camera::controller(), *request.views_build, request.views_build_compositing);
    }
    if (request.views_refs) {
        views::refs();
    }
    if (request.views_diff) {
        views::diff();
    }
    if (request.views_jobs) {
        views::dump_jobs("on request");
    }
    if (request.views_events) {
        events::arm(module, 3, 6000);
    }
    if (request.views_create) {
        views::create(module, camera::controller());
    }
    if (request.views_gpu) {
        events::arm(module, 3, 6000);
        gpu_trace::arm(3);
    }
    if (request.view_set) {
        views::set_view(request.view_set->index, request.view_set->placement, request.view_set->enabled);
    }
    if (request.views_layout) {
        views::set_layout(*request.views_layout);
        // The split-screen view appears a few seconds after ForceSplitscreen: look again if needed;
        // the layout picks the views up every frame.
        if (*request.views_layout != views::Layout::Full && views::found().size() < 2) {
            views::find(module, camera::controller());
        }
    }
    if (request.views_find) {
        if (*request.views_find) {
            views::find(module, camera::controller());
        } else {
            views::dump();
        }
    }
    if (request.probe) {
        if (const std::uintptr_t controller = camera::controller()) {
            probe::run(module, controller);
        } else {
            log::warn("probe: no validated controller to probe");
        }
    }
    camera::report();
    cull::report();
    skeleton::report();
    dialogue::report();
    picking::report();
    render::report();
#if BG3VR_HAVE_OPENXR
    headset::report();
#endif
}

int init(const wchar_t* root_dir) {
    g_root = root_dir;
    const bool have_config = g_config.load_file(g_root / "bg3vr.ini");
    log::init(g_root / "bg3vr.log", g_config.get_bool("dev", "console", false),
              parse_level(g_config.get("dev", "log_level", "info")));

    log::info("BG3 VR core starting (root {})", to_utf8(g_root.wstring()));
    if (!have_config) {
        log::warn("bg3vr.ini not found; using defaults");
    }

    const ModuleInfo module = query_main_module();
    log_module(module);

    if (g_config.get_bool("scan", "signatures", true)) {
        scan_signatures(module);
    }

    if (g_config.get_bool("trace", "startup", false)) {
        trace::start(module); // catches calls that only happen while the game starts (!vr_trace off reports)
    }
    camera::install(g_config, module);
    views::set_full_setup(g_config.get_bool("views", "full_setup", true));
    views::set_split_index(static_cast<int>(g_config.get_int("views", "split_index", 0)));
    stereo::install(g_config, module);
    render::install(module);
    watchdog::start(module);
#if BG3VR_HAVE_OPENXR
    headset::install(g_config);
#endif
    cull::init(module);
    skeleton::init(module);
    rayprobe::init(module);
    quality::install(g_config, module);
    dialogue::install(g_config, module);
    picking::install(g_config, module);
    camera::set_frame_callback(&on_game_frame);
    bridge::start(g_config, [module](const bridge::Request& request) { handle_bridge(module, request); });

    if (HANDLE thread = CreateThread(nullptr, 0, watch_modules, nullptr, 0, nullptr)) {
        CloseHandle(thread);
    }
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) int bg3vr_init(const wchar_t* root_dir) {
    int result = 0;
    std::call_once(g_once, [&] { result = init(root_dir); });
    return result;
}
