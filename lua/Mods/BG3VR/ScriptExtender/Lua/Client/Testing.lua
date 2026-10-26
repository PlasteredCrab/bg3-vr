-- Testing helpers, client side: !vr_unlock_travel asks the server (Server/Testing.lua) to unlock every waypoint and
-- turn the map's shroud off for the loaded game. Save afterwards to keep it in that save.

local kChannel = "BG3VR_Testing"
local channel = Ext.Net and Ext.Net.CreateChannel and Ext.Net.CreateChannel(ModuleUUID, kChannel) or nil

local function SendToServer(message)
    if channel then
        channel:SendToServer(message)
    else
        Ext.Net.PostMessageToServer(kChannel, Ext.Json.Stringify(message))
    end
end

function BG3VR.UnlockTravel()
    SendToServer({ Action = "unlock_travel" })
    BG3VR.Log("testing: asked the server to unlock every waypoint and the map")
end

Ext.RegisterConsoleCommand("vr_unlock_travel", function()
    BG3VR.UnlockTravel()
end)
