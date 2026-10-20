-- Bridge to the native core (bg3vr_core): a key=value file the core polls, under the profile's
-- "Script Extender" folder. Each write replaces the file; the core only acts on keys present.
--
-- The core checks the file's write time every 250 ms, so two writes closer together than that would lose the first.
-- Writes are therefore queued and merged: lines written within 0.3 s of the last write go out together on a later
-- tick (the core reads every key of one write, so merged lines all act; for a repeated key the last one wins).
BG3VR = BG3VR or {}

BG3VR.BridgeFile = "BG3VR/camera_bridge.txt"
BG3VR.StatusFile = "BG3VR/to_lua.txt" -- written by the core (bridge::write_status)

local kMinInterval = 0.3
local pending = nil
local clock = 0.0
local lastWrite = -1.0

-- Object proxies print as "TypeName (<hex address>)".
function BG3VR.ProxyAddress(proxy)
    return string.match(tostring(proxy), "%((%x+)")
end

function BG3VR.FlushBridge()
    if not pending then
        return true
    end
    local ok = Ext.IO.SaveFile(BG3VR.BridgeFile, table.concat(pending, "\n") .. "\n")
    pending = nil
    lastWrite = clock
    return ok
end

function BG3VR.WriteBridge(lines)
    pending = pending or {}
    for _, line in ipairs(lines) do
        pending[#pending + 1] = line
    end
    if clock - lastWrite >= kMinInterval then
        return BG3VR.FlushBridge()
    end
    return true -- goes out with the next flush
end

-- The core's state as key=value pairs (empty when the file isn't there).
function BG3VR.ReadStatus()
    local ok, text = pcall(Ext.IO.LoadFile, BG3VR.StatusFile)
    local status = {}
    if ok and text then
        for key, value in string.gmatch(text, "([%w_]+)=([^\r\n]*)") do
            status[key] = value
        end
    end
    return status
end

Ext.Events.Tick:Subscribe(function(e)
    clock = clock + ((e and e.Time and e.Time.DeltaTime) or 0.016)
    if pending and clock - lastWrite >= kMinInterval then
        BG3VR.FlushBridge()
    end
end)
