BG3VR = BG3VR or {}

-- Also into bg3vr.log ("lua:" lines) through the bridge: the Script Extender keeps no log file, so the Lua side's
-- decisions (VR start / stop and why, idles, head hiding) were invisible in a run's log.
local function ToCore(text)
    if BG3VR.WriteBridge and Ext.IsClient and Ext.IsClient() then
        pcall(BG3VR.WriteBridge, { "lua=" .. string.gsub(text, "[\r\n]", " ") })
    end
end

function BG3VR.Log(fmt, ...)
    local text = string.format(fmt, ...)
    Ext.Utils.Print("[BG3VR] " .. text)
    ToCore(text)
end

function BG3VR.Warn(fmt, ...)
    local text = string.format(fmt, ...)
    Ext.Utils.PrintWarning("[BG3VR] " .. text)
    ToCore("WARNING: " .. text)
end
