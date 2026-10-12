-- Phase 0 probes: confirm the Lua side loads and can read the host character's transform.
-- Run "!vr_probe" in the Script Extender server console.

local function FormatVec(v)
    local parts = {}
    for i = 1, #v do
        parts[#parts + 1] = string.format("%.3f", v[i])
    end
    return "(" .. table.concat(parts, ", ") .. ")"
end

local function ProbeHost()
    local guid = Osi.GetHostCharacter()
    if not guid then
        BG3VR.Warn("no host character yet; load a save first")
        return
    end

    local entity = Ext.Entity.Get(guid)
    local ok, transform = pcall(function() return entity.Transform.Transform end)
    if not ok or not transform then
        BG3VR.Warn("host %s has no readable Transform component: %s", guid, tostring(transform))
        return
    end

    BG3VR.Log("host %s", guid)
    BG3VR.Log("  position %s", FormatVec(transform.Translate))
    BG3VR.Log("  rotation %s", FormatVec(transform.RotationQuat))
    BG3VR.Log("  scale    %s", FormatVec(transform.Scale))
end

Ext.RegisterConsoleCommand("vr_probe", function()
    local ok, err = pcall(ProbeHost)
    if not ok then
        BG3VR.Warn("vr_probe failed: %s", tostring(err))
    end
end)
