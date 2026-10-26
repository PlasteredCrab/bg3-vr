-- Testing helpers, server side. !vr_unlock_travel (or the client's command, Client/Testing.lua) unlocks every
-- waypoint and turns the map's shroud off for the loaded game, through the game's own story procedures (read from the
-- game's goals, Shared.pak / Gustav.pak):
--   PROC_UnlockAllGroupWaypoints(GroupID): every waypoint of a DB_WaypointInfo group, for all players (_Waypoints.txt);
--     the groups are "Act1" and "Act1b", "Act2", "Act3", "Act3b" (_Gustav_Waypoints*.txt).
--   PROC_DisableShroud(): DB_ShroudDisabled(1) and ShroudRender(0): no unexplored darkness on the map (_Shroud.txt).
-- Both are stored in the save when the game is saved afterwards.

local kChannel = "BG3VR_Testing"
local kGroups = { "Act1", "Act1b", "Act2", "Act3", "Act3b" }

local function UnlockTravel(source)
    local unlocked = {}
    for _, group in ipairs(kGroups) do
        local ok, err = pcall(function() Osi.PROC_UnlockAllGroupWaypoints(group) end)
        unlocked[#unlocked + 1] = group .. (ok and "" or (" FAILED: " .. tostring(err)))
    end
    local ok, err = pcall(function() Osi.PROC_DisableShroud() end)
    local count = 0
    pcall(function()
        local rows = Osi.DB_WaypointUnlocked:Get(nil, nil)
        count = rows and #rows or 0
    end)
    BG3VR.Log("testing: waypoints unlocked for groups %s (%d waypoint/player rows now); map shroud %s (%s). Save the game "
        .. "to keep it.", table.concat(unlocked, ", "), count, ok and "off" or ("FAILED: " .. tostring(err)), source)
end

local function OnMessage(message)
    if type(message) == "table" and message.Action == "unlock_travel" then
        UnlockTravel("from the client")
    end
end

if Ext.Net and Ext.Net.CreateChannel then
    Ext.Net.CreateChannel(ModuleUUID, kChannel):SetHandler(OnMessage)
else
    Ext.RegisterNetListener(kChannel, function(_, payload)
        local ok, message = pcall(Ext.Json.Parse, payload)
        if ok then
            OnMessage(message)
        end
    end)
end

Ext.RegisterConsoleCommand("vr_unlock_travel", function()
    UnlockTravel("server console")
end)
