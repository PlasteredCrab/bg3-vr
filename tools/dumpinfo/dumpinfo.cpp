// bg3vr_dumpinfo: what a crash dump says, without a debugger.
//
//   bg3vr_dumpinfo <file.dmp> [module name, default bg3_dx11.exe]
//
// Prints the exception (code, address as module+offset), the faulting thread's registers, and every
// 8-byte value on that thread's stack (as far as the dump holds it) that points into a loaded module's
// code: an approximate call stack, innermost first. Uses dbghelp's minidump reader only.

#include <windows.h>

#include <dbghelp.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

struct Module {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::wstring name;
};

std::vector<Module> g_modules;
std::wstring g_main = L"bg3_dx11.exe";

std::wstring leaf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::string where(std::uint64_t address) {
    for (const Module& m : g_modules) {
        if (address >= m.base && address < m.base + m.size) {
            char text[512];
            std::snprintf(text, sizeof(text), "%ls+0x%llX", m.name.c_str(),
                          static_cast<unsigned long long>(address - m.base));
            return text;
        }
    }
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(address));
    return text;
}

bool in_module(std::uint64_t address) {
    for (const Module& m : g_modules) {
        if (address >= m.base && address < m.base + m.size) {
            return true;
        }
    }
    return false;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bg3vr_dumpinfo <file.dmp> [module]\n");
        return 1;
    }
    if (argc > 2) {
        g_main = argv[2];
    }
    const HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "can't open %ls\n", argv[1]);
        return 1;
    }
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* base = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (!base) {
        std::fprintf(stderr, "can't map the dump\n");
        return 1;
    }
    const auto rva = [base](RVA r) { return static_cast<const std::uint8_t*>(base) + r; };

    void* stream = nullptr;
    ULONG size = 0;
    if (MiniDumpReadDumpStream(base, ModuleListStream, nullptr, &stream, &size)) {
        const auto* list = static_cast<const MINIDUMP_MODULE_LIST*>(stream);
        for (ULONG i = 0; i < list->NumberOfModules; ++i) {
            const MINIDUMP_MODULE& m = list->Modules[i];
            const auto* name = reinterpret_cast<const MINIDUMP_STRING*>(rva(m.ModuleNameRva));
            g_modules.push_back({m.BaseOfImage, m.SizeOfImage,
                                 leaf(std::wstring(name->Buffer, name->Length / sizeof(wchar_t)))});
        }
    }

    if (!MiniDumpReadDumpStream(base, ExceptionStream, nullptr, &stream, &size)) {
        std::printf("no exception stream in this dump\n");
        return 2;
    }
    const auto* exception = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream);
    const MINIDUMP_EXCEPTION& record = exception->ExceptionRecord;
    std::printf("exception 0x%08lX at %s, thread %lu\n", record.ExceptionCode, where(record.ExceptionAddress).c_str(),
                exception->ThreadId);
    for (ULONG i = 0; i < record.NumberParameters && i < 4; ++i) {
        std::printf("  parameter %lu: 0x%llX\n", i, static_cast<unsigned long long>(record.ExceptionInformation[i]));
    }
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        std::printf("  (%s address 0x%llX)\n",
                    record.ExceptionInformation[0] == 0 ? "read of" : record.ExceptionInformation[0] == 1 ? "write to" : "execute at",
                    static_cast<unsigned long long>(record.ExceptionInformation[1]));
    }

    const auto* context = reinterpret_cast<const CONTEXT*>(rva(exception->ThreadContext.Rva));
    std::printf("registers: rip %s\n  rax %llX rbx %llX rcx %llX rdx %llX\n  rsi %llX rdi %llX rbp %llX rsp %llX\n"
                "  r8 %llX r9 %llX r10 %llX r11 %llX\n  r12 %llX r13 %llX r14 %llX r15 %llX\n",
                where(context->Rip).c_str(), context->Rax, context->Rbx, context->Rcx, context->Rdx, context->Rsi,
                context->Rdi, context->Rbp, context->Rsp, context->R8, context->R9, context->R10, context->R11,
                context->R12, context->R13, context->R14, context->R15);

    // The faulting thread's stack memory, scanned for code addresses.
    if (MiniDumpReadDumpStream(base, ThreadListStream, nullptr, &stream, &size)) {
        const auto* threads = static_cast<const MINIDUMP_THREAD_LIST*>(stream);
        for (ULONG i = 0; i < threads->NumberOfThreads; ++i) {
            const MINIDUMP_THREAD& thread = threads->Threads[i];
            if (thread.ThreadId != exception->ThreadId) {
                continue;
            }
            const std::uint64_t start = thread.Stack.StartOfMemoryRange;
            const auto* bytes = rva(thread.Stack.Memory.Rva);
            const std::size_t length = thread.Stack.Memory.DataSize;
            std::printf("stack 0x%llX..0x%llX (%zu bytes); code addresses from rsp up:\n",
                        static_cast<unsigned long long>(start), static_cast<unsigned long long>(start + length), length);
            int shown = 0;
            for (std::size_t at = context->Rsp >= start ? (context->Rsp - start) & ~std::size_t{7} : 0;
                 at + 8 <= length && shown < 48; at += 8) {
                std::uint64_t value = 0;
                std::memcpy(&value, bytes + at, sizeof(value));
                if (in_module(value)) {
                    std::printf("  rsp+0x%04llX: %s\n", static_cast<unsigned long long>(start + at - context->Rsp),
                                where(value).c_str());
                    ++shown;
                }
            }
        }
    }
    return 0;
}
