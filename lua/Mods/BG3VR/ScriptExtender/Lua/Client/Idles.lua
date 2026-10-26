-- Idle animations off in first person (agent AX, AX_idle_animations.md): the character's animation blueprint plays
-- a full-body random idle every 20-45 s and additive idles every 5-15 s, both gated by its IDLE_IsOn variable (435),
-- which only the blueprint's own IdleTurnOff / IdleTurnOn events set. The game sends IdleTurnOff to the character
-- creation preview the same way (0x2038C80 -> QueueGameplayEventToGenome 0x3BD4B90). A random idle playing when it
-- goes off ends at once; the base standing idle keeps playing. Back on: DisallowIdle first (drops a trigger latched
-- while off), then IdleTurnOn. The core decides when ("idles" status: off while [vr] first_person_no_idles is set and
-- the view is first person; third person and the free camera keep the game's idles).

local kIsOn = 435 + 1 -- IDLE_IsOn, Lua index into Instance.Variables (Characters_Common.lsbc; same in the creature
                       -- and mind flayer blueprints)

local idle = { off = false, entity = nil, key = nil, check = 0, logged = 0 }

local function System()
    local ok, system = pcall(function() return Ext.System.AnimationBlueprint end)
    return ok and system or nil
end

local function Instance(entity)
    local ok, instance = pcall(function() return entity.AnimationBlueprint.Instance end)
    return ok and instance or nil
end

local function IsOnValue(entity)
    local instance = Instance(entity)
    if not instance then
        return "no instance"
    end
    local ok, value = pcall(function() return instance.Variables[kIsOn] end)
    if not ok then
        return "unreadable"
    end
    if type(value) == "table" or type(value) == "userdata" then
        local okv, inner = pcall(function() return value.Value end)
        return tostring(okv and inner or value)
    end
    return tostring(value)
end

local function Send(entity, events)
    local system = System()
    if not system then
        BG3VR.Warn("idles: Ext.System.AnimationBlueprint isn't available on this client; idles stay as they are")
        return false
    end
    for _, event in ipairs(events) do
        local ok, err = pcall(function() system:QueueGameplayEventToGenome(entity, event, {}) end)
        if not ok then
            -- Some builds want the handle rather than the entity object.
            ok, err = pcall(function() system:QueueGameplayEventToGenome(entity.Handle, event, {}) end)
        end
        BG3VR.Log("idles: %s queued for %s%s", event, tostring(entity), ok and "" or (" FAILED: " .. tostring(err)))
        if not ok then
            return false
        end
    end
    return true
end

-- Called with each status read (VRAuto.lua, four times a second).
function BG3VR.UpdateIdles(status)
    local wantOff = status.idles == "off"
    local entity = wantOff and BG3VR.Controlled() or nil
    local key = entity and (tostring(entity) .. "/" .. tostring(Instance(entity))) or nil
    if wantOff and key ~= idle.key then
        -- First person, a new character or a new blueprint instance (which starts with idles on): off for it, back on
        -- for the one before.
        if idle.off and idle.entity and idle.entity ~= entity then
            Send(idle.entity, { "DisallowIdle", "IdleTurnOn" })
        end
        if entity and Send(entity, { "IdleTurnOff" }) then
            idle.off, idle.entity, idle.key, idle.check = true, entity, key, 2
            BG3VR.Log("idles: off for %s in first person (IDLE_IsOn was %s)", tostring(entity), IsOnValue(entity))
        else
            idle.key = key -- don't retry every tick; a new key retries
        end
    elseif not wantOff and idle.off then
        if idle.entity then
            Send(idle.entity, { "DisallowIdle", "IdleTurnOn" })
            BG3VR.Log("idles: back on for %s (left first person or the setting is off)", tostring(idle.entity))
        end
        idle.check = 2
        idle.off, idle.key = false, nil
    end
    -- A moment later: did the blueprint take it? (the queue is drained by the next animation update)
    if idle.check > 0 then
        idle.check = idle.check - 1
        if idle.check == 0 and idle.entity then
            BG3VR.Log("idles: IDLE_IsOn of %s is now %s (expected %s)", tostring(idle.entity), IsOnValue(idle.entity),
                idle.off and "false" or "true")
            if not idle.off then
                idle.entity = nil
            end
        end
    end
end
