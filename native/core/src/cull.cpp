#include "bg3vr/cull.hpp"

#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <initializer_list>
#include <mutex>
#include <optional>

namespace bg3vr::cull {

namespace {

struct Target {
    std::uintptr_t object = 0;
    std::uintptr_t vtable = 0;
};

std::mutex g_mutex;
std::vector<Target> g_targets; // hidden every check
std::vector<Target> g_restore; // shown again once, then dropped

std::atomic<std::uint64_t> g_checks{0};
std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_migrated{0};   // 0x600 given back (hidden the old way)
std::atomic<std::uint64_t> g_rehidden{0};   // the post-hook cleared 0x0001 again after a game recompute
std::atomic<std::uint64_t> g_recomputes{0}; // UpdateCullFlags calls on hidden visuals
std::atomic<bool> g_pending{false};         // a hide/show request is waiting
std::atomic<std::uint64_t> g_frames{0};     // view updates seen, for throttling
std::atomic<std::uint64_t> g_ticks{0};      // QPC ticks spent checking
constexpr std::uint64_t kCheckInterval = 32;

ModuleInfo g_module;

using ToggleCullFn = void (*)(std::uintptr_t, bool);
using SetCullFlagsFn = void (*)(std::uintptr_t, std::uint16_t, bool);
using UpdateCullFlagsFn = void (*)(std::uintptr_t);
SetCullFlagsFn g_set_flags = nullptr; // null: the setter differs in this build, the head is hidden the old way (0x600)
safetyhook::InlineHook g_update_hook;

// The hidden visuals and their attachments, for the post-hook (any thread): written by on_frame on the game thread,
// compared by pointer only.
constexpr std::size_t kMaxNodes = 64;
std::array<std::atomic<std::uintptr_t>, kMaxNodes> g_nodes{};
std::atomic<std::size_t> g_node_count{0};

void publish_nodes(const std::vector<std::uintptr_t>& nodes) {
    const std::size_t count = std::min(nodes.size(), kMaxNodes);
    g_node_count.store(0, std::memory_order_release);
    for (std::size_t i = 0; i < count; ++i) {
        g_nodes[i].store(nodes[i], std::memory_order_relaxed);
    }
    g_node_count.store(count, std::memory_order_release);
}

bool is_hidden_node(std::uintptr_t visual) {
    const std::size_t count = g_node_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        if (g_nodes[i].load(std::memory_order_relaxed) == visual) {
            return true;
        }
    }
    return false;
}

thread_local bool t_in_hook = false;

// The game recomputed a visual's cull flags (slot 23 sets 0x0001 again for a non-proxy visual): hide it again now.
void on_update_cull_flags(std::uintptr_t visual) {
    g_update_hook.call<void>(visual);
    if (t_in_hook || !g_set_flags || !is_hidden_node(visual)) {
        return;
    }
    g_recomputes.fetch_add(1, std::memory_order_relaxed);
    std::uint16_t flags = 0;
    if (mem::try_copy(&flags, visual + layout::kCullFlags, sizeof(flags)) && (flags & layout::kCameraBit)) {
        t_in_hook = true;
        g_set_flags(visual, layout::kCameraBit, false);
        t_in_hook = false;
        const std::uint64_t n = g_rehidden.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || n % 256 == 0) {
            log::info("hide: the game recomputed the cull flags of hidden visual 0x{:X} (0x{:X}, thread {}); hidden from "
                      "the eyes again ({} so far)",
                      visual, flags, GetCurrentThreadId(), n);
        }
    }
}

// Guarded reads, not VirtualQuery: in the game, mem::readable took ~2.6 ms per call (5.3 ms per
// check with two objects, measured 2026-10-12), which caused a stutter every few frames.
bool still_valid(const Target& target, std::uint16_t& flags) {
    std::uintptr_t vtable = 0;
    return mem::try_copy(&vtable, target.object, sizeof(vtable)) && vtable == target.vtable &&
           mem::try_copy(&flags, target.object + layout::kCullFlags, sizeof(flags));
}

// The visual and its attachments, depth first (what the setter recurses into).
void collect(std::uintptr_t visual, int depth, std::vector<std::uintptr_t>& out) {
    if (!visual || depth > 6 || out.size() >= kMaxNodes) {
        return;
    }
    out.push_back(visual);
    std::uintptr_t entries = 0;
    std::uint32_t count = 0;
    if (!mem::try_copy(&entries, visual + layout::kAttachments, sizeof(entries)) ||
        !mem::try_copy(&count, visual + layout::kAttachmentCount, sizeof(count)) || !entries || count > 64) {
        return;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uintptr_t child = 0;
        if (mem::try_copy(&child, entries + i * layout::kAttachmentStride, sizeof(child))) {
            collect(child, depth + 1, out);
        }
    }
}

std::uint16_t flags_of(std::uintptr_t visual) {
    std::uint16_t flags = 0;
    mem::try_copy(&flags, visual + layout::kCullFlags, sizeof(flags));
    return flags;
}

void toggle_600(const Target& target, bool visible) {
    const auto toggle = mem::read<ToggleCullFn>(target.vtable + layout::kToggleCullFlag600Slot * sizeof(void*));
    toggle(target.object, visible);
    g_calls.fetch_add(1, std::memory_order_relaxed);
}

void hide(const Target& target, std::uint16_t flags) {
    if (!g_set_flags) {
        if (flags & layout::kMainViewBits) {
            toggle_600(target, false);
        }
        return;
    }
    if ((flags & layout::kMainViewBits) != layout::kMainViewBits) {
        toggle_600(target, true); // hidden the old way: the shadow passes need 0x200 back
        g_migrated.fetch_add(1, std::memory_order_relaxed);
    }
    t_in_hook = true;
    g_set_flags(target.object, layout::kCameraBit, false);
    t_in_hook = false;
    g_calls.fetch_add(1, std::memory_order_relaxed);
}

// Shown again: the game's own recompute per visual (0x0001 only where the visual isn't a shadow proxy), and 0x600 back.
void show(const Target& target, std::uint16_t flags) {
    if ((flags & layout::kMainViewBits) != layout::kMainViewBits) {
        toggle_600(target, true);
    }
    if (!g_set_flags) {
        return;
    }
    std::vector<std::uintptr_t> nodes;
    collect(target.object, 0, nodes);
    for (const std::uintptr_t node : nodes) {
        std::uintptr_t vtable = 0, update = 0;
        if (mem::try_copy(&vtable, node, sizeof(vtable)) &&
            mem::in_section(g_module, ".rdata", vtable, (layout::kUpdateCullFlagsSlot + 1) * sizeof(void*)) &&
            mem::try_copy(&update, vtable + layout::kUpdateCullFlagsSlot * sizeof(void*), sizeof(update)) &&
            mem::in_section(g_module, ".text", update)) {
            reinterpret_cast<UpdateCullFlagsFn>(update)(node);
            g_calls.fetch_add(1, std::memory_order_relaxed);
        }
    }
    log::info("hide: object 0x{:X} shown again ({} visual(s) recomputed, flags now 0x{:X})", target.object, nodes.size(),
              flags_of(target.object));
}

std::optional<Target> check(std::uintptr_t object) {
    if (!mem::readable(object, 0x40)) {
        log::warn("hide: 0x{:X} is not readable", object);
        return std::nullopt;
    }
    const auto vtable = mem::read<std::uintptr_t>(object);
    if (!mem::in_section(g_module, ".rdata", vtable, (layout::kUpdateCullFlagsSlot + 1) * sizeof(void*))) {
        log::warn("hide: 0x{:X} has vtable 0x{:X} outside .rdata", object, vtable);
        return std::nullopt;
    }
    const auto toggle = mem::read<std::uintptr_t>(vtable + layout::kToggleCullFlag600Slot * sizeof(void*));
    const auto get_flags = mem::read<std::uintptr_t>(vtable + layout::kGetCullFlagsSlot * sizeof(void*));
    const auto update = mem::read<std::uintptr_t>(vtable + layout::kUpdateCullFlagsSlot * sizeof(void*));
    if (!mem::in_section(g_module, ".text", toggle, 64)) {
        log::warn("hide: slot {} of 0x{:X} is outside .text", layout::kToggleCullFlag600Slot, object);
        return std::nullopt;
    }
    const auto code = mem::read<std::array<std::uint8_t, 64>>(toggle);
    bool has_constant = false;
    for (std::size_t i = 0; i + 1 < code.size(); ++i) {
        has_constant = has_constant || (code[i] == 0x00 && code[i + 1] == 0x06) || (code[i] == 0xFF && code[i + 1] == 0xF9);
    }
    std::vector<std::uintptr_t> nodes;
    collect(object, 0, nodes);
    log::info("hide: object 0x{:X} vtable {} ToggleCullFlag600 {} GetCullFlags {} UpdateCullFlags {}{} flags 0x{:X}, {} "
              "attached visual(s), check {}",
              object, mem::describe(g_module, vtable), mem::describe(g_module, toggle), mem::describe(g_module, get_flags),
              mem::describe(g_module, update), update == g_module.base + layout::kUpdateCullFlagsRva ? " (hooked)" : "",
              mem::read<std::uint16_t>(object + layout::kCullFlags), nodes.size() - 1, has_constant ? "passed" : "FAILED");
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        log::info("hide:   attached 0x{:X} vtable {} flags 0x{:X}", nodes[i],
                  mem::describe(g_module, mem::read<std::uintptr_t>(nodes[i])), flags_of(nodes[i]));
    }
    if (!has_constant) {
        return std::nullopt;
    }
    return Target{object, vtable};
}

bool bytes_at(std::uintptr_t address, std::initializer_list<std::uint8_t> expected, std::size_t offset = 0) {
    std::array<std::uint8_t, 32> actual{};
    if (expected.size() > actual.size() || !mem::try_copy(actual.data(), address + offset, expected.size())) {
        return false;
    }
    std::size_t i = 0;
    for (const std::uint8_t b : expected) {
        if (actual[i++] != b) {
            return false;
        }
    }
    return true;
}

} // namespace

void init(const ModuleInfo& module) {
    g_module = module;
    const std::uintptr_t setter = module.base + layout::kSetCullFlagsRva;
    const std::uintptr_t update = module.base + layout::kUpdateCullFlagsRva;
    // Setter: prologue, movzx ebp, r8b / movzx esi, dx, and the store to +0xF4 (agent AA). UpdateCullFlags: the
    // VisualFlags read at +0xF0 and the 0x7FC1 mask.
    const bool setter_ok =
        bytes_at(setter, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
                          0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F, 0xB6, 0xE8, 0x0F, 0xB7, 0xF2}) &&
        bytes_at(setter, {0x66, 0x44, 0x89, 0x81, 0xF4, 0x00, 0x00, 0x00}, 0x3B);
    const bool update_ok = bytes_at(update, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0x89, 0xF0, 0x00, 0x00, 0x00,
                                             0x41, 0xBA, 0xC1, 0x7F, 0x00, 0x00});
    if (!setter_ok || !update_ok) {
        health::set("head shadow", health::State::Unavailable, "the cull flag functions differ in this build");
        log::warn("hide: the cull flag setter / UpdateCullFlags don't match this build (setter {}, update {}); the head "
                  "is hidden the old way and casts no shadow",
                  setter_ok ? "ok" : "differs", update_ok ? "ok" : "differs");
        return;
    }
    auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(update), reinterpret_cast<void*>(&on_update_cull_flags));
    if (!hook) {
        health::set("head shadow", health::State::Unavailable, "hook failed");
        log::warn("hide: hooking UpdateCullFlags failed; the head is hidden the old way and casts no shadow");
        return;
    }
    g_update_hook = std::move(*hook);
    g_set_flags = reinterpret_cast<SetCullFlagsFn>(setter);
    health::set("head shadow", health::State::Ready, "head hidden from the eyes only (cull bit 0x0001)");
    log::info("hide: the head is hidden from the eye cameras only (cull bit 0x0001 via exe+0x{:X}); it keeps casting "
              "shadows. UpdateCullFlags exe+0x{:X} post-hooked",
              layout::kSetCullFlagsRva, layout::kUpdateCullFlagsRva);
}

void set_hidden(const std::vector<std::uintptr_t>& objects) {
    std::vector<Target> validated;
    for (const std::uintptr_t object : objects) {
        if (const auto target = check(object)) {
            validated.push_back(*target);
        }
    }
    std::scoped_lock lock(g_mutex);
    g_restore.insert(g_restore.end(), g_targets.begin(), g_targets.end());
    g_targets = std::move(validated);
    publish_nodes({}); // the post-hook leaves everything alone until the next check publishes the new set
    g_pending.store(true, std::memory_order_release);
    log::info("hide: {} object(s) hidden from the eyes, {} to restore", g_targets.size(), g_restore.size());
}

void forget() {
    std::scoped_lock lock(g_mutex);
    const std::size_t dropped = g_targets.size() + g_restore.size();
    g_targets.clear();
    g_restore.clear();
    publish_nodes({});
    log::info("hide: forgot {} object(s) without touching them", dropped);
}

void on_frame() {
    const bool due = g_frames.fetch_add(1, std::memory_order_relaxed) % kCheckInterval == 0;
    if (!due && !g_pending.load(std::memory_order_acquire)) {
        return;
    }
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    std::scoped_lock lock(g_mutex);
    g_pending.store(false, std::memory_order_release);
    std::uint16_t flags = 0;
    for (const Target& target : g_restore) {
        if (still_valid(target, flags)) {
            show(target, flags);
        }
    }
    g_restore.clear();
    std::vector<std::uintptr_t> nodes;
    std::erase_if(g_targets, [&flags, &nodes](const Target& target) {
        if (!still_valid(target, flags)) {
            return true; // destroyed or replaced: drop it
        }
        const std::size_t first = nodes.size();
        collect(target.object, 0, nodes);
        // The setter recurses, so one call covers an attachment the game recomputed (a new helmet, hair).
        bool shown = false;
        for (std::size_t i = first; i < nodes.size(); ++i) {
            shown = shown || (g_set_flags ? (flags_of(nodes[i]) & layout::kCameraBit) != 0
                                          : (flags_of(nodes[i]) & layout::kMainViewBits) != 0);
        }
        if (shown || (g_set_flags && (flags & layout::kMainViewBits) != layout::kMainViewBits)) {
            hide(target, flags);
        }
        return false;
    });
    publish_nodes(nodes);
    if (!g_targets.empty()) {
        g_checks.fetch_add(1, std::memory_order_relaxed);
    }
    // Once a minute (at 32-frame checks): what the hidden head's flags are now (0x86FE expected: out of the camera
    // pass, in every shadow pass).
    static std::uint64_t logged = 0;
    if (!g_targets.empty() && g_checks.load(std::memory_order_relaxed) >= logged + 120) {
        logged = g_checks.load(std::memory_order_relaxed);
        for (const Target& target : g_targets) {
            log::info("hide: hidden object 0x{:X} flags 0x{:X} (camera bit {}, view bits {})", target.object,
                      flags_of(target.object), (flags_of(target.object) & layout::kCameraBit) ? "SET" : "clear",
                      (flags_of(target.object) & layout::kMainViewBits) == layout::kMainViewBits ? "kept" : "CLEARED");
        }
    }
    QueryPerformanceCounter(&end);
    g_ticks.fetch_add(static_cast<std::uint64_t>(end.QuadPart - start.QuadPart), std::memory_order_relaxed);
}

void report() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const std::uint64_t checks = g_checks.load(std::memory_order_relaxed);
    const double total_us =
        1e6 * static_cast<double>(g_ticks.load(std::memory_order_relaxed)) / static_cast<double>(frequency.QuadPart);
    log::info("hide: {} cull flag calls so far ({} from the old 0x600 hide given back); the game recomputed hidden "
              "visuals {} times, {} hidden again at once; {} checks, {:.1f} us total, {:.2f} us each",
              g_calls.load(std::memory_order_relaxed), g_migrated.load(std::memory_order_relaxed),
              g_recomputes.load(std::memory_order_relaxed), g_rehidden.load(std::memory_order_relaxed), checks, total_us,
              checks ? total_us / static_cast<double>(checks) : 0.0);
}

} // namespace bg3vr::cull
