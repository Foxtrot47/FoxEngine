-- A glowing ball fired by Actors/FirstPersonPlayer.lua. It is a dynamic
-- sphere, so it bounces off the level and shoves every other dynamic body it
-- hits. The shooter sets self.ball.velocity right after spawning it.

local Projectile = fox.actor {
    components = {
        { name = "ball", type = "sphere", radius = 0.12, body = "dynamic", mass = 1.5,
          friction = 0.4, restitution = 0.45, color = vec3(0.9, 0.45, 0.1),
          emissive = vec3(6, 2.2, 0.5), cast_shadows = false },
    },
    properties = {
        { name = "lifetime", type = "number", default = 6, min = 0.1, max = 60,
          tooltip = "Seconds before the projectile removes itself." },
        { name = "cool_time", type = "number", default = 1.5, min = 0.01, max = 60,
          tooltip = "Seconds over which the glow fades after the first second." },
    },
}

local HOT = vec3(6, 2.2, 0.5)

-- Glow multiplier at age t: full for the first second, then fades to a dim ember.
function Projectile.glow_at(age, cool_time)
    local t = math.max(0, age - 1) / cool_time
    return math.max(0.08, 1 - t)
end

function Projectile:on_start()
    self.age = 0
end

function Projectile:on_update(dt)
    self.age = self.age + dt
    if self.age >= self.lifetime or self.transform.world_position.y < -50 then
        self:destroy()
        return
    end
    self.ball.emissive = HOT * Projectile.glow_at(self.age, self.cool_time)
end

return Projectile
