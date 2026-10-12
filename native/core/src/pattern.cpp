#include "bg3vr/pattern.hpp"

#include "bg3vr/text.hpp"

#include <charconv>
#include <cstring>

namespace bg3vr {

std::optional<Pattern> parse_pattern(std::string_view text) {
    Pattern pattern;
    bool any_fixed = false;
    for (const std::string_view token : text::split_whitespace(text)) {
        if (token == "?" || token == "??") {
            pattern.bytes.push_back(0);
            pattern.mask.push_back(false);
            continue;
        }
        if (token.size() != 2) {
            return std::nullopt;
        }
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + 2, value, 16);
        if (ec != std::errc{} || ptr != token.data() + 2) {
            return std::nullopt;
        }
        pattern.bytes.push_back(static_cast<std::uint8_t>(value));
        pattern.mask.push_back(true);
        any_fixed = true;
    }
    if (!any_fixed) {
        return std::nullopt;
    }
    return pattern;
}

std::vector<std::size_t> find_all(std::span<const std::uint8_t> haystack, const Pattern& pattern, std::size_t max_hits) {
    std::vector<std::size_t> hits;
    const std::size_t length = pattern.size();
    if (length == 0 || haystack.size() < length || max_hits == 0) {
        return hits;
    }

    // Anchor on the first fixed byte and let memchr skip ahead to candidates.
    std::size_t anchor = 0;
    while (anchor < length && !pattern.mask[anchor]) {
        ++anchor;
    }
    if (anchor == length) {
        return hits;
    }

    const std::uint8_t* data = haystack.data();
    const std::size_t last_anchor = haystack.size() - length + anchor;
    std::size_t pos = anchor;
    while (pos <= last_anchor) {
        const void* found = std::memchr(data + pos, pattern.bytes[anchor], last_anchor - pos + 1);
        if (!found) {
            break;
        }
        const std::size_t at = static_cast<std::size_t>(static_cast<const std::uint8_t*>(found) - data);
        const std::size_t start = at - anchor;
        bool match = true;
        for (std::size_t k = 0; k < length; ++k) {
            if (pattern.mask[k] && data[start + k] != pattern.bytes[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            hits.push_back(start);
            if (hits.size() >= max_hits) {
                break;
            }
        }
        pos = at + 1;
    }
    return hits;
}

std::optional<std::size_t> find_first(std::span<const std::uint8_t> haystack, const Pattern& pattern) {
    const auto hits = find_all(haystack, pattern, 1);
    if (hits.empty()) {
        return std::nullopt;
    }
    return hits.front();
}

std::uintptr_t resolve_rip(std::uintptr_t instr, std::size_t disp_offset, std::size_t instr_len) {
    std::int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(instr + disp_offset), sizeof(disp));
    return static_cast<std::uintptr_t>(static_cast<std::intptr_t>(instr + instr_len) + disp);
}

} // namespace bg3vr
