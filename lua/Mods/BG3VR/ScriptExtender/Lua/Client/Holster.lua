-- Holsters on the body, client side. The core (native/core/src/holster.cpp) watches the hands against zones fixed to
-- the body and asks for
--   holster grab <zone> <hand>   pulled something out of a zone (the back: opened as soon as the grip closed)
--   holster stow <zone> <hand>   carried it back into a zone and let go
--   holster tap <zone> <hand>    closed and opened the grip without leaving the zone
-- (zones: shoulder_right, shoulder_left, hip_right, hip_left, back), each with what the zone holds ([holster] in
-- bg3vr.ini: melee, ranged, inventory, none), through the request= key in BG3VR/to_lua.txt, which Client/VRAuto.lua
-- reads and hands to BG3VR.OnHolster. Weapons: the server draws and sheathes (Server/Holster.lua); when it needs the
-- weapon set switched first it replies with the character's server entity handle, and the core does the switch
-- (bridge weaponset=). Inventory: the core presses the game's inventory key (I) itself.

local kChannel = "BG3VR_Holster"
local channel = Ext.Net and Ext.Net.CreateChannel and Ext.Net.CreateChannel(ModuleUUID, kChannel) or nil
local function OnReply(reply)
    if type(reply) == "table" and type(reply.Handle) == "string" and (reply.WeaponSet == "melee" or reply.WeaponSet == "ranged") then
        BG3VR.Log("holster: switching to the %s set (entity %s)", reply.WeaponSet, reply.Handle)
        BG3VR.WriteBridge({ "weaponset=" .. reply.Handle .. " " .. reply.WeaponSet })
    end
end

local function SendToServer(message)
    if channel then
        channel:RequestToServer(message, OnReply)
    else
        Ext.Net.PostMessageToServer(kChannel, Ext.Json.Stringify(message))
    end
end

local function Uuid(entity)
    local ok, uuid = pcall(function() return entity.Uuid.EntityUuid end)
    return ok and uuid or nil
end

function BG3VR.OnHolster(kind, zone, hand, holds)
    local character = _C()
    local uuid = character and Uuid(character)
    if not uuid then
        BG3VR.Warn("holster: %s %s (%s hand) with no controlled character", kind, zone, hand)
        return
    end
    BG3VR.Log("holster: %s %s with the %s hand (holds %s)", kind, zone, hand, tostring(holds))
    if holds == "inventory" or holds == "none" or holds == nil then
        return -- the core opens the inventory itself (the game's I key)
    end
    SendToServer({ Character = uuid, Kind = kind, Zone = zone, Hand = hand, Holds = holds })
end
