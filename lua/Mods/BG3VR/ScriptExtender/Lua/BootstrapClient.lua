Ext.Require("Shared/Log.lua")

-- The client context will host the native bridge (camera, UI, input). Phase 0 only confirms it loads.
Ext.Events.SessionLoaded:Subscribe(function()
    BG3VR.Log("client session loaded")
end)
