-- Free-fly and walking camera, ported from the old CameraController.
--
-- Free-fly: move_* actions (WASD, E/Q up/down), sprint multiplies the speed,
-- hold look (right mouse) to capture the cursor and turn. toggle_walk (Tab)
-- switches to walking: the character is placed under the camera eye, walks
-- with move_*, jumps with jump and the cursor stays captured for mouse look.
-- Input is ignored while the game is paused (fox.time.scale == 0, set by
-- Game/PauseMenu.lua). Levels with their own player (an actor whose class sets
-- controls_camera = true, e.g. Actors/FirstPersonPlayer.lua) take over: this
-- script stands down until that actor is gone.

local CameraMath = require("Game.CameraMath")

-- The first actor that drives the camera itself, if any.
local function find_pawn()
    for _, actor in ipairs(fox.world.actors()) do
        if actor.controls_camera then
            return actor
        end
    end
    return nil
end

local PlayerController = fox.script {
    properties = {
        { name = "fly_speed", type = "number", default = 20, min = 0, max = 200,
          tooltip = "Free-fly speed in m/s." },
        { name = "sprint_multiplier", type = "number", default = 3, min = 1, max = 10,
          tooltip = "Free-fly speed multiplier while sprint is held." },
        { name = "look_sensitivity", type = "number", default = 0.15, min = 0.01, max = 1, step = 0.01,
          tooltip = "Degrees per pixel of mouse movement." },
    },
}

-- on_scene_loaded may arrive before or after on_start; both initialise once.
function PlayerController:init_state()
    if self.mode ~= nil then
        return
    end
    self.mode = "fly"       -- "fly" or "walk"
    self.yaw = 0
    self.pitch = 0
    self.looking = false    -- free-fly: cursor captured for look
    self.skip_delta = false -- ignore the mouse delta of the capture frame
    self.scene_ready = false
end

function PlayerController:on_start()
    self:init_state()
end

function PlayerController:on_scene_loaded(scene)
    self:init_state()
    self.scene_ready = true
    self.yaw = fox.camera.yaw
    self.pitch = CameraMath.clamp_pitch(fox.camera.pitch)
    self:set_mode("fly")
    self.pawn = find_pawn()
end

function PlayerController:on_disable()
    self:release_cursor()
end

function PlayerController:on_destroy()
    self:release_cursor()
end

function PlayerController:release_cursor()
    if self.looking or self.mode == "walk" then
        fox.input.capture_cursor(false)
    end
    self.looking = false
end

function PlayerController:capture_cursor()
    fox.input.capture_cursor(true)
    self.skip_delta = true
end

-- Switches between "fly" and "walk". Entering walk mode teleports the
-- character so its eye sits at the camera position.
function PlayerController:set_mode(mode)
    self:release_cursor()
    self.mode = mode
    if mode == "walk" then
        fox.character.position = fox.camera.position - vec3(0, fox.character.eye_height, 0)
        fox.character.vertical_velocity = 0
    end
end

function PlayerController:on_update(dt)
    if not self.scene_ready or fox.scene.is_loading() then
        return
    end
    if self.pawn ~= nil then
        if self.pawn.entity:is_valid() then
            return
        end
        self.pawn = nil
        self.yaw = fox.camera.yaw
        self.pitch = CameraMath.clamp_pitch(fox.camera.pitch)
        self:set_mode("fly")
    end
    if fox.time.scale <= 0 then
        -- Paused: the menu owns the cursor; look capture restarts on resume.
        self.looking = false
        return
    end

    if fox.input.action_pressed("toggle_walk") then
        self:set_mode(self.mode == "fly" and "walk" or "fly")
    end

    if self.mode == "walk" then
        self:update_walk(dt)
    else
        self:update_fly(dt)
    end
end

function PlayerController:apply_mouse_look()
    local dx, dy = fox.input.mouse_delta()
    if self.skip_delta then
        self.skip_delta = false
        return
    end
    self.yaw, self.pitch = CameraMath.apply_look(self.yaw, self.pitch, dx, dy, self.look_sensitivity)
end

function PlayerController:update_fly(dt)
    local look = fox.input.action_down("look")
    if look and not self.looking and not fox.ui.wants_mouse() then
        self.looking = true
        self:capture_cursor()
    elseif not look and self.looking then
        self.looking = false
        fox.input.capture_cursor(false)
    end
    if self.looking then
        self:apply_mouse_look()
    end

    local forward = CameraMath.axis(fox.input.action_down("move_forward"), fox.input.action_down("move_back"))
    local right = CameraMath.axis(fox.input.action_down("move_right"), fox.input.action_down("move_left"))
    local up = CameraMath.axis(fox.input.action_down("move_up"), fox.input.action_down("move_down"))
    if forward ~= 0 or right ~= 0 or up ~= 0 then
        local speed = self.fly_speed
        if fox.input.action_down("sprint") then
            speed = speed * self.sprint_multiplier
        end
        fox.camera.position = fox.camera.position
            + CameraMath.fly_delta(self.yaw, self.pitch, forward, right, up, speed, dt)
    end
    fox.camera.set_rotation(self.yaw, self.pitch)
end

function PlayerController:update_walk(dt)
    -- Re-capture after resume or after focus loss released the cursor.
    if not fox.input.cursor_captured() then
        self:capture_cursor()
    end
    if fox.input.cursor_captured() then
        self:apply_mouse_look()
    end

    local forward = CameraMath.axis(fox.input.action_down("move_forward"), fox.input.action_down("move_back"))
    local right = CameraMath.axis(fox.input.action_down("move_right"), fox.input.action_down("move_left"))
    if fox.input.action_pressed("jump") then
        fox.character.jump()
    end
    fox.character.move(CameraMath.walk_velocity(self.yaw, forward, right, fox.character.move_speed), dt)

    fox.camera.position = fox.character.eye_position
    fox.camera.set_rotation(self.yaw, self.pitch)
end

return PlayerController
