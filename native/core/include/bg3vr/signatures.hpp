#pragma once

#include "bg3vr/pattern.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bg3vr::sig {

struct RipSpec {
    std::size_t disp_offset = 0;
    std::size_t instr_len = 0;
};

// One line of signatures.txt:
//   name = <pattern> [| offset <n>] [| rip <disp_offset> <instr_len>]
// offset is applied to the match first, then rip resolves from that address.
struct SignatureSpec {
    std::string name;
    std::string pattern_text;
    Pattern pattern;
    long long offset = 0;
    std::optional<RipSpec> rip;
};

// Returns nullopt with *error empty for blank and comment lines, nullopt with *error set on failure.
std::optional<SignatureSpec> parse_line(std::string_view line, std::string* error);
std::vector<SignatureSpec> load_file(const std::filesystem::path& path, std::vector<std::string>* errors);

struct ScanResult {
    std::size_t hit_count = 0;                // capped at kMaxHits
    std::optional<std::uintptr_t> address;    // set only for exactly one hit
};
inline constexpr std::size_t kMaxHits = 16;

// `region` is readable memory starting at address `region_base`.
ScanResult scan(const SignatureSpec& spec, std::span<const std::uint8_t> region, std::uintptr_t region_base);

// Registry of resolved addresses that later hooks look up by name.
void publish(const std::string& name, std::uintptr_t address);
std::optional<std::uintptr_t> lookup(std::string_view name);

} // namespace bg3vr::sig
