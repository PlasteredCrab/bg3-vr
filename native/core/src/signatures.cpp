#include "bg3vr/signatures.hpp"

#include "bg3vr/text.hpp"

#include <format>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace bg3vr::sig {

namespace {

std::mutex g_registry_mutex;
std::unordered_map<std::string, std::uintptr_t> g_registry;

std::vector<std::string_view> split_bars(std::string_view s) {
    std::vector<std::string_view> parts;
    while (true) {
        const auto bar = s.find('|');
        parts.push_back(text::trim(s.substr(0, bar)));
        if (bar == std::string_view::npos) {
            return parts;
        }
        s.remove_prefix(bar + 1);
    }
}

} // namespace

std::optional<SignatureSpec> parse_line(std::string_view line, std::string* error) {
    const auto fail = [error](std::string message) -> std::optional<SignatureSpec> {
        if (error) {
            *error = std::move(message);
        }
        return std::nullopt;
    };

    line = text::trim(line);
    if (line.empty() || line.front() == '#' || line.front() == ';') {
        return std::nullopt;
    }

    const auto eq = line.find('=');
    if (eq == std::string_view::npos) {
        return fail("missing '='");
    }

    SignatureSpec spec;
    spec.name = std::string(text::trim(line.substr(0, eq)));
    if (spec.name.empty()) {
        return fail("missing name");
    }

    const auto parts = split_bars(line.substr(eq + 1));
    spec.pattern_text = std::string(parts.front());
    auto pattern = parse_pattern(spec.pattern_text);
    if (!pattern) {
        return fail(std::format("{}: malformed pattern", spec.name));
    }
    spec.pattern = std::move(*pattern);

    for (std::size_t i = 1; i < parts.size(); ++i) {
        const auto words = text::split_whitespace(parts[i]);
        if (words.size() == 2 && words[0] == "offset") {
            const auto offset = text::parse_int(words[1]);
            if (!offset) {
                return fail(std::format("{}: bad offset '{}'", spec.name, words[1]));
            }
            spec.offset = *offset;
        } else if (words.size() == 3 && words[0] == "rip") {
            const auto disp_offset = text::parse_int(words[1]);
            const auto instr_len = text::parse_int(words[2]);
            if (!disp_offset || !instr_len || *disp_offset < 0 || *instr_len <= *disp_offset) {
                return fail(std::format("{}: bad rip modifier '{}'", spec.name, parts[i]));
            }
            spec.rip = RipSpec{static_cast<std::size_t>(*disp_offset), static_cast<std::size_t>(*instr_len)};
        } else {
            return fail(std::format("{}: unknown modifier '{}'", spec.name, parts[i]));
        }
    }
    return spec;
}

std::vector<SignatureSpec> load_file(const std::filesystem::path& path, std::vector<std::string>* errors) {
    std::vector<SignatureSpec> specs;
    std::ifstream in(path);
    if (!in) {
        if (errors) {
            errors->push_back(std::format("cannot open {}", path.string()));
        }
        return specs;
    }
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        std::string error;
        if (auto spec = parse_line(line, &error)) {
            specs.push_back(std::move(*spec));
        } else if (!error.empty() && errors) {
            errors->push_back(std::format("line {}: {}", number, error));
        }
    }
    return specs;
}

ScanResult scan(const SignatureSpec& spec, std::span<const std::uint8_t> region, std::uintptr_t region_base) {
    ScanResult result;
    const auto hits = find_all(region, spec.pattern, kMaxHits);
    result.hit_count = hits.size();
    if (hits.size() != 1) {
        return result;
    }
    std::uintptr_t address = static_cast<std::uintptr_t>(static_cast<long long>(region_base + hits.front()) + spec.offset);
    if (spec.rip) {
        address = resolve_rip(address, spec.rip->disp_offset, spec.rip->instr_len);
    }
    result.address = address;
    return result;
}

void publish(const std::string& name, std::uintptr_t address) {
    std::scoped_lock lock(g_registry_mutex);
    g_registry[name] = address;
}

std::optional<std::uintptr_t> lookup(std::string_view name) {
    std::scoped_lock lock(g_registry_mutex);
    const auto it = g_registry.find(std::string(name));
    if (it == g_registry.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace bg3vr::sig
