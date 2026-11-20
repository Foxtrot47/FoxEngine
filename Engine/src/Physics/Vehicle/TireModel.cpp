#include "Engine/Physics/Vehicle/TireModel.h"

#include <algorithm>
#include <cmath>

namespace SE {

namespace {

// Magic Formula curve with unit peak (D = 1).
float Shape(const MagicFormulaParams::Axis& a, float slip)
{
    const float bs = a.B * slip;
    return sinf(a.C * atanf(bs - a.E * (bs - atanf(bs))));
}

// Slip of the first maximum of the shape curve, by golden-section search on
// (0, upper]. The curve is unimodal there for physical parameter ranges.
float FindPeakSlip(const MagicFormulaParams::Axis& a, float upper)
{
    constexpr float k_invPhi = 0.6180339887f;
    float lo = 0.0f, hi = upper;
    float x1 = hi - k_invPhi * (hi - lo), x2 = lo + k_invPhi * (hi - lo);
    float f1 = Shape(a, x1), f2 = Shape(a, x2);
    for (int i = 0; i < 80; ++i)
    {
        if (f1 < f2) { lo = x1; x1 = x2; f1 = f2; x2 = lo + k_invPhi * (hi - lo); f2 = Shape(a, x2); }
        else         { hi = x2; x2 = x1; f2 = f1; x1 = hi - k_invPhi * (hi - lo); f1 = Shape(a, x1); }
    }
    return 0.5f * (lo + hi);
}

} // namespace

MagicFormulaTire::MagicFormulaTire(const MagicFormulaParams& params)
    : m_params(params)
{
    m_peakSlipRatio = FindPeakSlip(params.longitudinal, 1.0f);
    m_peakSlipAngle = FindPeakSlip(params.lateral, 1.0f);
}

TireForces MagicFormulaTire::Evaluate(const TireInput& input) const
{
    TireForces out;
    const float fz = input.normalLoad;
    if (fz <= 0.0f)
        return out;

    const float dfz = (fz - m_params.nominalLoad) / m_params.nominalLoad;
    const float loadScale = (std::max)(1.0f + m_params.loadSensitivity * dfz, 0.1f);
    const float dx = m_params.longitudinal.mu * loadScale * fz;
    const float dy = m_params.lateral.mu * loadScale * fz;

    // Similarity method: express each slip as a fraction of its peak, then
    // evaluate both pure-slip curves at the combined magnitude.
    const float sx = input.slipRatio / m_peakSlipRatio;
    const float sy = input.slipAngle / m_peakSlipAngle;
    const float rho = sqrtf(sx * sx + sy * sy);
    if (rho < 1e-9f)
        return out;
    out.longitudinal = (sx / rho) * dx * Shape(m_params.longitudinal, rho * m_peakSlipRatio);
    out.lateral      = -(sy / rho) * dy * Shape(m_params.lateral, rho * m_peakSlipAngle);
    return out;
}

} // namespace SE
