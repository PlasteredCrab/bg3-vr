#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace bg3vr {

// Minimal INI reader: [section] headers, key = value lines, '#' or ';' comment lines.
// Sections and keys are case-insensitive.
class Config {
public:
    bool load_file(const std::filesystem::path& path);
    void load_string(std::string_view content);

    [[nodiscard]] std::string get(std::string_view section, std::string_view key, std::string_view fallback = {}) const;
    [[nodiscard]] bool get_bool(std::string_view section, std::string_view key, bool fallback) const;
    [[nodiscard]] long long get_int(std::string_view section, std::string_view key, long long fallback) const;

private:
    std::map<std::string, std::string> values_;
};

} // namespace bg3vr
