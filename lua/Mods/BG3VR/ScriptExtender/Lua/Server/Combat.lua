-- Physical melee, server side: a client reports that its character's swing hit another character
-- (Client/Combat.lua). The game makes its normal attack with the equipped weapon: Osi.Attack(attacker, target, 0)
-- picks the weapon's attack spell and rolls as usual (agent I, re-local/ghidra/agents/I_picking_actions.md 3).

local kChannel = "BG3VR_PhysicalHit"

local function OnHit(message, user)
    local attacker, target = message.Attacker, message.Target
    if type(attacker) ~= "string" or type(target) ~= "string" or attacker == target then
        return
    end
    -- Only player characters attack this way.
    if Osi.IsPlayer(attacker) ~= 1 then
        BG3VR.Log("combat: %s isn't a player character; ignored", attacker)
        return
    end
    if Osi.IsDead(target) == 1 then
        return
    end
    BG3VR.Log("combat: %s attacks %s (physical swing, %.1f m/s)", attacker, target, tonumber(message.Speed) or 0)
    Osi.Attack(attacker, target, 0)
end

-- Ext.Net.CreateChannel where the Script Extender has it, else the older net listener with a JSON payload.
if Ext.Net and Ext.Net.CreateChannel then
    Ext.Net.CreateChannel(ModuleUUID, kChannel):SetHandler(OnHit)
else
    Ext.RegisterNetListener(kChannel, function(_, payload, user)
        local ok, message = pcall(Ext.Json.Parse, payload)
        if ok and type(message) == "table" then
            OnHit(message, user)
        end
    end)
end
