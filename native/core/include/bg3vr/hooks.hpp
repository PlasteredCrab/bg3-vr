#pragma once

#include <safetyhook.hpp>

#include <string_view>

namespace bg3vr::hooks {

// Installs an inline hook on the address published under `signature` (see signatures.txt).
// Returns an empty hook, and logs why, if the signature is unresolved or hooking fails.
safetyhook::InlineHook install(std::string_view signature, void* detour);

} // namespace bg3vr::hooks
