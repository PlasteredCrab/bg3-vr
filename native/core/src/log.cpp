#include "bg3vr/log.hpp"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bg3vr::log {

namespace {

// Lines are queued by the caller and written by a background thread, so the game and render threads never wait on
// the disk (the 2026-10-22 run wrote 10 MB in 9 minutes, one flushed write per line, with an on-access scanner
// looking at every write). flush() writes what is queued on the caller's thread (crash watch, shutdown).
std::mutex g_queue_mutex; // guards g_queue, g_min_level
std::condition_variable g_wake;
std::vector<std::string> g_queue;
std::mutex g_file_mutex; // guards g_file, g_console; held while writing
std::ofstream g_file;
bool g_console = false;
bool g_started = false;
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

// The caller holds the file lock (passed only to make that explicit).
void drain([[maybe_unused]] const std::unique_lock<std::mutex>& file_lock) {
    std::vector<std::string> lines;
    {
        std::scoped_lock lock(g_queue_mutex);
        lines.swap(g_queue);
    }
    if (lines.empty()) {
        return;
    }
    const bool debugger = IsDebuggerPresent() != 0;
    for (const std::string& line : lines) {
        if (g_file.is_open()) {
            g_file << line;
        }
        if (g_console) {
            std::fputs(line.c_str(), stdout);
        }
        if (debugger) {
            OutputDebugStringA(line.c_str());
        }
    }
    if (g_file.is_open()) {
        g_file.flush();
    }
}

void writer() {
    for (;;) {
        {
            std::unique_lock lock(g_queue_mutex);
            g_wake.wait_for(lock, std::chrono::milliseconds(100), [] { return g_queue.size() >= 256; });
        }
        std::unique_lock file_lock(g_file_mutex);
        drain(file_lock);
    }
}

} // namespace

void init(const std::filesystem::path& file, bool console, Level min_level) {
    {
        std::scoped_lock lock(g_queue_mutex);
        g_min_level = min_level;
    }
    {
        std::scoped_lock lock(g_file_mutex);
        g_file.open(file, std::ios::out | std::ios::trunc);
        if (console && AllocConsole()) {
            FILE* stream = nullptr;
            freopen_s(&stream, "CONOUT$", "w", stdout);
            SetConsoleTitleW(L"BG3 VR");
            g_console = true;
        }
    }
    if (!g_started) {
        g_started = true;
        std::thread(writer).detach(); // lives as long as the process (never joined: no waiting under the loader lock)
    }
}

void write(Level level, std::string_view message) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::string line = std::format("{:02}:{:02}:{:02}.{:03} [{}] {}\n", now.wHour, now.wMinute, now.wSecond,
                                   now.wMilliseconds, level_name(level), message);
    std::scoped_lock lock(g_queue_mutex);
    if (level < g_min_level || !g_started) {
        return;
    }
    g_queue.push_back(std::move(line));
    if (g_queue.size() >= 256) {
        g_wake.notify_one();
    }
}

void flush() {
    // Called from the crash watch, possibly on a thread that faulted while holding the file lock: never wait for it
    // for long (the writer thread finishes the lines in that case).
    std::unique_lock file_lock(g_file_mutex, std::defer_lock);
    for (int i = 0; i < 50 && !file_lock.try_lock(); ++i) {
        Sleep(10);
    }
    if (file_lock.owns_lock()) {
        drain(file_lock);
    }
}

} // namespace bg3vr::log
