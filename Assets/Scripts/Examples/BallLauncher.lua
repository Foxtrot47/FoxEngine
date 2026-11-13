-- Left click launches a physics ball from the camera through the cursor.
-- Works as an autoload or on any entity. Clicks over script UI (such as the
-- pause menu) are ignored.

local BallLauncher = fox.script {
    properties = {
        { name = "launch_speed", type = "number", default = 25, min = 0, max = 200,
          tooltip = "Initial ball speed in m/s." },
        { name = "radius", type = "number", default = 0.25, min = 0.01, max = 5, step = 0.01,
          tooltip = "Ball radius in m." },
        { name = "spawn_distance", type = "number", default = 1, min = 0, max = 10,
          tooltip = "Distance in front of the camera where the ball appears, in m." },
    },
}

-- Autoloads only: no camera exists until the first scene is ready.
function BallLauncher:on_scene_loaded(scene)
    self.scene_ready = true
end

function BallLauncher:on_update(dt)
    if not self.entity and not self.scene_ready then
        return
    end
    -- Hold fire while paused or between scenes.
    if fox.time.scale <= 0 or fox.scene.is_loading() then
        return
    end
    if not fox.input.key_pressed("MouseLeft") or fox.ui.wants_mouse() then
        return
    end
    local origin, direction = fox.camera.screen_ray(fox.input.mouse_position())
    fox.physics.spawn_ball(origin + direction * self.spawn_distance, self.radius, direction * self.launch_speed)
end

return BallLauncher
