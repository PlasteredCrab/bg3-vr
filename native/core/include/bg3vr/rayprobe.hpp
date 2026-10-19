#pragma once

#include "bg3vr/module_info.hpp"
#include "bg3vr/vr_math.hpp"

#include <cstdint>
#include <optional>

// Read-only physics raycast probe (physical interaction groundwork: what does the hand point at?). Casts a ray
// through the game's own RaycastClosest on the client level's main PhysX scene, from the game thread, and logs
// what it hits: position, normal, the Larian per-actor wrapper, its container, the PhysX actor type and the
// container's entity handle slot. Facts and evidence: re-local/ghidra/agents/D_physics.md section 8.
namespace bg3vr::rayprobe {

// 4.1.1.7631656.
namespace layout {
inline constexpr std::uintptr_t kLevelManager = 0x6007A28;   // ecl::gLevelManager*; +0x90 level (null while loading)
inline constexpr std::size_t kLevelManagerLevel = 0x90;
inline constexpr std::size_t kLevelScene = 0x30;             // main scene object
inline constexpr std::uintptr_t kPhxGlobal = 0x60490E8;      // phx global struct*; [0] = active scene (path C)
inline constexpr std::uintptr_t kMainSceneVtable = 0x5891B30;
inline constexpr std::size_t kRaycastSlot = 0x38;            // bool (scene, from, to, hit, 6 x u32, filter*)
inline constexpr std::uintptr_t kRaycastSlotTarget = 0x3A64990; // checked before use (disassembled 2026-10-19)
inline constexpr std::size_t kHitSize = 0x30;
inline constexpr std::size_t kHitNormal = 0x00;
inline constexpr std::size_t kHitPosition = 0x0C;
inline constexpr std::size_t kHitFraction = 0x18;            // FLT_MAX on a miss
inline constexpr std::size_t kHitGroup0 = 0x1C;
inline constexpr std::size_t kHitGroup1 = 0x20;
inline constexpr std::size_t kHitWrapper = 0x28;
inline constexpr std::size_t kWrapperContainer = 0x38;
inline constexpr std::size_t kWrapperActor = 0x40;           // PxRigidActor*
inline constexpr std::size_t kActorConcreteType = 0x08;      // uint16: 5 dynamic, 6 static, 13 articulation link
inline constexpr std::size_t kContainerEntity = 0x20;        // EntityHandle slot; 0xFFC0000000000000 = null
inline constexpr std::size_t kCameraToWorld = 0xD0;          // rf::CameraController InvViewMatrix (column-major)
} // namespace layout

void init(const ModuleInfo& module);

enum class Source { Off, Camera, Hand };
// Camera: from the attached camera controller along its view (+Z), i.e. the centre of the screen. Hand: along
// the right controller's aim ray, as placed in the world by the headset module (set_hand_aim).
void set_source(Source source);
void request_report();

// The right hand's aim pose in the game world (game convention: forward is +Z), or nullopt; from the headset.
void set_hand_aim(std::optional<vr::RigidPose> aim);

// Game thread (camera view update): casts every few updates while a source is set; logs changes.
void on_frame();

} // namespace bg3vr::rayprobe
