-- Holsters on the body, server side (Client/Holster.lua sends what the hands did). Drawing and sheathing go through
-- Osiris SetWeaponUnsheathed(character, bool) (agent K: handler 0x36512C0 -> 0x35F19A0 writes the wanted state into
-- esv::unsheath::DefaultSystem). Which weapon set a zone holds is decided in ZoneAction.

local kChannel = "BG3VR_Holster"

local kWeaponZones = {
    shoulder_right = true, shoulder_left = true, hip_right = true, hip_left = true,
}

local function OnHolster(message, user)
    local character, kind, zone = message.Character, message.Kind, message.Zone
    if type(character) ~= "string" or type(kind) ~= "string" or type(zone) ~= "string" then
        return
    end
    if Osi.IsPlayer(character) ~= 1 then
        BG3VR.Log("holster: %s isn't a player character; ignored", character)
        return
    end
    if kWeaponZones[zone] then
        if kind == "grab" then
            BG3VR.Log("holster: %s draws from %s", character, zone)
            Osi.SetWeaponUnsheathed(character, 1)
        elseif kind == "stow" then
            BG3VR.Log("holster: %s puts the weapon away at %s", character, zone)
            Osi.SetWeaponUnsheathed(character, 0)
        else
            BG3VR.Log("holster: %s tapped %s", character, zone)
        end
    else
        BG3VR.Log("holster: %s %s %s (nothing kept there yet)", character, kind, zone)
    end
end

if Ext.Net and Ext.Net.CreateChannel then
    Ext.Net.CreateChannel(ModuleUUID, kChannel):SetHandler(OnHolster)
else
    Ext.RegisterNetListener(kChannel, function(_, payload, user)
        local ok, message = pcall(Ext.Json.Parse, payload)
        if ok and type(message) == "table" then
            OnHolster(message, user)
        end
    end)
end
