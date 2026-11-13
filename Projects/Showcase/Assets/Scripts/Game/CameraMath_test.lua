local CameraMath = require("Game.CameraMath")

local test = fox.test
local EPS = 1e-9

test.case("clamp_pitch limits to +-89 by default", function()
    test.expect_eq(CameraMath.clamp_pitch(120), 89)
    test.expect_eq(CameraMath.clamp_pitch(-120), -89)
    test.expect_eq(CameraMath.clamp_pitch(45), 45)
    test.expect_eq(CameraMath.clamp_pitch(89), 89)
end)

test.case("clamp_pitch honours a custom limit", function()
    test.expect_eq(CameraMath.clamp_pitch(70, 60), 60)
    test.expect_eq(CameraMath.clamp_pitch(-70, 60), -60)
end)

test.case("apply_look turns right and looks down with positive deltas", function()
    local yaw, pitch = CameraMath.apply_look(10, 0, 100, 100, 0.15)
    test.expect_near(yaw, 25, EPS)
    test.expect_near(pitch, -15, EPS)
end)

test.case("apply_look clamps pitch", function()
    local _, up = CameraMath.apply_look(0, 80, 0, -1000, 0.15)
    local _, down = CameraMath.apply_look(0, -80, 0, 1000, 0.15)
    test.expect_eq(up, 89)
    test.expect_eq(down, -89)
end)

test.case("forward follows the fox.camera convention", function()
    test.expect_near(CameraMath.forward(0, 0), vec3(0, 0, 1), EPS)
    test.expect_near(CameraMath.forward(90, 0), vec3(1, 0, 0), EPS)
    test.expect_near(CameraMath.forward(-90, 0), vec3(-1, 0, 0), EPS)
    test.expect_near(CameraMath.forward(0, 90), vec3(0, 1, 0), EPS)
    local f = CameraMath.forward(37, -21)
    test.expect_near(f:length(), 1, EPS)
end)

test.case("right is horizontal and perpendicular to forward", function()
    test.expect_near(CameraMath.right(0), vec3(1, 0, 0), EPS)
    test.expect_near(CameraMath.right(90), vec3(0, 0, -1), EPS)
    for _, yaw in ipairs({ -135, -30, 0, 45, 170 }) do
        local r = CameraMath.right(yaw)
        test.expect_near(r.y, 0, EPS)
        test.expect_near(r:dot(CameraMath.forward(yaw, 30)), 0, EPS)
        test.expect_near(r:dot(CameraMath.flat_forward(yaw)), 0, EPS)
    end
end)

test.case("flat_forward ignores pitch", function()
    test.expect_near(CameraMath.flat_forward(90), vec3(1, 0, 0), EPS)
    test.expect_near(CameraMath.flat_forward(180), vec3(0, 0, -1), EPS)
end)

test.case("axis combines two buttons", function()
    test.expect_eq(CameraMath.axis(true, false), 1)
    test.expect_eq(CameraMath.axis(false, true), -1)
    test.expect_eq(CameraMath.axis(true, true), 0)
    test.expect_eq(CameraMath.axis(false, false), 0)
end)

test.case("fly_delta moves along the look direction", function()
    local d = CameraMath.fly_delta(0, 90, 1, 0, 0, 20, 0.5)
    test.expect_near(d, vec3(0, 10, 0), 1e-6)
    d = CameraMath.fly_delta(90, 0, 0, 1, 1, 10, 1)
    test.expect_near(d, vec3(0, 10, -10), EPS)
end)

test.case("fly_delta is zero without input or time", function()
    test.expect_near(CameraMath.fly_delta(30, 10, 0, 0, 0, 20, 0.1), vec3(0, 0, 0), EPS)
    test.expect_near(CameraMath.fly_delta(30, 10, 1, 1, 1, 20, 0), vec3(0, 0, 0), EPS)
end)

test.case("walk_velocity is horizontal and normalises diagonals", function()
    local v = CameraMath.walk_velocity(0, 1, 0, 8)
    test.expect_near(v, vec3(0, 0, 8), EPS)
    v = CameraMath.walk_velocity(45, 1, 1, 8)
    test.expect_near(v:length(), 8, EPS)
    test.expect_near(v.y, 0, EPS)
    test.expect_near(CameraMath.walk_velocity(10, 0, 0, 8), vec3(0, 0, 0), EPS)
end)
