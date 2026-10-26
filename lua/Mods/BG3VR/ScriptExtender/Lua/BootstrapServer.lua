Ext.Require("Shared/Log.lua")
Ext.Require("Server/Probe.lua")
Ext.Require("Server/Combat.lua")
Ext.Require("Server/Holster.lua")
Ext.Require("Server/Testing.lua")

Ext.Events.SessionLoaded:Subscribe(function()
    BG3VR.Log("server session loaded")
end)
