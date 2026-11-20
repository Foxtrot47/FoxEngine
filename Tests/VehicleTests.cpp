#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Physics/Vehicle/Vehicle.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace DirectX;

namespace {
void Require(bool value, const char* message)
{
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

constexpr const char* k_vehiclePath = "Assets/Vehicles/Porsche911GT3R/vehicle.json";
constexpr float k_dt = 1.0f / 120.0f;

struct TestTrack
{
    SE::PhysicsWorld       world;
    SE::TransformComponent transform;
    SE::RigidBodyComponent body;
    SE::Vehicle            car;

    TestTrack()
    {
        world.AddStaticBox(SE::OBB::FromAABB({ -2000.0f, -1.0f, -2000.0f }, { 2000.0f, 0.0f, 20000.0f }),
                           1.0f, 0.0f);
        SE::VehicleDesc desc;
        std::string error;
        Require(SE::LoadVehicleDesc(k_vehiclePath, desc, &error), error.c_str());
        Require(car.Init(world, desc, &transform, &body, &error), error.c_str());
    }

    void Run(float seconds, const SE::VehicleInput& input)
    {
        car.SetInput(input);
        for (float t = 0.0f; t < seconds; t += k_dt)
            world.Step(k_dt);
    }

    float SpeedKmh() const { return car.GetTelemetry().speed * 3.6f; }
};

float Planar(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return sqrtf((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

void TestSettlesAtDesignRideHeight()
{
    TestTrack track;
    track.Run(3.0f, {});
    const auto& tm = track.car.GetTelemetry();
    const auto& desc = track.car.GetDesc();
    for (int w = 0; w < SE::VehicleDesc::WheelCount; ++w)
    {
        Require(tm.wheels[w].contact, "all wheels on the ground at rest");
        const float droop = desc.AxleOf(w).suspension.droopTravel;
        std::printf("  wheel %d compression %.4f (design %.4f) load %.0f N\n", w,
                    tm.wheels[w].compression, droop, tm.wheels[w].load);
        Require(std::fabs(tm.wheels[w].compression - droop) < 0.005f,
                "static ride height within 5 mm of the modelled wheel position");
    }
    const float total = tm.wheels[0].load + tm.wheels[1].load + tm.wheels[2].load + tm.wheels[3].load;
    Require(std::fabs(total - desc.chassis.mass * 9.81f) < 0.01f * desc.chassis.mass * 9.81f,
            "wheel loads carry the car's weight");
    const float front = (tm.wheels[0].load + tm.wheels[1].load) / total;
    std::printf("  front weight share %.3f\n", front);
    Require(std::fabs(front - 0.44f) < 0.01f, "front/rear distribution follows the center of mass");
    Require(Planar(track.transform.position, { 0.0f, 0.0f, 0.0f }) < 0.01f, "parked car does not creep");
}

void TestLaunchAndTopSpeed()
{
    TestTrack track;
    track.Run(1.0f, {});
    SE::VehicleInput full;
    full.throttle = 1.0f;
    float t = 0.0f, t100 = -1.0f, t200 = -1.0f, topSpeed = 0.0f;
    track.car.SetInput(full);
    while (t < 60.0f)
    {
        track.world.Step(k_dt);
        t += k_dt;
        if (t100 < 0.0f && track.SpeedKmh() >= 100.0f) t100 = t;
        if (t200 < 0.0f && track.SpeedKmh() >= 200.0f) t200 = t;
        topSpeed = (std::max)(topSpeed, track.SpeedKmh());
    }
    const auto& tm = track.car.GetTelemetry();
    std::printf("  0-100 %.2f s, 0-200 %.2f s, top %.1f km/h in gear %d at %.0f rpm\n",
                t100, t200, topSpeed, tm.gear, tm.engineRpm);
    // Traction control is on; no launch control. The gearing tops out at
    // 296 km/h on the 9400 rpm limiter in 6th.
    Require(t100 > 3.0f && t100 < 4.0f, "0-100 km/h within 3.0-4.0 s");
    Require(t200 > 8.5f && t200 < 11.0f, "0-200 km/h within 8.5-11 s");
    Require(tm.gear == 6, "top speed reached in 6th");
    Require(topSpeed > 284.0f && topSpeed < 300.0f, "top speed near the 296 km/h limiter in 6th");
    Require(std::fabs(track.transform.position.x) < 2.0f, "straight-line run stays straight");
}

void TestBrakingDistance()
{
    TestTrack track;
    track.Run(1.0f, {});
    SE::VehicleInput full;
    full.throttle = 1.0f;
    track.car.SetInput(full);
    while (track.SpeedKmh() < 100.0f)
        track.world.Step(k_dt);
    const XMFLOAT3 start = track.transform.position;
    SE::VehicleInput brake;
    brake.brake = 1.0f;
    track.Run(6.0f, brake);
    const float distance = Planar(track.transform.position, start);
    std::printf("  100-0 km/h in %.1f m, final %.2f km/h\n", distance, track.SpeedKmh());
    Require(std::fabs(track.SpeedKmh()) < 0.5f, "car stops under full braking");
    Require(distance > 20.0f && distance < 30.0f, "100-0 km/h within 20-30 m on slicks");
}

// Engine-space convention: +steer turns right, i.e. toward +X when heading +Z.
void TestSteadyStateCornering()
{
    TestTrack track;
    track.Run(1.0f, {});
    SE::VehicleInput input;
    input.throttle = 1.0f;
    track.car.SetInput(input);
    while (track.SpeedKmh() < 60.0f)
        track.world.Step(k_dt);
    float maxLateral = 0.0f;
    for (float t = 0.0f; t < 8.0f; t += k_dt)
    {
        input.throttle = std::clamp((60.0f - track.SpeedKmh()) * 0.1f + 0.2f, 0.0f, 1.0f);
        input.steer = 0.15f;
        track.car.SetInput(input);
        track.world.Step(k_dt);
        if (t > 4.0f)
            maxLateral = (std::max)(maxLateral, std::fabs(track.car.GetTelemetry().acceleration.x));
    }
    std::printf("  heading %.1f deg, x %.1f m, lateral accel %.2f g at %.1f km/h\n",
                track.transform.eulerDeg.y, track.transform.position.x, maxLateral / 9.81f,
                track.SpeedKmh());
    Require(track.transform.position.x > 5.0f, "positive steer turns toward +X (right)");
    Require(maxLateral > 2.0f && maxLateral < 1.6f * 9.81f, "cornering acceleration is plausible");
}

// A spun car slides sideways with no forward wheel speed; the low-speed tire
// damping must not treat that as standing still (it used to roll the car).
void TestSidewaysSlideStaysOnWheels()
{
    TestTrack track;
    track.Run(1.0f, {});
    track.body.velocity = { 25.0f, 0.0f, 0.0f }; // 90 km/h broadside, heading +Z
    float maxHeight = 0.0f, minUp = 1.0f;
    for (float t = 0.0f; t < 5.0f; t += k_dt)
    {
        track.world.Step(k_dt);
        const auto state = track.world.GetBodyState(track.car.GetBody());
        const XMVECTOR up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), XMLoadFloat4(&state.rotation));
        maxHeight = (std::max)(maxHeight, state.position.y);
        minUp = (std::min)(minUp, XMVectorGetY(up));
    }
    std::printf("  max height %.3f m, min up.y %.3f, final %.2f m/s\n", maxHeight, minUp,
                XMVectorGetX(XMVector3Length(XMLoadFloat3(&track.body.velocity))));
    Require(maxHeight < 0.05f, "a broadside slide does not lift the car");
    Require(minUp > 0.95f, "a broadside slide does not roll the car over");
}
}

int main()
{
    std::puts("settle:");     TestSettlesAtDesignRideHeight();
    std::puts("launch:");     TestLaunchAndTopSpeed();
    std::puts("braking:");    TestBrakingDistance();
    std::puts("cornering:");  TestSteadyStateCornering();
    std::puts("slide:");      TestSidewaysSlideStaysOnWheels();
    std::puts("VehicleTests: all five groups passed");
    return 0;
}
