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


-- ---------------------------------------------------------------------------------------------------------- gloves
-- Stowing and moving items with the gravity gloves (native/core/src/hold.cpp) under the game's own crime rules
-- (agent AC 5, re-local/ghidra/agents/AC_item_outline.md; story Shared _CRIME_CrimeTriggers).
--   Stow: ToInventory(item, character, -1, 0, 0): the whole stack, the owner kept. It goes through the same inventory
--     system as a normal pick-up, which tags the item for the crime hook, whose Osiris event CharacterStoleItem makes
--     the story queue the "Steal" crime. If that event doesn't come within a second for an item with an NPC owner,
--     the story's own rule is replayed here.
--   Moved: the game makes moving an NPC-owned item the crime "MoveForbiddenItem" (its MovedBy rule); the gloves move
--     items on the client, so that rule is applied here, with the story's own query.

local stows = {} -- item -> { character, owner, template, amount, value, x, y, z, left, stole }

local function Owner(item)
    local ok, owner = pcall(Osi.GetOwner, item)
    if ok and owner and owner ~= "" and owner ~= "NULL_00000000-0000-0000-0000-000000000000" then
        return owner
    end
    return nil
end

local function NpcOwner(owner)
    if not owner then
        return false
    end
    local ok, result = pcall(Osi.QRY_CrimeIsNPCOwner, owner)
    return ok and result and true or false, ok and tostring(result) or ("failed: " .. tostring(result))
end

local function StowItem(character, item)
    if type(item) ~= "string" then
        return {}
    end
    local owner = Owner(item)
    local npc, why = NpcOwner(owner)
    local _, loot = pcall(function() return Ext.Entity.Get(item).OwnedAsLoot ~= nil end)
    local _, template = pcall(Osi.GetTemplate, item)
    local _, amount = pcall(Osi.GetStackAmount, item)
    local _, value = pcall(Osi.ItemGetGoldValue, item)
    local okPos, x, y, z = pcall(Osi.GetPosition, character)
    BG3VR.Log("gloves: %s stows %s (owner %s, NPC owner %s [%s], OwnedAsLoot %s, template %s, amount %s, value %s)",
        character, item, tostring(owner), tostring(npc), tostring(why), tostring(loot), tostring(template),
        tostring(amount), tostring(value))
    if npc then
        stows[item] = { character = character, owner = owner, template = template, amount = amount or 1,
            value = value or 0, x = okPos and x or 0, y = okPos and y or 0, z = okPos and z or 0, left = 1.0, stole = false }
    end
    local ok, err = pcall(Osi.ToInventory, item, character, -1, 0, 0)
    BG3VR.Log("gloves: ToInventory(%s, %s, -1, 0, 0) %s", item, character, ok and "called" or ("FAILED: " .. tostring(err)))
    return {}
end

local function MovedItem(character, item, how)
    if type(item) ~= "string" then
        return {}
    end
    local owner = Owner(item)
    if not owner then
        return {}
    end
    -- The story's own conditions (party member, an item, not something to sit on, not generic, an NPC owner).
    local ok, err = pcall(Osi.QRY_GetMoveForbiddenItemInfo, character, item, owner)
    local rows = {}
    if ok then
        local okRows, got = pcall(function() return Osi.DB_QRYRTN_GetMoveForbiddenItemInfo:Get(owner, nil) end)
        rows = okRows and got or {}
    end
    local crime = rows[1] and rows[1][2] or nil
    BG3VR.Log("gloves: %s moved %s (%s); owner %s; move crime %s%s", character, item, tostring(how), owner,
        tostring(crime), ok and "" or (" (query failed: " .. tostring(err) .. ")"))
    if crime then
        local okCrime, errCrime = pcall(Osi.PROC_CharacterRegisterCrime, character, crime, item, owner, 0)
        BG3VR.Log("gloves: crime %s registered for %s: %s", crime, character, okCrime and "yes" or ("FAILED: " .. tostring(errCrime)))
    end
    return {}
end

pcall(function()
    Ext.Osiris.RegisterListener("CharacterStoleItem", 10, "after",
        function(character, item, template, x, y, z, victim, container, amount, value)
            local s = stows[item]
            BG3VR.Log("gloves: CharacterStoleItem %s took %s from %s (container %s, amount %s, value %s)%s", tostring(character),
                tostring(item), tostring(victim), tostring(container), tostring(amount), tostring(value),
                s and " -- a gloves stow: the game's theft path ran" or "")
            if s then
                s.stole = true
            end
        end)
end)

-- A gloves stow of an NPC-owned item that the theft event didn't follow: the story's rule for CharacterStoleItem,
-- replayed (party member, not from a container: crime "Steal" queued at the thief's position).
local function StowTick(dt)
    for item, s in pairs(stows) do
        s.left = s.left - dt
        if s.stole then
            stows[item] = nil
        elseif s.left <= 0 then
            stows[item] = nil
            local ok, err = pcall(function()
                Osi.DB_CRIME_StealQueuedID(s.character, s.owner, s.x, s.y + 0.5, s.z, "Steal")
                Osi.DB_CRIME_StealQueued(s.character, item, s.template, s.owner, s.amount, s.value)
                Osi.PROC_CRIME_PreventMoveForbiddenItem(item)
                Osi.TimerCancel("CRIME_ProcessThefts")
                Osi.TimerLaunch("CRIME_ProcessThefts", 0)
            end)
            BG3VR.Log("gloves: no CharacterStoleItem for the stow of %s within a second; the story's Steal rule replayed: %s",
                item, ok and "queued" or ("FAILED: " .. tostring(err)))
        end
    end
end

Ext.Events.Tick:Subscribe(function(e)
    local ok, err = pcall(StowTick, (e and e.Time and e.Time.DeltaTime) or 0.016)
    if not ok then
        BG3VR.Warn("gloves: %s", tostring(err))
    end
end)

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
        return StowItem(character, message.Item)
    end
    if kind == "moved" then
        return MovedItem(character, message.Item, message.Hand)
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
