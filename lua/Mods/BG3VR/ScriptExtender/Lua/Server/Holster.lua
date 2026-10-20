-- Holsters on the body, server side (Client/Holster.lua sends what the hands did and what the zone holds).
--   melee / ranged: grab draws that weapon set. When the character is on the other set, the reply asks the client to
--     have the core switch it (bridge weaponset=, a hook on esv::WeaponSetSystem: agent L, L_weapon_sets.md 1.4; no
--     Osiris call can pick a set), and the draw waits until the set has changed (at most 0.8 s: without a ranged
--     weapon the game refuses the ranged set and the melee set is drawn). stow puts it away; tap toggles.
--   Drawing and sheathing: Osiris SetWeaponUnsheathed(character, bool, _) (0x36512C0; only the second argument is
--   read, L 2.3), which plays the game's own draw / sheathe animation.

local kChannel = "BG3VR_Holster"
local kSetWait = 0.8

local drawn = {}    -- character -> true while drawn by a holster
local pending = {}  -- character -> { set = "melee"|"ranged", left = seconds }

local function Entity(character)
    local ok, entity = pcall(Ext.Entity.Get, character)
    return ok and entity or nil
end

-- "melee" | "ranged" | nil (eoc::WeaponSetComponent: Melee 0, Ranged 1; SE may give the enum as a name or a number).
local function CurrentSet(character)
    local entity = Entity(character)
    local ok, value = pcall(function() return entity.WeaponSet.WeaponSet end)
    if not ok or value == nil then
        return nil
    end
    local v = tostring(value)
    if v == "Melee" or v == "0" then
        return "melee"
    elseif v == "Ranged" or v == "1" then
        return "ranged"
    end
    return nil
end

-- The server EntityHandle as hex, from the entity's "Entity (<hex>)" name.
local function HandleHex(character)
    local entity = Entity(character)
    return entity and string.match(tostring(entity), "Entity %((%x+)%)") or nil
end

local function Draw(character, why)
    BG3VR.Log("holster: %s draws (%s, set now %s)", character, why, tostring(CurrentSet(character)))
    Osi.SetWeaponUnsheathed(character, 1, 0)
    drawn[character] = true
end

local function Sheathe(character, why)
    BG3VR.Log("holster: %s puts the weapon away (%s)", character, why)
    pending[character] = nil
    Osi.SetWeaponUnsheathed(character, 0, 0)
    drawn[character] = nil
end

-- Returns the reply for the client: { Handle, WeaponSet } when the core must switch the set first.
local function OnHolster(message, user)
    local character, kind, zone, holds = message.Character, message.Kind, message.Zone, message.Holds
    if type(character) ~= "string" or type(kind) ~= "string" or type(zone) ~= "string" then
        return {}
    end
    if Osi.IsPlayer(character) ~= 1 then
        BG3VR.Log("holster: %s isn't a player character; ignored", character)
        return {}
    end
    if holds ~= "melee" and holds ~= "ranged" then
        BG3VR.Log("holster: %s %s %s (holds %s; nothing to do on the server)", character, kind, zone, tostring(holds))
        return {}
    end
    if kind == "stow" or (kind == "tap" and drawn[character]) then
        Sheathe(character, kind .. " at " .. zone)
        return {}
    end
    -- grab, or a tap while sheathed: draw the zone's set.
    local current = CurrentSet(character)
    if current == holds then
        Draw(character, kind .. " " .. zone)
        return {}
    end
    local handle = HandleHex(character)
    if not handle then
        BG3VR.Warn("holster: no entity handle for %s; drawing the current set (%s)", character, tostring(current))
        Draw(character, kind .. " " .. zone)
        return {}
    end
    pending[character] = { set = holds, left = kSetWait }
    BG3VR.Log("holster: %s wants the %s set (now %s); switching first", character, holds, tostring(current))
    return { Handle = handle, WeaponSet = holds }
end

local function Tick(dt)
    for character, p in pairs(pending) do
        p.left = p.left - dt
        if CurrentSet(character) == p.set then
            pending[character] = nil
            Draw(character, "switched to " .. p.set)
        elseif p.left <= 0 then
            pending[character] = nil
            Draw(character, "the " .. p.set .. " set wasn't taken (no such weapon?)")
        end
    end
end

Ext.Events.Tick:Subscribe(function(e)
    local ok, err = pcall(Tick, (e and e.Time and e.Time.DeltaTime) or 0.016)
    if not ok then
        BG3VR.Warn("holster: %s", tostring(err))
    end
end)

if Ext.Net and Ext.Net.CreateChannel then
    Ext.Net.CreateChannel(ModuleUUID, kChannel):SetRequestHandler(OnHolster)
else
    Ext.RegisterNetListener(kChannel, function(_, payload, user)
        local ok, message = pcall(Ext.Json.Parse, payload)
        if ok and type(message) == "table" then
            OnHolster(message, user)
        end
    end)
end
