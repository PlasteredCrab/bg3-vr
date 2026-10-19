#include "bg3vr/watchdog.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/render.hpp"
#include "bg3vr/views.hpp"

#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <array>
#include <atomic>

#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bg3vr::watchdog {

namespace {

ModuleInfo g_module;
constexpr auto kStall = std::chrono::seconds(3);

bool in_exe(DWORD64 address) {
    return address >= g_module.base && address < g_module.base + g_module.image_size;
}

// The non-volatile registers of a frame (what the function kept its locals and arguments in; e.g. the
// job wait 0x410E880 keeps its counter in r14, end-recording 0x3CC7010 the stage slot in rbx).
std::string registers(const CONTEXT& c) {
    return std::format("rbx {:X} rbp {:X} rsi {:X} rdi {:X} r12 {:X} r13 {:X} r14 {:X} r15 {:X}", c.Rbx, c.Rbp, c.Rsi,
                       c.Rdi, c.R12, c.R13, c.R14, c.R15);
}

// RtlVirtualUnwind on a thread that kept running after its registers were taken may read a stale frame:
// an access violation ends the walk instead of the game.
bool safe_unwind(DWORD64 image_base, PRUNTIME_FUNCTION function, CONTEXT* context) {
    __try {
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context->Rip, function, context, &handler_data, &establisher,
                         nullptr);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// Walks the stack; for the first few frames in the exe, records their registers (and what the
// registers point at, one level) as hints for the job dump.
std::string unwind(CONTEXT context, DWORD thread, std::vector<std::string>& frames, std::vector<views::Hint>& hints) {
    std::string text;
    int exe_frames = 0;
    for (int depth = 0; depth < 24 && context.Rip; ++depth) {
        text += " " + mem::describe(g_module, context.Rip);
        if (in_exe(context.Rip) && exe_frames < 6) {
            ++exe_frames;
            frames.push_back(std::format("{} {}", mem::describe(g_module, context.Rip), registers(context)));
            const std::string where = std::format("thread {} at {}", thread, mem::describe(g_module, context.Rip));
            for (const auto& [name, value] :
                 {std::pair{"rbx", context.Rbx}, std::pair{"rbp", context.Rbp}, std::pair{"rsi", context.Rsi},
                  std::pair{"rdi", context.Rdi}, std::pair{"r12", context.R12}, std::pair{"r13", context.R13},
                  std::pair{"r14", context.R14}, std::pair{"r15", context.R15}}) {
                if (value < 0x10000) {
                    continue;
                }
                hints.push_back({value, std::format("{} {}", where, name)});
                std::uintptr_t pointee = 0;
                if (mem::try_copy(&pointee, value, sizeof(pointee)) && pointee >= 0x10000) {
                    hints.push_back({pointee, std::format("{} [{}]", where, name)});
                }
            }
        }
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
        if (!safe_unwind(image_base, function, &context)) {
            text += " (unwind faulted)";
            break;
        }
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
    std::vector<views::Hint> hints;
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
                // Nothing that allocates or locks while the thread is suspended (it may hold the heap or
                // log lock): take the registers, resume, then unwind its stack, which for a hung thread
                // stays as it was.
                ResumeThread(thread);
                FILETIME created{}, exited{}, kernel{}, user{};
                GetThreadTimes(thread, &created, &exited, &kernel, &user);
                const ULONGLONG cpu_ms =
                    ((static_cast<ULONGLONG>(user.dwHighDateTime) << 32 | user.dwLowDateTime) +
                     (static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime)) / 10000;
                std::vector<std::string> frames;
                std::vector<views::Hint> thread_hints;
                const std::string stack = unwind(context, entry.th32ThreadID, frames, thread_hints);
                // Only threads that are in the game exe somewhere are interesting.
                if (stack.find("exe+") != std::string::npos) {
                    log::info("watchdog: thread {} (cpu {} ms):{}", entry.th32ThreadID, cpu_ms, stack);
                    for (const std::string& frame : frames) {
                        log::info("watchdog:   {}", frame);
                    }
                    hints.insert(hints.end(), thread_hints.begin(), thread_hints.end());
                    ++dumped;
                }
            } else {
                ResumeThread(thread);
            }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    log::info("watchdog: {} thread stack(s) logged; dumping the render views' job counters and stages", dumped);
    views::dump_jobs("freeze", hints);
    log::info("watchdog: job dump done");
}

// Crash watch: a vectored handler that logs the first occurrence of each faulting address outside this
// DLL (whose own memory probes fault by design): exception, registers, unwound stack with each game
// frame's registers, and for the first few, the render views' job dump with those registers as hints.
// It only observes (continues the search), so the game's own handlers still run.
std::uintptr_t g_self_begin = 0, g_self_end = 0;
std::array<std::atomic<std::uintptr_t>, 32> g_seen_faults{};
std::atomic<int> g_fault_dumps{0};
thread_local bool t_in_handler = false;

bool first_time(std::uintptr_t address) {
    for (auto& slot : g_seen_faults) {
        std::uintptr_t current = slot.load();
        if (current == address) {
            return false;
        }
        if (current == 0 && slot.compare_exchange_strong(current, address)) {
            return true;
        }
        if (current == address) {
            return false;
        }
    }
    return false; // table full: stop logging
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000374 /*heap*/) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    if ((address >= g_self_begin && address < g_self_end) || t_in_handler || !first_time(address)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    t_in_handler = true;
    const CONTEXT& c = *info->ContextRecord;
    const ULONG_PTR* params = info->ExceptionRecord->ExceptionInformation;
    const bool has_access = code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2;
    log::warn("crash watch: exception 0x{:X} at {} on thread {}{}", code, mem::describe(g_module, address),
              GetCurrentThreadId(),
              has_access ? std::format(" ({} 0x{:X})", params[0] == 0 ? "reading" : params[0] == 1 ? "writing" : "executing",
                                       params[1])
                         : std::string());
    log::warn("crash watch:   rax {:X} rbx {:X} rcx {:X} rdx {:X} rsi {:X} rdi {:X} rbp {:X} rsp {:X}", c.Rax, c.Rbx, c.Rcx,
              c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp);
    log::warn("crash watch:   r8 {:X} r9 {:X} r10 {:X} r11 {:X} r12 {:X} r13 {:X} r14 {:X} r15 {:X}", c.R8, c.R9, c.R10,
              c.R11, c.R12, c.R13, c.R14, c.R15);
    std::vector<std::string> frames;
    std::vector<views::Hint> hints;
    const std::string stack = unwind(c, GetCurrentThreadId(), frames, hints);
    log::warn("crash watch:   stack:{}", stack);
    for (const std::string& frame : frames) {
        log::warn("crash watch:     {}", frame);
    }
    for (const auto& [name, value] : {std::pair{"rax", c.Rax}, std::pair{"rcx", c.Rcx}, std::pair{"rdx", c.Rdx},
                                      std::pair{"r8", c.R8}, std::pair{"r9", c.R9}}) {
        hints.push_back({value, std::format("faulting frame {}", name)});
    }
    // The job dump only matters once a split view has been built (and isn't spent on startup faults).
    if (views::eye_views() && g_fault_dumps.fetch_add(1) < 3) {
        views::dump_jobs("exception", hints);
    }
    t_in_handler = false;
    return EXCEPTION_CONTINUE_SEARCH;
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
    HMODULE self = nullptr;
    MODULEINFO info{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&on_exception), &self) &&
        GetModuleInformation(GetCurrentProcess(), self, &info, sizeof(info))) {
        g_self_begin = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        g_self_end = g_self_begin + info.SizeOfImage;
    }
    AddVectoredExceptionHandler(0, on_exception);
    log::info("watchdog: crash watch on (faults outside this DLL are logged once per address)");
    std::thread(watch).detach();
}

} // namespace bg3vr::watchdog
