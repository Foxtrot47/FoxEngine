-- A simulated wooden crate. Stack them, shoot them, walk into them.
-- Placements override the box: "components": { "box": { "size": [1, 0.5, 1], "mass": 6 } }.

local Crate = fox.actor {
    components = {
        { name = "box", type = "box", size = vec3(0.7, 0.7, 0.7), body = "dynamic", mass = 4,
          friction = 0.6, restitution = 0.1, color = vec3(0.55, 0.36, 0.18) },
    },
}

return Crate
