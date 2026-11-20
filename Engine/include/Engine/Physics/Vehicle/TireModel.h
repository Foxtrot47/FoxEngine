#pragma once

namespace SE {

// Tire state in the contact frame. The slip angle is the relaxed (transient)
// value from the wheel's relaxation-length filter; the slip ratio is
// instantaneous (the vehicle solves it implicitly with wheel spin).
struct TireInput
{
    float slipRatio      = 0.0f; // kappa: (omega*r - Vx) / |Vx|, > 0 when driving
    float slipAngle      = 0.0f; // alpha, radians; > 0 when the contact slides to +lateral
    float normalLoad     = 0.0f; // Fz, N (>= 0)
};

struct TireForces
{
    float longitudinal = 0.0f; // Fx along the wheel heading, N
    float lateral      = 0.0f; // Fy along +lateral (opposes positive slip angle), N
};

// Steady-state force model of the contact patch. Wheel spin, slip
// relaxation, rolling resistance and suspension live in the vehicle; a model
// only maps slip + load to forces, so richer formulations (full Magic Formula
// with camber/pressure, thermal grip) can replace this interface's
// implementation without touching the rest of the simulation.
class TireModel
{
public:
    virtual ~TireModel() = default;
    virtual TireForces Evaluate(const TireInput& input) const = 0;
    // Slip ratio of peak longitudinal force; TC/ABS intervene around it.
    virtual float PeakSlipRatio() const = 0;
};

// Pacejka "Magic Formula" per axis:
//   F = D sin(C atan(B s - E (B s - atan(B s)))),  D = mu(Fz) * Fz
// with linear load sensitivity of the peak friction and combined slip by the
// similarity method (slip vector normalized by each axis' peak slip), which
// yields a friction-ellipse envelope.
struct MagicFormulaParams
{
    struct Axis
    {
        float B = 12.0f;  // stiffness factor
        float C = 1.6f;   // shape factor
        float E = 0.0f;   // curvature factor
        float mu = 1.3f;  // peak friction at nominal load
    };
    Axis  longitudinal;
    Axis  lateral;
    float nominalLoad     = 4000.0f; // Fz0, N
    float loadSensitivity = -0.1f;   // d(mu)/mu per unit (Fz - Fz0) / Fz0
};

class MagicFormulaTire final : public TireModel
{
public:
    explicit MagicFormulaTire(const MagicFormulaParams& params);
    TireForces Evaluate(const TireInput& input) const override;
    float PeakSlipRatio() const override { return m_peakSlipRatio; }

private:
    MagicFormulaParams m_params;
    float m_peakSlipRatio = 0.1f;
    float m_peakSlipAngle = 0.1f;
};

} // namespace SE
