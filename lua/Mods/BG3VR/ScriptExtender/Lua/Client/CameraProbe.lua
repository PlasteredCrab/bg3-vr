-- Phase 0 camera probes (client context). In the Script Extender console, type "client" first.
--   !vr_cam                 list camera entities and their controller state
--   !vr_cam_fov <degrees>   set the active camera's FOV once (does a write take effect?)
--   !vr_cam_follow [on|off] [height] [yaw]
--                           every tick, put the active camera at the controlled character's
--                           position + height (default 1.7), facing its rotation + yaw degrees

local function Fmt(v)
    local parts = {}
    for i = 1, #v do
        parts[#parts + 1] = string.format("%.3f", v[i])
    end
    return "(" .. table.concat(parts, ", ") .. ")"
end

local function CameraEntities()
    local result = {}
    for _, entity in ipairs(Ext.Entity.GetAllEntitiesWithComponent("Camera")) do
        result[#result + 1] = entity
    end
    return result
end

local function ActiveController()
    for _, entity in ipairs(CameraEntities()) do
        local camera = entity.Camera
        if camera.Active and camera.Controller then
            return camera.Controller
        end
    end
    return nil
end

local function Dump()
    local entities = CameraEntities()
    BG3VR.Log("%d camera entities", #entities)
    for i, entity in ipairs(entities) do
        local camera = entity.Camera
        local c = camera.Controller
        BG3VR.Log("[%d] active=%s input=%s behavior=%d", i, tostring(camera.Active), tostring(camera.AcceptsInput),
            camera.MasterBehaviorType)
        if c then
            BG3VR.Log("    id=%s fov=%.2f near=%.3f far=%.1f aspect=%.3f ortho=%s offcenter=%s", tostring(c.ID), c.FOV,
                c.NearPlane, c.FarPlane, c.AspectRatio, tostring(c.IsOrthographic), tostring(c.OffCenter))
            BG3VR.Log("    pos=%s rot=%s lookat=%s", Fmt(c.WorldTransform.Translate), Fmt(c.WorldTransform.RotationQuat),
                Fmt(c.LookAt))
        end
    end
end

-- Quaternion (x, y, z, w) for a rotation of `degrees` about +Y.
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

local follow = { handle = nil, height = 1.7, yaw = 0.0, ticks = 0 }

local function FollowTick()
    local controller = ActiveController()
    local character = _C()
    if not controller or not character then
        return
    end
    local t = character.Transform.Transform
    local p = t.Translate
    controller.WorldTransform.Translate = { p[1], p[2] + follow.height, p[3] }
    controller.WorldTransform.RotationQuat = QuatMul(t.RotationQuat, YawQuat(follow.yaw))
    controller.ViewDirty = true
    follow.ticks = follow.ticks + 1
end

local function SetFollow(enabled, height, yaw)
    if follow.handle then
        Ext.Events.Tick:Unsubscribe(follow.handle)
        follow.handle = nil
        BG3VR.Log("camera follow off after %d ticks", follow.ticks)
    end
    if enabled then
        follow.height = tonumber(height) or follow.height
        follow.yaw = tonumber(yaw) or follow.yaw
        follow.ticks = 0
        follow.handle = Ext.Events.Tick:Subscribe(function()
            local ok, err = pcall(FollowTick)
            if not ok then
                BG3VR.Warn("camera follow failed, stopping: %s", tostring(err))
                SetFollow(false)
            end
        end)
        BG3VR.Log("camera follow on (height %.2f, yaw %.1f)", follow.height, follow.yaw)
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

Ext.RegisterConsoleCommand("vr_cam", Safely(function() Dump() end))

Ext.RegisterConsoleCommand("vr_cam_fov", Safely(function(_, degrees)
    local controller = ActiveController()
    if not controller then
        BG3VR.Warn("no active camera controller")
        return
    end
    local before = controller.FOV
    controller.FOV = tonumber(degrees) or before
    controller.ProjectionDirty = true
    BG3VR.Log("fov %.2f -> %.2f (read back %.2f)", before, tonumber(degrees) or before, controller.FOV)
end))

Ext.RegisterConsoleCommand("vr_cam_follow", Safely(function(_, mode, height, yaw)
    SetFollow(mode ~= "off", height, yaw)
end))
