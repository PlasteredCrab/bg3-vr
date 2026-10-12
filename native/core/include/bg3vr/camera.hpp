#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bg3vr::camera {

// rf::CameraController field offsets, from the Script Extender's layout
// (GameDefinitions/Components/Camera.h). Checked against live values before any hook goes in.
namespace layout {
inline constexpr std::size_t kFov = 0x14;            // float, vertical, radians
inline constexpr std::size_t kNearPlane = 0x18;      // float
inline constexpr std::size_t kAspectRatio = 0x20;    // float
inline constexpr std::size_t kViewMatrix = 0x90;     // glm::mat4, column-major
inline constexpr std::size_t kInvViewMatrix = 0xD0;  // glm::mat4, camera-to-world
inline constexpr std::size_t kSize = 0x290;          // through FrustumPoints
inline constexpr std::size_t kUpdateSlot = 5;        // virtual void Update(GameTime const&)
inline constexpr std::size_t kVtableSlots = 24;
} // namespace layout

using Mat4 = std::array<float, 16>; // column-major, like glm

struct Pose {
    float x = 0, y = 0, z = 0;
    float yaw_deg = 0;   // about +Y; 0 looks along +Z
    float pitch_deg = 0; // positive looks up
};

// Camera-to-world and world-to-camera matrices for a camera that looks along +Z with +Y up,
// matching the game's gameplay camera.
void pose_matrices(const Pose& pose, Mat4& world, Mat4& view);

struct BridgeRequest {
    std::optional<std::uintptr_t> controller;
    std::optional<Pose> pose; // empty = no override
    bool pose_off = false;
};

// Parses the bridge file written by Client/CameraProbe.lua:
//   controller=<hex address>
//   pose=<x> <y> <z> <yaw> <pitch>   |   pose=off
BridgeRequest parse_bridge(std::string_view content);

// Starts the bridge watcher thread (Phase 0 camera experiment).
void start(const Config& config, const ModuleInfo& module);

} // namespace bg3vr::camera
