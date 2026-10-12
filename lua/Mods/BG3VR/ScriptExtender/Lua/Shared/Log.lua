BG3VR = BG3VR or {}

function BG3VR.Log(fmt, ...)
    Ext.Utils.Print(string.format("[BG3VR] " .. fmt, ...))
end

function BG3VR.Warn(fmt, ...)
    Ext.Utils.PrintWarning(string.format("[BG3VR] " .. fmt, ...))
end
