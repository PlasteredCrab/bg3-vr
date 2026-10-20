-- Physical melee, client side. The core publishes each swing that hits something as
--   hit=<sequence> <x> <y> <z> <speed>
-- in BG3VR/to_lua.txt ([combat] physical in bg3vr.ini; native/core/src/melee.cpp). Here the hit point becomes a target:
-- the nearest living character (not the one being played) within 1.2 m of it. The server then runs the game's normal
-- weapon attack (Server/Combat.lua: Osi.Attack(attacker, target, 0)), so the dice decide as usual.

local kChannel = "BG3VR_PhysicalHit"
-- Ext.Net.CreateChannel where the Script Extender has it, else the older PostMessageToServer with a JSON payload.
local channel = Ext.Net and Ext.Net.CreateChannel and Ext.Net.CreateChannel(ModuleUUID, kChannel) or nil
local function SendToServer(message)
    if channel then
        channel:SendToServer(message)
    else
        Ext.Net.PostMessageToServer(kChannel, Ext.Json.Stringify(message))
    end
end
local lastSequence = nil
local kRadius = 1.2

local function Position(entity)
    local ok, t = pcall(function() return entity.Transform.Transform.Translate end)
    return ok and t or nil
end

local function Alive(entity)
    local ok, hp = pcall(function() return entity.Health.Hp end)
    return not ok or hp == nil or hp > 0
end

local function Uuid(entity)
    local ok, uuid = pcall(function() return entity.Uuid.EntityUuid end)
    return ok and uuid or nil
end

local function NearestCharacter(x, y, z, exclude)
    local best, bestDistance = nil, kRadius * kRadius
    for _, entity in ipairs(Ext.Entity.GetAllEntitiesWithComponent("ClientCharacter")) do
        if entity ~= exclude and Alive(entity) then
            local p = Position(entity)
            if p then
                -- Characters' transforms sit at their feet: compare horizontally, and allow the body's height.
                local dx, dz = p[1] - x, p[3] - z
                local dy = math.max(0, math.max(p[2] - y, y - (p[2] + 2.2)))
                local d = dx * dx + dy * dy + dz * dz
                if d < bestDistance then
                    best, bestDistance = entity, d
                end
            end
        end
    end
    return best, math.sqrt(bestDistance)
end

local function OnHit(sequence, x, y, z, speed)
    local attacker = _C()
    if not attacker then
        return
    end
    local target, distance = NearestCharacter(x, y, z, attacker)
    if not target then
        BG3VR.Log("combat: swing %d hit (%.2f, %.2f, %.2f) at %.1f m/s: no character there", sequence, x, y, z, speed)
        return
    end
    local attackerUuid, targetUuid = Uuid(attacker), Uuid(target)
    if not attackerUuid or not targetUuid then
        BG3VR.Warn("combat: no UUID for the attacker or the target")
        return
    end
    BG3VR.Log("combat: swing %d hits %s (%.2f m from the hit point, %.1f m/s); asking the server to attack", sequence,
        targetUuid, distance, speed)
    SendToServer({ Attacker = attackerUuid, Target = targetUuid, Speed = speed })
end

local function Tick()
    local status = BG3VR.ReadStatus()
    local hit = status.hit
    if not hit then
        return
    end
    local sequence, x, y, z, speed = string.match(hit, "^(%d+) (%S+) (%S+) (%S+) (%S+)")
    sequence = tonumber(sequence)
    if not sequence then
        return
    end
    if lastSequence == nil then
        lastSequence = sequence -- a hit from before this Lua state started: don't replay it
        return
    end
    if sequence ~= lastSequence then
        lastSequence = sequence
        OnHit(sequence, tonumber(x), tonumber(y), tonumber(z), tonumber(speed))
    end
end

-- What the controlled character has drawn, for the core's swing and bow handling (combat.cpp): bridge
-- wield=melee|bow|crossbow|none. eoc::unsheath::StateComponent (SE "Unsheath"): State 1 melee, 2 ranged (agent K/L);
-- a ranged weapon is a bow or a crossbow by its stats' Proficiency Group.
local lastWield = nil
local wieldCheckIn = 0

local function RangedKind(weaponEntity)
    local ok, statsId = pcall(function() return weaponEntity.Data.StatsId end)
    if not ok or not statsId then
        return "bow"
    end
    local stat = Ext.Stats.Get(statsId)
    local ok2, groups = pcall(function() return stat["Proficiency Group"] end)
    local text = ""
    if ok2 and type(groups) == "table" then
        for _, g in pairs(groups) do
            text = text .. tostring(g) .. " "
        end
    elseif ok2 and groups then
        text = tostring(groups)
    end
    return string.find(text, "Crossbow") and "crossbow" or "bow"
end

local function Wield()
    local character = _C()
    if not character then
        return "none"
    end
    local ok, unsheath = pcall(function() return character.Unsheath end)
    if not ok or not unsheath then
        return "none"
    end
    local state = tostring(unsheath.State)
    if state == "Melee" or state == "1" then
        return "melee"
    elseif state == "Ranged" or state == "2" then
        local ok2, weapon = pcall(function() return unsheath.MainHandWeapon end)
        return (ok2 and weapon) and RangedKind(weapon) or "bow"
    end
    return "none"
end

local function WieldTick(dt)
    wieldCheckIn = wieldCheckIn - dt
    if wieldCheckIn > 0 then
        return
    end
    wieldCheckIn = 0.25
    local wield = Wield()
    if wield ~= lastWield then
        lastWield = wield
        BG3VR.WriteBridge({ "wield=" .. wield })
        BG3VR.Log("combat: wielding %s", wield)
    end
end

Ext.Events.Tick:Subscribe(function(e)
    local ok, err = pcall(Tick)
    if not ok then
        BG3VR.Warn("combat: %s", tostring(err))
    end
    ok, err = pcall(WieldTick, (e and e.Time and e.Time.DeltaTime) or 0.016)
    if not ok then
        BG3VR.Warn("combat: %s", tostring(err))
    end
end)
