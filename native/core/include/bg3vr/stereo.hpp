#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"
#include "bg3vr/pose.hpp"
#include "bg3vr/vr_math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// Same-frame stereo (Phase 1 step 5): two render views drawn in one frame, each with its own eye.
//
// The game camera holds the head centre with a field of view covering both eyes (as in mono mode),
// so culling and everything game-side see one camera. Each render view copies that camera into its
// two GameRenderView::PerCamera blocks when it begins recording (0x3CC7490, called twice from
// 0x3CC6C00); a mid-hook right after both are built and before they're uploaded (0x3CC6CB9,
// signature render_view_camera_built; rdi = the view) rewrites them for the view's eye: the view
// matrix moved to the eye, the projection's x/y terms set to the eye's field of view (keeping the
// game's depth terms and its TAA/DLSS jitter), and the derived matrix and position recomputed the way
// the game computes them. See docs/re-notes/stereo.md.
//
// The same hook serves the one-view head-tracked modes (mono, alternate): block [1], what the frame is
// drawn with, carries a symmetric projection rebuilt from the controller's FOV and aspect, so the
// projection override at the controller never reached it. A view showing the driven camera gets the
// field of view of the head view it was built from (found by its view matrix in
// camera::applied_views), with no eye offset.
namespace bg3vr::stereo {

// GameRenderView::PerCamera, as 0x3CC7490 fills it (4.1.1.7631656; confirmed from the decompile,
// 2026-10-14). Two per view: [0] at view+0x2660 takes the controller's own projection
// (controller+0x110), [1] at view+0x2740 a projection rebuilt by 0x3CCCFD0 from the controller's FOV,
// aspect, near and far plus the frame's jitter sample: always symmetric, so its [8] and [9] are the
// jitter alone.
namespace layout {
inline constexpr std::size_t kPerCamera0 = 0x2660;
inline constexpr std::size_t kPerCamera1 = 0x2740;
inline constexpr std::size_t kView = 0x00;           // mat4: the controller's ViewMatrix (controller+0x90)
inline constexpr std::size_t kProjection = 0x40;     // mat4
inline constexpr std::size_t kViewProjection = 0x80; // mat4: projection * view without its translation
inline constexpr std::size_t kNear = 0xC0;           // float
inline constexpr std::size_t kFar = 0xC4;            // float
inline constexpr std::size_t kPosition = 0xD0;       // vec3: the controller's GetWorldTranslate
inline constexpr std::size_t kReadStateController = 0x21D8; // in the view: ReadState's CameraController*
// GameRenderView::PerView (400 bytes, b12 in the shaders), also filled by 0x3CC7490 (agent N, N_stereo_lighting.md
// 1.2): the inverse view and the far-plane rays to the full-screen triangle's corners, from which the clustered
// lighting, light assignment, volumetric fog and camera velocity shaders rebuild world positions.
inline constexpr std::size_t kPerView0 = 0x2340;
inline constexpr std::size_t kPerView1 = 0x24D0;
inline constexpr std::size_t kViewInverse = 0x30;  // mat4: camera to world
inline constexpr std::size_t kViewCorners = 0x70;  // 3 x vec4 (w 0): NDC (-1, 1), (3, 1), (-1, -3) at the far plane
inline constexpr std::size_t kOutlineGate = 0x2961; // byte: the outline combine runs only when 0 (0x17FCF10)
inline constexpr std::size_t kControllerIsOrthographic = 0x12; // in the controller (bool)
} // namespace layout

// --- Math (no game state; unit-tested) -------------------------------------------------------------

// The eye's pose in the head centre's frame, in game convention, from two OpenXR poses located in the
// same space; the offset is scaled by world_scale (game units per metre).
vr::RigidPose eye_in_head(const vr::RigidPose& centre_xr, const vr::RigidPose& eye_xr, float world_scale);

// World-to-eye matrix: the head's world-to-camera matrix moved to an eye at `eye_in_head`.
camera::Mat4 eye_view(const camera::Mat4& head_view, const vr::RigidPose& eye_in_head);

// The game's projection with its x/y terms ([0], [5], [8], [9]) replaced by the eye's field of view
// (degrees, OpenXR signs: left and down negative), its depth terms kept, and (jitter_x, jitter_y) added
// to [8] and [9] (the NDC offsets the game's jittered projection carries).
camera::Mat4 eye_projection(const camera::Mat4& game, const camera::EyeFov& fov, float jitter_x, float jitter_y);

// projection * view with the view's translation dropped: PerCamera+0x80 as 0x3CC7490 builds it.
camera::Mat4 rotation_view_projection(const camera::Mat4& projection, const camera::Mat4& view);

// The world position of a camera whose world-to-camera matrix is `view` (rigid).
vr::Vec3 camera_position(const camera::Mat4& view);

// Rewrites a PerView block's inverse view and corner rays from a (rewritten) PerCamera block: the corner at NDC
// (nx, ny) is far * (F + R (nx - P[8]) / P[0] + U (ny - P[9]) / P[5]) with R, U, F the view's rows (agent N 3);
// with the game's own symmetric projection this is exactly what 0x3CC7490 writes.
void rewrite_view_block(std::uint8_t* per_view, const std::uint8_t* per_camera);

// Rewrites one PerCamera block (as laid out above) for an eye: the block's view matrix is the head's.
// `jittered`: the block is [1], whose projection's [8]/[9] are jitter to keep.
void rewrite_block(std::uint8_t* block, const vr::RigidPose& eye_in_head, const camera::EyeFov& fov, bool jittered);

// --- Runtime ---------------------------------------------------------------------------------------

// Installs the hook (signature render_view_camera_built); [xr] world_scale as the camera module.
void install(const Config& config, const ModuleInfo& module);
// [xr] world_scale and [xr] ipd_mm (0 = the headset's own IPD, else the eyes are moved apart to that distance, in
// millimetres of the real world); again whenever the VR menu changes one (any thread).
void configure(const Config& config);

// The eyes for the frames being rendered: the head centre and both eyes as OpenXR located them (same
// space and time), and each eye's field of view. nullopt: leave the views alone.
struct Eyes {
    vr::RigidPose centre_xr;
    std::array<vr::RigidPose, 2> eye_xr;
    std::array<camera::EyeFov, 2> fov;
};
void set_eyes(std::optional<Eyes> eyes);

// Which render view (its address) draws which eye; eye -1 removes the view.
void assign(std::uintptr_t view, int eye);
// Forgets every assigned view.
void clear();
bool active();

// Logs the counters.
void report();

} // namespace bg3vr::stereo
