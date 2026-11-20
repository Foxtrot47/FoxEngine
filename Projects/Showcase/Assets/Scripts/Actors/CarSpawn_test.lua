local CarSpawn = require("Actors.CarSpawn")

local test = fox.test
local EPS = 1e-6

test.case("approach moves towards the target without overshooting", function()
    test.expect_near(CarSpawn.approach(0, 1, 0.25), 0.25, EPS)
    test.expect_near(CarSpawn.approach(0.9, 1, 0.25), 1, EPS)
    test.expect_near(CarSpawn.approach(0, -1, 0.25), -0.25, EPS)
    test.expect_near(CarSpawn.approach(-0.9, -1, 0.25), -1, EPS)
end)

test.case("rotate_y turns +Z towards +X", function()
    local right = CarSpawn.rotate_y(vec3(0, 0, 1), 90)
    test.expect_near(right.x, 1, EPS)
    test.expect_near(right.z, 0, EPS)
    local back = CarSpawn.rotate_y(vec3(0, 0, 1), 180)
    test.expect_near(back.z, -1, EPS)
end)

test.case("rotate_y keeps the heading on the ground plane", function()
    local turned = CarSpawn.rotate_y(vec3(1, 0, 0), 37)
    test.expect_near(turned.y, 0, EPS)
    test.expect_near(turned:length(), 1, EPS)
end)

test.case("gear_label names reverse, neutral and forward gears", function()
    test.expect_eq(CarSpawn.gear_label(-1), "R")
    test.expect_eq(CarSpawn.gear_label(0), "N")
    test.expect_eq(CarSpawn.gear_label(4), "4")
end)

test.case("aids_label shows the gearbox and active aids", function()
    local car = { automatic = true, traction_control = true, traction_control_active = false,
                  abs = true, abs_active = false }
    test.expect_eq(CarSpawn.aids_label(car), "AUTO TC ABS")
    car.traction_control_active = true
    car.abs_active = true
    test.expect_eq(CarSpawn.aids_label(car), "AUTO [TC] [ABS]")
    car.automatic = false
    car.traction_control = false
    car.abs = false
    test.expect_eq(CarSpawn.aids_label(car), "MAN")
end)

test.case("wants_reverse only at a standstill while braking", function()
    test.expect(CarSpawn.wants_reverse(1, true, 0, 0.5))
    test.expect(not CarSpawn.wants_reverse(-1, true, 0, 0.5))
    test.expect(not CarSpawn.wants_reverse(1, false, 0, 0.5))
    test.expect(not CarSpawn.wants_reverse(1, true, 0.5, 0.5))
end)

test.case("wants_forward only while reversed and throttling", function()
    test.expect(CarSpawn.wants_forward(-1, true, 0.5, 0))
    test.expect(not CarSpawn.wants_forward(1, true, 0.5, 0))
    test.expect(not CarSpawn.wants_forward(-1, false, 0.5, 0))
end)
