-- A car spawn: loads a vehicle on start and drives it from the game view.
--
-- Place one in a level (its position and yaw are the spawn). While it exists
-- it owns fox.camera, and Game/PlayerController.lua stands down (see
-- controls_camera below). The car itself is runtime state: it is not saved
-- with the level and disappears when the scene reloads.
--
-- Controls: drive_throttle/drive_brake pedals, drive_left/drive_right steer,
-- drive_handbrake drifts, drive_shift_up/down shifts (forces manual), reset
-- returns to the spawn, drive_toggle_auto toggles the gearbox, drive_tc and
-- drive_abs toggle the driver aids. A gamepad uses the left stick to steer,
-- the triggers for pedals, the right stick to orbit the chase camera and the
-- shoulders to shift.

local k_steerRefSpeed  = 18.0  -- m/s where the assisted lock halves
local k_steerRatePad   = 6.0   -- full-lock fractions per second
local k_steerRateKeys  = 2.5
local k_reverseDelay   = 0.25  -- s held at standstill before engaging reverse
local k_standstill     = 1.0   -- m/s
local k_cameraStiffness = 9.0  -- 1/s

local CarSpawn = fox.actor {
    components = {
        -- Render-only spawn marker (hidden, no collider) so the placement is
        -- visible to level tooling and the class owns at least one part.
        { name = "marker", type = "box", size = vec3(0.9, 0.05, 2.2), visible = false,
          collision = false, cast_shadows = false },
    },
    properties = {
        { name = "vehicle", type = "string",
          default = "Assets/Vehicles/Porsche911GT3R/vehicle.json",
          tooltip = "Vehicle definition (physics plus the visual mesh block)." },
        { name = "camera_distance", type = "number", default = 4.5, min = 4, max = 6,
          tooltip = "Chase camera distance behind the car in m." },
        { name = "camera_height", type = "number", default = 1.8, min = 1.4, max = 2.0,
          tooltip = "Chase camera height above the car origin in m." },
        { name = "camera_look_ahead", type = "number", default = 6, min = 0, max = 50,
          tooltip = "How far past the car the camera aims, in m." },
    },
}

-- Game/PlayerController.lua leaves the camera alone while an actor whose class
-- sets this exists.
CarSpawn.controls_camera = true

-- Pure helpers (covered by CarSpawn_test.lua) -------------------------------

function CarSpawn.approach(value, target, maxDelta)
    return value + math.max(math.min(target - value, maxDelta), -maxDelta)
end

-- Rotates a ground-plane direction by yaw in degrees (positive turns +Z to +X).
function CarSpawn.rotate_y(direction, yaw)
    local a = math.rad(yaw)
    local c, s = math.cos(a), math.sin(a)
    return vec3(direction.x * c + direction.z * s, 0, -direction.x * s + direction.z * c)
end

function CarSpawn.gear_label(gear)
    if gear < 0 then return "R" end
    if gear == 0 then return "N" end
    return tostring(gear)
end

function CarSpawn.aids_label(car)
    local aids = car.automatic and "AUTO" or "MAN"
    if car.traction_control then
        aids = aids .. (car.traction_control_active and " [TC]" or " TC")
    end
    if car.abs then
        aids = aids .. (car.abs_active and " [ABS]" or " ABS")
    end
    return aids
end

function CarSpawn.wants_reverse(gear, stopped, throttle, brake)
    if gear < 0 or not stopped then return false end
    return brake > 0.2 and throttle < 0.05
end

function CarSpawn.wants_forward(gear, stopped, throttle, brake)
    if gear >= 0 or not stopped then return false end
    return throttle > 0.2 and brake < 0.05
end

local k_driverActions = {
    { action = "drive_toggle_auto", apply = function(car) car.automatic = not car.automatic end },
    { action = "drive_shift_up",    apply = function(car) car:shift_up() end },
    { action = "drive_shift_down",  apply = function(car) car:shift_down() end },
    { action = "drive_tc",          apply = function(car) car.traction_control = not car.traction_control end },
    { action = "drive_abs",         apply = function(car) car.abs = not car.abs end },
}

local function stick_curve(value)
    if value == 0 then return 0 end
    return (value < 0 and -1 or 1) * math.abs(value) ^ 1.5
end

local function key_steer()
    local keys = 0
    if fox.input.action_down("drive_left") then keys = keys - 1 end
    if fox.input.action_down("drive_right") then keys = keys + 1 end
    return keys
end

local function pedal_value(trigger, action)
    return math.max(trigger, fox.input.action_down(action) and 1 or 0)
end

function CarSpawn:on_start()
    local transform = self.transform
    self.spawn_position = transform.world_position
    self.spawn_yaw = transform.rotation.y
    self.steer = 0
    self.orbit_yaw = 0
    self.reverse_timer = 0
    self.camera_dir = nil
    self.car = fox.vehicle.spawn(self.vehicle, self.spawn_position, self.spawn_yaw)
end

function CarSpawn:on_update(dt)
    local car = self.car
    if not car or not car.ready or dt <= 0 then
        return
    end

    self:handle_driver_actions(car)
    local pad = fox.input.gamepad(0)
    local throttle = pedal_value(pad.right_trigger, "drive_throttle")
    local brake = pedal_value(pad.left_trigger, "drive_brake")
    self:update_steering(car, pad, dt)
    throttle, brake = self:resolve_reverse(car, throttle, brake, dt)
    car:set_input(throttle, brake, self.steer,
                  fox.input.action_down("drive_handbrake") and 1 or 0)
end

function CarSpawn:handle_driver_actions(car)
    if fox.input.action_pressed("reset") then
        car:reset(self.spawn_position, self.spawn_yaw)
        self.steer = 0
        self.reverse_timer = 0
    end
    for _, binding in ipairs(k_driverActions) do
        if fox.input.action_pressed(binding.action) then
            binding.apply(car)
        end
    end
end

-- Steering assist: progressive stick curve and reduced lock with speed
-- (a thumbstick has no force feedback to find the grip limit).
function CarSpawn:update_steering(car, pad, dt)
    local stick = stick_curve(pad.left_x)
    local keys = key_steer()
    local using_stick = math.abs(stick) > math.abs(keys) * 0.5
    local lock = 1 / (1 + math.abs(car.speed) / k_steerRefSpeed)
    local target = (using_stick and stick or keys) * lock
    local rate = using_stick and k_steerRatePad or k_steerRateKeys
    self.steer = CarSpawn.approach(self.steer, target, dt * rate)
end

-- Automatic: holding the brake at a standstill selects reverse, where the
-- pedals swap; holding throttle at a standstill returns to first.
function CarSpawn:resolve_reverse(car, throttle, brake, dt)
    if not car.automatic then
        return throttle, brake
    end
    local gear = car.gear
    local stopped = math.abs(car.speed) < k_standstill
    self.reverse_timer = self:reverse_hold(gear, stopped, throttle, brake, dt)
    if self.reverse_timer > k_reverseDelay then
        car:select_gear(gear < 0 and 1 or -1)
        self.reverse_timer = 0
    end
    if gear < 0 then
        return brake, throttle
    end
    return throttle, brake
end

function CarSpawn:reverse_hold(gear, stopped, throttle, brake, dt)
    if CarSpawn.wants_reverse(gear, stopped, throttle, brake) or
       CarSpawn.wants_forward(gear, stopped, throttle, brake) then
        return self.reverse_timer + dt
    end
    return 0
end

-- After the physics step, so the camera follows the same chassis transform
-- that this frame renders (on_update runs before the scene steps physics).
function CarSpawn:on_late_update(dt)
    local car = self.car
    if not car or not car.ready or dt <= 0 then
        return
    end
    self:update_chase_camera(dt, car)
end

function CarSpawn:update_chase_camera(dt, car)
    local pad = fox.input.gamepad(0)
    -- The right stick's direction picks the viewing direction around the car
    -- (right = right side, down = behind); released, the view returns forward.
    local target = 0
    if pad.right_x * pad.right_x + pad.right_y * pad.right_y > 0.25 * 0.25 then
        target = math.deg(math.atan(pad.right_x, pad.right_y))
    end
    local delta = (target - self.orbit_yaw + 180) % 360 - 180  -- shortest way round
    self.orbit_yaw = (self.orbit_yaw + delta * (1 - math.exp(-dt * 10)) + 180) % 360 - 180
    local view = CarSpawn.rotate_y(car.forward, self.orbit_yaw)

    -- Only the viewing direction is smoothed (so turns swing the camera in
    -- gently); distance to the car is fixed, so acceleration never pulls the
    -- camera back.
    if not self.camera_dir then
        self.camera_dir = view
    else
        local blend = 1 - math.exp(-dt * k_cameraStiffness)
        self.camera_dir = (self.camera_dir + (view - self.camera_dir) * blend):normalized()
    end
    local dir = self.camera_dir
    local car_position = car.position

    fox.camera.position = car_position - dir * self.camera_distance + vec3(0, self.camera_height, 0)
    -- Aim past the car so the road ahead stays in view over the wing.
    fox.camera.look_at(car_position + dir * self.camera_look_ahead + vec3(0, 0.8, 0))
end

function CarSpawn:on_gui()
    local car = self.car
    local width, height = fox.ui.screen_size()
    if not car or not car.ready then
        fox.ui.draw_text(24, height - 48, "Loading car...", fox.ui.rgba(1, 1, 1, 0.9))
        return
    end
    CarSpawn.draw_dashboard(car, width, height)
end

function CarSpawn.draw_dashboard(car, width, height)
    local speed = math.abs(car.speed) * 3.6
    fox.ui.draw_text(width - 250, height - 120, string.format("%3.0f", speed),
                     fox.ui.rgba(1, 1, 1, 0.95))
    fox.ui.draw_text(width - 150, height - 104, "km/h", fox.ui.rgba(0.8, 0.8, 0.8, 0.9))
    fox.ui.draw_text(width - 96, height - 124, CarSpawn.gear_label(car.gear),
                     fox.ui.rgba(1, 0.82, 0.24, 1))
    CarSpawn.draw_rpm_bar(car, width, height)
end

function CarSpawn.draw_rpm_bar(car, width, height)
    -- Full scale sits a little past the limiter so it stays visible.
    local ratio = math.min(car.rpm / (car.limiter_rpm * 1.01), 1)
    local x, y, w, h = width - 250, height - 54, 240, 10
    fox.ui.draw_rect(x, y, w, h, fox.ui.rgba(0, 0, 0, 0.55), true)
    local color = ratio > 0.9 and fox.ui.rgba(1, 0.28, 0.16, 1) or fox.ui.rgba(0.31, 0.78, 1, 1)
    fox.ui.draw_rect(x, y, w * ratio, h, color, true)
    fox.ui.draw_text(x, y + 14, string.format("%5.0f rpm  %s", car.rpm, CarSpawn.aids_label(car)),
                     fox.ui.rgba(0.86, 0.86, 0.86, 0.9))
end

function CarSpawn:on_destroy()
    self.car = nil
end

return CarSpawn
