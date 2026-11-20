#pragma once
#include <array>
#include <string>
#include <utility>
#include <vector>
#include <DirectXMath.h>
#include "Engine/Physics/Vehicle/TireModel.h"

namespace SE {

// Physical definition of a wheeled vehicle. Vehicle space is the engine's
// left-handed frame: +X right, +Y up, +Z forward, origin on the ground plane
// at the car's centerline (as authored in layout.json).
struct VehicleDesc
{
    enum WheelIndex { FrontLeft = 0, FrontRight = 1, RearLeft = 2, RearRight = 3, WheelCount = 4 };

    struct Chassis
    {
        float             mass = 1450.0f;            // kg, sprung + unsprung
        DirectX::XMFLOAT3 centerOfMass = {};         // vehicle space
        DirectX::XMFLOAT3 inertia = { 2000.0f, 2100.0f, 550.0f }; // pitch (X), yaw (Y), roll (Z), kg*m^2
        std::vector<DirectX::XMFLOAT3> hullPoints;   // collision hull, vehicle space
    };

    struct Aero
    {
        float airDensity = 1.225f; // kg/m^3
        float dragArea   = 0.75f;  // Cd * A, m^2
        float liftAreaFront = 0.9f; // downforce Cl * A acting at the front axle, m^2
        float liftAreaRear  = 1.2f; // ... at the rear axle
    };

    struct Wheel
    {
        DirectX::XMFLOAT3 center = {}; // static (design load) position, vehicle space
        float radius = 0.35f;
    };

    struct Suspension
    {
        float springRate      = 60000.0f; // wheel rate, N/m
        float bumpDamping     = 3000.0f;  // N/(m/s), compression
        float reboundDamping  = 6000.0f;  // N/(m/s), extension
        float bumpTravel      = 0.06f;    // m of compression above the design position
        float droopTravel     = 0.08f;    // m of extension below it
        float bumpStopRate    = 400000.0f; // N/m once compression exceeds bumpTravel
        float antiRollRate    = 30000.0f; // N per m of left/right compression difference
        float rollCenterHeight = 0.05f;   // m above the contact patch where tire forces act
    };

    struct Axle
    {
        Suspension         suspension;
        MagicFormulaParams tire;
        float relaxationLengthLat  = 0.45f; // m
        float rollingResistance    = 0.012f; // Fx / Fz
        float wheelInertia     = 1.2f;    // spin inertia per wheel incl. disc, kg*m^2
        float maxBrakeTorque   = 2500.0f; // per wheel at full pedal, N*m
        float handbrakeTorque  = 0.0f;    // per wheel
        float maxSteerAngleDeg = 0.0f;    // road-wheel angle at full lock (0 = not steered)
        float ackermann        = 1.0f;    // 0 = parallel steer, 1 = full Ackermann
        bool  driven           = false;
    };

    struct Engine
    {
        std::vector<std::pair<float, float>> torqueCurve; // (rpm, N*m) at full throttle
        float idleRpm      = 1000.0f;
        float limiterRpm   = 9000.0f;
        float inertia      = 0.15f;  // crank + flywheel, kg*m^2
        float frictionTorque  = 20.0f; // N*m at zero speed (closed throttle)
        float frictionPerRad  = 0.06f; // N*m per rad/s (engine braking grows with rpm)
    };

    struct Gearbox
    {
        std::vector<float> ratios;   // forward gears, 1st first
        float reverseRatio = 3.4f;
        float finalDrive   = 4.0f;
        float efficiency   = 0.92f;
        float shiftTime    = 0.10f;  // s of torque interruption
        float clutchMaxTorque = 900.0f; // N*m at the crank
        float autoUpshiftRpm   = 8800.0f;
        float autoDownshiftRpm = 4500.0f;
    };

    struct Differential
    {
        float preload    = 50.0f; // N*m locking torque at zero input
        float powerLock  = 0.3f;  // extra locking torque per N*m of drive input
        float coastLock  = 0.2f;  // ... per N*m of braking (overrun) input
    };

    std::string  name;
    Chassis      chassis;
    Aero         aero;
    std::array<Wheel, WheelCount> wheels;
    Axle         front;
    Axle         rear;
    Engine       engine;
    Gearbox      gearbox;
    Differential differential;

    const Axle& AxleOf(int wheel) const { return wheel < RearLeft ? front : rear; }
};

// Loads the physics part of a vehicle definition (JSON); the "visual" block
// belongs to the scene layer. Geometry (wheel placement, hull) comes from the
// layout file referenced relative to the definition.
bool LoadVehicleDesc(const std::string& path, VehicleDesc& out, std::string* error = nullptr);

} // namespace SE
