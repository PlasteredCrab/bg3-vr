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
