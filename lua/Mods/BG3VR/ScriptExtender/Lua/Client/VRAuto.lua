-- VR start and stop (client context).
--
-- With the headset ready (the core writes xr=ready to BG3VR/to_lua.txt) and a game running, VR starts by itself
-- ([vr] autostart in bg3vr.ini, on by default), and again after every level load:
--   follow the controlled character at eye height (calibrated by the core from the head bone), create the second
--   eye view, read the skeleton, hide the head from the eye views, turn on the [vr] profile in the core (hand
--   pointing, stick locomotion, controller bindings, turning), and three seconds later same-frame stereo.
-- Switching the controlled character moves all of that to the new character. When the headset stops, VR stops.
--
--   !vr_go [height]   start by hand (also when autostart is off)
--   !vr_stop          stop (the game camera comes back); autostart waits until the headset goes off and on again

local auto = {
    started = false,
    character = nil,  -- the controlled character VR was set up for
    stereoIn = nil,   -- seconds until stereo=on
    running = false,  -- client game state is Running
    checkIn = 0.0,    -- seconds until the next status read
    suppressed = false, -- stopped by hand: don't restart until the headset goes off
    lastXr = nil,
    third = false,       -- third person (the game's camera on a screen in the headset)
    chase = false,       -- third person in VR (stereo, the view behind the character; the core places it)
    free = false,        -- free camera (stereo; the sticks fly the view, the character stays)
    lastRequest = nil,   -- the last request= sequence handled
}

local function CharacterKey()
    local character = BG3VR.Controlled()
    return character and tostring(character) or nil
end

-- Everything VR needs for the controlled character, merged into one bridge write by the queue.
function VRStart(height, reason)
    if auto.third then
        -- Third person: VR play on, the game keeps its camera.
        BG3VR.WriteBridge({ "vr=start", "view=third", "stereo=off", "pose=off", "follow=off", "hide=off" })
        auto.started = true
        auto.character = CharacterKey()
        BG3VR.Log("VR started in third person (%s)", reason or "by hand")
        return true
    end
    local extra = { "views=create", "vr=start", auto.free and "view=free" or auto.chase and "view=chase" or "view=first" }
    local skeleton = SkeletonBridgeLine and SkeletonBridgeLine()
    if skeleton then
        extra[#extra + 1] = skeleton
    end
    -- In third person (VR) the whole character is seen: no head hiding.
    local hide, names = nil, {}
    if not auto.chase and not auto.free and HeadHideBridgeLine then
        hide, names = HeadHideBridgeLine()
    end
    extra[#extra + 1] = hide or "hide=off"
    auto.hideLine = hide or "hide=off"
    BG3VR.Log("VR start: hiding %d head part(s): %s", #names, table.concat(names, ", "))
    if not StartNativeFollow(height or 1.7, extra) then
        return false
    end
    auto.started = true
    auto.character = CharacterKey()
    auto.stereoIn = 3.0
    BG3VR.Log("VR started (%s); stereo in 3 s", reason or "by hand")
    return true
end

function VRStop(reason)
    BG3VR.WriteBridge({ "vr=stop", "stereo=off", "follow=off", "pose=off", "hide=off", "skeleton=off" })
    auto.started = false
    auto.character = nil
    auto.stereoIn = nil
    BG3VR.Log("VR stopped (%s)", reason or "by hand")
end

-- A different character is now controlled: move skeleton reading and head hiding to it (the follow tick in
-- CameraProbe.lua moves the camera itself).
local function OnCharacterChanged()
    local lines = {}
    local skeleton = SkeletonBridgeLine and SkeletonBridgeLine()
    if skeleton then
        lines[#lines + 1] = skeleton
    end
    -- The head hiding follows the camera's body (the status check below), not this: force a refresh.
    auto.hideCheck = 0
    if #lines > 0 then
        BG3VR.WriteBridge(lines)
    end
    auto.character = CharacterKey()
    BG3VR.Log("VR: now playing %s", tostring(auto.character))
end

-- What the core asks for, through request=<n> <text>|<n> <text>|... (the last 16, numbered): first / third person
-- (the "view" controller binding, the VR menu), holster events (Client/Holster.lua).
local function HandleOne(what)
    local holster, zone, hand, holds = string.match(what, "^holster (%S+) (%S+) (%S+) ?(%S*)")
    if holster then
        if auto.started and BG3VR.OnHolster then
            BG3VR.OnHolster(holster, zone, hand, holds ~= "" and holds or nil)
        end
        return
    end
    local stow, stowHand = string.match(what, "^stow (0x%x+) ?(%S*)")
    if stow then
        if auto.started and BG3VR.OnStowItem then
            BG3VR.OnStowItem(stow, stowHand)
        end
        return
    end
    local moved, how = string.match(what, "^moved (0x%x+) ?(%S*)")
    if moved then
        if auto.started and BG3VR.OnMovedItem then
            BG3VR.OnMovedItem(moved, how)
        end
        return
    end
    if what == "unlock_travel" then
        if BG3VR.UnlockTravel then
            BG3VR.UnlockTravel()
        end
        return
    end
    if what == "view_free" then
        -- Free camera: as third person in VR, but the core stops following and the sticks fly the view.
        auto.third = false
        auto.chase = false
        auto.free = true
        if auto.started then
            BG3VR.WriteBridge({ "view=free", "hide=off" })
            BG3VR.Log("VR: free camera")
        end
        return
    end
    if what == "view_chase" then
        -- Third person in VR: everything as in first person, the core places the view behind the character.
        auto.third = false
        auto.free = false
        auto.chase = true
        if auto.started then
            VRStart(1.7, "third person in VR")
        end
        return
    end
    if what == "view_third" or what == "view_first" then
        auto.third = what == "view_third"
        auto.chase = false
        auto.free = false
        if auto.started then
            if auto.third then
                auto.stereoIn = nil -- a pending stereo=on from a just-started first person mustn't follow
                BG3VR.WriteBridge({ "view=third", "stereo=off", "pose=off", "follow=off", "hide=off" })
                BG3VR.Log("VR: third person")
            else
                VRStart(1.7, "back to first person")
            end
        end
    end
end

local function HandleRequest(status)
    local entries = {}
    for entry in string.gmatch(status.request or "", "[^|]+") do
        local seq, what = string.match(entry, "^(%d+) (.+)$")
        if seq then
            entries[#entries + 1] = { tonumber(seq), what }
        end
    end
    if #entries == 0 then
        auto.lastRequest = auto.lastRequest or 0 -- nothing asked yet: the first request counts
        return
    end
    if auto.lastRequest == nil then
        auto.lastRequest = entries[#entries][1] -- from before this Lua state: don't replay them
        return
    end
    for _, e in ipairs(entries) do
        if e[1] > auto.lastRequest then
            auto.lastRequest = e[1]
            HandleOne(e[2])
        end
    end
end

local function Tick(dt)
    if auto.stereoIn then
        auto.stereoIn = auto.stereoIn - dt
        if auto.stereoIn <= 0 then
            auto.stereoIn = nil
            if auto.started then
                BG3VR.WriteBridge({ "stereo=on" })
                BG3VR.Log("VR: same-frame stereo on")
            end
        end
    end
    auto.checkIn = auto.checkIn - dt
    if auto.checkIn > 0 then
        return
    end
    auto.checkIn = 0.25
    local status = BG3VR.ReadStatus()
    BG3VR.ActingHex = status.acting ~= "" and status.acting or nil -- the hotbar's character (Controlled.lua)
    if BG3VR.UpdateIdles then
        BG3VR.UpdateIdles(status) -- idle animations off in first person (Idles.lua)
    end
    -- The head parts of the body the camera is in (the character, or a scene's stand-in for it), hidden. Rebuilt
    -- once a second in first person and sent when it changed: a stand-in gets its head, hair and equipment copied
    -- from the character after it is shown, and helmets come and go, so a list built once at the switch went stale
    -- (2026-10-26: the head showed in cutscenes although two stand-in parts were hidden).
    -- The body is the one the core's camera is in ("body" status: the character, a party member the game handed control
    -- to, or a scene's stand-in), so the hidden head is always that body's.
    local followed = status.followed ~= nil and status.followed ~= "" and status.followed or nil
    local body = status.body ~= nil and status.body ~= "" and status.body or followed
    local firstPerson = auto.started and not auto.third and not auto.chase and not auto.free
    local switched = auto.started and body ~= auto.followed
    auto.hideCheck = (auto.hideCheck or 0) - 1
    if switched or (firstPerson and auto.hideCheck <= 0) then
        auto.followed = auto.started and body or nil
        auto.hideCheck = 4
        if firstPerson and HeadHideBridgeLine then
            local entity = nil
            if body then
                local ok, found = pcall(function()
                    return Ext.Entity.Get(Ext.Utils.IntegerToHandle(math.tointeger(tonumber(body))))
                end)
                entity = ok and found or nil
            end
            local hide, names = HeadHideBridgeLine(nil, entity)
            local line = hide or "hide=off"
            if switched or line ~= auto.hideLine then
                auto.hideLine = line
                BG3VR.WriteBridge({ line })
                BG3VR.Log("VR: %s %s; hiding %d head part(s): %s", switched and "following" or "the head parts changed on",
                    followed and ("the stand-in " .. followed) or ("the body " .. tostring(body)), #names, table.concat(names, ", "))
            end
        end
    end
    HandleRequest(status)
    local ready = status.xr == "ready" and status.mode ~= "screen"
    if status.xr ~= auto.lastXr then
        auto.lastXr = status.xr
        if status.xr ~= "ready" then
            auto.suppressed = false -- the headset went off: a later start is wanted again
        end
    end
    if auto.started then
        if not ready then
            VRStop("the headset stopped")
        elseif auto.running and CharacterKey() and CharacterKey() ~= auto.character then
            OnCharacterChanged()
        end
        -- In combat or not (the game UI, hidden by default in VR, comes up in combat): SE's IsInCombat tag
        -- (eoc::combat::IsInCombatComponent) or a combat handle on CombatParticipant.
        local character = BG3VR.Controlled()
        local ok, inCombat = pcall(function()
            if not character then
                return false
            end
            local participant = character.CombatParticipant
            return character.IsInCombat ~= nil or (participant ~= nil and participant.CombatHandle ~= nil)
        end)
        inCombat = ok and inCombat or false
        if inCombat ~= auto.inCombat then
            auto.inCombat = inCombat
            BG3VR.WriteBridge({ "incombat=" .. (inCombat and "on" or "off") })
            BG3VR.Log("VR: %s combat", inCombat and "in" or "out of")
        end
        return
    end
    if ready and auto.running and status.autostart ~= "0" and not auto.suppressed and _C() then
        VRStart(1.7, "headset ready")
    end
end

-- Loaded into a game that is already running (e.g. after a Lua reset): no state change will tell us.
do
    local ok, state = pcall(Ext.Utils.GetGameState)
    auto.running = ok and tostring(state) == "Running"
end

Ext.Events.Tick:Subscribe(function(e)
    local ok, err = pcall(Tick, (e and e.Time and e.Time.DeltaTime) or 0.016)
    if not ok then
        BG3VR.Warn("VR auto: %s", tostring(err))
    end
end)

Ext.Events.GameStateChanged:Subscribe(function(e)
    local to = tostring(e.ToState)
    auto.running = to == "Running"
    if auto.started and not auto.running then
        -- Loading or leaving: views, visuals and transforms may be freed. vr=pause drops everything that points at
        -- them without touching them (stereo=off / hide=off would restore through them); start again once running.
        auto.started = false
        auto.stereoIn = nil
        BG3VR.WriteBridge({ "vr=pause", "pose=off", "follow=off", "skeleton=off" })
        BG3VR.Log("VR paused (game state %s)", to)
    end
end)

Ext.RegisterConsoleCommand("vr_stop", function()
    auto.suppressed = true
    VRStop("!vr_stop")
end)
