#pragma once
#include <vector>

namespace SE {

// rFactor-style slip curve: normalised force against slip sampled at
// x = i * step (Catmull-Rom). Slip is rescaled so the peak lands on the
// load-dependent peak slip.
struct TireCurve
{
    float step    = 0.01f;
    float dropoff = 0.0f; // post-peak slip stretch exponent (DropoffFunction)
    std::vector<float> data;
};

struct TireAxisParams
{
    float mu = 1.0f;                // peak friction (DryLatLong)
    float loadSens[3] = {};         // a, m, Fmax (LoadSensLat/Long)
    float peakSlip[3] = {};         // p0, p1, Lref (LatPeak/LongPeak)
};

// One axle's tire, from tires.json (generated from the car's .tbc compound).
struct TireParams
{
    TireAxisParams lateral, longitudinal;
    TireCurve lateralCurve, tractionCurve, brakingCurve;
    float speedHalfGrip        = 0.0f; // m/s at which grip halves (0 disables)
    float speedLoadEquivalency = 0.0f; // N of peak-slip load per m/s
    float camberPeakDeg   = 1.0f;
    float camberLatGain   = 0.0f;
    float camberLongLoss  = 0.0f;
    float rollingResistance = 0.0f; // N*m per m of tire deflection
    float verticalStiffness = 1.0f; // N/m at the setup pressure
};

// Tire state in the contact frame. The slip angle is the relaxed (transient)
// value from the wheel's relaxation-length filter; the slip ratio is
// instantaneous (the vehicle solves it implicitly with wheel spin).
struct TireInput
{
    float slipRatio  = 0.0f; // kappa: (omega*r - Vx) / |Vx|, > 0 when driving
    float slipAngle  = 0.0f; // alpha, radians; > 0 when the contact slides to +lateral
    float normalLoad = 0.0f; // Fz, N (>= 0)
    float speed      = 0.0f; // contact patch speed, m/s
    float camber     = 0.0f; // inclination to the road, rad; > 0 = top leaning toward +lateral
};

struct TireForces
{
    float longitudinal = 0.0f; // Fx along the wheel heading, N
    float lateral      = 0.0f; // Fy along +lateral (opposes positive slip angle), N
};

// Steady-state slip-curve tire (rFactor 2 .tbc semantics); combined slip by
// the similarity method. Temperature, pressure, wear and aligning torque are
// not modelled.
class TireModel
{
public:
    explicit TireModel(const TireParams& params);

    TireForces Evaluate(const TireInput& input) const;
    // Slip ratio of peak longitudinal force; TC/ABS intervene around it.
    float PeakSlipRatio(float load, float speed) const;
    // dFy/dalpha at zero slip, N/rad (low-speed lateral damping).
    float CorneringStiffness(float load, float speed) const;
    // Pure-lateral peak force without camber, N (damper clamp).
    float PeakLateralForce(float load, float speed) const;
    // Rolling resistance torque from the tire's deflection under load, N*m.
    float RollingResistanceTorque(float load) const;

private:
    struct Curve
    {
        std::vector<float> data;
        float step    = 0.01f;
        float dropoff = 0.0f;
        float peakX   = 0.0f;
    };

    static Curve Prepare(const TireCurve& curve);
    static float Spline(const Curve& curve, float x);
    static float Lookup(const Curve& curve, float s, float sp);

    float SpeedGrip(float speed) const;
    float PeakSlip(const TireAxisParams& axis, float load, float speed) const;

    TireParams m_params;
    Curve m_lateral, m_traction, m_braking;
};

// Peak friction multiplier g(F) of a TBC (a, m, Fmax) triple at load F (N).
float TireLoadSensitivity(const float (&loadSens)[3], float load);

} // namespace SE
