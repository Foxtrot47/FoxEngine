-- First-person character with a low-poly blaster.
--
-- Place one in a level (its position and yaw are the spawn). While it exists
-- it owns fox.camera and fox.character, and Game/PlayerController.lua stands
-- down (see controls_camera below).
--
-- Controls: move_* walk, sprint runs, jump jumps, fire shoots (hold for
-- automatic fire), reset reloads the level. Mouse looks; a gamepad uses the
-- left stick to walk, the right stick to look and the right trigger to fire.
--
-- The gun is a view model: render-only boxes (collision = false) laid out in
-- camera space (x right, y up, z forward) and moved with the camera every
-- frame. Shots are Actors/Projectile.lua dynamic spheres aimed from the muzzle
-- at whatever the crosshair points at.

local CameraMath = require("Game.CameraMath")
local Projectile = require("Actors.Projectile")
local Target = require("Actors.Target")

local GUNMETAL = vec3(0.1, 0.11, 0.13)
local DARK = vec3(0.04, 0.04, 0.05)
local ACCENT = vec3(0.9, 0.38, 0.08)
local GLOW = vec3(0.3, 1.8, 2.6)

-- Shared by every view-model part: drawn, never collides, never shadows.
local function part(name, size, position, color, extra)
    local spec = { name = name, type = "box", size = size, position = position, color = color,
                   collision = false, cast_shadows = false }
    for key, value in pairs(extra or {}) do
        spec[key] = value
    end
    return spec
end

local MUZZLE = vec3(0.18, -0.185, 0.93)

local FirstPersonPlayer = fox.actor {
    components = {
        part("receiver", vec3(0.07, 0.1, 0.3), vec3(0.18, -0.21, 0.52), GUNMETAL),
        part("barrel", vec3(0.04, 0.04, 0.24), vec3(0.18, -0.185, 0.78), DARK),
        part("muzzle_ring", vec3(0.056, 0.056, 0.04), vec3(0.18, -0.185, 0.89), DARK, { emissive = GLOW }),
        part("sight", vec3(0.024, 0.03, 0.12), vec3(0.18, -0.145, 0.5), DARK),
        part("grip", vec3(0.05, 0.13, 0.06), vec3(0.18, -0.29, 0.44), GUNMETAL, { rotation = vec3(15, 0, 0) }),
        part("cell", vec3(0.045, 0.08, 0.07), vec3(0.18, -0.28, 0.6), ACCENT),
        part("stripe", vec3(0.074, 0.014, 0.2), vec3(0.18, -0.19, 0.53), DARK, { emissive = GLOW }),
        { name = "muzzle_flash", type = "particle_emitter",
          preset = "Assets/Particles/Presets/muzzle_flash_small.json", position = MUZZLE },
        { name = "muzzle_light", type = "point_light", color = vec3(2.4, 1.3, 0.5), intensity = 0,
          radius = 7, position = MUZZLE },
    },
    properties = {
        { name = "walk_speed", type = "number", default = 5.5, min = 0, max = 30,
          tooltip = "Walking speed in m/s." },
        { name = "sprint_multiplier", type = "number", default = 1.7, min = 1, max = 5,
          tooltip = "Speed multiplier while sprint is held." },
        { name = "look_sensitivity", type = "number", default = 0.12, min = 0.01, max = 1, step = 0.01,
          tooltip = "Degrees per pixel of mouse movement." },
        { name = "gamepad_look_speed", type = "number", default = 160, min = 10, max = 720,
          tooltip = "Degrees per second at full right-stick deflection." },
        { name = "fire_rate", type = "number", default = 7, min = 0.5, max = 30,
          tooltip = "Shots per second while fire is held." },
        { name = "muzzle_speed", type = "number", default = 45, min = 1, max = 200,
          tooltip = "Projectile launch speed in m/s." },
        { name = "recoil_pitch", type = "number", default = 0.5, min = 0, max = 5, step = 0.05,
          tooltip = "Degrees the view kicks up per shot; it settles back between shots." },
        { name = "max_projectiles", type = "integer", default = 40, min = 1, max = 500,
          tooltip = "Live projectiles; firing past this removes the oldest." },
        { name = "projectile_lifetime", type = "number", default = 6, min = 0.1, max = 60,
          tooltip = "Seconds a projectile lives." },
        { name = "fov", type = "number", default = 75, min = 30, max = 120,
          tooltip = "Vertical field of view in degrees." },
    },
}

-- Game/PlayerController.lua leaves the camera alone while an actor whose class
-- sets this exists.
FirstPersonPlayer.controls_camera = true

-- Pure helpers (covered by FirstPersonPlayer_test.lua) ------------------------

-- Shots owed this frame and the new cooldown. Holding the trigger fires at a
-- steady rate however dt varies; a released trigger re-arms, so the next
-- press fires immediately.
function FirstPersonPlayer.fire_step(cooldown, dt, interval, trigger)
    cooldown = cooldown - dt
    if not trigger then
        return 0, math.max(cooldown, 0)
    end
    local shots = 0
    while cooldown <= 0 do
        shots = shots + 1
        cooldown = cooldown + interval
    end
    return shots, cooldown
end

-- Exponential decay towards 0 that is independent of frame rate.
function FirstPersonPlayer.decay(value, rate, dt)
    return value * math.exp(-rate * dt)
end

-- How far (m) to pull the gun back so it stays out of a wall hit_distance
-- ahead of the eye; reach is how far the gun sticks out.
function FirstPersonPlayer.pull_back(hit_distance, reach)
    if hit_distance == nil then
        return 0
    end
    return math.min(math.max(reach - hit_distance, 0), reach)
end

-- Transform rotation (euler degrees) of a view at yaw/pitch: positive
-- transform pitch tips +Z down, camera pitch is degrees above the horizon.
function FirstPersonPlayer.view_rotation(yaw, pitch)
    return vec3(-pitch, yaw, 0)
end

-- Unit direction from the muzzle to the aim point, or the view direction when
-- the aim point is too close to give a stable direction.
function FirstPersonPlayer.aim_direction(muzzle, aim_point, view_forward)
    local to_aim = aim_point - muzzle
    if to_aim:length() < 1.5 or to_aim:dot(view_forward) <= 0 then
        return view_forward
    end
    return to_aim:normalized()
end

-- Unit launch direction that lands a projectile fired from `from` at `speed`
-- on `to` under gravity g (m/s^2, pointing down), on the flatter of the two
-- arcs. nil when `to` is out of range or straight above or below.
function FirstPersonPlayer.launch_direction(from, to, speed, g)
    local delta = to - from
    local flat = vec3(delta.x, 0, delta.z)
    local distance = flat:length()
    if distance < 1e-4 then
        return nil
    end
    local v2 = speed * speed
    local disc = v2 * v2 - g * (g * distance * distance + 2 * delta.y * v2)
    if disc < 0 then
        return nil
    end
    local slope = (v2 - math.sqrt(disc)) / (g * distance)
    local cos = 1 / math.sqrt(1 + slope * slope)
    return flat / distance * cos + vec3(0, slope * cos, 0)
end

-- Lifecycle ---------------------------------------------------------------

local REACH = 1.0         -- gun tip distance from the eye
local AIM_RANGE = 400

function FirstPersonPlayer:on_start()
    self.yaw = self.transform.rotation.y
    self.pitch = 0
    self.punch = 0         -- recoil view kick in degrees, settles back
    self.cooldown = 0
    self.kick = 0          -- recoil, 1 per shot, decays
    self.flash = 0         -- muzzle light, 1 per shot, decays
    self.bob_phase = 0
    self.pull = 0
    self.projectiles = {}
    self.skip_delta = true

    fox.character.position = self.transform.world_position
    fox.character.vertical_velocity = 0
    fox.camera.fov = self.fov
    self:update_view(0)
end

function FirstPersonPlayer:on_disable()
    fox.input.capture_cursor(false)
end

function FirstPersonPlayer:on_update(dt)
    if fox.time.scale <= 0 or fox.scene.is_loading() then
        return -- paused: the menu owns the cursor
    end
    if fox.input.action_pressed("reset") then
        fox.scene.reload()
        return
    end
    if not fox.input.cursor_captured() then
        fox.input.capture_cursor(true)
        self.skip_delta = true
    end

    self:update_look(dt)
    self:update_move(dt)
    self:update_fire(dt)
    self:update_view(dt)
end

function FirstPersonPlayer:update_look(dt)
    local dx, dy = fox.input.mouse_delta()
    if self.skip_delta then
        self.skip_delta = false
        dx, dy = 0, 0
    end
    self.yaw, self.pitch = CameraMath.apply_look(self.yaw, self.pitch, dx, dy, self.look_sensitivity)

    local pad = fox.input.gamepad(0)
    if pad.connected then
        local turn = self.gamepad_look_speed * dt
        self.yaw = self.yaw + pad.right_x * turn
        self.pitch = CameraMath.clamp_pitch(self.pitch + pad.right_y * turn)
    end
end

function FirstPersonPlayer:update_move(dt)
    local forward = CameraMath.axis(fox.input.action_down("move_forward"), fox.input.action_down("move_back"))
    local right = CameraMath.axis(fox.input.action_down("move_right"), fox.input.action_down("move_left"))
    local pad = fox.input.gamepad(0)
    if pad.connected then
        forward = forward + pad.left_y
        right = right + pad.left_x
    end

    local speed = self.walk_speed
    if fox.input.action_down("sprint") then
        speed = speed * self.sprint_multiplier
    end
    local velocity = CameraMath.walk_velocity(self.yaw, forward, right, speed)
    if fox.input.action_pressed("jump") then
        fox.character.jump()
    end
    fox.character.move(velocity, dt)

    if fox.character.grounded then
        -- Two bob cycles per stride of about 1.6 m.
        self.bob_phase = self.bob_phase + velocity:length() * dt * (math.pi / 1.6)
    end
end

function FirstPersonPlayer:update_fire(dt)
    local trigger = fox.input.action_down("fire")
    local pad = fox.input.gamepad(0)
    if pad.connected and pad.right_trigger > 0.5 then
        trigger = true
    end
    local shots
    shots, self.cooldown = FirstPersonPlayer.fire_step(self.cooldown, dt, 1 / self.fire_rate, trigger)
    for _ = 1, shots do
        self:fire()
    end

    self.kick = FirstPersonPlayer.decay(self.kick, 14, dt)
    self.punch = FirstPersonPlayer.decay(self.punch, 8, dt)
    self.flash = FirstPersonPlayer.decay(self.flash, 40, dt)
    self.muzzle_light.intensity = 3 * self.flash
end

function FirstPersonPlayer:fire()
    local eye = fox.character.eye_position
    local forward = CameraMath.forward(self.yaw, self:view_pitch())
    local muzzle = self.muzzle_light.transform.world_position

    -- Never spawn inside a wall the gun is pressed against.
    local start = muzzle
    local to_muzzle = muzzle - eye
    local blocked = fox.physics.raycast(eye, to_muzzle:normalized(), to_muzzle:length())
    if blocked and not blocked.dynamic then
        start = eye + to_muzzle:normalized() * math.max(blocked.distance - 0.12, 0)
    end

    -- Aim from the muzzle at what the crosshair covers, arcing over the drop
    -- so the shot lands there; with nothing under the crosshair, fire straight.
    local direction = forward
    local hit = fox.physics.raycast(eye, forward, AIM_RANGE)
    if hit then
        direction = FirstPersonPlayer.aim_direction(start, hit.position, forward)
        local gravity = -fox.physics.gravity.y
        if direction ~= forward and gravity > 0 then
            direction = FirstPersonPlayer.launch_direction(start, hit.position, self.muzzle_speed, gravity)
                or direction
        end
    end

    local projectile = fox.world.spawn(Projectile, {
        position = start,
        properties = { lifetime = self.projectile_lifetime },
    })
    projectile.ball.velocity = direction * self.muzzle_speed
    self:track(projectile)

    self.muzzle_flash:emit(1)
    self.kick = math.min(self.kick + 1, 1.5)
    self.flash = 1
    self.punch = self.punch + self.recoil_pitch
end

-- Look pitch plus the recoil kick.
function FirstPersonPlayer:view_pitch()
    return CameraMath.clamp_pitch(self.pitch + self.punch)
end

-- Keeps at most max_projectiles alive, removing the oldest first.
function FirstPersonPlayer:track(projectile)
    local live = {}
    for _, p in ipairs(self.projectiles) do
        if p.entity:is_valid() then
            live[#live + 1] = p
        end
    end
    live[#live + 1] = projectile
    while #live > self.max_projectiles do
        table.remove(live, 1):destroy()
    end
    self.projectiles = live
end

-- Camera at the eye; the view model follows it with bob, recoil and wall pull-back.
function FirstPersonPlayer:update_view(dt)
    local eye = fox.character.eye_position
    fox.camera.position = eye
    local pitch = self:view_pitch()
    fox.camera.set_rotation(self.yaw, pitch)

    local forward = CameraMath.forward(self.yaw, pitch)
    local hit = fox.physics.raycast(eye, forward, REACH)
    local wall = hit and not hit.dynamic and hit.distance or nil
    local target_pull = FirstPersonPlayer.pull_back(wall, REACH)
    self.pull = target_pull + FirstPersonPlayer.decay(self.pull - target_pull, 20, dt)

    local bob_x = math.sin(self.bob_phase) * 0.008
    local bob_y = -math.abs(math.cos(self.bob_phase)) * 0.01
    local transform = self.transform
    transform.rotation = FirstPersonPlayer.view_rotation(self.yaw, pitch + 4 * self.kick)
    transform.position = eye
        + transform.right * bob_x
        + transform.up * (bob_y - 0.25 * self.pull)
        + transform.forward * (-0.05 * self.kick - self.pull)
end

-- HUD ---------------------------------------------------------------------

local CROSSHAIR = fox.ui.rgba(1, 1, 1, 0.85)
local SHADOW = fox.ui.rgba(0, 0, 0, 0.6)
local HINT = fox.ui.rgba(1, 1, 1, 0.55)
local SCORE = fox.ui.rgba(1, 0.85, 0.5, 1)

function FirstPersonPlayer:on_gui()
    if fox.time.scale <= 0 then
        return
    end
    local w, h = fox.ui.screen_size()
    local cx, cy = w * 0.5, h * 0.5
    local gap, len = 5 + 6 * self.kick, 8
    for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
        local x1, y1 = cx + d[1] * gap, cy + d[2] * gap
        local x2, y2 = cx + d[1] * (gap + len), cy + d[2] * (gap + len)
        fox.ui.draw_line(x1 + 1, y1 + 1, x2 + 1, y2 + 1, SHADOW, 2)
        fox.ui.draw_line(x1, y1, x2, y2, CROSSHAIR, 2)
    end

    local hit, total = Target.tally()
    if total > 0 then
        local text = string.format("Targets %d / %d", hit, total)
        local x = cx - fox.ui.calc_text_width(text) * 0.5
        fox.ui.draw_text(x + 1, 21, text, SHADOW)
        fox.ui.draw_text(x, 20, text, SCORE)
    end
    fox.ui.draw_text(16, h - 30, "WASD move   Shift sprint   Space jump   LMB fire   R reset   Esc menu", HINT)
end

return FirstPersonPlayer
