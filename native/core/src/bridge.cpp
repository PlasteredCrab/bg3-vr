#include "bg3vr/bridge.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/text.hpp"
#include "bg3vr/win_util.hpp"

#include <windows.h>

#include <cmath>
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
        } else if (key == "follow") {
            if (text::to_lower(value) == "off") {
                request.follow = std::optional<camera::Follow>{};
                continue;
            }
            const auto words = text::split_whitespace(value);
            const auto transform = words.empty() ? std::nullopt : parse_address(words[0]);
            camera::Follow follow;
            bool ok = transform.has_value() && *transform != 0 && words.size() <= 2;
            if (ok && words.size() == 2) {
                const std::string word(words[1]);
                char* end = nullptr;
                follow.height = std::strtof(word.c_str(), &end);
                ok = end == word.c_str() + word.size() && std::isfinite(follow.height) && std::abs(follow.height) < 10.0f;
            }
            if (ok) {
                follow.transform = *transform;
                request.follow = follow;
            }        } else if (key == "trace") {
            const std::string action = text::to_lower(value);
            if (action == "on" || action == "off" || action == "dump") {
                request.trace = action;
            }
        } else if (key == "views") {
            const std::string action = text::to_lower(value);
            if (action == "find") {
                request.views_find = true;
            } else if (action == "dump") {
                request.views_find = false;
            } else if (action == "sbs") {
                request.views_layout = views::Layout::SideBySide;
            } else if (action == "refs") {
                request.views_refs = true;
            } else if (action == "diff") {
                request.views_diff = true;
            } else if (action == "jobs") {
                request.views_jobs = true;
            } else if (action == "events") {
                request.views_events = true;
            } else if (action == "gpu") {
                request.views_gpu = true;
            } else if (action == "create") {
                request.views_create = true;
            } else if (action == "swap") {
                request.views_layout = views::Layout::SideBySideSwapped;
            } else if (action == "full") {
                request.views_layout = views::Layout::Full;
            } else {
                // set <index> <sx> <sy> <ox> <oy>  |  enable <index> <0|1>
                const auto words = text::split_whitespace(value);
                std::vector<float> numbers;
                for (std::size_t i = 1; i < words.size(); ++i) {
                    const std::string word(words[i]);
                    char* end = nullptr;
                    const float number = std::strtof(word.c_str(), &end);
                    if (end != word.c_str() + word.size() || !std::isfinite(number)) {
                        numbers.clear();
                        break;
                    }
                    numbers.push_back(number);
                }
                const std::string verb = words.empty() ? std::string() : text::to_lower(words[0]);
                const bool index_ok = !numbers.empty() && numbers[0] >= 0.0f && numbers[0] < 4.0f &&
                                      numbers[0] == std::floor(numbers[0]);
                const bool full = !words.empty() && text::to_lower(words.back()) == "full";
                if (full) {
                    numbers.clear(); // "full" isn't a number; parse what's before it
                    for (std::size_t i = 1; i + 1 < words.size(); ++i) {
                        const std::string word(words[i]);
                        char* end = nullptr;
                        numbers.push_back(std::strtof(word.c_str(), &end));
                    }
                }
                const bool index_given = !numbers.empty() && numbers[0] >= 0.0f && numbers[0] < 4.0f;
                if (verb == "build" && numbers.empty() && words.size() == (full ? 2u : 1u)) {
                    request.views_build = views::kAutomatic;
                    request.views_build_compositing = full;
                } else if (verb == "build" && index_given && numbers.size() == 1) {
                    request.views_build = static_cast<std::size_t>(numbers[0]);
                    request.views_build_compositing = full;
                } else if (verb == "set" && index_ok && numbers.size() == 5 && numbers[1] > 0.0f && numbers[1] <= 1.0f &&
                    numbers[2] > 0.0f && numbers[2] <= 1.0f && numbers[3] >= 0.0f && numbers[3] < 1.0f &&
                    numbers[4] >= 0.0f && numbers[4] < 1.0f) {
                    request.view_set = Request::ViewSet{static_cast<std::size_t>(numbers[0]),
                                                        views::Placement{numbers[1], numbers[2], numbers[3], numbers[4]},
                                                        std::nullopt};
                } else if (verb == "enable" && index_ok && numbers.size() == 2 &&
                           (numbers[1] == 0.0f || numbers[1] == 1.0f)) {
                    request.view_set =
                        Request::ViewSet{static_cast<std::size_t>(numbers[0]), std::nullopt, numbers[1] == 1.0f};
                }
            }
        } else if (key == "xr_lag") {
            if (text::to_lower(value) == "auto") {
                request.xr_lag = -1;
            } else if (const auto lag = text::parse_int(std::string(value)); lag && *lag >= 0 && *lag < 8) {
                request.xr_lag = static_cast<int>(*lag);
            }
        } else if (key == "recenter") {
            const std::string flag = text::to_lower(value);
            request.recenter = flag == "1" || flag == "true" || flag == "yes";
        } else if (key == "stereo") {
            const std::string word = text::to_lower(value);
            if (word == "on" || word == "off" || word == "report") {
                request.stereo = word;
            }
        } else if (key == "pick") {
            const std::string word = text::to_lower(value);
            if (word == "hand" || word == "off") {
                request.pick_hand = word == "hand";
            }
        } else if (key == "quality") {
            request.quality_report = text::to_lower(value) == "report";
        } else if (key == "ray") {
            const std::string word = text::to_lower(value);
            if (word == "camera" || word == "hand" || word == "off" || word == "report") {
                request.ray = word;
            }
        } else if (key == "skeleton") {
            if (text::to_lower(value) == "off") {
                request.skeleton = 0;
            } else if (const auto address = parse_address(value)) {
                request.skeleton = *address;
            }
        } else if (key == "hands") {
            const std::string word = text::to_lower(value);
            if (word == "report" || word == "buzz") {
                request.hands = word;
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
