#include "bg3vr/watch.hpp"

#include <safetyhook.hpp>
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <map>
#include <utility>

namespace bg3vr::watch {

namespace {

// ---- hardware write breakpoints ------------------------------------------------------------

constexpr std::size_t kMaxDistinctHits = 32;

struct HitSlot {
    std::atomic<std::uintptr_t> rip{0};
    std::atomic<std::uint32_t> count{0};
    std::size_t watch_index = 0;
    std::array<std::uintptr_t, kCallerDepth> callers{};
};

std::array<HitSlot, kMaxDistinctHits> g_hits;
std::atomic<bool> g_recording{false};

void unwind_callers(const CONTEXT& fault, std::array<std::uintptr_t, kCallerDepth>& callers) {
    CONTEXT context = fault;
    for (std::size_t i = 0; i < kCallerDepth; ++i) {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function) {
            void* handler_data = nullptr;
            DWORD64 establisher_frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                             &establisher_frame, nullptr);
        } else {
            // Leaf function: the return address is on top of the stack.
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
        }
        if (context.Rip == 0) {
            break;
        }
        callers[i] = context.Rip;
    }
}

void record_hit(const CONTEXT& context, std::size_t watch_index) {
    const std::uintptr_t rip = context.Rip;
    for (HitSlot& slot : g_hits) {
        std::uintptr_t current = slot.rip.load(std::memory_order_acquire);
        if (current == rip) {
            slot.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (current == 0) {
            std::uintptr_t expected = 0;
            if (slot.rip.compare_exchange_strong(expected, rip, std::memory_order_acq_rel)) {
                // This thread claimed the slot, so it alone fills in the details.
                slot.watch_index = watch_index;
                unwind_callers(context, slot.callers);
                slot.count.fetch_add(1, std::memory_order_release);
                return;
            }
            if (expected == rip) {
                slot.count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    // Table full: drop the hit.
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* context = info->ContextRecord;
    const DWORD64 triggered = context->Dr6 & 0xF;
    if (triggered == 0) {
        return EXCEPTION_CONTINUE_SEARCH; // a real single-step, not ours
    }
    context->Dr6 = 0;
    if (g_recording.load(std::memory_order_acquire)) {
        for (std::size_t i = 0; i < kMaxWatchAddresses; ++i) {
            if (triggered & (1ull << i)) {
                record_hit(*context, i);
            }
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Applies DR0..DR3 and DR7 to every thread in the process except the calling one. Nothing that
// can allocate or take locks runs while a thread is suspended.
void apply_debug_registers(const std::array<DWORD64, kMaxWatchAddresses>& addresses, DWORD64 dr7) {
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                                   entry.th32ThreadID);
        if (!thread) {
            continue;
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                context.Dr0 = addresses[0];
                context.Dr1 = addresses[1];
                context.Dr2 = addresses[2];
                context.Dr3 = addresses[3];
                context.Dr6 = 0;
                context.Dr7 = dr7;
                SetThreadContext(thread, &context);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

// ---- call counting ------------------------------------------------------------------------

constexpr std::size_t kMaxCounted = 32;
std::array<std::atomic<std::uint64_t>, kMaxCounted> g_counts{};

template <std::size_t I>
void count_hit(safetyhook::Context&) {
    g_counts[I].fetch_add(1, std::memory_order_relaxed);
}

template <std::size_t... I>
constexpr std::array<safetyhook::MidHookFn, sizeof...(I)> make_counters(std::index_sequence<I...>) {
    return {&count_hit<I>...};
}

constexpr auto kCounters = make_counters(std::make_index_sequence<kMaxCounted>{});

} // namespace

std::vector<WriteHit> watch_writes(std::span<const std::uintptr_t> addresses, std::chrono::milliseconds duration) {
    std::vector<WriteHit> result;
    const std::size_t count = std::min(addresses.size(), kMaxWatchAddresses);
    if (count == 0) {
        return result;
    }

    for (HitSlot& slot : g_hits) {
        slot.rip.store(0);
        slot.count.store(0);
        slot.callers = {};
    }

    std::array<DWORD64, kMaxWatchAddresses> regs{};
    DWORD64 dr7 = 0;
    for (std::size_t i = 0; i < count; ++i) {
        regs[i] = addresses[i] & ~static_cast<std::uintptr_t>(7);
        dr7 |= 1ull << (i * 2);              // local enable
        dr7 |= 0b01ull << (16 + i * 4);      // break on write
        dr7 |= 0b10ull << (18 + i * 4);      // length 8 bytes
    }

    PVOID handler = AddVectoredExceptionHandler(1, on_exception);
    if (!handler) {
        return result;
    }
    g_recording.store(true, std::memory_order_release);
    apply_debug_registers(regs, dr7);
    Sleep(static_cast<DWORD>(duration.count()));
    apply_debug_registers({}, 0);
    g_recording.store(false, std::memory_order_release);
    Sleep(100); // let in-flight exceptions drain before the handler goes away
    RemoveVectoredExceptionHandler(handler);

    for (const HitSlot& slot : g_hits) {
        const std::uintptr_t rip = slot.rip.load();
        if (rip == 0) {
            break;
        }
        result.push_back({slot.watch_index, rip, slot.count.load(), slot.callers});
    }
    return result;
}

std::vector<std::uint64_t> count_calls(std::span<const std::uintptr_t> functions, std::chrono::milliseconds duration) {
    std::vector<std::uint64_t> result(functions.size(), UINT64_MAX);
    std::vector<safetyhook::MidHook> hooks;
    std::map<std::uintptr_t, std::size_t> first_index; // several slots can share one function

    for (std::size_t i = 0; i < functions.size() && i < kMaxCounted; ++i) {
        g_counts[i].store(0);
        if (first_index.contains(functions[i])) {
            continue;
        }
        first_index[functions[i]] = i;
        auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(functions[i]), kCounters[i]);
        if (hook) {
            hooks.push_back(std::move(*hook));
        } else {
            first_index[functions[i]] = SIZE_MAX;
        }
    }

    Sleep(static_cast<DWORD>(duration.count()));
    hooks.clear(); // unhook before reading

    for (std::size_t i = 0; i < functions.size() && i < kMaxCounted; ++i) {
        const std::size_t owner = first_index[functions[i]];
        if (owner != SIZE_MAX) {
            result[i] = g_counts[owner].load();
        }
    }
    return result;
}

} // namespace bg3vr::watch
