#pragma once

#include "bg3vr/camera.hpp"
#include "bg3vr/config.hpp"
#include "bg3vr/views.hpp"
#include "bg3vr/pose.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
    bool views_jobs = false;                                   // dump the render views' job counters and stages
    bool views_events = false;                                 // log the renderer's events for a few frames
    bool views_gpu = false;
    bool views_create = false;                                 // the game's own player view (0x17F82F0) as the split view                                    // ... and the GPU draw runs (render targets, viewports)
    std::optional<views::Layout> views_layout;                 // layout experiment
    struct ViewSet {
        std::size_t index = 0;
        std::optional<views::Placement> placement;
        std::optional<bool> enabled;
    };
    std::optional<ViewSet> view_set;                           // one view's placement or Enabled flag                            // render views: true = scan, false = dump
    std::optional<int> xr_lag;                                 // frames between camera update and present; -1 = auto
    std::optional<std::string> stereo;                         // same-frame stereo: on | off | report
    std::optional<std::string> hands;                          // controllers: report | buzz
    std::optional<std::uintptr_t> skeleton;                    // follow this visual's skeleton; 0 = off
    std::optional<std::string> ray;                            // raycast probe: camera | hand | off | report
    bool quality_report = false;                               // log LOD / texture quality state
    std::optional<bool> pick_hand;                             // picking ray from the right hand (true) or mouse
    std::optional<bool> move_stick;                            // thumbstick locomotion on/off
    std::optional<std::string> vr;                             // VR play: start | stop | pause (level unloading)
    std::optional<bool> ik;                                    // arm IK on/off
    std::optional<bool> third_person;                          // view=third (true) | view=first (false)
    std::vector<std::pair<std::string, std::string>> settings; // set=<section>.<key> <value> (the VR menu)
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
//   views=find | views=dump | views=sbs | views=swap | views=full | views=refs | views=diff | views=jobs | views=events | views=gpu | views=create
//   views=build [<index>] [full]
//   views=set <index> <scale x> <scale y> <offset x> <offset y>   |   views=enable <index> <0|1>
//   follow=<hex transform> [height]  |   follow=off
//   recenter=1
//   stereo=on | stereo=off | stereo=report   (same-frame stereo on the built split view)
//   hands=report | hands=buzz        (log the motion controllers; buzz also pulses both)
//   skeleton=<hex visual> | skeleton=off   (read the character's skeleton: bones, Head_M)
//   ray=camera | ray=hand | ray=off | ray=report   (physics raycast probe)
//   quality=report                    (LOD and texture streaming state per view)
//   pick=hand | pick=off              (the game's picking ray from the right hand's aim, or the mouse)
//   move=stick | move=off             (left thumbstick locomotion through the game's movement override)
//   vr=start | vr=stop | vr=pause     (VR play: [vr] profile, bindings, turning, eye height; pause = loading)
//   ik=on | ik=off                    (arms follow the controllers)
//   view=first | view=third           (first person, or the game's camera on a flat screen)
//   set=<section>.<key> <value>       (change a setting now and save it to menu.ini; may repeat)
//   trace=on | trace=off | trace=dump
Request parse(std::string_view content);

// Starts the watcher thread ([camera] bridge / bridge_file in bg3vr.ini). `on_request` runs on that
// thread for every new version of the file.
void start(const Config& config, std::function<void(const Request&)> on_request);

// The other direction (docs/decisions/0002-native-lua-bridge.md): `to_lua.txt` next to the bridge file holds the
// core's state as key=value lines, which the Lua side reads with Ext.IO.LoadFile("BG3VR/to_lua.txt"). set_status
// changes one key and rewrites the file atomically (any thread; written once start() knows the path). An empty
// value removes the key. reset_status() clears every key (start-up: nothing from a previous run counts).
void set_status(std::string_view key, std::string_view value);

// Lines in the bridge file's form from inside the core (e.g. the VR menu's set= lines): handled on the watcher
// thread like a file write, within 250 ms, in the order posted.
void post(std::string lines);
void reset_status();

} // namespace bg3vr::bridge
