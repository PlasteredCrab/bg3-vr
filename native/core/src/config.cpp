#include "bg3vr/config.hpp"

#include "bg3vr/text.hpp"

#include <fstream>
#include <sstream>

namespace bg3vr {

namespace {

std::string make_key(std::string_view section, std::string_view key) {
    return text::to_lower(section) + "." + text::to_lower(key);
}

} // namespace

bool Config::load_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    load_string(buffer.str());
    return true;
}

void Config::load_string(std::string_view content) {
    std::string section;
    while (!content.empty()) {
        const auto eol = content.find('\n');
        std::string_view line = text::trim(content.substr(0, eol));
        content = eol == std::string_view::npos ? std::string_view{} : content.substr(eol + 1);

        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = std::string(text::trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        values_[make_key(section, text::trim(line.substr(0, eq)))] = std::string(text::trim(line.substr(eq + 1)));
    }
}

std::string Config::get(std::string_view section, std::string_view key, std::string_view fallback) const {
    const auto it = values_.find(make_key(section, key));
    return it == values_.end() ? std::string(fallback) : it->second;
}

bool Config::get_bool(std::string_view section, std::string_view key, bool fallback) const {
    const std::string value = text::to_lower(get(section, key));
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    return fallback;
}

long long Config::get_int(std::string_view section, std::string_view key, long long fallback) const {
    return text::parse_int(get(section, key)).value_or(fallback);
}

} // namespace bg3vr
