#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bg3vr {

// An IDA-style byte pattern such as "48 8B 05 ?? ?? ?? ??". "?" and "??" are wildcards.
struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<bool> mask; // true = byte must match

    [[nodiscard]] std::size_t size() const { return bytes.size(); }
};

// Returns nullopt for malformed tokens or a pattern with no fixed bytes.
std::optional<Pattern> parse_pattern(std::string_view text);

std::vector<std::size_t> find_all(std::span<const std::uint8_t> haystack, const Pattern& pattern, std::size_t max_hits = 16);
std::optional<std::size_t> find_first(std::span<const std::uint8_t> haystack, const Pattern& pattern);

// Resolves a RIP-relative operand: instr + instr_len + disp32 read at (instr + disp_offset).
std::uintptr_t resolve_rip(std::uintptr_t instr, std::size_t disp_offset, std::size_t instr_len);

} // namespace bg3vr
