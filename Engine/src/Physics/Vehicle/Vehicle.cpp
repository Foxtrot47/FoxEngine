#include "Engine/Physics/Vehicle/Vehicle.h"

#include <algorithm>
#include <cmath>

namespace SE {

using namespace DirectX;

namespace {

constexpr float k_rpmPerRad     = 60.0f / XM_2PI;
constexpr float k_lowSpeed      = 3.0f;  // m/s below which tire damping fades in
constexpr float k_dampingRatio  = 0.7f;  // low-speed tire damping vs critical
constexpr float k_maxSlip       = 5.0f;
constexpr float k_slipSpeedFloor = 1.0f; // m/s, slip-ratio denominator floor near standstill
constexpr float k_slopeEpsilon  = 1e-3f; // slip step for the tire force slope
constexpr float k_autoShiftHold = 0.5f;  // s after a shift before the automatic may shift again
constexpr float k_shiftMarginRpm = 500.0f;
// Driver aids (TC/ABS): intervention starts at k_aidSlipStart x the tire's
// peak slip and reaches full cut k_aidSlipWindow x peak later.
constexpr float k_aidSlipStart  = 1.2f;
constexpr float k_aidSlipWindow = 0.8f;
constexpr float k_aidResponse   = 25.0f; // 1/s

XMVECTOR Load(const XMFLOAT3& v) { return XMLoadFloat3(&v); }
XMFLOAT3 Store(FXMVECTOR v) { XMFLOAT3 out; XMStoreFloat3(&out, v); return out; }
float Dot(FXMVECTOR a, FXMVECTOR b) { return XMVectorGetX(XMVector3Dot(a, b)); }

float Smoothstep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float SampleCurve(const std::vector<std::pair<float, float>>& curve, float rpm)
{
    if (rpm <= curve.front().first) return curve.front().second;
    if (rpm >= curve.back().first)  return curve.back().second;
    for (size_t i = 1; i < curve.size(); ++i)
    {
        if (rpm <= curve[i].first)
        {
            const auto& a = curve[i - 1];
            const auto& b = curve[i];
            return a.second + (b.second - a.second) * (rpm - a.first) / (b.first - a.first);
        }
    }
    return curve.back().second;
}

XMVECTOR OnPlane(FXMVECTOR v, FXMVECTOR n)
{
    return XMVector3Normalize(XMVectorSubtract(v, XMVectorScale(n, Dot(v, n))));
}

XMVECTOR ToWorld(FXMVECTOR position, FXMVECTOR rotation, const XMFLOAT3& local)
{
    return XMVectorAdd(position, XMVector3Rotate(Load(local), rotation));
}

// TC/ABS output (1 = no intervention) for a wheel slipping by `slip`, tire force peaking at `peak`.
float AidTarget(bool enabled, float slip, float peak)
{
    if (!enabled)
        return 1.0f;
    return std::clamp(1.0f - (slip - k_aidSlipStart * peak) / (k_aidSlipWindow * peak), 0.0f, 1.0f);
}

} // namespace

Vehicle::~Vehicle()
{
    Shutdown();
}

bool Vehicle::Init(PhysicsWorld& world, const VehicleDesc& desc, TransformComponent* transform,
                   RigidBodyComponent* body, std::string* error)
{
    Shutdown();
    if (desc.chassis.hullPoints.size() < 4)
    {
        if (error) *error = "Vehicle '" + desc.name + "' has no collision hull";
        return false;
    }
    if (desc.front.driven == desc.rear.driven)
    {
        if (error) *error = "Vehicle '" + desc.name + "' needs exactly one driven axle";
        return false;
    }

    m_desc = desc;
    body->mass        = desc.chassis.mass;
    body->useGravity  = true;
    body->friction    = 0.5f;
    body->restitution = 0.1f;
    body->velocity    = {};

    PhysicsWorld::ConvexBodyDesc chassis;
    chassis.hullPoints   = desc.chassis.hullPoints;
    chassis.centerOfMass = desc.chassis.centerOfMass;
    chassis.inertia      = desc.chassis.inertia;
    // Forces are applied every step, and resistance is modelled explicitly
    // (aero, rolling); Jolt's velocity damping would be a hidden drag of ~5%
    // of speed per second.
    chassis.linearDamping  = 0.0f;
    chassis.angularDamping = 0.0f;
    chassis.allowSleeping  = false;
    m_body = world.AddConvexBody(transform, body, chassis);
    if (m_body == k_invalidBody)
    {
        if (error) *error = "Vehicle '" + desc.name + "' chassis body could not be created";
        return false;
    }
    m_world     = &world;
    m_transform = transform;
    m_rigidBody = body;

    m_tires.clear();
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
        m_tires.emplace_back(desc.AxleOf(w).tire);
    ResetState();
    world.AddStepListener(this);
    return true;
}

void Vehicle::Shutdown()
{
    if (!m_world)
        return;
    m_world->RemoveStepListener(this);
    m_world->RemoveBody(m_body);
    m_world = nullptr;
    m_body  = k_invalidBody;
}

void Vehicle::ResetState()
{
    // Static axle loads from the center of mass split the weight; spring
    // preload puts each wheel at its design position under that load.
    const float gravity = -m_world->GetSettings().gravity.y;
    const auto& wheels = m_desc.wheels;
    const float zFront = 0.5f * (wheels[VehicleDesc::FrontLeft].center.z + wheels[VehicleDesc::FrontRight].center.z);
    const float zRear  = 0.5f * (wheels[VehicleDesc::RearLeft].center.z + wheels[VehicleDesc::RearRight].center.z);
    const float frontShare = std::clamp((m_desc.chassis.centerOfMass.z - zRear) / (zFront - zRear), 0.0f, 1.0f);
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        const auto& sus = m_desc.AxleOf(w).suspension;
        const float share = w < VehicleDesc::RearLeft ? frontShare : 1.0f - frontShare;
        const float staticLoad = 0.5f * share * m_desc.chassis.mass * gravity;
        m_wheels[w] = WheelState{};
        m_wheels[w].preload     = staticLoad - sus.springRate * sus.droopTravel;
        m_wheels[w].compression = sus.droopTravel;
    }
    m_gear = m_pendingGear = 1;
    m_shiftTimer  = 0.0f;
    m_engineOmega = m_desc.engine.idleRpm / k_rpmPerRad;
    m_limiterCut  = false;
    m_tcFactor    = 1.0f;
    m_absFactor   = { 1.0f, 1.0f, 1.0f, 1.0f };
    m_prevVelocity = {};
    m_telemetry = VehicleTelemetry{};
}

void Vehicle::Reset(XMFLOAT3 groundPosition, float yawDeg)
{
    if (!m_world)
        return;
    m_transform->position = groundPosition;
    m_transform->eulerDeg = { 0.0f, yawDeg, 0.0f };
    m_rigidBody->velocity = {};
    m_world->SetAngularVelocity(m_body, {});
    ResetState();
}

void Vehicle::ShiftUp()
{
    const int top = static_cast<int>(m_desc.gearbox.ratios.size());
    SelectGear((std::min)((m_shiftTimer > 0.0f ? m_pendingGear : m_gear) + 1, top));
}

void Vehicle::ShiftDown()
{
    SelectGear((std::max)((m_shiftTimer > 0.0f ? m_pendingGear : m_gear) - 1, -1));
}

void Vehicle::SelectGear(int gear)
{
    const int top = static_cast<int>(m_desc.gearbox.ratios.size());
    gear = std::clamp(gear, -1, top);
    if (gear == (m_shiftTimer > 0.0f ? m_pendingGear : m_gear))
        return;
    m_pendingGear = gear;
    m_shiftTimer  = m_desc.gearbox.shiftTime;
}

float Vehicle::GearRatio(int gear) const
{
    const auto& gb = m_desc.gearbox;
    if (gear == 0) return 0.0f;
    if (gear < 0)  return -gb.reverseRatio * gb.finalDrive;
    return gb.ratios[static_cast<size_t>(gear - 1)] * gb.finalDrive;
}

float Vehicle::EngineTorque(float omega, float throttle) const
{
    const auto& e = m_desc.engine;
    const float rpm = omega * k_rpmPerRad;
    const float drive = SampleCurve(e.torqueCurve, rpm);
    const float friction = e.frictionTorque + e.frictionPerRad * (std::max)(omega, 0.0f);
    // The curve is the net full-throttle output; closed throttle leaves the
    // pumping/friction losses (engine braking).
    return throttle * drive - (1.0f - throttle) * friction;
}

void Vehicle::UpdateGearbox(float dt, float drivelineRpm)
{
    if (AdvanceShiftTimer(dt))
        return;
    m_autoShiftHold = (std::max)(m_autoShiftHold - dt, 0.0f);
    if (m_autoShiftHold > 0.0f)
        return;
    TryAutomaticShift(drivelineRpm);
}

bool Vehicle::AdvanceShiftTimer(float dt)
{
    if (m_shiftTimer <= 0.0f)
        return false;
    m_shiftTimer -= dt;
    if (m_shiftTimer > 0.0f)
        return true;
    m_shiftTimer = 0.0f;
    m_gear = m_pendingGear;
    m_autoShiftHold = k_autoShiftHold;
    return true;
}

void Vehicle::TryAutomaticShift(float drivelineRpm)
{
    if (!m_automatic || m_gear < 1)
        return;
    const auto& gb = m_desc.gearbox;
    const int top = static_cast<int>(gb.ratios.size());
    if (drivelineRpm > gb.autoUpshiftRpm)
    {
        if (m_gear < top)
            SelectGear(m_gear + 1);
        return;
    }
    if (CanDownshift(drivelineRpm))
        SelectGear(m_gear - 1);
}

bool Vehicle::CanDownshift(float drivelineRpm) const
{
    if (m_gear <= 1 || drivelineRpm >= m_desc.gearbox.autoDownshiftRpm)
        return false;
    const float lowerRpm = drivelineRpm * GearRatio(m_gear - 1) / GearRatio(m_gear);
    return lowerRpm < m_desc.gearbox.autoUpshiftRpm - k_shiftMarginRpm;
}

void Vehicle::ComputeSteering()
{
    const auto& w = m_desc.wheels;
    for (int axle = 0; axle < 2; ++axle)
    {
        const auto& def = axle == 0 ? m_desc.front : m_desc.rear;
        const int left  = axle == 0 ? VehicleDesc::FrontLeft : VehicleDesc::RearLeft;
        const int right = left + 1;
        const float delta = std::clamp(m_input.steer, -1.0f, 1.0f) * XMConvertToRadians(def.maxSteerAngleDeg);
        m_wheels[left].steer = m_wheels[right].steer = delta;
        if (fabsf(delta) < 1e-4f || def.ackermann <= 0.0f)
            continue;
        // Ackermann: both wheels turn about a common center on the other
        // axle's line; the inner wheel steers more.
        const float wheelbase = fabsf(0.5f * (w[0].center.z + w[1].center.z) -
                                      0.5f * (w[2].center.z + w[3].center.z));
        const float track = fabsf(w[right].center.x - w[left].center.x);
        const float radius = wheelbase / tanf(fabsf(delta));
        const float inner = atanf(wheelbase / (std::max)(radius - 0.5f * track, 0.01f));
        const float outer = atanf(wheelbase / (radius + 0.5f * track));
        const float sign = delta > 0.0f ? 1.0f : -1.0f;
        // Turning right (+): the right wheel is inside.
        const float rightAngle = sign * (sign > 0.0f ? inner : outer);
        const float leftAngle  = sign * (sign > 0.0f ? outer : inner);
        m_wheels[left].steer  = delta + def.ackermann * (leftAngle - delta);
        m_wheels[right].steer = delta + def.ackermann * (rightAngle - delta);
    }
}

void Vehicle::OnPhysicsStep(PhysicsWorld& world, float dt)
{
    const PhysicsWorld::BodyState state = world.GetBodyState(m_body);
    const XMVECTOR position = Load(state.position);
    const XMVECTOR rotation = XMLoadFloat4(&state.rotation);
    const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), rotation);
    const XMVECTOR up    = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), rotation);
    const XMVECTOR fwd   = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), rotation);

    StepState step;
    PrepareDrivetrain(dt, step);
    SampleContacts(world, position, rotation, right, up, fwd);
    ApplySuspensionForces(world, up);
    SolveWheelDynamics(dt, step);
    ApplyTireForces(world, up);
    ApplyAerodynamics(world, position, rotation);
    UpdateTelemetry(state, dt, right, up, fwd, step);
}

void Vehicle::PrepareDrivetrain(float dt, StepState& out)
{
    out.drivenLeft  = m_desc.front.driven ? VehicleDesc::FrontLeft : VehicleDesc::RearLeft;
    out.drivenRight = out.drivenLeft + 1;
    const float drivelineOmega =
        0.5f * (m_wheels[out.drivenLeft].omega + m_wheels[out.drivenRight].omega);
    UpdateGearbox(dt, fabsf(drivelineOmega * GearRatio(m_gear)) * k_rpmPerRad);

    const float engineRpm = m_engineOmega * k_rpmPerRad;
    float throttle = RevLimitedThrottle(engineRpm);
    throttle *= TractionControlFactor(dt, out.drivenLeft, out.drivenRight);
    UpdateAbsFactors(dt);
    throttle = IdleThrottle(throttle, engineRpm);

    out.shifting = m_shiftTimer > 0.0f;
    out.ratio    = out.shifting ? 0.0f : GearRatio(m_gear);
    out.throttle = throttle;
    out.clutch   = LaunchClutch(engineRpm, out.ratio);
    out.brake    = std::clamp(m_input.brake, 0.0f, 1.0f);
    out.handbrake = std::clamp(m_input.handbrake, 0.0f, 1.0f);
    out.drivenInertia = m_desc.AxleOf(out.drivenLeft).wheelInertia;
    ComputeSteering();
}

float Vehicle::RevLimitedThrottle(float engineRpm)
{
    const float limiter = m_desc.engine.limiterRpm;
    if (engineRpm >= limiter)
        m_limiterCut = true;
    else if (engineRpm < limiter - 300.0f)
        m_limiterCut = false;
    return m_limiterCut ? 0.0f : std::clamp(m_input.throttle, 0.0f, 1.0f);
}

float Vehicle::TractionControlFactor(float dt, int drivenLeft, int drivenRight)
{
    // Trims throttle when a driven wheel spins past the tire's peak slip.
    float target = 1.0f;
    for (int w : { drivenLeft, drivenRight })
    {
        const float peak = m_tires[w].PeakSlipRatio(m_wheels[w].load, PatchSpeed(w));
        target = (std::min)(target, AidTarget(m_tractionControl, m_wheels[w].slipRatio, peak));
    }
    m_tcFactor += (target - m_tcFactor) * (std::min)(1.0f, dt * k_aidResponse);
    return m_tcFactor;
}

void Vehicle::UpdateAbsFactors(float dt)
{
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        const float peak = m_tires[w].PeakSlipRatio(m_wheels[w].load, PatchSpeed(w));
        const float target = AidTarget(m_abs, -m_wheels[w].slipRatio, peak);
        m_absFactor[w] += (target - m_absFactor[w]) * (std::min)(1.0f, dt * k_aidResponse);
    }
}

float Vehicle::PatchSpeed(int wheel) const
{
    return hypotf(m_contacts[wheel].vx, m_contacts[wheel].vy);
}

float Vehicle::IdleThrottle(float throttle, float engineRpm) const
{
    // Keeps the engine running below idle speed.
    if (engineRpm >= m_desc.engine.idleRpm)
        return throttle;
    const float idle = std::clamp((m_desc.engine.idleRpm - engineRpm) / 300.0f, 0.0f, 1.0f);
    return (std::max)(throttle, idle * 0.3f);
}

float Vehicle::LaunchClutch(float engineRpm, float ratio) const
{
    // Engagement follows engine speed, so the car neither creeps at idle nor
    // stalls when braking to a stop in gear.
    if (ratio == 0.0f)
        return 0.0f;
    const float idle = m_desc.engine.idleRpm;
    return Smoothstep(idle + 500.0f, idle + 4000.0f, engineRpm);
}

void Vehicle::SampleContacts(PhysicsWorld& world, XMVECTOR position, XMVECTOR rotation,
                             XMVECTOR right, XMVECTOR up, XMVECTOR fwd)
{
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        WheelState& ws = m_wheels[w];
        WheelContact& c = m_contacts[w];
        const auto& wheel = m_desc.wheels[w];
        const auto& sus = m_desc.AxleOf(w).suspension;
        XMFLOAT3 top = wheel.center;
        top.y += sus.bumpTravel;
        const XMVECTOR origin = ToWorld(position, rotation, top);
        const float travel = sus.bumpTravel + sus.droopTravel;

        Ray ray;
        ray.origin = Store(origin);
        ray.direction = Store(XMVectorNegate(up));
        ws.contact = world.Raycast(ray, c.hit, travel + wheel.radius, m_body);
        if (!ws.contact)
        {
            ws.compression = 0.0f;
            continue;
        }
        const float length = c.hit.t - wheel.radius;
        ws.compression = travel - length;
        c.hub   = XMVectorSubtract(origin, XMVectorScale(up, length));
        c.point = Load(c.hit.point);
        const XMVECTOR relative = XMVectorSubtract(Load(world.GetPointVelocity(m_body, Store(c.hub))),
                                                   Load(c.hit.surfaceVelocity));
        const float compressionRate = -Dot(relative, up);

        float force = ws.preload + sus.springRate * ws.compression;
        force += (compressionRate > 0.0f ? sus.bumpDamping : sus.reboundDamping) * compressionRate;
        if (ws.compression > travel)
            force += sus.bumpStopRate * (ws.compression - travel);
        m_suspensionForce[w] = force;

        const XMVECTOR n = Load(c.hit.normal);
        const float s = sinf(ws.steer), co = cosf(ws.steer);
        c.forward = OnPlane(XMVectorAdd(XMVectorScale(fwd, co), XMVectorScale(right, s)), n);
        c.lateral = OnPlane(XMVectorSubtract(XMVectorScale(right, co), XMVectorScale(fwd, s)), n);
        c.vx = Dot(relative, c.forward);
        c.vy = Dot(relative, c.lateral);
        // Negative setup camber leans the top inboard; roll and banking add to it.
        const float setup = XMConvertToRadians(m_desc.AxleOf(w).camberDeg);
        ws.camber = setup * (w % 2 == 1 ? 1.0f : -1.0f) +
                    asinf(std::clamp(Dot(up, c.lateral), -1.0f, 1.0f));
    }
}

void Vehicle::ApplySuspensionForces(PhysicsWorld& world, XMVECTOR up)
{
    for (int left : { VehicleDesc::FrontLeft, VehicleDesc::RearLeft })
    {
        const int r = left + 1;
        const float bar = m_desc.AxleOf(left).suspension.antiRollRate *
                          (m_wheels[left].compression - m_wheels[r].compression);
        if (m_wheels[left].contact) m_suspensionForce[left] += bar;
        if (m_wheels[r].contact)    m_suspensionForce[r]    -= bar;
    }
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        WheelState& ws = m_wheels[w];
        ws.load = ws.contact ? (std::max)(m_suspensionForce[w], 0.0f) : 0.0f;
        if (ws.load <= 0.0f)
            continue;
        world.AddForceAtPoint(m_body, Store(XMVectorScale(up, ws.load)), Store(m_contacts[w].hub));
        if (m_contacts[w].hit.dynamic)
            world.AddForceAtPoint(m_contacts[w].hit.body, Store(XMVectorScale(up, -ws.load)),
                                  Store(m_contacts[w].point));
    }
}

void Vehicle::SolveWheelDynamics(float dt, const StepState& step)
{
    ComputeLowSpeedDamping();
    m_fxSum.fill(0.0f);
    m_fySum.fill(0.0f);
    const float h = dt / k_subSteps;
    float engineOmega = m_engineOmega;
    for (int substep = 0; substep < k_subSteps; ++substep)
        engineOmega = IntegrateSubstep(h, step, engineOmega);
    m_engineOmega = engineOmega;
}

void Vehicle::ComputeLowSpeedDamping()
{
    // Below walking pace the lateral relaxation state acts as a spring;
    // near-critical damping against the corner mass keeps a stopped car from
    // oscillating sideways on its tires. The fade uses the full contact-patch
    // speed: a car sliding sideways has no forward wheel speed but is not slow.
    m_latDamping.fill(0.0f);
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        const WheelState& ws = m_wheels[w];
        if (!ws.contact || ws.load <= 0.0f)
            continue;
        const auto& axle = m_desc.AxleOf(w);
        const float patchSpeed = PatchSpeed(w);
        const float fade = 1.0f - (std::min)(patchSpeed / k_lowSpeed, 1.0f);
        const float kLat = m_tires[w].CorneringStiffness(ws.load, patchSpeed) / axle.relaxationLengthLat;
        m_latDamping[w] = fade * 2.0f * k_dampingRatio * sqrtf(kLat * 0.25f * m_desc.chassis.mass);
    }
}

void Vehicle::ComputeWheelTorques(const StepState& step)
{
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        WheelState& ws = m_wheels[w];
        const auto& axle = m_desc.AxleOf(w);
        const float r = m_desc.wheels[w].radius;
        ws.fx = ws.fy = 0.0f;
        if (ws.contact && ws.load > 0.0f)
        {
            // Longitudinal slip is instantaneous: wheel spin is stiff
            // against it, so it is integrated implicitly below instead of
            // through a lagging relaxation state (which makes a light wheel
            // overshoot after clutch/brake transients).
            const float vd = (std::max)(fabsf(m_contacts[w].vx), k_slipSpeedFloor);
            ws.slipRatio = std::clamp((ws.omega * r - m_contacts[w].vx) / vd, -k_maxSlip, k_maxSlip);
            const TireModel& tire = m_tires[w];
            TireInput in;
            in.slipRatio  = ws.slipRatio;
            in.slipAngle  = atanf(ws.tanSlip);
            in.normalLoad = ws.load;
            in.speed      = PatchSpeed(w);
            in.camber     = ws.camber;
            const TireForces f = tire.Evaluate(in);
            in.slipRatio += k_slopeEpsilon;
            const float slope = (tire.Evaluate(in).longitudinal - f.longitudinal) / k_slopeEpsilon;
            m_forceSlope[w] = (std::max)(slope, 0.0f) * r / vd;
            ws.fx = f.longitudinal;
            // The damper stands in for static friction, so it never pushes
            // the lateral force past the friction limit.
            const float limit = (std::max)(fabsf(f.lateral), tire.PeakLateralForce(ws.load, in.speed));
            ws.fy = std::clamp(f.lateral - m_latDamping[w] * m_contacts[w].vy, -limit, limit);
        }
        m_externalTorque[w] = -ws.fx * r;
        m_brakeTorque[w] = step.brake * axle.maxBrakeTorque * m_absFactor[w] +
                           step.handbrake * axle.handbrakeTorque +
                           m_tires[w].RollingResistanceTorque(ws.load);
    }
}

float Vehicle::ClutchTorqueForSubstep(float h, const StepState& step, float engineOmega,
                                      float engineTorque) const
{
    // The torque that keeps crank and axle speeds locked through this
    // sub-step, unless it exceeds clutch capacity (slip).
    if (step.clutch <= 0.0f)
        return 0.0f;
    const auto& eng = m_desc.engine;
    const auto& gb = m_desc.gearbox;
    const float carrier = 0.5f * (m_wheels[step.drivenLeft].omega + m_wheels[step.drivenRight].omega);
    const float denom = h / eng.inertia +
                        step.ratio * step.ratio * gb.efficiency * h / (2.0f * step.drivenInertia);
    const float locked = (engineOmega - step.ratio * carrier + h * engineTorque / eng.inertia -
                          step.ratio * h * (m_externalTorque[step.drivenLeft] +
                                            m_externalTorque[step.drivenRight]) /
                          (2.0f * step.drivenInertia)) / denom;
    const float capacity = step.clutch * gb.clutchMaxTorque;
    return std::clamp(locked, -capacity, capacity);
}

void Vehicle::DistributeDifferential(float h, const StepState& step, float clutchTorque)
{
    // Clutch-type limited-slip differential: transfers torque from the faster
    // to the slower wheel up to preload + ramp * input torque.
    const auto& gb = m_desc.gearbox;
    const float input = clutchTorque * step.ratio * gb.efficiency;
    const float divergence = (m_wheels[step.drivenLeft].omega - m_wheels[step.drivenRight].omega) +
        h * (m_externalTorque[step.drivenLeft] - m_externalTorque[step.drivenRight]) /
        step.drivenInertia;
    const auto& diff = m_desc.differential;
    const float lockLimit = diff.preload +
        (input >= 0.0f ? diff.powerLock : diff.coastLock) * fabsf(input);
    const float transfer = std::clamp(divergence * step.drivenInertia / (2.0f * h), -lockLimit, lockLimit);
    m_externalTorque[step.drivenLeft]  += 0.5f * input - transfer;
    m_externalTorque[step.drivenRight] += 0.5f * input + transfer;
}

float Vehicle::IntegrateSubstep(float h, const StepState& step, float engineOmega)
{
    ComputeWheelTorques(step);
    const float engineTorque = EngineTorque(engineOmega, step.throttle);
    const float clutchTorque = ClutchTorqueForSubstep(h, step, engineOmega, engineTorque);
    engineOmega = (std::max)(engineOmega + h * (engineTorque - clutchTorque) / m_desc.engine.inertia,
                             0.0f);
    DistributeDifferential(h, step, clutchTorque);
    IntegrateWheelSpeeds(h);
    return engineOmega;
}

void Vehicle::IntegrateWheelSpeeds(float h)
{
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        WheelState& ws = m_wheels[w];
        const auto& axle = m_desc.AxleOf(w);
        const float inertia = axle.wheelInertia;
        const float r = m_desc.wheels[w].radius;
        // Backward Euler with the tire force linearized in wheel speed:
        //   Fx(omega') = Fx + s * (omega' - omega)
        const float deltaOmega = (h * m_externalTorque[w] / inertia) /
                                 (1.0f + h * r * m_forceSlope[w] / inertia);
        ws.omega += deltaOmega;
        const float fx = ws.fx + m_forceSlope[w] * deltaOmega;
        // Brakes are friction: they stop the wheel but never reverse it.
        const float brakeDelta = h * m_brakeTorque[w] / inertia;
        ws.omega = fabsf(ws.omega) <= brakeDelta ? 0.0f
                 : ws.omega - (ws.omega > 0.0f ? brakeDelta : -brakeDelta);
        ws.spinAngle = fmodf(ws.spinAngle + ws.omega * h, XM_2PI);

        if (!ws.contact)
        {
            ws.slipRatio = ws.tanSlip = 0.0f;
            continue;
        }
        // Lateral slip relaxes over the relaxation length (integrated
        // implicitly):  sigma * d(tan a)/dt + |Vx| * tan a = Vy
        const float sy = axle.relaxationLengthLat;
        ws.tanSlip = (ws.tanSlip + h / sy * m_contacts[w].vy) /
                     (1.0f + h * fabsf(m_contacts[w].vx) / sy);
        ws.tanSlip = std::clamp(ws.tanSlip, -k_maxSlip, k_maxSlip);
        m_fxSum[w] += fx;
        m_fySum[w] += ws.fy;
    }
}

void Vehicle::ApplyTireForces(PhysicsWorld& world, XMVECTOR up)
{
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        WheelState& ws = m_wheels[w];
        if (!ws.contact)
            continue;
        ws.fx = m_fxSum[w] / k_subSteps;
        ws.fy = m_fySum[w] / k_subSteps;
        const WheelContact& c = m_contacts[w];
        const XMVECTOR force = XMVectorAdd(XMVectorScale(c.forward, ws.fx),
                                           XMVectorScale(c.lateral, ws.fy));
        // Acting above the patch at the axle's roll center height stands in
        // for the suspension linkage geometry.
        const XMVECTOR apply = XMVectorAdd(c.point,
            XMVectorScale(up, m_desc.AxleOf(w).suspension.rollCenterHeight));
        world.AddForceAtPoint(m_body, Store(force), Store(apply));
        if (c.hit.dynamic)
            world.AddForceAtPoint(c.hit.body, Store(XMVectorNegate(force)), Store(c.point));
    }
}

void Vehicle::ApplyAerodynamics(PhysicsWorld& world, XMVECTOR position, XMVECTOR rotation)
{
    // The aero reference plane follows suspension travel from the static ride heights.
    const AeroParams& aero = m_desc.aero;
    float rideHeight[VehicleDesc::WheelCount];
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
        rideHeight[w] = aero.staticRideHeight[w < VehicleDesc::RearLeft ? 0 : 1] -
                        (m_wheels[w].compression - m_desc.AxleOf(w).suspension.droopTravel);
    AeroInput& in = m_aeroInput;
    in.frontRideHeight = 0.5f * (rideHeight[VehicleDesc::FrontLeft] + rideHeight[VehicleDesc::FrontRight]);
    in.rearRideHeight  = 0.5f * (rideHeight[VehicleDesc::RearLeft] + rideHeight[VehicleDesc::RearRight]);
    in.rollSlope = 0.0f;
    for (int left : { VehicleDesc::FrontLeft, VehicleDesc::RearLeft })
        in.rollSlope += 0.5f * (rideHeight[left + 1] - rideHeight[left]) /
                        (m_desc.wheels[left + 1].center.x - m_desc.wheels[left].center.x);

    XMVECTOR points[AeroElementCount];
    for (int e = 0; e < AeroElementCount; ++e)
    {
        points[e] = ToWorld(position, rotation, AeroElementCenter(aero, e));
        const XMVECTOR velocity = Load(world.GetPointVelocity(m_body, Store(points[e])));
        in.elementVelocity[e] = Store(XMVector3InverseRotate(velocity, rotation));
    }
    m_aeroForces = EvaluateAero(aero, in);
    for (int e = 0; e < AeroElementCount; ++e)
        world.AddForceAtPoint(m_body, Store(XMVector3Rotate(Load(m_aeroForces.elements[e].force), rotation)),
                              Store(points[e]));
}

void Vehicle::UpdateTelemetry(const PhysicsWorld::BodyState& state, float dt,
                              XMVECTOR right, XMVECTOR up, XMVECTOR fwd, const StepState& step)
{
    const XMVECTOR velocity = Load(state.linearVelocity);
    VehicleTelemetry& t = m_telemetry;
    t.speed     = Dot(velocity, fwd);
    t.engineRpm = m_engineOmega * k_rpmPerRad;
    t.gear      = step.shifting ? m_pendingGear : m_gear;
    t.clutch    = step.clutch;
    t.throttle  = step.throttle;
    t.shifting  = step.shifting;
    t.tractionControlActive = m_tcFactor < 0.95f && m_input.throttle > 0.0f;
    const float minAbs = (std::min)((std::min)(m_absFactor[0], m_absFactor[1]),
                                    (std::min)(m_absFactor[2], m_absFactor[3]));
    t.absActive = step.brake > 0.0f && minAbs < 0.95f;
    const XMVECTOR accel = XMVectorScale(XMVectorSubtract(velocity, Load(m_prevVelocity)), 1.0f / dt);
    t.acceleration = { Dot(accel, right), Dot(accel, up), Dot(accel, fwd) };
    m_prevVelocity = state.linearVelocity;
    t.downforceFront  = m_aeroForces.downforceFront;
    t.downforceRear   = m_aeroForces.downforceRear;
    t.drag            = m_aeroForces.drag;
    t.rideHeightFront = m_aeroInput.frontRideHeight;
    t.rideHeightRear  = m_aeroInput.rearRideHeight;
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        const WheelState& ws = m_wheels[w];
        WheelTelemetry& wt = t.wheels[w];
        wt.contact           = ws.contact;
        wt.compression       = ws.compression;
        wt.load              = ws.load;
        wt.slipRatio         = ws.slipRatio;
        wt.slipAngle         = atanf(ws.tanSlip);
        wt.longitudinalForce = ws.fx;
        wt.lateralForce      = ws.fy;
        wt.angularVelocity   = ws.omega;
        wt.steerAngle        = ws.steer;
        wt.camber            = ws.camber;
    }
}

XMMATRIX Vehicle::GetSpindleLocalMatrix(int wheel) const
{
    const WheelState& ws = m_wheels[wheel];
    const auto& sus = m_desc.AxleOf(wheel).suspension;
    XMFLOAT3 center = m_desc.wheels[wheel].center;
    center.y += (std::min)(ws.compression, sus.bumpTravel + sus.droopTravel) - sus.droopTravel;
    return XMMatrixRotationY(ws.steer) * XMMatrixTranslation(center.x, center.y, center.z);
}

XMMATRIX Vehicle::GetWheelLocalMatrix(int wheel) const
{
    return XMMatrixRotationX(m_wheels[wheel].spinAngle) * GetSpindleLocalMatrix(wheel);
}

} // namespace SE
