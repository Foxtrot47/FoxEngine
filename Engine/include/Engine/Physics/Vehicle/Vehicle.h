#pragma once
#include <array>
#include <memory>
#include <string>
#include <DirectXMath.h>
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Physics/Vehicle/TireModel.h"
#include "Engine/Physics/Vehicle/VehicleDesc.h"

namespace SE {

// Driver controls, already filtered (steering assists, pedal maps and
// reverse logic belong to the caller).
struct VehicleInput
{
    float throttle  = 0.0f; // 0..1
    float brake     = 0.0f; // 0..1
    float steer     = 0.0f; // -1..1 of full lock, + = right
    float handbrake = 0.0f; // 0..1
};

struct WheelTelemetry
{
    bool  contact     = false;
    float compression = 0.0f; // m from full droop
    float load        = 0.0f; // N
    float slipRatio   = 0.0f;
    float slipAngle   = 0.0f; // relaxed alpha, rad
    float longitudinalForce = 0.0f; // N
    float lateralForce      = 0.0f; // N
    float angularVelocity   = 0.0f; // rad/s
    float steerAngle        = 0.0f; // rad
};

struct VehicleTelemetry
{
    float speed      = 0.0f; // m/s along the chassis forward axis
    float engineRpm  = 0.0f;
    int   gear       = 1;    // -1 = reverse, 0 = neutral, 1..n forward
    float clutch     = 0.0f; // engagement 0..1
    float throttle   = 0.0f; // effective (after limiter/idle control)
    bool  shifting   = false;
    bool  tractionControlActive = false;
    bool  absActive  = false;
    DirectX::XMFLOAT3 acceleration = {}; // chassis-space, m/s^2 (x right, y up, z forward)
    std::array<WheelTelemetry, VehicleDesc::WheelCount> wheels;
};

// Wheeled vehicle simulated on top of a Jolt chassis body. Jolt integrates
// the chassis and handles its collisions; this class owns everything at the
// wheels: suspension (spring, dampers, bump stops, anti-roll bars), tire
// contact via ray queries, slip (longitudinal solved implicitly with wheel
// spin, lateral through a relaxation length), a pluggable tire force model,
// brakes with ABS, and the drivetrain (engine, launch clutch, gearbox,
// limited-slip differential, traction control) integrated in sub-steps of
// each fixed physics step.
class Vehicle final : public PhysicsStepListener
{
public:
    static constexpr int k_subSteps = 20;

    Vehicle() = default;
    ~Vehicle() override;
    Vehicle(const Vehicle&) = delete;
    Vehicle& operator=(const Vehicle&) = delete;

    // Creates the chassis body at the transform's position/orientation (the
    // vehicle origin sits on the ground plane) with the suspension at its
    // static design position. transform/body are mirrored by the world and
    // must outlive the vehicle.
    bool Init(PhysicsWorld& world, const VehicleDesc& desc, TransformComponent* transform,
              RigidBodyComponent* body, std::string* error = nullptr);
    void Shutdown();

    void SetInput(const VehicleInput& input) { m_input = input; }
    const VehicleInput& GetInput() const { return m_input; }

    // Shifting interrupts drive for the gearbox shift time.
    void ShiftUp();
    void ShiftDown();
    void SelectGear(int gear); // -1 = reverse, 0 = neutral, 1..n
    void SetAutomatic(bool automatic) { m_automatic = automatic; }
    bool IsAutomatic() const { return m_automatic; }

    // Driver aids, both on by default as on the road car.
    void SetTractionControl(bool enabled) { m_tractionControl = enabled; }
    void SetAbs(bool enabled) { m_abs = enabled; }
    bool IsTractionControlEnabled() const { return m_tractionControl; }
    bool IsAbsEnabled() const { return m_abs; }

    // Teleports upright to the given ground position/heading and clears all
    // dynamic state (recovery after a crash or flip).
    void Reset(DirectX::XMFLOAT3 groundPosition, float yawDeg);

    const VehicleTelemetry& GetTelemetry() const { return m_telemetry; }
    const VehicleDesc&      GetDesc() const { return m_desc; }
    BodyHandle              GetBody() const { return m_body; }

    // Wheel pose in vehicle space (suspension travel, steering, spin), for
    // rendering on top of the chassis transform.
    DirectX::XMMATRIX GetWheelLocalMatrix(int wheel) const;
    // Upright pose: the wheel pose without spin (brake calipers).
    DirectX::XMMATRIX GetSpindleLocalMatrix(int wheel) const;

    void OnPhysicsStep(PhysicsWorld& world, float dt) override;

private:
    struct WheelState
    {
        float omega      = 0.0f;
        float spinAngle  = 0.0f;
        float slipRatio  = 0.0f;
        float tanSlip    = 0.0f; // relaxed tan(alpha)
        float compression = 0.0f;
        float preload    = 0.0f;
        float steer      = 0.0f;
        float load       = 0.0f;
        bool  contact    = false;
        float fx = 0.0f, fy = 0.0f;
    };

    // One wheel's contact patch in world space for the current step.
    struct WheelContact
    {
        DirectX::XMVECTOR hub = {};
        DirectX::XMVECTOR point = {};
        DirectX::XMVECTOR forward = {};
        DirectX::XMVECTOR lateral = {};
        float vx = 0.0f, vy = 0.0f;
        PhysicsWorld::RaycastHit hit;
    };

    // Driver and drivetrain values prepared once per fixed step.
    struct StepState
    {
        float throttle = 0.0f;
        float brake = 0.0f;
        float handbrake = 0.0f;
        float clutch = 0.0f;
        float ratio = 0.0f; // signed crank-to-axle, 0 while shifting
        float drivenInertia = 1.0f;
        int   drivenLeft = 0;
        int   drivenRight = 1;
        bool  shifting = false;
    };

    float EngineTorque(float omega, float throttle) const;
    float GearRatio(int gear) const; // signed, crank-to-axle, includes final drive
    void  UpdateGearbox(float dt, float drivelineRpm);
    bool  AdvanceShiftTimer(float dt);
    void  TryAutomaticShift(float drivelineRpm);
    bool  CanDownshift(float drivelineRpm) const;
    void  ComputeSteering();
    void  ResetState();

    void PrepareDrivetrain(float dt, StepState& out);
    void SampleContacts(PhysicsWorld& world, DirectX::XMVECTOR position, DirectX::XMVECTOR rotation,
                        DirectX::XMVECTOR right, DirectX::XMVECTOR up, DirectX::XMVECTOR fwd);
    void ApplySuspensionForces(PhysicsWorld& world, DirectX::XMVECTOR up);
    void SolveWheelDynamics(float dt, const StepState& step);
    void ApplyTireForces(PhysicsWorld& world, DirectX::XMVECTOR up);
    void ApplyAerodynamics(PhysicsWorld& world, const PhysicsWorld::BodyState& state,
                           DirectX::XMVECTOR position, DirectX::XMVECTOR rotation,
                           DirectX::XMVECTOR up, DirectX::XMVECTOR fwd);
    void UpdateTelemetry(const PhysicsWorld::BodyState& state, float dt,
                         DirectX::XMVECTOR right, DirectX::XMVECTOR up, DirectX::XMVECTOR fwd,
                         const StepState& step);

    float RevLimitedThrottle(float engineRpm);
    float TractionControlFactor(float dt, int drivenLeft, int drivenRight);
    void  UpdateAbsFactors(float dt);
    float IdleThrottle(float throttle, float engineRpm) const;
    float LaunchClutch(float engineRpm, float ratio) const;

    void  ComputeLowSpeedDamping();
    void  ComputeWheelTorques(const StepState& step);
    float IntegrateSubstep(float dt, const StepState& step, float engineOmega);
    float ClutchTorqueForSubstep(float h, const StepState& step, float engineOmega,
                                 float engineTorque) const;
    void  DistributeDifferential(float h, const StepState& step, float clutchTorque);
    void  IntegrateWheelSpeeds(float h);

    PhysicsWorld*      m_world = nullptr;
    BodyHandle         m_body  = k_invalidBody;
    TransformComponent* m_transform = nullptr;
    RigidBodyComponent* m_rigidBody = nullptr;
    VehicleDesc        m_desc;
    std::array<std::unique_ptr<TireModel>, VehicleDesc::WheelCount> m_tires;
    std::array<WheelState, VehicleDesc::WheelCount> m_wheels;

    // Per-step scratch.
    std::array<WheelContact, VehicleDesc::WheelCount> m_contacts;
    std::array<float, VehicleDesc::WheelCount> m_suspensionForce = {};
    std::array<float, VehicleDesc::WheelCount> m_latDamping = {};
    std::array<float, VehicleDesc::WheelCount> m_externalTorque = {};
    std::array<float, VehicleDesc::WheelCount> m_brakeTorque = {};
    std::array<float, VehicleDesc::WheelCount> m_forceSlope = {};
    std::array<float, VehicleDesc::WheelCount> m_fxSum = {};
    std::array<float, VehicleDesc::WheelCount> m_fySum = {};

    VehicleInput     m_input;
    VehicleTelemetry m_telemetry;
    bool  m_automatic   = true;
    int   m_gear        = 1;
    int   m_pendingGear = 1;
    float m_shiftTimer  = 0.0f;
    float m_autoShiftHold = 0.0f;
    float m_engineOmega = 0.0f;
    bool  m_limiterCut  = false;
    bool  m_tractionControl = true;
    bool  m_abs         = true;
    float m_tcFactor    = 1.0f;
    std::array<float, VehicleDesc::WheelCount> m_absFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 m_prevVelocity = {};
};

} // namespace SE
