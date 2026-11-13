-- A free-standing target plate. It glows red until a projectile strikes it,
-- then turns green and stays green. Rest it on a Block pedestal.

local Projectile = require("Actors.Projectile")

local Target = fox.actor {
    components = {
        { name = "plate", type = "box", size = vec3(0.6, 0.6, 0.08), body = "dynamic", mass = 1.5,
          friction = 0.6, restitution = 0.1, color = vec3(0.85, 0.85, 0.82),
          emissive = vec3(2.4, 0.15, 0.08) },
    },
    properties = {
        { name = "min_speed", type = "number", default = 2, min = 0, max = 100,
          tooltip = "Slowest projectile impact (m/s) that counts as a hit; rolling shots below it do not." },
    },
}

local LIVE = vec3(2.4, 0.15, 0.08)
local DOWN = vec3(0.15, 2.2, 0.35)

-- True when an on_hit contact is a projectile striking at min_speed or faster.
function Target.is_strike(hit, min_speed)
    return hit.actor ~= nil and hit.actor:is_a(Projectile) and hit.speed >= min_speed
end

function Target:on_start()
    self.hit = false
    self.plate.emissive = LIVE
end

function Target:on_hit(hit)
    if not self.hit and Target.is_strike(hit, self.min_speed) then
        self.hit = true
        self.plate.emissive = DOWN
    end
end

-- Hit and total target counts in the current scene.
function Target.tally()
    local hit, total = 0, 0
    for _, target in ipairs(fox.world.actors(Target)) do
        total = total + 1
        if target.hit then
            hit = hit + 1
        end
    end
    return hit, total
end

return Target
