-- Rotates its entity at a constant rate around an axis (euler degrees per
-- second along each component of the normalised axis).

local Spinner = fox.script {
    properties = {
        { name = "axis", type = "vec3", default = vec3(0, 1, 0),
          tooltip = "Rotation axis (x = pitch, y = yaw, z = roll); normalised." },
        { name = "speed", type = "number", default = 90, min = -720, max = 720,
          tooltip = "Degrees per second." },
    },
}

function Spinner:on_start()
    if not self.entity then
        fox.log.warn("Spinner needs an entity; disabling")
        self.enabled = false
    end
end

function Spinner:on_update(dt)
    if dt <= 0 then
        return
    end
    self.transform:rotate(self.axis:normalized() * (self.speed * dt))
end

return Spinner
