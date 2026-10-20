#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/pose.hpp"
#include "bg3vr/vr_math.hpp"

#include <array>
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
inline constexpr std::size_t kFarPlane = 0x1C;        // float
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

// Installs the hooks (signatures camera_view_update, camera_set_view_call and, for set_eye_fov,
// camera_set_projection_call). [camera] force_view in
// bg3vr.ini also overwrites the built view matrix at the setter (the Phase 0 method).
void install(const Config& config, const ModuleInfo& module);

// Checks `controller` against the layout. On success it becomes the driven camera; on failure no
// camera is driven. Logs what it found either way.
bool attach(std::uintptr_t controller);
std::uintptr_t controller();

// The pose to hold the driven camera at; nullopt releases it.
void set_pose(std::optional<Pose> pose);
// True while a pose is held (the mod drives the camera).
bool pose_held();

// Replaces the driven camera's projection with an asymmetric per-eye one (in the game's reversed-Z
// form, using the controller's own near and far planes) at the projection setter call
// (signature camera_set_projection_call); nullopt goes back to the game's projection.
void set_eye_fov(std::optional<EyeFov> fov);

// Automatic look-around while a pose is held (overrides mouse look); nullopt stops it and returns the
// view to where the sweep started.
void set_sweep(std::optional<Sweep> sweep);

// Desktop stand-in for head rotation: while a pose is held, holding the middle mouse button and
// dragging turns it ([camera] mouselook / mouselook_sensitivity in bg3vr.ini; on by default).
void set_mouselook(bool enabled);

// Following the character while a pose is held: every view update reads the character's position
// from its ls::TransformComponent (rotation quaternion at +0x00, translate at +0x10) and moves the
// held pose there, `height` above it. The held yaw is kept, so walking doesn't turn the view (the
// player turns with their head). Implausible reads (not finite, or a jump of more than 10 units
// in one update) are ignored and counted.
struct Follow {
    std::uintptr_t transform = 0;
    float height = 1.7f;
};
// nullopt stops following (the pose stays where it is).
void set_follow(std::optional<Follow> follow);
// Changes the height above the followed transform (e.g. calibrated from the head bone); no-op when not following.
void set_follow_height(float height);
// [xr] world_scale (game units per real metre of head and hand movement); live from the VR menu.
void set_world_scale(float scale);
float world_scale();
// The followed transform's last accepted position (game world), or nullopt when not following.
std::optional<vr::Vec3> follow_position();
// Turns the held pose's yaw (snap / smooth turn): with head tracking this rotates the whole tracked space around
// the anchor. No-op while no pose is held.
void add_yaw(float degrees);
// Head tracking (Phase 1 step 3). A view located by OpenXR, in OpenXR's LOCAL space and convention:
// while a pose is held, the camera is placed at that pose's position and yaw (vr::anchor) plus this
// view (vr::from_openxr, position times [xr] world_scale), with full head rotation, and the
// projection becomes `fov`. `eye` is carried through to say which eye image the frame is for.
struct HeadView {
    vr::RigidPose pose; // OpenXR convention
    EyeFov fov;
    int eye = 0;
    // Both eyes as located with this view (OpenXR convention, same space), for same-frame stereo: the
    // poses each eye's image is submitted with.
    std::array<vr::RigidPose, 2> eye_pose{};
    std::array<EyeFov, 2> eye_fov{};
};
// nullopt stops head tracking (back to the held pose and the game's projection).
void set_head(std::optional<HeadView> view);

// Recentering: head views are applied relative to origin (OpenXR convention, like HeadView::pose):
// its position becomes the anchor point and its yaw becomes the held pose's forward. Only the yaw
// of its orientation is used, so the horizon stays level.
void set_recenter(const vr::RigidPose& origin);

// A tracked pose (e.g. a controller's grip, OpenXR convention and LOCAL space, like HeadView::pose) in the
// game world, placed exactly as the head is: same recentering, world scale and anchor (the held pose at
// the last view update). nullopt while no pose is held.
std::optional<vr::RigidPose> tracked_to_world(const vr::RigidPose& xr_pose);

// A head view as the driven camera actually used it: `serial` counts view updates of the driven
// camera, and `view_matrix` is the world-to-camera matrix written for it, to recognise the frame later.
struct AppliedView {
    HeadView head;
    Mat4 view_matrix{};
    std::uint64_t serial = 0;
};
inline constexpr std::size_t kAppliedHistory = 8;
// The last `kAppliedHistory` applied head views, newest first; returns how many were written.
std::size_t applied_views(std::array<AppliedView, kAppliedHistory>& out);

// Called on the game thread at the start of every view update (several per frame), before the pose
// is applied. Used for per-frame work that must run on the game thread.
using FrameCallback = void (*)();
void set_frame_callback(FrameCallback callback);

// Logs the hook counters and every controller seen in the view update, then resets the per-report
// counters.
void report();

} // namespace bg3vr::camera
