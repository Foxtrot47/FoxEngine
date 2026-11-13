-- Bobs its entity up and down around the position it started at.

local Bobber = fox.script {
    properties = {
        { name = "amplitude", type = "number", default = 0.5, min = 0, max = 10,
          tooltip = "Peak offset from the start position in m." },
        { name = "frequency", type = "number", default = 0.5, min = 0, max = 10,
          tooltip = "Full up-and-down cycles per second." },
    },
}

function Bobber:on_start()
    if not self.entity then
        fox.log.warn("Bobber needs an entity; disabling")
        self.enabled = false
        return
    end
    self.start_position = self.transform.position
    self.elapsed = 0
end

function Bobber:on_update(dt)
    if dt <= 0 then
        return
    end
    self.elapsed = self.elapsed + dt
    local offset = self.amplitude * math.sin(2 * math.pi * self.frequency * self.elapsed)
    self.transform.position = self.start_position + vec3(0, offset, 0)
end

return Bobber
