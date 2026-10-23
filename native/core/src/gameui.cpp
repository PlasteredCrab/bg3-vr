#include "bg3vr/gameui.hpp"

#include "bg3vr/bridge.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/text.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <array>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <format>
#include <initializer_list>
#include <string>
#include <vector>

namespace bg3vr::gameui {

namespace {

ModuleInfo g_module;

// 4.1.1.7631656, agent AB (hook table, 1.5, 1.6, 3.3).
constexpr std::uintptr_t kGameUiHolderRva = 0x6047958; // [holder] + 0x130 = GameUI
constexpr std::uintptr_t kUpdateRva = 0x1210210;       // GameUI::Update(ui, ctx), slot 0xC0
constexpr std::uintptr_t kMouseOverUiRva = 0x120E0C0;  // slot 0x50: movzx eax, byte [rcx+0x240]; ret
constexpr std::size_t kWindowManager = 0x4C0;
constexpr std::size_t kWidgets = 0x108, kWidgetCount = 0x114, kWidgetLock = 0x150;
constexpr std::size_t kFileName = 0x328, kFileNameCapacity = 0x33C, kLayer = 0x320, kPlayer = 0x394;

// The pages that are windows (Keyboard.xaml panels, AB 1.5): any of these open shows the VR UI panel.
constexpr std::array<const char*, 36> kWindowPages{
    "characterpanel", "partypanel",       "journalquests",    "journalmap",          "journalinspiration",
    "journaldialogues", "journalinfo",    "journaltutorials", "trade",               "donate",
    "container",      "pickpocket",       "combine",          "insert",              "examine",
    "splititem",      "dialoguehistory",  "activeroll",       "reactiondecisionpopup", "selectionflyout",
    "gamemenu",       "options",          "loadgame",         "savegame",            "messagebox",
    "tadpolepowerstree", "learnspells",   "companionspanel",  "rewardpanel",         "longrest",
    "makecamp",       "book",             "characterlevelup", "characterrespec",     "characterchangeappearance",
    "henchmen"};

// The acting character (agent AF, AF_acting_character.md): PlayerManager [exe+0x60487A0], its first player id +0xD0,
// VMPlayer = 0x1300990(GameUI+0x878, &id), SelectedCharacter VMCharacter* at +0x6B8 (the hotbar's UseSlot 0x1718060 and
// UseGameObject 0x159DB00 act with it), its entity handle at +0xA8. Pending selection +0x750, IsCurrentTurn +0x650,
// InTurnBasedMode VMCharacter+0x628 are logged with it.
constexpr std::uintptr_t kPlayerManagerRva = 0x60487A0;
constexpr std::uintptr_t kVmPlayerRva = 0x1300990;
constexpr std::size_t kVmProvider = 0x878, kPlayerId = 0xD0, kSelected = 0x6B8, kPending = 0x750, kCurrentTurn = 0x650;
constexpr std::size_t kCharacterHandle = 0xA8, kTurnBased = 0x628;
using VmPlayerFn = std::uintptr_t (*)(std::uintptr_t provider, const std::uint16_t* player);
std::atomic<std::uint64_t> g_acting{0};
bool g_acting_ready = false;
struct ActingRead {
    std::uint16_t player = 0;
    std::uintptr_t vm_player = 0, vm_character = 0, pending = 0;
    std::uint64_t handle = 0;
    std::uint8_t current_turn = 0, turn_based = 0;
};

int read_acting(std::uintptr_t ui, ActingRead& out) {
    __try {
        const std::uintptr_t manager = *reinterpret_cast<const std::uintptr_t*>(g_module.base + kPlayerManagerRva);
        if (!manager) {
            return 1;
        }
        out.player = *reinterpret_cast<const std::uint16_t*>(manager + kPlayerId);
        out.vm_player = reinterpret_cast<VmPlayerFn>(g_module.base + kVmPlayerRva)(ui + kVmProvider, &out.player);
        if (!out.vm_player) {
            return 2;
        }
        out.vm_character = *reinterpret_cast<const std::uintptr_t*>(out.vm_player + kSelected);
        out.pending = *reinterpret_cast<const std::uintptr_t*>(out.vm_player + kPending);
        out.current_turn = *reinterpret_cast<const std::uint8_t*>(out.vm_player + kCurrentTurn);
        if (out.vm_character) {
            out.handle = *reinterpret_cast<const std::uint64_t*>(out.vm_character + kCharacterHandle);
            out.turn_based = *reinterpret_cast<const std::uint8_t*>(out.vm_character + kTurnBased);
        }
        return 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 3;
    }
}

void update_acting(std::uintptr_t ui) {
    if (!g_acting_ready) {
        return;
    }
    ActingRead r;
    const int result = read_acting(ui, r);
    constexpr std::uint64_t kNull = 0xFFC0000000000000ull;
    const std::uint64_t handle = result == 0 && r.handle && r.handle != kNull ? r.handle : 0;
    const std::uint64_t before = g_acting.exchange(handle, std::memory_order_relaxed);
    if (handle != before) {
        bridge::set_status("acting", handle ? std::format("0x{:016X}", handle) : std::string());
        log::info("gameui: the character the player acts with (the hotbar's) is now 0x{:016X} (was 0x{:016X}); player {}, "
                  "VMPlayer 0x{:X}, VMCharacter 0x{:X}, pending 0x{:X}, current turn {}, turn-based {}{}",
                  handle, before, r.player, r.vm_player, r.vm_character, r.pending, r.current_turn, r.turn_based,
                  result ? std::format(" (read stopped at step {})", result) : std::string());
    }
}

safetyhook::InlineHook g_update_hook;
safetyhook::InlineHook g_mouse_hook;
std::atomic<bool> g_window_open{false};
std::atomic<bool> g_vr_first_person{false};
std::atomic<bool> g_on_panel{false};
bool g_override_mouse = true; // [ui] world_clicks_through
std::atomic<std::uint64_t> g_mouse_queries{0};
std::atomic<std::uint64_t> g_mouse_overridden{0};
std::atomic<std::uint64_t> g_updates{0};
std::string g_last_open = "(not read yet)"; // UI update thread
std::uint64_t g_list_logs = 0;

std::string file_name(std::uintptr_t widget) {
    std::uint32_t capacity = 0;
    if (!mem::try_copy(&capacity, widget + kFileNameCapacity, sizeof(capacity))) {
        return {};
    }
    std::uintptr_t text = widget + kFileName;
    if (capacity >= 16 && !mem::try_copy(&text, widget + kFileName, sizeof(text))) {
        return {};
    }
    char buffer[160]{};
    if (!mem::try_copy(buffer, text, sizeof(buffer) - 1)) {
        // Near the end of a page: read what fits.
        for (std::size_t i = 0; i + 1 < sizeof(buffer); ++i) {
            if (!mem::try_copy(buffer + i, text + i, 1) || buffer[i] == 0) {
                break;
            }
        }
    }
    buffer[sizeof(buffer) - 1] = 0;
    return buffer;
}

std::string page_of(const std::string& file) {
    std::string lower = text::to_lower(file);
    const auto slash = lower.find_last_of("/\\");
    if (slash != std::string::npos) {
        lower = lower.substr(slash + 1);
    }
    const auto dot = lower.find(".xaml");
    return dot != std::string::npos ? lower.substr(0, dot) : lower;
}

bool is_window(const std::string& page) {
    for (const char* w : kWindowPages) {
        if (page == w) {
            return true;
        }
    }
    return false;
}

// After the game's UI update: which window pages are open.
void scan_widgets(std::uintptr_t ui) {
    const std::uintptr_t wm = ui + kWindowManager;
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(wm + kWidgetLock);
    std::vector<std::string> open;
    std::string all; // the whole list, only while it is still being logged
    const bool want_all = g_list_logs < 12;
    bool window = false;
    EnterCriticalSection(lock);
    std::uintptr_t widgets = 0;
    std::uint32_t count = 0;
    mem::try_copy(&widgets, wm + kWidgets, sizeof(widgets));
    mem::try_copy(&count, wm + kWidgetCount, sizeof(count));
    for (std::uint32_t i = 0; widgets && i < std::min<std::uint32_t>(count, 256); ++i) {
        std::uintptr_t widget = 0;
        if (!mem::try_copy(&widget, widgets + i * sizeof(void*), sizeof(widget)) || !widget) {
            continue;
        }
        const std::string file = file_name(widget);
        const std::string page = page_of(file);
        std::int32_t layer = -1;
        std::int16_t player = -1;
        mem::try_copy(&layer, widget + kLayer, sizeof(layer));
        mem::try_copy(&player, widget + kPlayer, sizeof(player));
        if (is_window(page)) {
            window = true;
            open.push_back(page);
        }
        if (want_all) {
            all += std::format(" {}[L{} p{}]", file, layer, player);
        }
    }
    LeaveCriticalSection(lock);
    std::string joined;
    for (const auto& p : open) {
        joined += (joined.empty() ? "" : ", ") + p;
    }
    if (joined != g_last_open) {
        log::info("gameui: open windows now: {} (was: {})", joined.empty() ? "none" : joined,
                  g_last_open.empty() ? "none" : g_last_open);
        // The whole list now and then (the file name format is what this confirms; AB 7).
        if (want_all) {
            ++g_list_logs;
            log::info("gameui: {} widget(s):{}", count, all);
        }
        g_last_open = joined;
    }
    g_window_open.store(window, std::memory_order_relaxed);
}

void on_update(std::uintptr_t ui, std::uintptr_t ctx) {
    g_update_hook.call<void>(ui, ctx);
    if (g_updates.fetch_add(1, std::memory_order_relaxed) == 0) {
        log::info("gameui: the game UI updates on thread {} (GameUI 0x{:X})", GetCurrentThreadId(), ui);
    }
    scan_widgets(ui);
    update_acting(ui);
}

std::uint8_t on_mouse_over_ui(std::uintptr_t ui) {
    const std::uint8_t over = g_mouse_hook.call<std::uint8_t>(ui);
    g_mouse_queries.fetch_add(1, std::memory_order_relaxed);
    if (over && g_override_mouse && g_vr_first_person.load(std::memory_order_relaxed) &&
        !g_on_panel.load(std::memory_order_relaxed)) {
        g_mouse_overridden.fetch_add(1, std::memory_order_relaxed);
        return 0; // the laser points into the world: the invisible screen UI under the cursor doesn't count
    }
    return over;
}

bool bytes_at(std::uintptr_t address, std::initializer_list<std::uint8_t> expected) {
    std::array<std::uint8_t, 32> actual{};
    if (expected.size() > actual.size() || !mem::try_copy(actual.data(), address, expected.size())) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), actual.begin());
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    g_override_mouse = config.get_bool("ui", "world_clicks_through", true);
    {
        // mov [rsp+18h], rbx ; push rsi ; sub rsp, 20h ; mov rax, [rip+..]
        const bool ok = bytes_at(module.base + kVmPlayerRva, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x05});
        g_acting_ready = ok && config.get_bool("vr", "follow_hotbar_character", true);
        log::info("gameui: {}", g_acting_ready ? "the camera, hands, head hiding and gloves follow the hotbar's character"
                                               : ok ? "following the player manager's character ([vr] follow_hotbar_character)"
                                                    : "the VMPlayer lookup doesn't match this build; following the player "
                                                      "manager's character");
    }
    const std::uintptr_t update = module.base + kUpdateRva;
    const std::uintptr_t mouse = module.base + kMouseOverUiRva;
    if (!bytes_at(update, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55}) ||
        !bytes_at(mouse, {0x0F, 0xB6, 0x81, 0x40, 0x02, 0x00, 0x00, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC})) {
        health::set("game UI watch", health::State::Unavailable, "GameUI functions differ in this build");
        log::warn("gameui: GameUI::Update / MouseOverUI don't match this build; open windows won't show the hidden UI "
                  "panel, and the invisible UI may block world clicks");
        return;
    }
    if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(update), reinterpret_cast<void*>(&on_update))) {
        g_update_hook = std::move(*h);
    } else {
        log::warn("gameui: hooking GameUI::Update failed");
    }
    if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(mouse), reinterpret_cast<void*>(&on_mouse_over_ui))) {
        g_mouse_hook = std::move(*h);
    } else {
        log::warn("gameui: hooking MouseOverUI failed");
    }
    health::set("game UI watch", g_update_hook ? health::State::Ready : health::State::Unavailable, "windows open the panel");
    log::info("gameui: watching open windows (GameUI::Update exe+0x{:X}){}", kUpdateRva,
              g_mouse_hook && g_override_mouse ? "; world clicks pass through the invisible UI while the laser is off "
                                                 "the panel (MouseOverUI exe+0x120E0C0)"
                                               : "");
}

std::uint64_t acting_character() {
    return g_acting.load(std::memory_order_relaxed);
}

bool window_open() {
    return g_window_open.load(std::memory_order_relaxed);
}

void set_pointer(bool vr_first_person, bool on_panel) {
    g_vr_first_person.store(vr_first_person, std::memory_order_relaxed);
    g_on_panel.store(on_panel, std::memory_order_relaxed);
}

void report() {
    log::info("gameui: {} UI updates seen; MouseOverUI asked {} times, answered \"no\" for the invisible UI {} times; a "
              "window {}open now",
              g_updates.load(), g_mouse_queries.load(), g_mouse_overridden.load(), g_window_open.load() ? "is " : "isn't ");
}

} // namespace bg3vr::gameui
