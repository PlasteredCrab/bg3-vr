#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/pose.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

// Driving the gameplay camera. The controller's view update (0x4085150 in 4.1.1.7631656) builds the
// view from GetWorldTransform / GetWorldTranslate, which for the gameplay camera return the camera
// entity's ECS Transform component; every system that asks the camera where it is reads the same
// component. At the start of the view update we write the requested pose into that Transform, so
// the view, its inverse, the frustum, culling and all other readers agree. See
// docs/re-notes/camera.md.
namespace bg3vr::camera {

// rf::CameraController layout: the Script Extender's field offsets, confirmed by disassembly.
namespace layout {
inline constexpr std::size_t kViewDirty = 0x10;       // bool
inline constexpr std::size_t kProjectionDirty = 0x11; // bool
inline constexpr std::size_t kIsOrthographic = 0x12;  // bool
inline constexpr std::size_t kOffCenter = 0x13;       // bool; only honoured for orthographic cameras
inline constexpr std::size_t kFov = 0x14;             // float, vertical, radians
inline constexpr std::size_t kNearPlane = 0x18;       // float
inline constexpr std::size_t kAspectRatio = 0x20;     // float
inline constexpr std::size_t kWorldTranslate = 0x34;  // base class: in-object Transform at 0x24 (quat, then vec3)
inline constexpr std::size_t kViewMatrix = 0x90;      // glm::mat4, column-major
inline constexpr std::size_t kInvViewMatrix = 0xD0;   // glm::mat4, camera-to-world
inline constexpr std::size_t kSize = 0x290;           // through FrustumPoints
inline constexpr std::size_t kVtableSlots = 24;
// virtual Transform const& GetWorldTransform(): quat (x, y, z, w) at +0x00, translate at +0x10.
inline constexpr std::size_t kGetWorldTransformSlot = 13;
inline constexpr std::size_t kTransformTranslate = 0x10;
} // namespace layout

// Installs the hooks (signatures camera_view_update and camera_set_view_call). [camera] force_view in
// bg3vr.ini also overwrites the built view matrix at the setter (the Phase 0 method).
void install(const Config& config, const ModuleInfo& module);

// Checks `controller` against the layout. On success it becomes the driven camera; on failure no
// camera is driven. Logs what it found either way.
bool attach(std::uintptr_t controller);
std::uintptr_t controller();

// The pose to hold the driven camera at; nullopt releases it.
void set_pose(std::optional<Pose> pose);

// Called on the game thread at the start of every view update (several per frame), before the pose
// is applied. Used for per-frame work that must run on the game thread.
using FrameCallback = void (*)();
void set_frame_callback(FrameCallback callback);

// Logs the hook counters and every controller seen in the view update, then resets the per-report
// counters.
void report();

} // namespace bg3vr::camera
