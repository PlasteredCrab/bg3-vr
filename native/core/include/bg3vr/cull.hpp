#pragma once

#include "bg3vr/module_info.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// Hiding visuals from the main view (the first-person head), using the game's own
// MoveableObject::ToggleCullFlag600. Calls run on the game thread from the camera's per-frame
// callback; objects are re-validated before every call, so a destroyed visual is dropped.
namespace bg3vr::cull {

// MoveableObject vtable (Visual vtable 0x588CB30 in 4.1.1.7631656, verified by disassembly; the
// Script Extender lists both slots one later).
namespace layout {
inline constexpr std::size_t kGetCullFlagsSlot = 13;      // uint16_t GetCullFlags(uint16_t&) = word [rcx+0xF4]
inline constexpr std::size_t kToggleCullFlag600Slot = 15; // set ? flags |= 0x600 : flags &= ~0x600, recursive
inline constexpr std::size_t kCullFlags = 0xF4;
inline constexpr std::uint16_t kMainViewBits = 0x600;     // an inclusion mask: set = drawn in the main view
} // namespace layout

void init(const ModuleInfo& module);

// Replaces the hidden set: each object is checked first (vtable in .rdata, slot functions in .text,
// ToggleCullFlag600 containing its 0x600 constant). Objects hidden before are shown again.
void set_hidden(const std::vector<std::uintptr_t>& objects);

// Game thread, at every view update. Throttled: does work only when a request is pending or every
// 32nd call (the game hasn't been seen to restore the flags).
void on_frame();

void report();

} // namespace bg3vr::cull
