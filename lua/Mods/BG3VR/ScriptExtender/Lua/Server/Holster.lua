-- Holsters on the body, server side (Client/Holster.lua sends what the hands did and what the zone holds).
--   melee / ranged: grab draws that weapon set. When the character is on the other set, the reply asks the client to
--     have the core switch it (bridge weaponset=, a hook on esv::WeaponSetSystem: agent L, L_weapon_sets.md 1.4; no
--     Osiris call can pick a set), and the draw waits until the set has changed (at most 0.8 s: without a ranged
--     weapon the game refuses the ranged set and the melee set is drawn). stow puts it away; tap toggles.
--   light: a torch flame with its light in the hand that squeezed (the game's torch effect, looping on the hand's FX
--     bone: Osiris PlayLoopEffect(target, effect, bone, scale) -> handle, 0x367C490; agent M 3.4); again to put it
--     out (StopLoopEffect 0x367C680). The flame follows the hand because the IK moves the hand bones.
--   Drawing and sheathing: Osiris SetWeaponUnsheathed(character, bool, _) (0x36512C0; only the second argument is
--   read, L 2.3), which plays the game's own draw / sheathe animation.

local kChannel = "BG3VR_Holster"
local kSetWait = 2.0 -- client -> bridge (250 ms polls) -> core -> next server update

local kTorchEffect = "VFX_LightSources_WPN_Torch_A_01_b6314fa1-8ac0-7a52-5865-5a4e9f98ddad"

local drawn = {}    -- character -> true while drawn by a holster
local lights = {}   -- character -> loop effect handle of the hand flame
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

-- Drawn now, by the character's own state (eoc::unsheath::StateComponent: Melee / Ranged / Instrument = drawn), so a
-- weapon the game drew itself can be put away; the holster's own record when the component can't be read.
local function IsDrawn(character)
    local entity = Entity(character)
    local ok, state = pcall(function() return entity.Unsheath.State end)
    if ok and state ~= nil then
        local s = tostring(state)
        return s ~= "Sheathed" and s ~= "3"
    end
    return drawn[character] == true
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
    if kind == "item" then
        local item = message.Item
        if type(item) ~= "string" then
            return {}
        end
        -- Stowing must go through the theft system like a normal pick-up. Until the mod uses the game's own pick-up
        -- path (agent AC is decompiling it), an item that belongs to anyone outside the party isn't stowed at all;
        -- it stays where it was let go. Both ownership fields are logged (which one the crime system reads is not
        -- confirmed yet), and either one pointing outside the party refuses.
        local function OwnerOf(field, key)
            local ok, owner = pcall(function()
                local entity = Ext.Entity.Get(item)
                return entity[field] and entity[field][key] or nil
            end)
            return ok and owner or nil
        end
        local function Describe(owner)
            if owner == nil then
                return "none", false
            end
            local ok, info = pcall(function()
                local uuid = owner.Uuid and owner.Uuid.EntityUuid or "?"
                return { uuid = uuid, party = owner.PartyMember ~= nil or uuid == character }
            end)
            if not ok then
                return "unreadable", true
            end
            return string.format("%s (%s)", tostring(info.uuid), info.party and "party" or "NOT party"), not info.party
        end
        local ownee, owneeForeign = Describe(OwnerOf("OwneeCurrent", "Ownee"))
        local owned, ownedForeign = Describe(OwnerOf("InventoryIsOwned", "Owner"))
        BG3VR.Log("gloves: %s wants to stow %s: OwneeCurrent.Ownee %s, InventoryIsOwned.Owner %s", character, item, ownee, owned)
        if owneeForeign or ownedForeign then
            BG3VR.Log("gloves: %s belongs to someone outside the party: not stowed (taking it must go through the "
                .. "theft system, not in yet)", item)
            return {}
        end
        local ok, err = pcall(Osi.ToInventory, item, character, 1, 1, 0)
        BG3VR.Log("gloves: %s stows %s: ToInventory %s%s", character, item, ok and "called" or "FAILED",
            ok and "" or (": " .. tostring(err)))
        return {}
    end
    if holds == "light" then
        if kind ~= "grab" then
            return {}
        end
        if lights[character] then
            BG3VR.Log("holster: %s's hand flame goes out", character)
            pcall(Osi.StopLoopEffect, lights[character])
            lights[character] = nil
        else
            local bone = message.Hand == "left" and "Dummy_L_HandFX" or "Dummy_R_HandFX"
            local ok, handle = pcall(Osi.PlayLoopEffect, character, kTorchEffect, bone, 1.0)
            if ok and handle then
                lights[character] = handle
                BG3VR.Log("holster: %s lights a flame in the %s hand (%s)", character, tostring(message.Hand), bone)
            else
                BG3VR.Warn("holster: PlayLoopEffect failed for %s: %s", character, tostring(handle))
            end
        end
        return {}
    end
    if holds ~= "melee" and holds ~= "ranged" then
        BG3VR.Log("holster: %s %s %s (holds %s; nothing to do on the server)", character, kind, zone, tostring(holds))
        return {}
    end
    if kind == "stow" or (kind == "tap" and IsDrawn(character)) then
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

-- Loop effects don't survive a level change: forget the flames' handles then.
Ext.Events.GameStateChanged:Subscribe(function(e)
    if tostring(e.ToState) ~= "Running" then
        lights = {}
        pending = {}
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
