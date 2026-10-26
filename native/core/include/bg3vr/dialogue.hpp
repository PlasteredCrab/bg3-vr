#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

#include <cstddef>
#include <cstdint>

// Keeping NPC conversations in first person (agent G, re-local/ghidra/agents/G_camera.md). Every dialogue/timeline
// camera switch goes through 0x2007570, which skips the view switch when the timeline's byte +0x4C5 (SE
// InGameTimelineFreeCamEnabled; nothing in the exe writes it) is set. A mid-hook on 0x1D5B890 (a player joins a
// timeline: r8 = ecl::Timeline*, r9 = timeline data with the type at +0x78) logs every timeline and, while the
// mod drives the camera, sets +0x4C5 for the types in [dialogue] first_person_types (default "1"), so the gameplay
// camera (and the headset) stays on the view. Other types (cutscenes, combat cinematics) keep their cameras.
namespace bg3vr::dialogue {

// 4.1.1.7631656 (agent G; entry and the +0x4C5 test re-disassembled 2026-10-19).
namespace layout {
inline constexpr std::size_t kDataType = 0x78;          // u8 TimelineType
inline constexpr std::size_t kDataPlayback = 0x79;      // u8 PlaybackState
inline constexpr std::size_t kDataInstance = 0x60;      // DialogInstanceId (SE)
inline constexpr std::size_t kDataGate = 0x74;          // u32; the function returns at once when 0
inline constexpr std::size_t kTimelinePreview = 0x488;  // IsPreviewTimeline (SE)
inline constexpr std::size_t kTimelineGameplay = 0x489; // set for types 0, 2, 3, 7, 9
inline constexpr std::size_t kTimelineCombat = 0x48A;   // +0x48A systemic / +0x48B authored combat
inline constexpr std::size_t kTimelineFreeCam = 0x4C5;  // skip the view camera switch
inline constexpr std::size_t kTimelineCamera = 0x4E0;   // current camera actor; +0x4F0 override
} // namespace layout

void install(const Config& config, const ModuleInfo& module);
// [dialogue] scene_lighting (the VR menu changes it live).
void configure(const Config& config);
void report();

} // namespace bg3vr::dialogue
