#include "bg3vr/bridge.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/win_util.hpp"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace bg3vr::bridge {

namespace {

std::optional<std::uintptr_t> parse_address(std::string_view item) {
    item = text::trim(item);
    if (item.starts_with("0x") || item.starts_with("0X")) {
        item.remove_prefix(2);
    }
    if (const auto address = text::parse_int(std::string("0x") + std::string(item))) {
        return static_cast<std::uintptr_t>(*address);
    }
    return std::nullopt;
}

std::filesystem::path default_path() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    // Ext.IO.SaveFile writes under the profile's "Script Extender" folder.
    return std::filesystem::path(std::wstring(buffer, length)) / L"Larian Studios" / L"Baldur's Gate 3" /
           L"Script Extender" / L"BG3VR" / L"camera_bridge.txt";
}

struct WatchArgs {
    std::filesystem::path path;
    std::function<void(const Request&)> on_request;
};

DWORD WINAPI watch(void* param) {
    const std::unique_ptr<WatchArgs> args(static_cast<WatchArgs*>(param));
    log::info("bridge: watching {}", to_utf8(args->path.wstring()));

    // A file left over from a previous session holds addresses from another process: only act on
    // writes made after we start.
    std::error_code ignored;
    std::filesystem::file_time_type last_write = std::filesystem::last_write_time(args->path, ignored);
    if (!ignored) {
        log::info("bridge: ignoring the existing file from a previous session");
    }
    for (;;) {
        Sleep(250);
        std::error_code ec;
        const auto write_time = std::filesystem::last_write_time(args->path, ec);
        if (ec || write_time == last_write) {
            continue;
        }
        last_write = write_time;

        std::ifstream in(args->path, std::ios::binary);
        std::stringstream content;
        content << in.rdbuf();
        args->on_request(parse(content.str()));
    }
}

} // namespace

Request parse(std::string_view content) {
    Request request;
    while (!content.empty()) {
        const auto eol = content.find('\n');
        const std::string_view line = text::trim(content.substr(0, eol));
        content = eol == std::string_view::npos ? std::string_view{} : content.substr(eol + 1);

        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string key = text::to_lower(text::trim(line.substr(0, eq)));
        const std::string_view value = text::trim(line.substr(eq + 1));

        if (key == "controller") {
            request.controller = parse_address(value);
        } else if (key == "hide") {
            std::vector<std::uintptr_t> objects;
            if (text::to_lower(value) != "off") {
                std::string_view rest = value;
                while (!rest.empty()) {
                    const auto comma = rest.find(',');
                    if (const auto address = parse_address(rest.substr(0, comma))) {
                        objects.push_back(*address);
                    }
                    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                }
            }
            request.hide = std::move(objects);
        } else if (key == "sweep") {
            const std::string flag = text::to_lower(value);
            if (flag == "off" || flag == "0") {
                request.sweep = std::optional<camera::Sweep>{};
            } else {
                camera::Sweep sweep;
                const auto words = text::split_whitespace(value);
                float* fields[] = {&sweep.yaw_deg, &sweep.pitch_deg, &sweep.period_s};
                bool ok = true;
                for (std::size_t i = 0; i < words.size() && i < 3 && flag != "on" && flag != "1"; ++i) {
                    const std::string word(words[i]);
                    char* end = nullptr;
                    const float number = std::strtof(word.c_str(), &end);
                    ok = ok && end == word.c_str() + word.size();
                    *fields[i] = number;
                }
                if (ok) {
                    request.sweep = sweep;
                }
            }
        } else if (key == "fov") {
            if (text::to_lower(value) == "off") {
                request.eye_fov = std::optional<camera::EyeFov>{};
                continue;
            }
            const auto words = text::split_whitespace(value);
            float numbers[4];
            bool ok = words.size() == 4;
            for (std::size_t i = 0; i < 4 && ok; ++i) {
                const std::string word(words[i]);
                char* end = nullptr;
                numbers[i] = std::strtof(word.c_str(), &end);
                ok = end == word.c_str() + word.size();
            }
            // Each side must be a real angle and the frustum must have a width and a height.
            ok = ok && numbers[0] > -89.0f && numbers[1] < 89.0f && numbers[2] < 89.0f && numbers[3] > -89.0f &&
                 numbers[1] > numbers[0] && numbers[2] > numbers[3];
            if (ok) {
                request.eye_fov = camera::EyeFov{numbers[0], numbers[1], numbers[2], numbers[3]};
            }
        } else if (key == "mouselook") {
            const std::string flag = text::to_lower(value);
            if (flag == "on" || flag == "1" || flag == "true") {
                request.mouselook = true;
            } else if (flag == "off" || flag == "0" || flag == "false") {
                request.mouselook = false;
            }
        } else if (key == "xr_lag") {
            if (text::to_lower(value) == "auto") {
                request.xr_lag = -1;
            } else if (const auto lag = text::parse_int(std::string(value)); lag && *lag >= 0 && *lag < 8) {
                request.xr_lag = static_cast<int>(*lag);
            }
        } else if (key == "probe") {
            const std::string flag = text::to_lower(value);
            request.probe = flag == "1" || flag == "true" || flag == "yes";
        } else if (key == "pose") {
            if (text::to_lower(value) == "off") {
                request.pose_off = true;
                continue;
            }
            const auto words = text::split_whitespace(value);
            if (words.size() != 5) {
                continue;
            }
            float numbers[5];
            bool ok = true;
            for (std::size_t i = 0; i < 5 && ok; ++i) {
                const std::string word(words[i]);
                char* end = nullptr;
                numbers[i] = std::strtof(word.c_str(), &end);
                ok = end == word.c_str() + word.size();
            }
            if (ok) {
                request.pose = camera::Pose{numbers[0], numbers[1], numbers[2], numbers[3], numbers[4]};
            }
        }
    }
    return request;
}

void start(const Config& config, std::function<void(const Request&)> on_request) {
    if (!config.get_bool("camera", "bridge", true)) {
        log::info("bridge: disabled in bg3vr.ini");
        return;
    }
    const std::string configured = config.get("camera", "bridge_file");
    auto args = std::make_unique<WatchArgs>();
    args->path = configured.empty() ? default_path() : std::filesystem::path(configured);
    args->on_request = std::move(on_request);
    if (args->path.empty()) {
        log::error("bridge: no file path (LOCALAPPDATA unset?)");
        return;
    }
    if (HANDLE thread = CreateThread(nullptr, 0, watch, args.get(), 0, nullptr)) {
        args.release(); // owned by the thread now
        CloseHandle(thread);
    }
}

} // namespace bg3vr::bridge
