#pragma once

#include "bg3vr/module_info.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Reading game memory safely enough for probes and validation.
namespace bg3vr::mem {

// True if [address, address + size) is committed, readable, non-guard memory within one region.
bool readable(std::uintptr_t address, std::size_t size);

// Copies `size` bytes from `address`, returning false instead of crashing if the memory isn't
// readable (structured exception handling; costs nothing when the read succeeds). Use it on hot
// paths instead of readable(), whose VirtualQuery can be slow under endpoint protection.
bool try_copy(void* destination, std::uintptr_t address, std::size_t size);

template <class T>
T read(std::uintptr_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

// True if [address, address + size) lies inside the named section of `module`.
bool in_section(const ModuleInfo& module, const char* name, std::uintptr_t address, std::size_t size = 1);

// "exe+0x1234" inside the game exe, "module.dll+0x1234" inside another module, else "0x1234".
std::string describe(const ModuleInfo& module, std::uintptr_t address);

} // namespace bg3vr::mem
