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
// The latest right-hand aim pose in the game world (any thread), or nullopt.
std::optional<vr::RigidPose> hand_aim();

// Game thread (camera view update): casts every few updates while a source is set; logs changes.
void on_frame();

// The aiming beam (headset): while on, every game frame casts the pointing hand's aim ray against the ground (the
// picking ground job's masks 5 / 0x820 / 0x194280) and characters and items (0xF / 0x4CA20 / 0x210080), up to
// `range` metres; beam_distance() is the nearer hit (game units), nullopt when nothing is hit.
void set_beam(bool on, float range);
// The client's main physics scene (phx::PhysXScene), or 0 while no level is loaded.
std::uintptr_t main_scene();
std::optional<float> beam_distance();

// One raycast through the game's RaycastClosest (game thread). Masks as at the game's call sites; the defaults are
// the picking objects job's (characters and items, agent I 1.4).
struct Hit {
    vr::Vec3 position{};
    vr::Vec3 normal{};
    float distance = 0.0f;           // metres from `from`
    std::uintptr_t wrapper = 0;      // Larian per-actor wrapper (PxActor userData)
    std::uintptr_t container = 0;    // wrapper+0x38
    std::uint64_t entity = 0;        // container+0x20, 0xFFC0000000000000 = none
    std::uint16_t actor_type = 0xFFFF; // PhysX concrete type: 5 dynamic, 6 static, 13 articulation link
};
inline constexpr std::uint64_t kNullEntity = 0xFFC0000000000000ull;
std::optional<Hit> raycast(const vr::Vec3& from, const vr::Vec3& to, std::uint32_t query_flags = 0xF,
                           std::uint32_t group = 0x4CA20, std::uint32_t exclude = 0x210080);

} // namespace bg3vr::rayprobe
