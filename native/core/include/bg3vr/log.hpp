#pragma once

#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

namespace bg3vr::log {

enum class Level { Debug, Info, Warn, Error };

// Opens (truncates) the log file and optionally a console window that mirrors it.
void init(const std::filesystem::path& file, bool console, Level min_level = Level::Info);
void write(Level level, std::string_view message);

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace bg3vr::log
