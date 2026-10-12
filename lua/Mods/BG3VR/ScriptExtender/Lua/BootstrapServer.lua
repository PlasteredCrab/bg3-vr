Ext.Require("Shared/Log.lua")
Ext.Require("Server/Probe.lua")

Ext.Events.SessionLoaded:Subscribe(function()
    BG3VR.Log("server session loaded")
end)
