-- A fire bowl: flipbook flames, a flickering warm light and a solid collider.
-- Place it in a level (Outliner > Add > Actors) or spawn it from a script:
--   fox.world.spawn("Assets/Scripts/Actors/Brazier.lua", { position = vec3(0, 0, 4) })

local Brazier = fox.actor {
    components = {
        { name = "flames", type = "particle_emitter",
          preset = "Assets/Particles/Presets/fire_flipbook_default.json", position = vec3(0, 1, 0) },
        { name = "light", type = "point_light", color = vec3(2.6, 1.2, 0.45), intensity = 1,
          radius = 9, position = vec3(0, 1.6, 0) },
        { name = "base", type = "box", size = vec3(0.8, 1, 0.8), color = vec3(0.09, 0.075, 0.065),
          position = vec3(0, 0.5, 0) },
    },
    properties = {
        { name = "brightness", type = "number", default = 1, min = 0, max = 10,
          tooltip = "Light intensity before flicker." },
        { name = "flicker", type = "number", default = 0.25, min = 0, max = 1,
          tooltip = "Flicker depth as a fraction of brightness." },
        { name = "lit", type = "boolean", default = true,
          tooltip = "Unlit braziers emit no flames and no light." },
    },
}

-- Flicker multiplier at time t: two detuned sines, so it never visibly loops.
function Brazier.flicker_at(t, depth)
    local wave = 0.6 * math.sin(t * 11.0) + 0.4 * math.sin(t * 17.3 + 1.7)
    return 1 + depth * wave
end

function Brazier:on_start()
    self:set_lit(self.lit)
end

function Brazier:set_lit(lit)
    self.lit = lit
    self.flames.enabled = lit
    self.light.enabled = lit
end

function Brazier:on_update(dt)
    if dt <= 0 or not self.lit then
        return
    end
    self.light.intensity = self.brightness * Brazier.flicker_at(fox.time.time, self.flicker)
end

return Brazier
