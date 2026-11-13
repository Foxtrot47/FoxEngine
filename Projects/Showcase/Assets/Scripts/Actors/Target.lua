-- A free-standing target plate. It glows red until something knocks it out
-- of place, then turns green and stays down. Rest it on a Block pedestal.

local Target = fox.actor {
    components = {
        { name = "plate", type = "box", size = vec3(0.6, 0.6, 0.08), body = "dynamic", mass = 1.5,
          friction = 0.6, restitution = 0.1, color = vec3(0.85, 0.85, 0.82),
          emissive = vec3(2.4, 0.15, 0.08) },
    },
    properties = {
        { name = "knock_distance", type = "number", default = 0.25, min = 0.01, max = 5,
          tooltip = "How far (m) the plate must move from where it settled to count as hit." },
    },
}

local LIVE = vec3(2.4, 0.15, 0.08)
local DOWN = vec3(0.15, 2.2, 0.35)

-- True once the plate has moved knock_distance from its rest position.
function Target.is_knocked(rest, position, knock_distance)
    return rest:distance(position) >= knock_distance
end

function Target:on_start()
    self.hit = false
    self.rest = nil
    self:start_coroutine(function()
        -- Placements can start a little above their pedestal; take the rest
        -- position once the plate has settled.
        fox.wait(0.5)
        self.rest = self.transform.world_position
    end)
    self.plate.emissive = LIVE
end

function Target:on_update(dt)
    if self.hit or self.rest == nil then
        return
    end
    if Target.is_knocked(self.rest, self.transform.world_position, self.knock_distance) then
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
