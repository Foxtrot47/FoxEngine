-- Static level geometry: a solid box that follows its actor (walls, platforms,
-- ramps, pedestals). Placements set the size and colour:
--   "components": { "block": { "size": [8, 1, 4], "color": [0.3, 0.3, 0.32] } }

local Block = fox.actor {
    components = {
        { name = "block", type = "box", size = vec3(1, 1, 1), color = vec3(0.42, 0.43, 0.45) },
    },
}

return Block
