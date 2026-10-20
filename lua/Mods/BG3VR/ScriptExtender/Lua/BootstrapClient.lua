Ext.Require("Shared/Log.lua")
Ext.Require("Shared/Bridge.lua")
Ext.Require("Client/CameraProbe.lua")
Ext.Require("Client/FirstPerson.lua")
Ext.Require("Client/VRAuto.lua")
Ext.Require("Client/Combat.lua")

-- The client context will host the native bridge (camera, UI, input).
Ext.Events.SessionLoaded:Subscribe(function()
    BG3VR.Log("client session loaded")
end)
