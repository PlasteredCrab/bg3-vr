-- Phase 0 camera probes (client context). In the Script Extender console, type "client" first.
--   !vr_cam                   list camera entities: controller state, real position (from the
--                             inverse view matrix), forward vector, FOV, components
--   !vr_cam_fov <degrees>     set the active camera's FOV once (stored in radians)
--   !vr_cam_follow <mode> [height] [yaw]
--                             every tick, place the active camera at the controlled character's
--                             position + height (default 1.7) facing its rotation + yaw degrees.
--                             mode: "view" writes the view matrices, "transform" writes
--                             WorldTransform, "lookat" writes LookAt; "off" stops
--
-- Native camera hook (bg3vr_core), through the bridge file Script Extender\BG3VR\camera_bridge.txt:
--   !vr_cam_set here [height] [yaw] [pitch]
--                             hold the camera at the character's position + height (default 1.7),
--                             facing where the character faces + yaw degrees, pitched up by pitch
--                             degrees (-90..90)
--   !vr_cam_calib             run, walk forward, run again: measures the character facing offset
--   !vr_cam_set <x> <y> <z> <yaw> <pitch>
--                             hold the camera at an explicit pose (yaw 0 looks along +Z)
--   !vr_cam_set off           release the camera
--   !vr_cam_probe             count vtable calls and find what writes the camera (bg3vr.log)
--   !vr_mouselook on|off      while held, middle-mouse drag turns the camera (native; on by default)
--   !vr_cam_eye left|right|off | <l> <r> <u> <d>
--                             per-eye asymmetric projection on the camera (approximate Quest 2 values)
--   !vr_cam_sweep [yaw] [pitch] [seconds] | off
--                             while held, the camera pans and nods by itself (default 60 20 8)
--   !vr_xr_lag <0..7>|auto    headset: camera updates between a frame's pose and its display
--                             (auto finds it; set it by hand if the world swims when you turn)
--   !vr_views [find|dump]     find the render views that draw the camera (memory scan, a few seconds;
--                             results in bg3vr.log); dump re-logs them; sbs puts the first two views
--                             side by side (after !vr_split on), swap the other way round, full puts them back;
--                             set <view> <sx> <sy> <ox> <oy> places one view, enable <view> 0|1 turns it off/on
--   !vr_follow [height] | off  hold the camera at the character's eyes and keep it there as they move
--                             (native, every frame; your yaw stays put while walking)
--   !vr_recenter              headset: your current head position and facing become the character's eyes
--   !vr_stereo on|off|report  same-frame stereo on the second view (after !vr_views create)
--   !vr_go [height]           start VR by hand (normally automatic: Client/VRAuto.lua); !vr_stop stops it
--   !vr_hands [buzz]          log the motion controllers (poses, buttons, bound profile); buzz pulses both
--   !vr_ray camera|hand|off|report  physics raycast probe from the screen centre or the right hand's aim
--   !vr_quality               log LOD / texture streaming state per eye view ([quality] in bg3vr.ini)
--   !vr_pick hand|off         point with the right controller instead of the mouse (hover, path preview, click target)
--   !vr_move stick|off        walk with the left thumbstick (relative to where you look)
--   !vr_fps                   log the frame rate since the last report (and the other native reports)
--   !vr_trace on|off|dump     log who calls the render view / pipeline functions (reverse engineering)
--   !vr_split [on|off]        set the game's ForceSplitscreen global switch (no argument: show it);
--                             then !vr_cam and !vr_views find to see what split screen created
--
-- Matrices are glm mat4: 16 numbers, column-major (translation in elements 13..15).

local function Fmt(v, count)
    local parts = {}
    for i = 1, count or #v do
        parts[#parts + 1] = string.format("%.3f", v[i])
    end
    return "(" .. table.concat(parts, ", ") .. ")"
end

local function ActiveCamera()
    for _, entity in ipairs(Ext.Entity.GetAllEntitiesWithComponent("Camera")) do
        local camera = entity.Camera
        if camera.Active and camera.Controller then
            return entity, camera.Controller
        end
    end
    return nil, nil
end

local function Dump()
    local entities = Ext.Entity.GetAllEntitiesWithComponent("Camera")
    BG3VR.Log("%d camera entities", #entities)
    for i, entity in ipairs(entities) do
        local camera = entity.Camera
        local c = camera.Controller
        BG3VR.Log("[%d] active=%s input=%s behavior=%d", i, tostring(camera.Active), tostring(camera.AcceptsInput),
            camera.MasterBehaviorType)
        if c then
            local inv = c.Camera.InvViewMatrix
            BG3VR.Log("    fov=%.4f rad (%.1f deg) near=%.3f far=%.1f aspect=%.3f offcenter=%s dirty=%s/%s", c.FOV,
                math.deg(c.FOV), c.NearPlane, c.FarPlane, c.AspectRatio, tostring(c.OffCenter), tostring(c.ViewDirty),
                tostring(c.ProjectionDirty))
            BG3VR.Log("    camera pos=%s forward(+Z)=%s up=%s", Fmt({ inv[13], inv[14], inv[15] }),
                Fmt({ inv[9], inv[10], inv[11] }), Fmt({ inv[5], inv[6], inv[7] }))
            BG3VR.Log("    WorldTransform pos=%s rot=%s  LookAt=%s", Fmt(c.WorldTransform.Translate),
                Fmt(c.WorldTransform.RotationQuat), Fmt(c.LookAt))
            local proj = c.Camera.ProjectionMatrix
            BG3VR.Log("    proj diag=%s  [9]=%.4f [10]=%.4f [11]=%.4f [12]=%.4f [15]=%.4f", Fmt({ proj[1], proj[6], proj[11], proj[16] }),
                proj[9], proj[10], proj[11], proj[12], proj[15])
        end
        local ok, names = pcall(function() return entity:GetAllComponentNames(false) end)
        if ok and names then
            BG3VR.Log("    components: %s", table.concat(names, ", "))
        end
    end
end

-- Quaternion helpers; quaternions are (x, y, z, w).
local function YawQuat(degrees)
    local half = math.rad(degrees) / 2
    return { 0.0, math.sin(half), 0.0, math.cos(half) }
end

local function QuatMul(a, b)
    return {
        a[4] * b[1] + a[1] * b[4] + a[2] * b[3] - a[3] * b[2],
        a[4] * b[2] - a[1] * b[3] + a[2] * b[4] + a[3] * b[1],
        a[4] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[4],
        a[4] * b[4] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
    }
end

-- World (camera-to-world) and view (world-to-camera) matrices for position p and rotation q.
local function PoseMatrices(p, q)
    local x, y, z, w = q[1], q[2], q[3], q[4]
    local m00, m01, m02 = 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)
    local m10, m11, m12 = 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)
    local m20, m21, m22 = 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)
    local world = {
        m00, m10, m20, 0,
        m01, m11, m21, 0,
        m02, m12, m22, 0,
        p[1], p[2], p[3], 1,
    }
    local view = {
        m00, m01, m02, 0,
        m10, m11, m12, 0,
        m20, m21, m22, 0,
        -(m00 * p[1] + m10 * p[2] + m20 * p[3]),
        -(m01 * p[1] + m11 * p[2] + m21 * p[3]),
        -(m02 * p[1] + m12 * p[2] + m22 * p[3]),
        1,
    }
    return world, view
end

local follow = { handle = nil, mode = "view", height = 1.7, yaw = 0.0, ticks = 0, camera = nil, character = nil }

local function FollowTick()
    -- Entity handles survive across ticks; component and controller proxies expire at the end of
    -- the tick that produced them. So cache the entities (searching all entities costs
    -- milliseconds) and re-read the controller every tick.
    if follow.ticks % 60 == 0 or not follow.camera or not follow.character then
        follow.camera = ActiveCamera()
        follow.character = _C()
    end
    follow.ticks = follow.ticks + 1
    if not follow.camera or not follow.character then
        return
    end
    local controller = follow.camera.Camera.Controller
    local character = follow.character
    if not controller then
        return
    end

    local t = character.Transform.Transform
    local p = t.Translate
    local eye = { p[1], p[2] + follow.height, p[3] }
    local rot = QuatMul(t.RotationQuat, YawQuat(follow.yaw))

    if follow.mode == "view" then
        local world, view = PoseMatrices(eye, rot)
        controller.Camera.InvViewMatrix = world
        controller.Camera.ViewMatrix = view
    elseif follow.mode == "transform" then
        controller.WorldTransform.Translate = eye
        controller.WorldTransform.RotationQuat = rot
        controller.ViewDirty = true
    elseif follow.mode == "lookat" then
        -- The camera looks along its +Z axis.
        local world = PoseMatrices(eye, rot)
        controller.LookAt = { eye[1] + world[9], eye[2] + world[10], eye[3] + world[11] }
    end
end

local function StopFollow()
    if follow.handle then
        Ext.Events.Tick:Unsubscribe(follow.handle)
        follow.handle = nil
        BG3VR.Log("camera follow off after %d ticks", follow.ticks)
    end
end

local function StartFollow(mode, height, yaw)
    StopFollow()
    follow.mode = mode
    follow.height = tonumber(height) or follow.height
    follow.yaw = tonumber(yaw) or follow.yaw
    follow.ticks = 0
    follow.camera, follow.character = nil, nil
    follow.handle = Ext.Events.Tick:Subscribe(function()
        local ok, err = pcall(FollowTick)
        if not ok then
            BG3VR.Warn("camera follow failed, stopping: %s", tostring(err))
            StopFollow()
        end
    end)
    BG3VR.Log("camera follow '%s' on (height %.2f, yaw %.1f)", follow.mode, follow.height, follow.yaw)
end

local function Safely(fn)
    return function(...)
        local ok, err = pcall(fn, ...)
        if not ok then
            BG3VR.Warn("%s", tostring(err))
        end
    end
end

Ext.RegisterConsoleCommand("vr_cam", Safely(function() Dump() end))

Ext.RegisterConsoleCommand("vr_cam_fov", Safely(function(_, degrees)
    local _, controller = ActiveCamera()
    local value = tonumber(degrees)
    if not controller or not value then
        BG3VR.Warn("usage: !vr_cam_fov <degrees> (needs an active camera)")
        return
    end
    local before = controller.FOV
    controller.FOV = math.rad(value)
    controller.ProjectionDirty = true
    BG3VR.Log("fov %.1f -> %.1f deg (read back %.1f)", math.deg(before), value, math.deg(controller.FOV))
end))

local BRIDGE_FILE = BG3VR.BridgeFile
local ProxyAddress = BG3VR.ProxyAddress

local function YawOf(q)
    return math.deg(2 * math.atan(q[2], q[4]))
end

Ext.RegisterConsoleCommand("vr_cam_set", Safely(function(_, a, b, c, d, e)
    local _, controller = ActiveCamera()
    if not controller then
        BG3VR.Warn("no active camera controller")
        return
    end
    local address = ProxyAddress(controller)
    if not address then
        BG3VR.Warn("couldn't read the controller address from '%s'", tostring(controller))
        return
    end

    local pose
    if a == "off" then
        pose = "off"
    elseif a == "here" then
        -- A character faces along its rotation's -Z (seen 2026-10-12: yaw + 180 looks where the
        -- character looks), so the extra yaw is relative to that facing.
        local t = _C().Transform.Transform
        local p = t.Translate
        local yaw = YawOf(t.RotationQuat) + 180 + (tonumber(c) or 0)
        pose = string.format("%.4f %.4f %.4f %.2f %.2f", p[1], p[2] + (tonumber(b) or 1.7), p[3], yaw, tonumber(d) or 0)
    elseif tonumber(a) and tonumber(b) and tonumber(c) then
        pose = string.format("%.4f %.4f %.4f %.2f %.2f", tonumber(a), tonumber(b), tonumber(c), tonumber(d) or 0,
            tonumber(e) or 0)
    else
        BG3VR.Warn("usage: !vr_cam_set here [height] [yaw] [pitch] | <x> <y> <z> [yaw] [pitch] | off")
        return
    end

    local content = string.format("controller=%s\npose=%s\n", address, pose)
    if Ext.IO.SaveFile(BRIDGE_FILE, content) then
        BG3VR.Log("bridge: controller %s, pose %s (see bg3vr.log for the native side)", address, pose)
    else
        BG3VR.Warn("couldn't write %s", BRIDGE_FILE)
    end
end))

-- Per-eye projection on the driven camera (the Phase 1 projection hook), for a desktop check.
-- "left"/"right" use approximate Quest 2 eye frustums (degrees; the real values come from the
-- headset: bg3vr_xr_test prints them). The image looks off-centre and stretched on a monitor.
local EYES = {
    left = { -54, 40, 44, -48 },
    right = { -40, 54, 44, -48 },
}
Ext.RegisterConsoleCommand("vr_cam_eye", Safely(function(_, a, b, c, d)
    local _, controller = ActiveCamera()
    local address = controller and ProxyAddress(controller)
    if not address then
        BG3VR.Warn("no active camera controller")
        return
    end
    local value
    if a == "off" then
        value = "off"
    elseif EYES[a] then
        value = table.concat(EYES[a], " ")
    elseif tonumber(a) and tonumber(b) and tonumber(c) and tonumber(d) then
        value = string.format("%s %s %s %s", a, b, c, d)
    else
        BG3VR.Warn("usage: !vr_cam_eye left|right|off | <left> <right> <up> <down> (degrees)")
        return
    end
    if BG3VR.WriteBridge({ "controller=" .. address, "fov=" .. value }) then
        BG3VR.Log("eye projection %s on controller %s", value, address)
    end
end))

-- Automatic look-around while the camera is held: pans left-right and nods up-down on its own.
Ext.RegisterConsoleCommand("vr_cam_sweep", Safely(function(_, a, b, c)
    local value
    if a == "off" then
        value = "off"
    elseif a == nil then
        value = "on"
    elseif tonumber(a) then
        value = table.concat({ a, b or "", c or "" }, " ")
    else
        BG3VR.Warn("usage: !vr_cam_sweep [yaw_degrees] [pitch_degrees] [seconds_per_cycle] | off")
        return
    end
    if BG3VR.WriteBridge({ "sweep=" .. value }) then
        BG3VR.Log("camera sweep %s", value)
    end
end))

-- Headset ([xr] mode mono/alternate): how many camera updates a frame's pose leads its Present.
Ext.RegisterConsoleCommand("vr_xr_lag", Safely(function(_, value)
    local frames = tonumber(value)
    if value ~= "auto" and not (frames and frames >= 0 and frames <= 7 and math.floor(frames) == frames) then
        BG3VR.Warn("usage: !vr_xr_lag 0..7|auto")
        return
    end
    if BG3VR.WriteBridge({ "xr_lag=" .. value }) then
        BG3VR.Log("headset lag %s", value)
    end
end))
-- Native follow: the core reads the character's TransformComponent every camera update, so the
-- camera moves with them smoothly. The component can move in memory (e.g. when components are
-- added), so a tick handler re-sends its address whenever it changes, and when the controlled
-- character changes.
local nativeFollow = { handle = nil, height = 1.7, address = nil, ticks = 0 }

local function NativeFollowTick()
    nativeFollow.ticks = nativeFollow.ticks + 1
    if nativeFollow.ticks % 15 ~= 0 then
        return
    end
    local character = _C()
    local address = character and ProxyAddress(character.Transform)
    if address and address ~= nativeFollow.address then
        nativeFollow.address = address
        BG3VR.WriteBridge({ string.format("follow=%s %.2f", address, nativeFollow.height) })
        BG3VR.Log("follow: character transform now at %s", address)
    end
end

Ext.RegisterConsoleCommand("vr_follow", Safely(function(_, a)
    if a == "off" then
        if nativeFollow.handle then
            Ext.Events.Tick:Unsubscribe(nativeFollow.handle)
            nativeFollow.handle = nil
        end
        nativeFollow.address = nil
        BG3VR.WriteBridge({ "follow=off" })
        BG3VR.Log("follow off (the camera stays where it is; !vr_cam_set off releases it)")
        return
    end
    local height = tonumber(a or "1.7")
    if not height then
        BG3VR.Warn("usage: !vr_follow [height] | off")
        return
    end
    StartNativeFollow(height, {})
end))

-- Holds the camera at the character's eyes (native, every frame); extra bridge lines go in the same write
-- (two quick writes could be collapsed into the last one by the bridge watcher).
function StartNativeFollow(height, extra)
    local _, controller = ActiveCamera()
    local address = controller and ProxyAddress(controller)
    local t = _C().Transform
    local transform = ProxyAddress(t)
    if not address or not transform then
        BG3VR.Warn("no active camera controller or character transform (%s)", tostring(t))
        return
    end
    local p = t.Transform.Translate
    local yaw = YawOf(t.Transform.RotationQuat) + 180
    nativeFollow.height = height
    nativeFollow.address = transform
    local lines = {
        "controller=" .. address,
        string.format("pose=%.4f %.4f %.4f %.2f 0", p[1], p[2] + height, p[3], yaw),
        string.format("follow=%s %.2f", transform, height),
    }
    for _, line in ipairs(extra) do
        lines[#lines + 1] = line
    end
    if BG3VR.WriteBridge(lines) then
        BG3VR.Log("following the character (transform %s) at %.2f, facing yaw %.1f", transform, height, yaw)
    end
    if not nativeFollow.handle then
        nativeFollow.ticks = 0
        nativeFollow.handle = Ext.Events.Tick:Subscribe(function()
            local ok, err = pcall(NativeFollowTick)
            if not ok then
                BG3VR.Warn("follow tick: %s", tostring(err))
            end
        end)
    end
    return true
end

-- One command for the same-frame stereo setup: follow the character's eyes, create the second view (the
-- game's player view), and switch the stereo layout on three seconds later (the view is built on the render
-- thread and gets its camera on the game thread first).
Ext.RegisterConsoleCommand("vr_go", Safely(function(_, a)
    -- The same start as the automatic one (Client/VRAuto.lua): follow, eye view, skeleton, head hide, the [vr]
    -- profile, then stereo after 3 s. The height is only the start value; the core calibrates it from the head bone.
    VRStart(tonumber(a or "1.7") or 1.7, "!vr_go")
end))
-- Headset: take the current head pose as the centre (also automatic when head tracking starts).
Ext.RegisterConsoleCommand("vr_recenter", Safely(function()
    if BG3VR.WriteBridge({ "recenter=1" }) then
        BG3VR.Log("recentering the headset")
    end
end))
-- Same-frame stereo: the main view draws the left eye, the built split view the right (needs !vr_views build).
Ext.RegisterConsoleCommand("vr_stereo", Safely(function(_, mode)
    mode = mode or "report"
    if mode ~= "on" and mode ~= "off" and mode ~= "report" then
        BG3VR.Log("usage: !vr_stereo on|off|report")
        return
    end
    if BG3VR.WriteBridge({ "stereo=" .. mode }) then
        BG3VR.Log("stereo " .. mode .. " (see bg3vr.log)")
    end
end))
Ext.RegisterConsoleCommand("vr_hands", Safely(function(_, mode)
    mode = mode or "report"
    if mode ~= "report" and mode ~= "buzz" then
        BG3VR.Log("usage: !vr_hands [buzz]")
        return
    end
    if BG3VR.WriteBridge({ "hands=" .. mode }) then
        BG3VR.Log("hands " .. mode .. " (see 'xr input:' lines in bg3vr.log)")
    end
end))
Ext.RegisterConsoleCommand("vr_ray", Safely(function(_, mode)
    mode = mode or "camera"
    if mode ~= "camera" and mode ~= "hand" and mode ~= "off" and mode ~= "report" then
        BG3VR.Log("usage: !vr_ray camera|hand|off|report")
        return
    end
    local lines = { "ray=" .. mode }
    if mode == "camera" then
        local _, controller = ActiveCamera()
        local address = controller and ProxyAddress(controller)
        if not address then
            BG3VR.Warn("no active camera controller")
            return
        end
        table.insert(lines, 1, "controller=" .. address)
    end
    if BG3VR.WriteBridge(lines) then
        BG3VR.Log("ray probe %s (see 'ray:' lines in bg3vr.log)", mode)
    end
end))
Ext.RegisterConsoleCommand("vr_pick", Safely(function(_, mode)
    mode = mode or "hand"
    if mode ~= "hand" and mode ~= "off" then
        BG3VR.Log("usage: !vr_pick hand|off")
        return
    end
    if BG3VR.WriteBridge({ "pick=" .. mode }) then
        BG3VR.Log("picking from the %s ('pick:' lines in bg3vr.log)", mode == "hand" and "right hand" or "mouse")
    end
end))
Ext.RegisterConsoleCommand("vr_move", Safely(function(_, mode)
    mode = mode or "stick"
    if mode ~= "stick" and mode ~= "off" then
        BG3VR.Log("usage: !vr_move stick|off")
        return
    end
    if BG3VR.WriteBridge({ "move=" .. mode }) then
        BG3VR.Log("thumbstick locomotion %s ('locomotion:' lines in bg3vr.log)", mode == "stick" and "on" or "off")
    end
end))
Ext.RegisterConsoleCommand("vr_quality", Safely(function()
    if BG3VR.WriteBridge({ "quality=report" }) then
        BG3VR.Log("quality report written to bg3vr.log ('quality:' lines)")
    end
end))
-- Any bridge write makes the core log its reports (frame rate, camera, head hide, headset).
Ext.RegisterConsoleCommand("vr_fps", Safely(function()
    if BG3VR.WriteBridge({ "report=1" }) then
        BG3VR.Log("reports written to bg3vr.log (look for 'render: ... fps')")
    end
end))
-- Call tracing for reverse engineering: who calls the render view and pipeline functions.
Ext.RegisterConsoleCommand("vr_trace", Safely(function(_, mode)
    if mode ~= "on" and mode ~= "off" and mode ~= "dump" then
        BG3VR.Warn("usage: !vr_trace on|off|dump")
        return
    end
    if BG3VR.WriteBridge({ "trace=" .. mode }) then
        BG3VR.Log("trace %s (results in bg3vr.log)", mode)
    end
end))
-- Split screen (Phase 1 step 5): the game's ForceSplitscreen switch. Whether it splits with one
-- local player is what we want to find out.
Ext.RegisterConsoleCommand("vr_split", Safely(function(_, mode)
    local switches = Ext.Utils.GetGlobalSwitches()
    if not switches then
        BG3VR.Warn("no global switches")
        return
    end
    if mode == "on" or mode == "off" then
        switches.ForceSplitscreen = mode == "on"
    elseif mode ~= nil then
        BG3VR.Warn("usage: !vr_split [on|off]")
        return
    end
    BG3VR.Log("ForceSplitscreen = %s", tostring(switches.ForceSplitscreen))
end))
-- Render views (Phase 1 step 5): which views draw the camera, their viewports and split-screen
-- index, and where the camera's matrices sit inside them. Results go to bg3vr.log.
Ext.RegisterConsoleCommand("vr_views", Safely(function(_, action, ...)
    action = action or "find"
    local args = { ... }
    if action == "set" or action == "enable" or action == "build" then
        action = action .. " " .. table.concat(args, " ")
    elseif action ~= "find" and action ~= "dump" and action ~= "sbs" and action ~= "swap" and action ~= "full"
        and action ~= "refs" and action ~= "diff" and action ~= "jobs" and action ~= "events" and action ~= "gpu" and action ~= "create" then
        BG3VR.Warn("usage: !vr_views [find|dump|sbs|swap|full|refs|diff|jobs|events|gpu|create] | set <view> <scale x> <scale y> <offset x> <offset y> | enable <view> <0|1> | build <view>")
        return
    end
    local _, controller = ActiveCamera()
    local address = controller and ProxyAddress(controller)
    if not address then
        BG3VR.Warn("no active camera controller")
        return
    end
    if BG3VR.WriteBridge({ "controller=" .. address, "views=" .. action }) then
        BG3VR.Log("render views %s for controller %s (see bg3vr.log)", action, address)
    end
end))
-- Mouse look while a pose is held: hold the middle mouse button and drag (native, on by default).
Ext.RegisterConsoleCommand("vr_mouselook", Safely(function(_, mode)
    if mode ~= "on" and mode ~= "off" then
        BG3VR.Warn("usage: !vr_mouselook on|off")
        return
    end
    if BG3VR.WriteBridge({ "mouselook=" .. mode }) then
        BG3VR.Log("mouse look %s (hold the middle mouse button and drag while the camera is held)", mode)
    end
end))

-- Facing calibration: run once, walk straight forward a few metres, run again. Compares the
-- direction actually travelled with the character's rotation.
local calibration = nil
Ext.RegisterConsoleCommand("vr_cam_calib", Safely(function()
    local t = _C().Transform.Transform
    local p = t.Translate
    if not calibration then
        calibration = { p[1], p[2], p[3] }
        BG3VR.Log("calibration start at %s; now walk straight forward and run !vr_cam_calib again", Fmt(p))
        return
    end
    local dx, dz = p[1] - calibration[1], p[3] - calibration[3]
    calibration = nil
    if math.sqrt(dx * dx + dz * dz) < 1.0 then
        BG3VR.Warn("moved less than 1 m; try again")
        return
    end
    local travelled = math.deg(math.atan(dx, dz)) -- same convention as the pose yaw: 0 = +Z
    local rotation = YawOf(t.RotationQuat)
    local offset = (travelled - rotation + 540) % 360 - 180
    BG3VR.Log("travelled yaw %.1f, rotation yaw %.1f, facing offset %.1f deg (expect ~180)", travelled, rotation, offset)
end))

-- Asks the core to count calls on the controller's vtable slots (3 s) and record which
-- instructions write its pose and matrices (2 s). Results go to bg3vr.log.
Ext.RegisterConsoleCommand("vr_cam_probe", Safely(function()
    local _, controller = ActiveCamera()
    local address = controller and ProxyAddress(controller)
    if not address then
        BG3VR.Warn("no active camera controller")
        return
    end
    if Ext.IO.SaveFile(BRIDGE_FILE, string.format("controller=%s\npose=off\nprobe=1\n", address)) then
        BG3VR.Log("probe requested for controller %s; results in bg3vr.log in about 6 s", address)
    end
end))

Ext.RegisterConsoleCommand("vr_cam_follow", Safely(function(_, mode, height, yaw)
    if mode == nil or mode == "off" then
        StopFollow()
    elseif mode == "view" or mode == "transform" or mode == "lookat" then
        StartFollow(mode, height, yaw)
    else
        BG3VR.Warn("usage: !vr_cam_follow view|transform|lookat|off [height] [yaw]")
    end
end))
