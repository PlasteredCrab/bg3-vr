// Core entry point. Phase 0: identify the game build, report graphics modules as they load,
// and scan signatures.txt so reverse-engineering finds can be checked without recompiling.

#include "bg3vr/bodyik.hpp"
#include "bg3vr/bridge.hpp"
#include "bg3vr/camera.hpp"
#include "bg3vr/combat.hpp"
#include "bg3vr/corpse.hpp"
#include "bg3vr/config.hpp"
#include "bg3vr/cull.hpp"
#include "bg3vr/events.hpp"
#include "bg3vr/gpu_trace.hpp"
#if BG3VR_HAVE_OPENXR
#include "bg3vr/headset.hpp"
#endif
#include "bg3vr/probe.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/resolution.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/signatures.hpp"
#include "bg3vr/dialogue.hpp"
#include "bg3vr/dolls.hpp"
#include "bg3vr/dice.hpp"
#include "bg3vr/fading.hpp"
#include "bg3vr/foveation.hpp"
#include "bg3vr/gameui.hpp"
#include "bg3vr/eyefix.hpp"
#include "bg3vr/hold.hpp"
#include "bg3vr/holster.hpp"
#include "bg3vr/locomotion.hpp"
#include "bg3vr/melee.hpp"
#include "bg3vr/vrplay.hpp"
#include "bg3vr/paperdoll.hpp"
#include "bg3vr/picking.hpp"
#include "bg3vr/quality.hpp"
#include "bg3vr/rayprobe.hpp"
#include "bg3vr/shadows.hpp"
#include "bg3vr/sharing.hpp"
#include "bg3vr/skeleton.hpp"
#include "bg3vr/stage_timing.hpp"
#include "bg3vr/stereo.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/trace.hpp"
#include "bg3vr/views.hpp"
#include "bg3vr/watchdog.hpp"
#include "bg3vr/weaponset.hpp"
#include "bg3vr/win_util.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>
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
// A conversation (or another camera-taking timeline: the game sets the player view's cinematic flag +0x2961 on join
// and clears it on leave, agent G 2.5) changes the controls and where the game UI is shown.
void update_conversation() {
    bool on = false;
    if (const auto eyes = views::eye_views(); eyes && (*eyes)[0]) {
        std::uint8_t flag = 0;
        on = mem::try_copy(&flag, (*eyes)[0] + stereo::layout::kOutlineGate, sizeof(flag)) && flag != 0;
    }
    vrplay::set_dialogue(on);
#if BG3VR_HAVE_OPENXR
    headset::set_dialogue(on);
#endif
}

// Grabbing (bodies, items) runs on the game's main thread when the event pump is hooked (resolution.cpp): moving items
// from the worker that runs the camera update crashed (14:54).
bool g_grab_on_main_thread = false;
void grab_main_thread_task() {
    corpse::on_frame(); // before hold: a grip that takes a body doesn't also pick up an item
    hold::on_frame();
}

void on_game_frame() {
    cull::on_frame();
    views::on_frame();
    update_conversation();
    skeleton::on_frame();
    rayprobe::on_frame();
    quality::on_frame();
    locomotion::on_frame();
    vrplay::on_frame();
    if (!g_grab_on_main_thread) {
        corpse::on_frame(); // before hold: a grip that takes a body doesn't also pick up an item
        hold::on_frame();
    }
    melee::on_frame();
    combat::on_frame();
}

// The VR menu's settings (menu.ini next to bg3vr.ini; overrides it). Only touched on the bridge thread after start-up.
Config g_menu_settings;

void save_menu_settings() {
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> sections;
    for (const auto& [name, value] : g_menu_settings.values()) {
        const auto dot = name.find('.');
        sections[name.substr(0, dot)].emplace_back(name.substr(dot + 1), value);
    }
    std::string content = "# Written by the VR menu in the headset. These override bg3vr.ini; delete this file to go back to it.\n";
    for (const auto& [section, entries] : sections) {
        content += std::format("\n[{}]\n", section);
        for (const auto& [key, value] : entries) {
            content += std::format("{} = {}\n", key, value);
        }
    }
    const std::filesystem::path path = g_root / "menu.ini";
    const std::filesystem::path temp = g_root / "menu.ini.tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << content;
        if (!out) {
            log::warn("menu: couldn't write {}", to_utf8(temp.wstring()));
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        log::warn("menu: couldn't save {}: {}", to_utf8(path.wstring()), ec.message());
    }
}

// Settings changed in the VR menu: take them now, save them for the next start.
void apply_settings(const std::vector<std::pair<std::string, std::string>>& settings) {
    for (const auto& [name, value] : settings) {
        const auto dot = name.find('.');
        g_config.set(name.substr(0, dot), name.substr(dot + 1), value);
        g_menu_settings.set(name.substr(0, dot), name.substr(dot + 1), value);
        log::info("menu: {} = {}", name, value);
    }
    // Only the modules whose settings changed are reconfigured (re-running everything recalibrated the arms, reset
    // combo state and overrode bridge switches such as ik=off on every click).
    const auto changed = [&](std::string_view prefix) {
        return std::any_of(settings.begin(), settings.end(), [&](const auto& s) { return s.first.rfind(prefix, 0) == 0; });
    };
    const bool third = vrplay::third_person();
    if (changed("dice.")) {
        dice::configure(g_config);
    }
    if (changed("vr.")) {
        vrplay::configure(g_config);
        fading::configure(g_config);
        dice::configure(g_config);
        if (changed("vr.eye_offset")) {
            vrplay::recalibrate(); // after the new offset is in
        }
        if (vrplay::active()) {
            picking::set_hand(!third && g_config.get_bool("vr", "hand_pointing", true));
            locomotion::set_enabled(g_config.get_bool("vr", "stick_locomotion", true));
        }
    }
    if (changed("bindings.") || changed("combos.")) {
        vrplay::configure_bindings(g_config);
    }
    if (changed("ui.stereo_dolls")) {
        dolls::configure(g_config);
    }
    if (changed("views.share_shadows")) {
        shadows::configure(g_config);
    }
    if (changed("xr.") || changed("views.eye_origin")) {
        stereo::configure(g_config);
        const std::string scale = g_config.get("xr", "world_scale");
        camera::set_world_scale(scale.empty() ? 1.0f : std::strtof(scale.c_str(), nullptr));
    }
    if (changed("views.share") || changed("views.skip_eye_cull")) {
        sharing::configure(g_config);
    }
    if (changed("perf.")) {
        stage_timing::configure(g_config);
    }
    if (changed("render.")) {
        resolution::configure(g_config);
        foveation::configure(g_config);
    }
#if BG3VR_HAVE_OPENXR
    if (changed("ui.") || changed("pointer.") || changed("vr.dominant_hand") || changed("xr.desktop_view")) {
        headset::configure_ui(g_config);
    }
#endif
    if (changed("holster.")) {
        holster::set_enabled(g_config.get_bool("holster", "enabled", true));
    }
    if (changed("grab.")) {
        hold::set_hold(g_config.get_bool("grab", "hold", true));
        corpse::set_enabled(g_config.get_bool("grab", "bodies", true));
    }
    if (changed("combat.")) {
        combat::configure(g_config);
        // Swings: "hit" is the physical blade contact (melee.cpp); "gaze" clicks on who you look at (combat.cpp).
        melee::set_enabled(text::to_lower(g_config.get("combat", "swing", g_config.get_bool("combat", "physical", false) ? "hit" : "gaze")) == "hit");
    }
    if (changed("ik.")) {
        bodyik::configure_hands(g_config);
        if (changed("ik.arms")) {
            bodyik::set_enabled(!third && g_config.get_bool("ik", "arms", true));
        }
    }
    save_menu_settings();
}

// Runs on the bridge thread for every new version of the bridge file; only keys present act.
void handle_bridge(const ModuleInfo& module, const bridge::Request& request) {
    if (!request.settings.empty()) {
        apply_settings(request.settings);
    }
    if (request.in_combat) {
#if BG3VR_HAVE_OPENXR
        headset::set_in_combat(*request.in_combat);
#endif
    }
    if (request.wield) {
        combat::set_wield(*request.wield);
    }
    if (request.weapon_set) {
        weaponset::request(request.weapon_set->first, request.weapon_set->second);
    }
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
    if (request.vr) {
        // The VR play profile: what turns on together when the headset is ready (Lua sends vr=start).
        const bool on = *request.vr == "start";
        picking::set_hand(on && g_config.get_bool("vr", "hand_pointing", true));
        locomotion::set_enabled(on && g_config.get_bool("vr", "stick_locomotion", true));
        vrplay::set_active(on);
        // The VR render size only while playing in first person (menus, the flat screen and third person keep the
        // game's own size).
        resolution::set_suspended(!on || vrplay::third_person());
        if (on) {
            health::log_summary("VR play started");
        }
        if (*request.vr == "pause") {
            // A level is unloading: drop everything that points at game objects without touching them.
            views::forget_layout();
            stereo::clear();
#if BG3VR_HAVE_OPENXR
            headset::set_stereo(false);
#endif
            cull::forget();
            hold::forget();
            dice::forget();
        }
    }
    if (request.ik) {
        bodyik::set_enabled(*request.ik);
    }
    if (request.free_camera) {
        // Free camera: the sticks fly the view; the character stands (no stick walking), arms on the game's animation.
        camera::set_free(*request.free_camera);
        if (*request.free_camera) {
            locomotion::set_enabled(false);
            bodyik::set_enabled(false);
        } else if (vrplay::active()) {
            locomotion::set_enabled(g_config.get_bool("vr", "stick_locomotion", true));
        }
    }
    if (request.chase) {
        // Third person in VR: stereo and head tracking stay; the view sits behind and above the character, the arms
        // follow the game's animation (not the controllers).
        const auto number = [&](const char* key, float fallback) {
            const std::string v = g_config.get("vr", key);
            return v.empty() ? fallback : std::strtof(v.c_str(), nullptr);
        };
        camera::set_chase(*request.chase, number("third_person_back", 2.5f), number("third_person_up", 0.6f));
        if (*request.chase) {
            bodyik::set_enabled(false);
        }
    }
    if (request.third_person) {
        // Third person: the game's camera and its mouse picking; first person: the [vr] profile again.
        const bool third = *request.third_person;
        vrplay::set_third_person(third);
        resolution::set_suspended(third);
        picking::set_hand(!third && g_config.get_bool("vr", "hand_pointing", true));
        bodyik::set_enabled(!third && !camera::chase() && !camera::free_camera() && g_config.get_bool("ik", "arms", true));
    }
    if (request.move_stick) {
        locomotion::set_enabled(*request.move_stick);
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
    // Every module's report, for requests from the Lua side and the console; not for the in-process / frequent ones
    // (menu settings, what is wielded, weapon set switches, the follow tick's new transform address: the 2026-10-22
    // run got a full report every 0.4 s from that alone).
    const bool follow_tick = (request.follow || request.skeleton) && !request.controller && !request.pose;
    if (!request.settings.empty() || request.wield || request.weapon_set || request.in_combat || follow_tick) {
        return;
    }
    health::log_summary("report");
    camera::report();
    cull::report();
    gameui::report();
    fading::report();
    dice::report();
    stage_timing::report();
    sharing::report();
    foveation::report();
    skeleton::report();
    dialogue::report();
    eyefix::report();
    shadows::report();
    picking::report();
    locomotion::report();
    vrplay::report();
    bodyik::report();
    hold::report();
    melee::report();
    holster::report();
    combat::report();
    corpse::report();
    weaponset::report();
    resolution::report();
    paperdoll::report();
    dolls::report();
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
    if (g_menu_settings.load_file(g_root / "menu.ini") && !g_menu_settings.values().empty()) {
        std::string list;
        for (const auto& [name, value] : g_menu_settings.values()) {
            const auto dot = name.find('.');
            g_config.set(name.substr(0, dot), name.substr(dot + 1), value);
            list += std::format("{}{} = {}", list.empty() ? "" : ", ", name, value);
        }
        log::info("menu.ini (the VR menu's settings, over bg3vr.ini): {}", list);
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
    views::configure(g_config);
    views::set_full_setup(g_config.get_bool("views", "full_setup", true));
    views::set_split_index(static_cast<int>(g_config.get_int("views", "split_index", 0)));
    stereo::install(g_config, module);
    shadows::install(g_config, module);
    dolls::install(g_config, module);
    gameui::install(g_config, module);
    fading::install(g_config, module);
    dice::install(g_config, module);
    stage_timing::install(g_config, module);
    sharing::install(g_config, module);
    foveation::install(g_config, module);
    render::configure(g_config);
    render::install(module);
    watchdog::start(module);
#if BG3VR_HAVE_OPENXR
    headset::install(g_config);
    headset::set_root(g_root);
#endif
    cull::init(module);
    bodyik::configure(g_config, module);
    skeleton::init(module);
    rayprobe::init(module);
    quality::install(g_config, module);
    dialogue::install(g_config, module);
    eyefix::install(g_config, module);
    picking::install(g_config, module);
    locomotion::install(g_config, module);
    vrplay::install(g_config, module);
    hold::install(g_config, module);
    melee::install(g_config);
    holster::install(g_config);
#if BG3VR_HAVE_OPENXR
    headset::set_first_run(!g_config.get_bool("menu", "welcomed", false));
#endif
    combat::install(g_config);
    corpse::install(g_config, module);
    melee::set_enabled(text::to_lower(g_config.get("combat", "swing", g_config.get_bool("combat", "physical", false) ? "hit" : "gaze")) == "hit");
    weaponset::install(g_config, module);
    resolution::install(g_config, module);
    g_grab_on_main_thread = resolution::set_main_thread_task(&grab_main_thread_task);
    log::info("grab: bodies and items are moved {}", g_grab_on_main_thread ? "on the game's main thread (event pump)" : "from the camera update (event pump not hooked)");
    paperdoll::install(g_config, module);
    camera::set_frame_callback(&on_game_frame);
    bridge::start(g_config, [module](const bridge::Request& request) { handle_bridge(module, request); });
    bridge::reset_status(); // a fresh start: nothing from a previous run counts
    bridge::set_status("xr", "off");

    health::log_summary("start-up; the headset and the UI panel report when they start");
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
