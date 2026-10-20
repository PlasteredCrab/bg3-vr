-- Holsters on the body, client side. The core (native/core/src/holster.cpp) watches the hands against zones fixed to
-- the body and asks for
--   holster grab <zone> <hand>   pulled something out of a zone (the back: opened as soon as the grip closed)
--   holster stow <zone> <hand>   carried it back into a zone and let go
--   holster tap <zone> <hand>    closed and opened the grip without leaving the zone
-- (zones: shoulder_right, shoulder_left, hip_right, hip_left, back) through the request= key in BG3VR/to_lua.txt,
-- which Client/VRAuto.lua reads and hands to BG3VR.OnHolster. The server does what the game needs for the controlled
-- character (Server/Holster.lua).

local kChannel = "BG3VR_Holster"
local channel = Ext.Net and Ext.Net.CreateChannel and Ext.Net.CreateChannel(ModuleUUID, kChannel) or nil
local function SendToServer(message)
    if channel then
        channel:SendToServer(message)
    else
        Ext.Net.PostMessageToServer(kChannel, Ext.Json.Stringify(message))
    end
end

local function Uuid(entity)
    local ok, uuid = pcall(function() return entity.Uuid.EntityUuid end)
    return ok and uuid or nil
end

function BG3VR.OnHolster(kind, zone, hand)
    local character = _C()
    local uuid = character and Uuid(character)
    if not uuid then
        BG3VR.Warn("holster: %s %s (%s hand) with no controlled character", kind, zone, hand)
        return
    end
    BG3VR.Log("holster: %s %s with the %s hand", kind, zone, hand)
    SendToServer({ Character = uuid, Kind = kind, Zone = zone, Hand = hand })
end
