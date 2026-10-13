#pragma once

#include "bg3vr/config.hpp"
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
    std::optional<std::vector<std::uintptr_t>> hide;           // visuals to hide; empty = show all again
};

// Parses the bridge file:
//   controller=<hex address>
//   pose=<x> <y> <z> <yaw> <pitch>   |   pose=off
//   probe=1
//   hide=<hex>,<hex>,...             |   hide=off
//   mouselook=on                     |   mouselook=off
//   sweep=on | sweep=<yaw> [<pitch> [<seconds>]] | sweep=off
Request parse(std::string_view content);

// Starts the watcher thread ([camera] bridge / bridge_file in bg3vr.ini). `on_request` runs on that
// thread for every new version of the file.
void start(const Config& config, std::function<void(const Request&)> on_request);

} // namespace bg3vr::bridge
