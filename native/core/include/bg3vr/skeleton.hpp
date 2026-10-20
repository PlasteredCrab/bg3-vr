#pragma once

#include "bg3vr/module_info.hpp"
#include "bg3vr/vr_math.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

// The controlled character's skeleton, read-only (Phase 2 groundwork: first-person eye point, body IK).
// From a character's root ls::Visual: the default skeleton slot (Visual+0x88 array of SkeletonSlot*,
// count byte +0x91; slot {+0 FixedString remapper slot, -1 = default; +8 Skeleton*}), validated by the
// skeleton vtable. Bones are read straight from the Granny skeleton; the head bone's matrix through the
// skeleton's own GetBoneWorldMatrix. See re-local/ghidra/agents/C_animation_systems.md sections 1.4-1.6.
namespace bg3vr::skeleton {

// 4.1.1.7631656. The vtables, the frame counter, the skeleton and Granny offsets and slot 10 are confirmed by
// disassembly (agent C report; slots 1, 10, 18, 19 re-checked with bg3vr_disasm). The visual's slot array and
// world transform are the Script Extender's layout, used the same way by 0x3C52E10 / 0x3C6B900 (slots) and
// checked at run time by the logs (world transform).
namespace layout {
inline constexpr std::uintptr_t kVisualVtable = 0x588CB30;   // ls::Visual
inline constexpr std::uintptr_t kSkeletonVtable = 0x588D4A8; // the game's Skeleton class
inline constexpr std::uintptr_t kFrameCounter = 0x614FB88;   // uint32, the game loop's frame counter
inline constexpr std::size_t kVisualWorldTransform = 0x08;   // Transform: quat +0x00, translate +0x10, scale +0x1C
inline constexpr std::size_t kVisualSkeletonSlots = 0x88;    // SkeletonSlot**
inline constexpr std::size_t kVisualSkeletonSlotCount = 0x91; // uint8
inline constexpr std::size_t kSlotSkeleton = 0x08;
inline constexpr std::size_t kSkeletonGranny = 0xD8;         // granny_skeleton*
inline constexpr std::size_t kSkeletonBoneCount = 0x98;      // int16
inline constexpr std::size_t kSkeletonPoseFrame = 0x124;     // uint32: frame the pose was last built
inline constexpr std::size_t kGrannyBoneCount = 0x08;        // int32 (compared as int16 by the game)
inline constexpr std::size_t kGrannyBones = 0x0C;            // granny_bone* (packed, unaligned)
inline constexpr std::size_t kGrannyBoneStride = 0xA4;
inline constexpr std::size_t kGrannyBoneName = 0x00;         // char*
inline constexpr std::size_t kGrannyBoneParent = 0x08;       // int16, -1 = root
inline constexpr std::size_t kGetBoneWorldMatrixSlot = 10;   // bool (Skeleton*, int16 bone, float out[16]); model space
inline constexpr std::uintptr_t kGetBoneWorldMatrix = 0x3A1F170; // what slot 10 points to
inline constexpr std::uintptr_t kUpdateMeshBindings = 0x3A20590; // slot 30, the body-IK override point
} // namespace layout

// Also installs a counting hook on Skeleton slot 30 (signature skeleton_update_mesh_bindings, the body-IK
// override point) that records, for the followed skeleton only, how often and on which threads it runs and
// whether it runs before or after on_frame's head read in the same game frame. Read-only.
void init(const ModuleInfo& module);

// Follow the skeleton of this ls::Visual (the character's root visual); 0 stops. The skeleton is found and
// its bones logged on the next game frame.
void attach_visual(std::uintptr_t visual);

// Game thread, every view update: re-validates, reads the head bone, logs periodically.
void on_frame();

// The head bone (Head_M) of the followed skeleton at the last game frame: in model space (relative to the
// visual, game units) and placed in the world with the visual's world transform.
struct Head {
    vr::Vec3 model{};
    vr::Vec3 world{};
    vr::Quat world_rotation{0.0f, 0.0f, 0.0f, 1.0f}; // the head bone's orientation in the world
    std::uint32_t frame = 0;                         // game frame counter when read
    bool fresh = false;                              // the pose was built this frame or the last
    // The eyes (Dummy_EyeFX_01 / _02, children of Head_M), when the skeleton has them: their midpoint in the world
    // and how far apart they are (game units).
    bool has_eyes = false;
    vr::Vec3 eyes_world{};
    float eye_separation = 0.0f;
};
std::optional<Head> head();

// Increases every time a skeleton is (re)attached; 0 = none yet. Lets consumers redo per-skeleton calibration.
std::uint64_t generation();

void report();

} // namespace bg3vr::skeleton
