#pragma once

#include "bg3vr/config.hpp"
#include "bg3vr/module_info.hpp"

#include <cstdint>

// Switching a character's weapon set (melee / ranged) to a SPECIFIC set, which no Osiris call and no client message
// can do (agent L, re-local/ghidra/agents/L_weapon_sets.md 1.3-1.4): the server's esv::WeaponSetSystem update
// (0x2B89930, rcx = the system) works through a request array at system+0x10 of 16-byte entries
// {u64 entity, u8 set (0 melee, 1 ranged), u8 toggle}. An inline hook on that update appends the mod's requests with
// the game's own array push (0x5BC780, as the 0xEC handler does) before the original runs, on the server thread.
// The game drops a ranged request when slots 5 and 6 are empty, and re-resolves the drawn weapons when the set
// changes. Requests come from the Lua server side (holsters) through the bridge: weaponset=<hex entity> melee|ranged.
namespace bg3vr::weaponset {

void install(const Config& config, const ModuleInfo& module);
bool available();
// Any thread: switch `entity` (a server EntityHandle) to set 0 (melee) or 1 (ranged) on the next server update.
void request(std::uint64_t entity, std::uint8_t set);
void report();

} // namespace bg3vr::weaponset
