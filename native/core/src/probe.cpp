#include "bg3vr/probe.hpp"

#include "bg3vr/camera.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/watch.hpp"

#include <array>
#include <chrono>
#include <string>
#include <vector>

namespace bg3vr::probe {

void run(const ModuleInfo& module, std::uintptr_t controller) {
    namespace layout = camera::layout;

    const auto vtable = mem::read<std::uintptr_t>(controller);
    std::vector<std::uintptr_t> slots(layout::kVtableSlots);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        slots[i] = mem::read<std::uintptr_t>(vtable + i * sizeof(void*));
    }

    log::info("probe: counting calls to {} vtable slots for 3 s", slots.size());
    const auto counts = watch::count_calls(slots, std::chrono::milliseconds(3000));
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (counts[i] == UINT64_MAX) {
            log::info("probe:   slot {:2} {} not hookable (too short or shared)", i, mem::describe(module, slots[i]));
        } else if (counts[i] > 0) {
            log::info("probe:   slot {:2} {} called {} times", i, mem::describe(module, slots[i]), counts[i]);
        }
    }

    const std::array<std::uintptr_t, 3> watched = {
        controller + layout::kWorldTranslate,
        controller + layout::kViewMatrix + 12 * sizeof(float),
        controller + layout::kInvViewMatrix + 12 * sizeof(float),
    };
    constexpr std::array<const char*, 3> kNames = {"WorldTransform.Translate", "ViewMatrix[12..13]",
                                                   "InvViewMatrix[12..13]"};
    log::info("probe: watching writes to the controller's pose and matrices for 2 s");
    const auto hits = watch::watch_writes(watched, std::chrono::milliseconds(2000));
    if (hits.empty()) {
        log::info("probe:   no writes seen");
    }
    for (const auto& hit : hits) {
        log::info("probe:   {} written at {} x{}", kNames[hit.watch_index], mem::describe(module, hit.rip), hit.count);
        std::string stack;
        for (std::uintptr_t caller : hit.callers) {
            if (caller == 0) {
                break;
            }
            stack += "  <- " + mem::describe(module, caller);
        }
        log::info("probe:     stack{}", stack);
    }
    log::info("probe: done");
}

} // namespace bg3vr::probe
