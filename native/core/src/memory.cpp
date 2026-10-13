#include "bg3vr/memory.hpp"

#include "bg3vr/win_util.hpp"

#include <windows.h>

#include <filesystem>
#include <format>

namespace bg3vr::mem {

bool readable(std::uintptr_t address, std::size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) {
        return false;
    }
    const DWORD readable_flags = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return info.State == MEM_COMMIT && (info.Protect & readable_flags) && !(info.Protect & PAGE_GUARD) &&
           address + size <= region_end;
}

bool try_copy(void* destination, std::uintptr_t address, std::size_t size) {
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool in_section(const ModuleInfo& module, const char* name, std::uintptr_t address, std::size_t size) {
    const Section* section = find_section(module, name);
    return section && address >= section->start && address + size <= section->start + section->size;
}

std::string describe(const ModuleInfo& module, std::uintptr_t address) {
    if (address >= module.base && address < module.base + module.image_size) {
        return std::format("exe+0x{:X}", address - module.base);
    }
    HMODULE other = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &other)) {
        wchar_t name[MAX_PATH];
        const DWORD length = GetModuleFileNameW(other, name, MAX_PATH);
        const std::wstring file = std::filesystem::path(std::wstring(name, length)).filename().wstring();
        return std::format("{}+0x{:X}", to_utf8(file), address - reinterpret_cast<std::uintptr_t>(other));
    }
    return std::format("0x{:X}", address);
}

} // namespace bg3vr::mem
