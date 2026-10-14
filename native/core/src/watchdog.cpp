#include "bg3vr/watchdog.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/render.hpp"

#include <windows.h>

#include <tlhelp32.h>

#include <chrono>
#include <string>
#include <thread>

namespace bg3vr::watchdog {

namespace {

ModuleInfo g_module;
constexpr auto kStall = std::chrono::seconds(3);

std::string unwind(CONTEXT context) {
    std::string text;
    for (int depth = 0; depth < 24 && context.Rip; ++depth) {
        text += " " + mem::describe(g_module, context.Rip);
        DWORD64 image_base = 0;
        const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (!function) {
            // Leaf function: the return address is at rsp.
            DWORD64 ret = 0;
            if (!mem::try_copy(&ret, context.Rsp, sizeof(ret))) {
                break;
            }
            context.Rip = ret;
            context.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher,
                         nullptr);
    }
    return text;
}

void dump_threads() {
    const DWORD self = GetCurrentThreadId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        log::warn("watchdog: can't list threads");
        return;
    }
    THREADENTRY32 entry{sizeof(entry)};
    int dumped = 0;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
            continue;
        }
        const HANDLE thread =
            OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) {
            continue;
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(thread, &context)) {
                FILETIME created{}, exited{}, kernel{}, user{};
                GetThreadTimes(thread, &created, &exited, &kernel, &user);
                const ULONGLONG cpu_ms =
                    ((static_cast<ULONGLONG>(user.dwHighDateTime) << 32 | user.dwLowDateTime) +
                     (static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime)) / 10000;
                const std::string stack = unwind(context);
                ResumeThread(thread);
                // Only threads that are in the game exe somewhere are interesting.
                if (stack.find("exe+") != std::string::npos) {
                    log::info("watchdog: thread {} (cpu {} ms):{}", entry.th32ThreadID, cpu_ms, stack);
                    ++dumped;
                }
            } else {
                ResumeThread(thread);
            }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    log::info("watchdog: {} thread stack(s) logged", dumped);
}

void watch() {
    std::uint64_t last = render::frame_count();
    auto last_change = std::chrono::steady_clock::now();
    bool dumped = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const std::uint64_t now_frames = render::frame_count();
        const auto now = std::chrono::steady_clock::now();
        if (now_frames != last) {
            last = now_frames;
            last_change = now;
            dumped = false;
            continue;
        }
        if (!dumped && now_frames > 0 && now - last_change > kStall) {
            log::warn("watchdog: no frame presented for {} s after {} frames; logging thread stacks",
                      std::chrono::duration_cast<std::chrono::seconds>(now - last_change).count(), now_frames);
            dump_threads();
            dumped = true;
        }
    }
}

} // namespace

void start(const ModuleInfo& module) {
    g_module = module;
    std::thread(watch).detach();
}

} // namespace bg3vr::watchdog
