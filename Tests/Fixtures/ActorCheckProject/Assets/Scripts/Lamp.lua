local Lamp = fox.actor {
    components = {
        { name = "light", type = "point_light", radius = 6 },
        { name = "base", type = "box", size = vec3(0.5, 1, 0.5) },
    },
    properties = {
        { name = "brightness", type = "number", default = 1 },
    },
}

return Lamp
