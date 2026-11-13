local Player = require("Actors.FirstPersonPlayer")
local Projectile = require("Actors.Projectile")
local Target = require("Actors.Target")

local test = fox.test
local EPS = 1e-9

-- Total shots over a sequence of frame times with the trigger held.
local function shots_over(frames, interval)
    local cooldown, total = 0, 0
    for _, dt in ipairs(frames) do
        local shots
        shots, cooldown = Player.fire_step(cooldown, dt, interval, true)
        total = total + shots
    end
    return total
end

test.case("fire_step fires on the first held frame", function()
    local shots, cooldown = Player.fire_step(0, 1 / 60, 0.1, true)
    test.expect_eq(shots, 1)
    test.expect(cooldown > 0, "a shot starts the cooldown")
end)

test.case("fire_step keeps the rate steady at any frame rate", function()
    local steady, choppy = {}, {}
    for i = 1, 1200 do steady[i] = 1 / 120 end          -- 10 s at 120 fps
    for i = 1, 100 do choppy[i] = 0.1 end               -- 10 s at 10 fps
    -- A shot at t = 0, then one every 0.15 s: 1 + floor(10 / 0.15) = 67 at either frame rate.
    test.expect_eq(shots_over(steady, 0.15), 67)
    test.expect_eq(shots_over(choppy, 0.15), 67)
end)

test.case("fire_step re-arms when the trigger is released", function()
    local _, cooldown = Player.fire_step(0, 0.01, 0.5, true)
    _, cooldown = Player.fire_step(cooldown, 0.01, 0.5, false)
    _, cooldown = Player.fire_step(cooldown, 0.6, 0.5, false)
    test.expect_eq(cooldown, 0)
    local shots = Player.fire_step(cooldown, 0.01, 0.5, true)
    test.expect_eq(shots, 1)
end)

test.case("decay is independent of how a second is split", function()
    local split = 1
    for _ = 1, 60 do split = Player.decay(split, 14, 1 / 60) end
    test.expect_near(split, Player.decay(1, 14, 1), 1e-12)
end)

test.case("pull_back only moves the gun for walls inside its reach", function()
    test.expect_eq(Player.pull_back(nil, 0.9), 0)
    test.expect_eq(Player.pull_back(2, 0.9), 0)
    test.expect_near(Player.pull_back(0.5, 0.9), 0.4, EPS)
    test.expect_eq(Player.pull_back(0, 0.9), 0.9)
end)

test.case("view_rotation looks up with positive pitch", function()
    local rotation = Player.view_rotation(30, 20)
    test.expect_eq(rotation.x, -20)
    test.expect_eq(rotation.y, 30)
    test.expect_eq(rotation.z, 0)
end)

test.case("aim_direction converges on the crosshair point", function()
    local muzzle = vec3(0.2, 1.4, 0.8)
    local aim = vec3(0, 1.6, 20)
    local direction = Player.aim_direction(muzzle, aim, vec3(0, 0, 1))
    test.expect_near(direction:length(), 1, 1e-6)
    local along = (aim - muzzle):normalized()
    test.expect_near(direction:dot(along), 1, 1e-6)
end)

test.case("aim_direction falls back to the view for near or behind points", function()
    local forward = vec3(0, 0, 1)
    test.expect_eq(Player.aim_direction(vec3(0, 0, 1), vec3(0, 0, 1.5), forward), forward)
    test.expect_eq(Player.aim_direction(vec3(0, 0, 1), vec3(0, 0, -5), forward), forward)
end)

test.case("projectile glow holds for a second, then fades to an ember", function()
    test.expect_eq(Projectile.glow_at(0.5, 1.5), 1)
    test.expect_near(Projectile.glow_at(1.75, 1.5), 0.5, EPS)
    test.expect_eq(Projectile.glow_at(10, 1.5), 0.08)
end)

-- Height of a projectile launched along direction at speed when it has
-- travelled the horizontal distance to `to`.
local function height_at_target(from, to, direction, speed, g)
    local flat = vec3(to.x - from.x, 0, to.z - from.z):length()
    local horizontal = vec3(direction.x, 0, direction.z):length() * speed
    local t = flat / horizontal
    return from.y + direction.y * speed * t - 0.5 * g * t * t
end

test.case("launch_direction lands the shot on the aim point", function()
    local from, g, speed = vec3(0.2, 1.4, -11), 9.81, 45
    for _, to in ipairs({ vec3(3, 1.5, 20), vec3(-6, 0.4, 6), vec3(0, 5, 40) }) do
        local direction = Player.launch_direction(from, to, speed, g)
        test.expect(direction ~= nil, "target should be in range")
        test.expect_near(direction:length(), 1, 1e-9)
        test.expect_near(height_at_target(from, to, direction, speed, g), to.y, 1e-6)
        test.expect(direction.y > (to - from):normalized().y, "the arc starts above the straight line")
    end
end)

test.case("launch_direction gives up beyond the projectile's range", function()
    -- Maximum flat range is v^2 / g, about 10.2 m at 10 m/s.
    test.expect(Player.launch_direction(vec3(0, 0, 0), vec3(0, 0, 12), 10, 9.81) == nil, "out of range")
    test.expect(Player.launch_direction(vec3(0, 0, 0), vec3(0, 5, 0), 10, 9.81) == nil, "straight up")
end)

test.case("only fast enough projectile impacts strike a target", function()
    local function actor_of(class)
        return { is_a = function(_, other) return other == class end }
    end
    local shot, crate = actor_of(Projectile), actor_of({})
    test.expect(Target.is_strike({ actor = shot, speed = 30 }, 2), "a fresh shot is a hit")
    test.expect(not Target.is_strike({ actor = shot, speed = 0.5 }, 2), "a rolling shot is not a hit")
    test.expect(not Target.is_strike({ actor = crate, speed = 30 }, 2), "a flying crate is not a hit")
    test.expect(not Target.is_strike({ speed = 30 }, 2), "level geometry is not a hit")
end)
