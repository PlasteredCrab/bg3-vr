#pragma once

#include "bg3vr/module_info.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// Hiding visuals from the eye cameras (the first-person head) while they keep casting shadows. Calls run on the game
// thread from the camera's per-frame callback; objects are re-validated before every call, so a destroyed visual is
// dropped.
//
// A scene cull query lists the flag bits an object must have, all of them (agent AA, 0x3F1E940). The camera pass
// (CullStage) requires 0x0001 plus the view bit 0x200; every shadow pass (sun cascades, local lights, the distant
// shadow cache) requires 0x200 plus its own bit, never 0x0001. 0x0001 is the engine's own "not a shadow proxy" bit
// (UpdateCullFlags clears it for IsShadowProxy visuals). So clearing 0x0001 leaves the head out of both eyes and in
// all of its shadows. Older builds cleared 0x600 (ToggleCullFlag600), which removed the shadow too.
namespace bg3vr::cull {

// MoveableObject / Visual (vtable 0x588CB30 in 4.1.1.7631656, verified by disassembly).
namespace layout {
inline constexpr std::size_t kGetCullFlagsSlot = 13;      // uint16_t GetCullFlags(uint16_t&) = word [rcx+0xF4]
inline constexpr std::size_t kToggleCullFlag600Slot = 15; // set ? flags |= 0x600 : flags &= ~0x600, recursive
inline constexpr std::size_t kUpdateCullFlagsSlot = 23;   // 0x3C64730: recomputes 0x0001 / 0x803E / 0x80 / 0x800 from VisualFlags
inline constexpr std::uintptr_t kSetCullFlagsRva = 0x3C6B810;    // (visual, u16 mask, bool set), recursive into attachments
inline constexpr std::uintptr_t kUpdateCullFlagsRva = 0x3C64730; // slot 23 of the Visual class, post-hooked
inline constexpr std::size_t kCullFlags = 0xF4;
inline constexpr std::size_t kAttachments = 0xB8;      // array of 0x30-byte entries, the visual pointer first
inline constexpr std::size_t kAttachmentCount = 0xC4;  // u32
inline constexpr std::size_t kAttachmentStride = 0x30;
inline constexpr std::uint16_t kCameraBit = 0x0001;    // required by the camera cull only
inline constexpr std::uint16_t kMainViewBits = 0x600;  // the split-view bits: required by the camera and every shadow cull
} // namespace layout

// Checks the setter and UpdateCullFlags against this build and post-hooks UpdateCullFlags, so a recompute by the
// game (equipment changes) can't bring the head back for the up to 32 frames until the next check.
void init(const ModuleInfo& module);

// Replaces the hidden set: each object is checked first (vtable in .rdata, slot functions in .text). Objects hidden
// before are shown again.
void set_hidden(const std::vector<std::uintptr_t>& objects);

// Game thread, at every view update. Throttled: does work only when a request is pending or every 32nd call.
void on_frame();

// Drops every hidden object without showing it again (a level is unloading: the objects may be gone).
void forget();

void report();

} // namespace bg3vr::cull
