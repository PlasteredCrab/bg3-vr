-- First-person helpers (client context; type "client" in the Script Extender console first).
--   !vr_vis                 list the controlled character's visual parts (slot, template, flags)
--   !vr_head hide [match]   hide head parts from the main view, keeping their shadows
--   !vr_head show           show them again
--   !vr_skel [off]          read the character's skeleton natively: logs every bone and tracks Head_M
--
-- Hiding goes through the native core: it calls the game's own MoveableObject::ToggleCullFlag600
-- on each visual every frame, from the game thread. (Writing the flags from Lua doesn't work: the
-- Script Extender rejects VisualCullFlags values with bits outside its enum, and doesn't map the
-- flags on renderables. The flags act as an inclusion mask: 0x600 = drawn in the main view.)

local HEAD_PATTERNS = { "head", "hair", "beard", "horn", "helmet", "hat", "mask", "circlet", "eye" }

local function ResourceName(visual)
    local ok, resource = pcall(function() return visual.VisualResource end)
    if not ok or not resource then
        return "?", "?"
    end
    return tostring(resource.Slot), tostring(resource.Template)
end

local function BitsOf(value)
    local ok, bits = pcall(function() return value.__Value end)
    return ok and bits or 0
end

-- Calls fn(visual, depth, attachment) for the root visual and its attachments, two levels deep.
local function ForEachVisual(root, fn)
    fn(root, 0, nil)
    for _, attachment in ipairs(root.Attachments) do
        if attachment.Visual then
            fn(attachment.Visual, 1, attachment)
            for _, child in ipairs(attachment.Visual.Attachments) do
                if child.Visual then
                    fn(child.Visual, 2, child)
                end
            end
        end
    end
end

local function RootVisual()
    local character = BG3VR.Controlled()
    local component = character and character.Visual
    return component and component.Visual
end

local function Matches(visual, patterns)
    local slot, template = ResourceName(visual)
    local text = string.lower(slot .. " " .. template)
    for _, pattern in ipairs(patterns) do
        if string.find(text, pattern, 1, true) then
            return true
        end
    end
    return false
end

local function Dump()
    local root = RootVisual()
    if not root then
        BG3VR.Warn("no controlled character visual")
        return
    end
    local count = 0
    ForEachVisual(root, function(visual, depth, attachment)
        count = count + 1
        local slot, template = ResourceName(visual)
        BG3VR.Log("%s[%d] slot=%s template=%s cull=0x%X visual=0x%X attach=0x%X%s", string.rep("  ", depth), count,
            slot, template, BitsOf(visual.CullFlags), BitsOf(visual.VisualFlags),
            attachment and BitsOf(attachment.Flags) or 0, Matches(visual, HEAD_PATTERNS) and "  <- head part" or "")
    end)
end

-- The "hide=" bridge line for the controlled character's head parts (or those matching `match`), and their names;
-- nil when there is no visual or nothing matches.
function HeadHideBridgeLine(match)
    local root = RootVisual()
    if not root then
        return nil, {}
    end
    local patterns = match and { string.lower(match) } or HEAD_PATTERNS
    local addresses, names = {}, {}
    ForEachVisual(root, function(visual, depth)
        if depth > 0 and Matches(visual, patterns) then
            addresses[#addresses + 1] = BG3VR.ProxyAddress(visual)
            names[#names + 1] = (ResourceName(visual))
        end
    end)
    if #addresses == 0 then
        return nil, names
    end
    return "hide=" .. table.concat(addresses, ","), names
end

local function Hide(match)
    if not RootVisual() then
        BG3VR.Warn("no controlled character visual")
        return
    end
    local line, names = HeadHideBridgeLine(match)
    if not line then
        BG3VR.Warn("no matching parts")
        return
    end
    if BG3VR.WriteBridge({ line }) then
        BG3VR.Log("head hide requested for %d parts (%s); see bg3vr.log", #names, table.concat(names, ", "))
    end
end

local function Show()
    if BG3VR.WriteBridge({ "hide=off" }) then
        BG3VR.Log("head show requested")
    end
end

local function Safely(fn)
    return function(...)
        local ok, err = pcall(fn, ...)
        if not ok then
            BG3VR.Warn("%s", tostring(err))
        end
    end
end

Ext.RegisterConsoleCommand("vr_vis", Safely(function() Dump() end))

-- The native "skeleton=" bridge line for the controlled character's root visual, or nil.
function SkeletonBridgeLine()
    local root = RootVisual()
    local address = root and BG3VR.ProxyAddress(root)
    return address and ("skeleton=" .. address) or nil
end

Ext.RegisterConsoleCommand("vr_skel", Safely(function(_, mode)
    if mode == "off" then
        if BG3VR.WriteBridge({ "skeleton=off" }) then
            BG3VR.Log("skeleton reading stopped")
        end
        return
    end
    local line = SkeletonBridgeLine()
    if not line then
        BG3VR.Warn("no controlled character visual")
        return
    end
    if BG3VR.WriteBridge({ line }) then
        BG3VR.Log("reading the character's skeleton (%s); bones and Head_M in bg3vr.log", line)
    end
end))

Ext.RegisterConsoleCommand("vr_head", Safely(function(_, mode, match)
    if mode == "hide" then
        Hide(match)
    elseif mode == "show" then
        Show()
    else
        BG3VR.Warn("usage: !vr_head hide [match] | show")
    end
end))
