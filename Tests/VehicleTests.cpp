#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Physics/Vehicle/AeroModel.h"
#include "Engine/Physics/Vehicle/TireModel.h"
#include "Engine/Physics/Vehicle/Vehicle.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <tuple>
#include <nlohmann/json.hpp>

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
    // Traction control is on; no launch control. Drag (Cd*A ~1.4 m^2) caps
    // the car below the gearing's 296 km/h on the 9400 rpm limiter in 6th.
    Require(t100 > 3.0f && t100 < 4.0f, "0-100 km/h within 3.0-4.0 s");
    Require(t200 > 8.5f && t200 < 11.0f, "0-200 km/h within 8.5-11 s");
    Require(tm.gear == 6, "top speed reached in 6th");
    Require(topSpeed > 250.0f && topSpeed < 285.0f, "drag-limited top speed in 6th, below the limiter");
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

const SE::VehicleDesc& PorscheDesc()
{
    static const SE::VehicleDesc desc = [] {
        SE::VehicleDesc d;
        std::string error;
        Require(SE::LoadVehicleDesc(k_vehiclePath, d, &error), error.c_str());
        return d;
    }();
    return desc;
}

// Peak slip at zero speed: (p0, p1, Lref) interpolated by load.
float ExpectedPeakSlip(const SE::TireAxisParams& axis, float load)
{
    return axis.peakSlip[0] + (axis.peakSlip[1] - axis.peakSlip[0]) * std::clamp(load / axis.peakSlip[2], 0.0f, 1.0f);
}

struct LateralPeak
{
    float sinAlpha = 0.0f;
    float force = 0.0f; // |Fy|
};

LateralPeak FindLateralPeak(const SE::TireModel& tire, float load, float camber)
{
    LateralPeak peak;
    for (float s = 0.0f; s < 0.4f; s += 1e-4f)
    {
        SE::TireInput in;
        in.slipAngle  = asinf(s);
        in.normalLoad = load;
        in.camber     = camber;
        const float force = std::fabs(tire.Evaluate(in).lateral);
        if (force > peak.force)
            peak = { s, force };
    }
    return peak;
}

// Slip of one curve sample once the curve is rescaled to peak at sp.
float CurveStepAsSlip(const SE::TireCurve& curve, float sp)
{
    const auto peak = std::max_element(curve.data.begin(), curve.data.end());
    const float peakX = curve.step * static_cast<float>(peak - curve.data.begin());
    return curve.step * sp / peakX;
}

void TestTirePeakSlip()
{
    const SE::TireParams& p = PorscheDesc().front.tire;
    const SE::TireModel tire(p);
    for (float load : { 2000.0f, 4000.0f, 6000.0f })
    {
        const LateralPeak peak = FindLateralPeak(tire, load, 0.0f);
        const float sp = ExpectedPeakSlip(p.lateral, load);
        std::printf("  %4.0f N: Fy peaks at sin(alpha) %.4f, sp(L) %.4f (%.2f deg)\n", load, peak.sinAlpha, sp,
                    asinf(sp) * 57.2958f);
        Require(std::fabs(peak.sinAlpha - sp) <= CurveStepAsSlip(p.lateralCurve, sp),
                "pure lateral force peaks at sin(alpha) = sp(L) within one curve step");
    }
}

void TestTireLoadSensitivity()
{
    const SE::TireParams& p = PorscheDesc().front.tire;
    const SE::TireModel tire(p);
    float mu[2] = {};
    const float loads[2] = { 2000.0f, 6000.0f };
    for (int i = 0; i < 2; ++i)
    {
        mu[i] = FindLateralPeak(tire, loads[i], 0.0f).force / loads[i];
        const float expected = p.lateral.mu * SE::TireLoadSensitivity(p.lateral.loadSens, loads[i]);
        std::printf("  %4.0f N: peak lateral mu %.3f (mu * g(F) = %.3f)\n", loads[i], mu[i], expected);
        Require(std::fabs(mu[i] / expected - 1.0f) < 0.01f, "peak lateral mu follows mu * g(F)");
    }
    Require(mu[1] < mu[0], "peak lateral mu falls with load");
}

void TestTireCamber()
{
    const SE::TireParams& p = PorscheDesc().front.tire;
    const SE::TireModel tire(p);
    // Positive slip angle: Fy acts toward -lateral, so negative camber leans into the force.
    const float peakRad = p.camberPeakDeg / 57.2958f;
    const float base   = FindLateralPeak(tire, 4000.0f, 0.0f).force;
    const float toward = FindLateralPeak(tire, 4000.0f, -peakRad).force;
    const float away   = FindLateralPeak(tire, 4000.0f, peakRad).force;
    std::printf("  camber %.1f deg toward the force x%.3f (gain %.3f), away x%.3f\n", p.camberPeakDeg,
                toward / base, p.camberLatGain, away / base);
    Require(std::fabs(toward / base - (1.0f + p.camberLatGain)) < 0.005f,
            "camber at its peak toward the force raises peak Fy by the gain");
    Require(std::fabs(away / base - (1.0f - p.camberLatGain)) < 0.005f && away < base,
            "camber at its peak leaning away from the force lowers peak Fy by the gain");
}

void TestTireCombinedSlip()
{
    const SE::TireParams& p = PorscheDesc().front.tire;
    const SE::TireModel tire(p);
    const float load = 4000.0f;
    SE::TireInput in;
    in.normalLoad = load;
    in.slipRatio  = tire.PeakSlipRatio(load, 0.0f);
    const float pure = tire.Evaluate(in).longitudinal;
    in.slipAngle = asinf(ExpectedPeakSlip(p.lateral, load));
    const float combined = tire.Evaluate(in).longitudinal;
    const float peak = p.longitudinal.mu * SE::TireLoadSensitivity(p.longitudinal.loadSens, load) * load;
    std::printf("  Fx at peak slip ratio %.3f: pure %.0f N (mu*g*F %.0f), with peak slip angle %.0f N\n",
                in.slipRatio, pure, peak, combined);
    Require(std::fabs(pure / peak - 1.0f) < 0.01f, "pure longitudinal force peaks at the peak slip ratio");
    Require(combined > 0.0f && combined < 0.9f * pure, "combined slip reduces the longitudinal force");
}

constexpr float k_kmh = 1.0f / 3.6f;

SE::AeroInput StraightAero(const SE::AeroParams& p, float speed, float yawDeg = 0.0f)
{
    SE::AeroInput in;
    in.frontRideHeight = p.staticRideHeight[0];
    in.rearRideHeight  = p.staticRideHeight[1];
    const float yaw = yawDeg / 57.2958f;
    in.elementVelocity.fill({ speed * sinf(yaw), 0.0f, speed * cosf(yaw) });
    return in;
}

float Downforce(const SE::AeroForces& f) { return f.downforceFront + f.downforceRear; }

void TestAeroSpeedSquared()
{
    const SE::AeroParams& p = PorscheDesc().aero;
    const SE::AeroForces slow = SE::EvaluateAero(p, StraightAero(p, 100.0f * k_kmh));
    const SE::AeroForces fast = SE::EvaluateAero(p, StraightAero(p, 200.0f * k_kmh));
    const float q = 0.5f * p.airDensity * (200.0f * k_kmh) * (200.0f * k_kmh);
    std::printf("  static ride heights: Cl*A %.3f, Cd*A %.3f m^2, front balance %.1f%%; 200 km/h %.0f N\n",
                Downforce(fast) / q, fast.drag / q, 100.0f * fast.downforceFront / Downforce(fast), Downforce(fast));
    Require(Downforce(slow) > 0.0f && fast.drag > 0.0f, "the aero map makes downforce and drag");
    Require(std::fabs(Downforce(fast) / Downforce(slow) / 4.0f - 1.0f) < 0.01f &&
            std::fabs(fast.drag / slow.drag / 4.0f - 1.0f) < 0.01f, "aero forces scale with v^2");
}

void TestAeroDiffuserStall()
{
    const SE::AeroParams& p = PorscheDesc().aero;
    SE::AeroInput in = StraightAero(p, 200.0f * k_kmh);
    in.frontRideHeight = in.rearRideHeight = 0.0f;
    SE::AeroParams noStall = p;
    noStall.diffuser.limits[0] = 0.0f;
    const float stalled = SE::EvaluateAero(p, in).elements[SE::AeroDiffuser].force.y;
    const float clean   = SE::EvaluateAero(noStall, in).elements[SE::AeroDiffuser].force.y;
    std::printf("  diffuser at zero ride height: %.1f N stalled vs %.1f N (stall share %.2f)\n", stalled, clean,
                p.diffuser.stall[1]);
    Require(clean != 0.0f && std::fabs(stalled / clean - (1.0f - p.diffuser.stall[1])) < 1e-4f,
            "the diffuser loses its stall share at zero ride height");
}

float LutValue(const SE::AeroParams::FrontWing& wing, float h)
{
    const float last = static_cast<float>(wing.heightLut.size() - 1);
    const float f = std::clamp((h - wing.heightRange[0]) / (wing.heightRange[1] - wing.heightRange[0]) * last, 0.0f, last);
    const size_t i = (std::min)(static_cast<size_t>(f), wing.heightLut.size() - 2);
    return wing.heightLut[i] + (wing.heightLut[i + 1] - wing.heightLut[i]) * (f - static_cast<float>(i));
}

void TestAeroFrontWingHeight()
{
    // Rake terms off so the wing lift is the LUT alone.
    SE::AeroParams p = PorscheDesc().aero;
    p.frontWing.rakeLift = {};
    const float speed = 200.0f * k_kmh;
    const float q = 0.5f * p.airDensity * speed * speed;
    const float t = (p.frontWing.center.z - p.rearAxleZ) / (p.frontAxleZ - p.rearAxleZ);
    for (float front : { 0.03f, 0.04f, 0.05f, 0.06f })
    {
        SE::AeroInput in = StraightAero(p, speed);
        in.frontRideHeight = front;
        const float h = in.rearRideHeight + (front - in.rearRideHeight) * t;
        const float force = SE::EvaluateAero(p, in).elements[SE::AeroFrontWing].force.y;
        const float expected = q * p.frontWing.lift * LutValue(p.frontWing, h);
        std::printf("  front ride height %.3f m (wing %.3f m): %.0f N, LUT %.0f N\n", front, h, force, expected);
        Require(std::fabs(force / expected - 1.0f) < 1e-3f, "front wing lift follows the height LUT");
    }
}

void TestAeroYaw()
{
    const SE::AeroParams& p = PorscheDesc().aero;
    const float speed = 200.0f * k_kmh;
    const SE::AeroForces straight = SE::EvaluateAero(p, StraightAero(p, speed));
    const SE::AeroForces yawed    = SE::EvaluateAero(p, StraightAero(p, speed, 20.0f));
    const float cos2 = cosf(20.0f / 57.2958f) * cosf(20.0f / 57.2958f);
    for (int e : { SE::AeroFrontWing, SE::AeroRearWing })
    {
        const SE::AeroParams::Yaw& yaw = e == SE::AeroFrontWing ? p.frontWing.yaw : p.rearWing.yaw;
        const float factor = yaw.peakMult * (1.0f - yaw.sideways * sinf((20.0f - yaw.peakDeg) / 57.2958f));
        const float ratio = yawed.elements[e].force.y / straight.elements[e].force.y;
        std::printf("  %s wing downforce at 20 deg yaw x%.3f (yaw factor %.3f x cos^2)\n",
                    e == SE::AeroFrontWing ? "front" : "rear", ratio, factor);
        Require(ratio < 1.0f && std::fabs(ratio / (factor * cos2) - 1.0f) < 0.005f, "yaw reduces wing downforce");
    }
}

nlohmann::json ReadJsonFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    return nlohmann::json::parse(file, nullptr, true, true);
}

void WriteJsonFile(const std::filesystem::path& path, const nlohmann::json& j)
{
    std::ofstream(path) << j.dump(2);
}

std::filesystem::path ScratchDir() { return std::filesystem::temp_directory_path() / "FoxVehicleTests"; }

// Loads a Porsche copy with one generated file edited; expects an error naming the file and key.
void ExpectLoadError(const char* file, const std::function<void(nlohmann::json&)>& edit, const char* key)
{
    namespace fs = std::filesystem;
    const fs::path source = fs::absolute(k_vehiclePath).parent_path();
    const fs::path dir = ScratchDir();
    fs::create_directories(dir);
    nlohmann::json vehicle = ReadJsonFile(source / "vehicle.json");
    vehicle["layout"] = (source / vehicle.at("layout").get<std::string>()).generic_string();
    WriteJsonFile(dir / "vehicle.json", vehicle);
    for (const char* name : { "tires.json", "aero.json" })
    {
        nlohmann::json j = ReadJsonFile(source / name);
        if (std::string(name) == file)
            edit(j);
        WriteJsonFile(dir / name, j);
    }
    SE::VehicleDesc desc;
    std::string error;
    const bool loaded = SE::LoadVehicleDesc((dir / "vehicle.json").string(), desc, &error);
    std::printf("  %s\n", error.c_str());
    Require(!loaded && error.find(file) != std::string::npos && error.find(key) != std::string::npos,
            "invalid generated file is rejected with its file and key");
}

void TestLoaderValidation()
{
    // Both shipped cars load (the Ferrari is CarSpawn.lua's default).
    SE::VehicleDesc ferrari;
    std::string error;
    Require(SE::LoadVehicleDesc("Assets/Vehicles/Ferrari488GTEEvo/vehicle.json", ferrari, &error), error.c_str());
    ExpectLoadError("tires.json", [](nlohmann::json& j) { j["front"]["lateral"]["extra"] = 1; },
                    "front.lateral.extra");
    ExpectLoadError("tires.json", [](nlohmann::json& j) { j["rear"]["longitudinal"]["brakingCurve"] = "Missing"; },
                    "rear.longitudinal.brakingCurve");
    ExpectLoadError("aero.json", [](nlohmann::json& j) { j["version"] = 2; }, "version");
    ExpectLoadError("aero.json", [](nlohmann::json& j) { j["diffuser"]["limits"] = { 0.01, 0.1 }; },
                    "diffuser.limits");
    ExpectLoadError("aero.json", [](nlohmann::json& j) { j["body"].erase("maxHeight"); }, "body.maxHeight");
    std::filesystem::remove_all(ScratchDir());
}

// Throttle that holds a target speed.
float HoldSpeed(float speedKmh, float targetKmh)
{
    return std::clamp((targetKmh - speedKmh) * 0.2f + 0.5f, 0.0f, 1.0f);
}

void TestAeroSquatAtSpeed()
{
    TestTrack track;
    track.Run(1.0f, {});
    SE::VehicleInput input;
    input.throttle = 1.0f;
    track.car.SetInput(input);
    for (float t = 0.0f; track.SpeedKmh() < 250.0f && t < 40.0f; t += k_dt)
        track.world.Step(k_dt);
    Require(track.SpeedKmh() >= 250.0f, "the car reaches 250 km/h");

    const auto& tm = track.car.GetTelemetry();
    float loads = 0.0f, downforce = 0.0f, front = 0.0f, drag = 0.0f, rhFront = 0.0f, rhRear = 0.0f, speed = 0.0f;
    int samples = 0;
    for (float t = 0.0f; t < 6.0f; t += k_dt)
    {
        input.throttle = HoldSpeed(track.SpeedKmh(), 250.0f);
        track.Run(k_dt, input);
        if (t < 4.0f)
            continue;
        loads += tm.wheels[0].load + tm.wheels[1].load + tm.wheels[2].load + tm.wheels[3].load;
        downforce += tm.downforceFront + tm.downforceRear;
        front += tm.downforceFront;
        drag += tm.drag;
        rhFront += tm.rideHeightFront;
        rhRear += tm.rideHeightRear;
        speed += tm.speed;
        ++samples;
    }
    const float n = static_cast<float>(samples);
    loads /= n; downforce /= n; front /= n; drag /= n; rhFront /= n; rhRear /= n; speed /= n;
    const auto& desc = track.car.GetDesc();
    const float weight = desc.chassis.mass * 9.81f;
    const float q = 0.5f * desc.aero.airDensity * speed * speed;
    std::printf("  %.1f km/h: wheel loads - weight %.0f N, downforce %.0f N, drag %.0f N\n", speed * 3.6f,
                loads - weight, downforce, drag);
    std::printf("  Cl*A %.3f, Cd*A %.3f m^2, front balance %.1f%%, ride height front %.1f mm (static %.1f), "
                "rear %.1f mm (static %.1f)\n",
                downforce / q, drag / q, 100.0f * front / downforce, rhFront * 1000.0f,
                desc.aero.staticRideHeight[0] * 1000.0f, rhRear * 1000.0f, desc.aero.staticRideHeight[1] * 1000.0f);
    Require(std::fabs(loads - weight - downforce) < 0.03f * downforce, "wheel loads carry weight plus downforce");
    Require(rhFront < desc.aero.staticRideHeight[0] && rhRear < desc.aero.staticRideHeight[1],
            "downforce squats both axles below the static ride height");
}

struct SkidpadResult
{
    float lateralG = 0.0f;
    float speedKmh = 0.0f;
};

// Follows a right-hand circle while the target speed rises 1 km/h per second;
// returns the highest 1 s mean lateral acceleration reached within 1.5 m of
// the circle.
SkidpadResult Skidpad(float radius, float startKmh, float endKmh)
{
    TestTrack track;
    track.Run(1.0f, {});
    SE::VehicleInput input;
    input.throttle = 1.0f;
    track.car.SetInput(input);
    for (float t = 0.0f; track.SpeedKmh() < startKmh && t < 30.0f; t += k_dt)
        track.world.Step(k_dt);

    const auto& desc = track.car.GetDesc();
    const float wheelbase = desc.aero.frontAxleZ - desc.aero.rearAxleZ;
    const float maxSteer = desc.front.maxSteerAngleDeg / 57.2958f;
    const float cx = track.transform.position.x + radius, cz = track.transform.position.z;
    constexpr float k_rampKmhPerS = 1.0f, k_settle = 4.0f, k_average = 1.0f;
    SkidpadResult best;
    float lateral = 0.0f;
    for (float t = 0.0f; t < k_settle + (endKmh - startKmh) / k_rampKmhPerS; t += k_dt)
    {
        const SE::PhysicsWorld::BodyState state = track.world.GetBodyState(track.car.GetBody());
        const float dx = state.position.x - cx, dz = state.position.z - cz;
        const float dist = sqrtf(dx * dx + dz * dz);
        const float error = dist - radius; // > 0 outside the circle
        const float tangent = atan2f(dz / dist, -dx / dist);
        float courseError = tangent - atan2f(state.linearVelocity.x, state.linearVelocity.z);
        courseError = atan2f(sinf(courseError), cosf(courseError));
        if (std::fabs(error) > 6.0f)
            break; // lost the circle

        const float steer = atanf(wheelbase / radius) + 0.02f * error + 0.5f * courseError;
        const float target = startKmh + k_rampKmhPerS * (std::max)(t - k_settle, 0.0f);
        input.steer = std::clamp(steer / maxSteer, -1.0f, 1.0f);
        input.throttle = HoldSpeed(track.SpeedKmh(), target);
        track.Run(k_dt, input);
        lateral += (std::fabs(track.car.GetTelemetry().acceleration.x) - lateral) * k_dt / k_average;
        if (t > k_settle && std::fabs(error) < 1.5f && lateral / 9.81f > best.lateralG)
            best = { lateral / 9.81f, track.SpeedKmh() };
    }
    return best;
}

void PrintRealLifeCheck()
{
    for (auto [radius, start, end] : { std::tuple<float, float, float>{ 40.0f, 60.0f, 130.0f },
                                       std::tuple<float, float, float>{ 200.0f, 160.0f, 260.0f } })
    {
        const SkidpadResult r = Skidpad(radius, start, end);
        std::printf("  skidpad R %.0f m: %.2f g at %.0f km/h\n", radius, r.lateralG, r.speedKmh);
    }

    TestTrack track;
    track.Run(1.0f, {});
    SE::VehicleInput input;
    input.throttle = 1.0f;
    track.car.SetInput(input);
    for (float t = 0.0f; track.SpeedKmh() < 200.0f && t < 30.0f; t += k_dt)
        track.world.Step(k_dt);
    const XMFLOAT3 start = track.transform.position;
    SE::VehicleInput brake;
    brake.brake = 1.0f;
    track.car.SetInput(brake);
    float t = 0.0f;
    for (; track.SpeedKmh() > 0.5f && t < 15.0f; t += k_dt)
        track.world.Step(k_dt);
    std::printf("  200-0 km/h braking: %.1f m in %.2f s\n", Planar(track.transform.position, start), t);
}
}

int main()
{
    std::puts("tire peak slip:");       TestTirePeakSlip();
    std::puts("tire load sensitivity:"); TestTireLoadSensitivity();
    std::puts("tire camber:");          TestTireCamber();
    std::puts("tire combined slip:");   TestTireCombinedSlip();
    std::puts("aero v^2:");             TestAeroSpeedSquared();
    std::puts("aero diffuser stall:");  TestAeroDiffuserStall();
    std::puts("aero front wing height:"); TestAeroFrontWingHeight();
    std::puts("aero yaw:");             TestAeroYaw();
    std::puts("loader validation:");    TestLoaderValidation();
    std::puts("settle:");     TestSettlesAtDesignRideHeight();
    std::puts("launch:");     TestLaunchAndTopSpeed();
    std::puts("braking:");    TestBrakingDistance();
    std::puts("cornering:");  TestSteadyStateCornering();
    std::puts("slide:");      TestSidewaysSlideStaysOnWheels();
    std::puts("aero squat:"); TestAeroSquatAtSpeed();
    std::puts("real-life check (printed, not asserted):"); PrintRealLifeCheck();
    std::puts("VehicleTests: all groups passed");
    return 0;
}
