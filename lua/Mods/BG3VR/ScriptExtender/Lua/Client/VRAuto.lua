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
    lastRequest = nil,   -- the last request= sequence handled
}

local function CharacterKey()
    local character = _C()
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
    local extra = { "views=create", "vr=start", "view=first" }
    local skeleton = SkeletonBridgeLine and SkeletonBridgeLine()
    if skeleton then
        extra[#extra + 1] = skeleton
    end
    local hide = HeadHideBridgeLine and HeadHideBridgeLine()
    if hide then
        extra[#extra + 1] = hide
    end
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
    local hide = HeadHideBridgeLine and HeadHideBridgeLine()
    lines[#lines + 1] = hide or "hide=off"
    BG3VR.WriteBridge(lines)
    auto.character = CharacterKey()
    BG3VR.Log("VR: now playing %s", tostring(auto.character))
end

-- First / third person, asked for by the core (the "view" controller binding) through request=<n> view_<mode>.
local function HandleRequest(status)
    local seq, what = string.match(status.request or "", "^(%d+) (%S+)")
    if not seq or seq == auto.lastRequest then
        return
    end
    if auto.lastRequest == nil then
        auto.lastRequest = seq -- from before this Lua state: don't replay it
        return
    end
    auto.lastRequest = seq
    if what == "view_third" or what == "view_first" then
        auto.third = what == "view_third"
        if auto.started then
            if auto.third then
                BG3VR.WriteBridge({ "view=third", "stereo=off", "pose=off", "follow=off", "hide=off" })
                BG3VR.Log("VR: third person")
            else
                VRStart(1.7, "back to first person")
            end
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
