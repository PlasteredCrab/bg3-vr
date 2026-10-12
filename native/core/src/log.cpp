#include "bg3vr/log.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

namespace bg3vr::log {

namespace {

std::mutex g_mutex;
std::ofstream g_file;
bool g_console = false;
Level g_min_level = Level::Info;

const char* level_name(Level level) {
    switch (level) {
    case Level::Debug: return "debug";
    case Level::Info: return "info ";
    case Level::Warn: return "warn ";
    case Level::Error: return "error";
    }
    return "?    ";
}

} // namespace

void init(const std::filesystem::path& file, bool console, Level min_level) {
    std::scoped_lock lock(g_mutex);
    g_min_level = min_level;
    g_file.open(file, std::ios::out | std::ios::trunc);
    if (console && AllocConsole()) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        SetConsoleTitleW(L"BG3 VR");
        g_console = true;
    }
}

void write(Level level, std::string_view message) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    const std::string line = std::format("{:02}:{:02}:{:02}.{:03} [{}] {}\n", now.wHour, now.wMinute, now.wSecond,
                                         now.wMilliseconds, level_name(level), message);

    std::scoped_lock lock(g_mutex);
    if (level < g_min_level) {
        return;
    }
    if (g_file.is_open()) {
        g_file << line;
        g_file.flush();
    }
    if (g_console) {
        std::fputs(line.c_str(), stdout);
    }
    OutputDebugStringA(line.c_str());
}

} // namespace bg3vr::log
