Ext.Require("Shared/Log.lua")
Ext.Require("Client/CameraProbe.lua")

-- The client context will host the native bridge (camera, UI, input).
Ext.Events.SessionLoaded:Subscribe(function()
    BG3VR.Log("client session loaded")
end)
