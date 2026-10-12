#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bg3vr::text {

inline std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

inline std::string to_lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline std::vector<std::string_view> split_whitespace(std::string_view s) {
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
        }
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') {
            ++j;
        }
        if (j > i) {
            words.push_back(s.substr(i, j - i));
        }
        i = j;
    }
    return words;
}

// Decimal or 0x-prefixed hex, with an optional leading '-'.
inline std::optional<long long> parse_int(std::string_view s) {
    s = trim(s);
    bool negative = false;
    if (!s.empty() && s.front() == '-') {
        negative = true;
        s.remove_prefix(1);
    }
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }
    if (s.empty()) {
        return std::nullopt;
    }
    long long value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

} // namespace bg3vr::text
