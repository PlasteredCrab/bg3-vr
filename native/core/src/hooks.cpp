#include "bg3vr/hooks.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/signatures.hpp"

namespace bg3vr::hooks {

safetyhook::InlineHook install(std::string_view signature, void* detour) {
    const auto target = sig::lookup(signature);
    if (!target) {
        log::error("hook {}: signature not resolved", signature);
        return {};
    }

    auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(*target), detour);
    if (!hook) {
        log::error("hook {}: failed at 0x{:X} (error type {})", signature, *target, static_cast<int>(hook.error().type));
        return {};
    }
    log::info("hook {}: installed at 0x{:X}", signature, *target);
    return std::move(*hook);
}

} // namespace bg3vr::hooks
