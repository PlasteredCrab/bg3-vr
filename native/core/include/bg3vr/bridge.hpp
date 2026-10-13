#pragma once

#include "bg3vr/camera.hpp"
#include "bg3vr/config.hpp"
#include "bg3vr/views.hpp"
#include "bg3vr/pose.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

// The Lua -> native bridge file (Phase 0 form; ADR 0002 describes its successor). Lua writes it with
// Ext.IO.SaveFile; the core polls it and hands each new version to a callback. Only keys present in
// a write are acted on.
namespace bg3vr::bridge {

struct Request {
    std::optional<std::uintptr_t> controller;                  // camera controller to drive
    std::optional<camera::Pose> pose;                          // pose to hold
    bool pose_off = false;                                     // release the camera
    bool probe = false;                                        // run the RE probes on the controller
    std::optional<bool> mouselook;                             // middle-drag turns the held pose
    std::optional<std::optional<camera::Sweep>> sweep;         // present: start (value) or stop (nullopt)
    std::optional<std::optional<camera::EyeFov>> eye_fov;      // present: per-eye projection or game's (nullopt)
    std::optional<std::vector<std::uintptr_t>> hide;           // visuals to hide; empty = show all again
    std::optional<std::optional<camera::Follow>> follow;       // present: follow (value) or stop (nullopt)
    bool recenter = false;                                     // headset: take the current head pose as centre
    std::optional<bool> views_find;
    std::optional<std::string> trace;                          // on | off | dump
    std::optional<std::size_t> views_build;                    // build stage lists for this view
    bool views_build_compositing = false;                      // ... including 0x45 / 0x07
    bool views_refs = false;                                   // what points at each view
    bool views_diff = false;                                   // small differences between view 0 and 1
    std::optional<views::Layout> views_layout;                 // layout experiment
    struct ViewSet {
        std::size_t index = 0;
        std::optional<views::Placement> placement;
        std::optional<bool> enabled;
    };
    std::optional<ViewSet> view_set;                           // one view's placement or Enabled flag                            // render views: true = scan, false = dump
    std::optional<int> xr_lag;                                 // frames between camera update and present; -1 = auto
};

// Parses the bridge file:
//   controller=<hex address>
//   pose=<x> <y> <z> <yaw> <pitch>   |   pose=off
//   probe=1
//   hide=<hex>,<hex>,...             |   hide=off
//   mouselook=on                     |   mouselook=off
//   sweep=on | sweep=<yaw> [<pitch> [<seconds>]] | sweep=off
//   fov=<left> <right> <up> <down>   |   fov=off              (degrees; left and down negative)
//   xr_lag=<0..7>                    |   xr_lag=auto
//   views=find | views=dump | views=sbs | views=swap | views=full | views=refs | views=diff
//   views=build [<index>] [full]
//   views=set <index> <scale x> <scale y> <offset x> <offset y>   |   views=enable <index> <0|1>
//   follow=<hex transform> [height]  |   follow=off
//   recenter=1
//   trace=on | trace=off | trace=dump
Request parse(std::string_view content);

// Starts the watcher thread ([camera] bridge / bridge_file in bg3vr.ini). `on_request` runs on that
// thread for every new version of the file.
void start(const Config& config, std::function<void(const Request&)> on_request);

} // namespace bg3vr::bridge
