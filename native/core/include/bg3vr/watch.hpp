#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

// Reverse-engineering probes that run inside the game for a short, bounded time.
namespace bg3vr::watch {

inline constexpr std::size_t kMaxWatchAddresses = 4; // x64 has four debug-address registers
inline constexpr std::size_t kCallerDepth = 6;

struct WriteHit {
    std::size_t watch_index = 0;                     // which of the watched addresses was written
    std::uintptr_t rip = 0;                          // instruction after the write
    std::uint32_t count = 0;                         // times this rip hit during the watch
    std::array<std::uintptr_t, kCallerDepth> callers{}; // return addresses, innermost first
};

// Hardware write breakpoints on [address, address + 8) for each address (8-byte aligned, at most
// four), set on every thread of the process for `duration`. Returns each distinct writing
// instruction with its count and the call stack of its first hit.
std::vector<WriteHit> watch_writes(std::span<const std::uintptr_t> addresses, std::chrono::milliseconds duration);

// Counts calls to each function for `duration` with mid-function hooks at their entry points.
// A function that can't be hooked (too short, already hooked) reports UINT64_MAX.
std::vector<std::uint64_t> count_calls(std::span<const std::uintptr_t> functions, std::chrono::milliseconds duration);

} // namespace bg3vr::watch
