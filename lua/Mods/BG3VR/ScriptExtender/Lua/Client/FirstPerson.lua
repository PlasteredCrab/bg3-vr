-- First-person helpers (client context; type "client" in the Script Extender console first).
--   !vr_vis                       list the controlled character's visual parts (slot, template, flags)
--   !vr_head hide [mask] [match]  hide head parts from the main view, keeping their shadows
--   !vr_head show                 restore them
--
-- Despite the Script Extender's name, VisualCullFlags acts as an inclusion mask: every visible part
-- has 0x200 and 0x400 set, and every shadow-casting part has the shadow bits (0x04, 0x08, 0x10,
-- 0x8000) set (seen 2026-10-12: head/hair 0x86FF, armour 0x867F). Hiding therefore CLEARS `mask`
-- (default 0x600) on matching visuals' renderables (and on the visuals themselves where the Script
-- Extender allows it), and leaves the shadow bits alone.
-- While hidden, the flags are re-checked every few ticks and cleared again if the game restores them.

local HEAD_PATTERNS = { "head", "hair", "beard", "horn", "helmet", "hat", "mask", "circlet", "eye" }
local DEFAULT_MASK = 0x600

local function ResourceName(visual)
    local ok, resource = pcall(function() return visual.VisualResource end)
    if not ok or not resource then
        return "?", "?"
    end
    return tostring(resource.Slot), tostring(resource.Template)
end

local function BitsOf(value)
    if type(value) == "number" then
        return value
    end
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
    local character = _C()
    local component = character and character.Visual
    return component and component.Visual
end

local function MatchesHead(visual, patterns)
    local slot, template = ResourceName(visual)
    local text = string.lower(slot .. " " .. template)
    for _, pattern in ipairs(patterns) do
        if string.find(text, pattern, 1, true) then
            return true
        end
    end
    return false
end

local function RenderableFlags(visual)
    local parts = {}
    for _, desc in ipairs(visual.ObjectDescs) do
        if desc.Renderable then
            parts[#parts + 1] = string.format("0x%X", BitsOf(desc.Renderable.CullFlags))
        end
    end
    return table.concat(parts, ",")
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
        BG3VR.Log("%s[%d] slot=%s template=%s cull=0x%X renderables=[%s] visual=0x%X attach=0x%X%s",
            string.rep("  ", depth), count, slot, template, BitsOf(visual.CullFlags), RenderableFlags(visual),
            BitsOf(visual.VisualFlags), attachment and BitsOf(attachment.Flags) or 0,
            MatchesHead(visual, HEAD_PATTERNS) and "  <- head part" or "")
    end)
end

local hidden = { handle = nil, mask = DEFAULT_MASK, patterns = HEAD_PATTERNS, ticks = 0, reapplied = 0 }

-- Returns the flags with `mask` cleared (hide) or set (show).
local function Adjusted(bits, hide)
    return hide and (bits & ~hidden.mask) or (bits | hidden.mask)
end

-- Applies hide/show to matching visuals and their renderables; returns (parts matched, values changed).
local function Apply(hide)
    local root = RootVisual()
    if not root then
        return 0, 0
    end
    local matched, changed = 0, 0
    ForEachVisual(root, function(visual, depth)
        if depth == 0 or not MatchesHead(visual, hidden.patterns) then
            return
        end
        matched = matched + 1
        -- The Script Extender rejects VisualCullFlags values containing bits its enum doesn't name
        -- (the game uses 0x01, 0x20, 0x40, 0x100 too), so this usually fails; renderables below are
        -- plain uint16 and always writable.
        local bits = BitsOf(visual.CullFlags)
        local wanted = Adjusted(bits, hide)
        if wanted ~= bits and pcall(function() visual.CullFlags = wanted end) then
            changed = changed + 1
        end
        for _, desc in ipairs(visual.ObjectDescs) do
            local renderable = desc.Renderable
            if renderable then
                local rbits = BitsOf(renderable.CullFlags)
                local rwanted = Adjusted(rbits, hide)
                if rwanted ~= rbits then
                    renderable.CullFlags = rwanted
                    changed = changed + 1
                end
            end
        end
    end)
    return matched, changed
end

local function StopWatching()
    if hidden.handle then
        Ext.Events.Tick:Unsubscribe(hidden.handle)
        hidden.handle = nil
    end
end

local function Hide(mask, match)
    StopWatching()
    hidden.mask = tonumber(mask) or DEFAULT_MASK
    hidden.patterns = match and { string.lower(match) } or HEAD_PATTERNS
    hidden.ticks, hidden.reapplied = 0, 0
    local matched, changed = Apply(true)
    BG3VR.Log("head hide: %d parts matched, %d flag values cleared (mask 0x%X)", matched, changed, hidden.mask)
    hidden.handle = Ext.Events.Tick:Subscribe(function()
        hidden.ticks = hidden.ticks + 1
        if hidden.ticks % 10 ~= 0 then
            return
        end
        local ok, _, reapplied = pcall(Apply, true)
        if ok and reapplied > 0 then
            hidden.reapplied = hidden.reapplied + 1
            if hidden.reapplied <= 5 then
                BG3VR.Log("head hide: game restored the flags; cleared %d values again (tick %d)", reapplied,
                    hidden.ticks)
            end
        end
    end)
end

local function Show()
    StopWatching()
    local matched, changed = Apply(false)
    BG3VR.Log("head show: %d parts matched, %d flag values restored (game restored them %d times while hidden)",
        matched, changed, hidden.reapplied)
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

Ext.RegisterConsoleCommand("vr_head", Safely(function(_, mode, mask, match)
    if mode == "hide" then
        Hide(mask, match)
    elseif mode == "show" then
        Show()
    else
        BG3VR.Warn("usage: !vr_head hide [mask] [match] | show")
    end
end))
