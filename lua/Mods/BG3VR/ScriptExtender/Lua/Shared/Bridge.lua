-- Bridge to the native core (bg3vr_core): a key=value file the core polls, under the profile's
-- "Script Extender" folder. Each write replaces the file; the core only acts on keys present.
BG3VR = BG3VR or {}

BG3VR.BridgeFile = "BG3VR/camera_bridge.txt"

-- Object proxies print as "TypeName (<hex address>)".
function BG3VR.ProxyAddress(proxy)
    return string.match(tostring(proxy), "%((%x+)")
end

function BG3VR.WriteBridge(lines)
    return Ext.IO.SaveFile(BG3VR.BridgeFile, table.concat(lines, "\n") .. "\n")
end
