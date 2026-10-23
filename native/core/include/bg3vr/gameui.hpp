#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

// The game's own UI (Noesis), as seen from its update (agent AB, AB_ui_system.md):
//   - which windows are open: after GameUI::Update 0x1210210 the WindowManager's widget list (GameUI+0x4C0, widgets
//     +0x108, count +0x114, lock +0x150; per widget the XAML file name at +0x328) is read, and a window page
//     (inventory / character panel, journal and map, trade, loot, game menu, ...) being open brings the hidden VR UI
//     panel up in front of you, so opening the inventory with A shows it;
//   - "mouse over UI": picking asks GameUI slot 0x50 (0x120E0C0, byte GameUI+0x240) whether the cursor is over the
//     UI and then ignores world clicks. In VR the UI is on a panel (or hidden) while hand pointing moves the cursor over
//     the world, so the invisible screen layout (the hotbar along the bottom) ate world clicks. While VR first person
//     is on and the laser isn't on the UI panel it answers "no".
namespace bg3vr::gameui {

void install(const Config& config, const ModuleInfo& module);

// A window page is open (any thread).
bool window_open();

// The headset (presenting thread): the laser is on the UI panel, and whether the override applies at all
// (VR play on, first person).
void set_pointer(bool vr_first_person, bool on_panel);

void report();

} // namespace bg3vr::gameui
