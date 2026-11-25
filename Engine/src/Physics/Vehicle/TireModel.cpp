#include "Engine/Physics/Vehicle/TireModel.h"

#include <algorithm>
#include <cmath>

namespace SE {

namespace {

constexpr float k_degPerRad = 57.29577951f;
constexpr float k_minPeakSlip = 1e-4f;

} // namespace

float TireLoadSensitivity(const float (&loadSens)[3], float load)
{
    const float a = loadSens[0], m = loadSens[1], fmax = loadSens[2];
    if (load >= fmax)
        return m;
    // Quadratic through g(0) = 1 with slope a that reaches m at Fmax.
    const float c = (m - 1.0f - a * fmax) / (fmax * fmax);
    return 1.0f + a * load + c * load * load;
}

TireModel::TireModel(const TireParams& params)
    : m_params(params)
    , m_lateral(Prepare(params.lateralCurve))
    , m_traction(Prepare(params.tractionCurve))
    , m_braking(Prepare(params.brakingCurve))
{
}

TireModel::Curve TireModel::Prepare(const TireCurve& source)
{
    Curve curve;
    curve.step    = source.step;
    curve.dropoff = source.dropoff;
    curve.data    = source.data;
    const auto peak = std::max_element(curve.data.begin(), curve.data.end());
    const float invMax = 1.0f / *peak;
    curve.peakX = source.step * static_cast<float>(peak - curve.data.begin());
    for (float& value : curve.data)
        value *= invMax;
    return curve;
}

float TireModel::Spline(const Curve& curve, float x)
{
    // Catmull-Rom with the end samples repeated; constant past the last sample.
    const auto& d = curve.data;
    const size_t last = d.size() - 1;
    const float fi = (std::max)(x, 0.0f) / curve.step;
    if (fi >= static_cast<float>(last))
        return d[last];
    const size_t i = static_cast<size_t>(fi);
    const float t = fi - static_cast<float>(i);
    const float p0 = d[i > 0 ? i - 1 : 0];
    const float p1 = d[i];
    const float p2 = d[i + 1];
    const float p3 = d[(std::min)(i + 2, last)];
    return 0.5f * (2.0f * p1 + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                   (3.0f * (p1 - p2) + p3 - p0) * t * t * t);
}

float TireModel::Lookup(const Curve& curve, float s, float sp)
{
    // The curve peak is rescaled onto sp; the dropoff exponent shapes the falloff beyond it.
    const float scale = curve.peakX / sp;
    const float u = s <= sp ? s * scale
                            : curve.peakX + (s - sp) * powf(scale, curve.dropoff);
    return Spline(curve, u);
}

float TireModel::SpeedGrip(float speed) const
{
    const float half = m_params.speedHalfGrip;
    return half > 0.0f ? 1.0f / (1.0f + fabsf(speed) / half) : 1.0f;
}

float TireModel::PeakSlip(const TireAxisParams& axis, float load, float speed) const
{
    const float l = load + m_params.speedLoadEquivalency * fabsf(speed);
    const float p0 = axis.peakSlip[0], p1 = axis.peakSlip[1], lref = axis.peakSlip[2];
    return (std::max)(p0 + (p1 - p0) * std::clamp(l / lref, 0.0f, 1.0f), k_minPeakSlip);
}

TireForces TireModel::Evaluate(const TireInput& input) const
{
    TireForces out;
    const float fz = input.normalLoad;
    if (fz <= 0.0f)
        return out;

    const float spx = PeakSlip(m_params.longitudinal, fz, input.speed);
    const float spy = PeakSlip(m_params.lateral, fz, input.speed);
    // TBC lateral slip is the sine of the slip angle.
    const float nx = input.slipRatio / spx;
    const float ny = sinf(input.slipAngle) / spy;
    const float rho = sqrtf(nx * nx + ny * ny);
    if (rho < 1e-9f)
        return out;

    // Camber gains grip when the top leans toward the side the force acts on
    // (Fy opposes the slip angle) and mirrors that rise when leaning away.
    float camberLat = 1.0f;
    if (input.slipAngle != 0.0f)
    {
        const float ce = input.camber * k_degPerRad * (input.slipAngle > 0.0f ? -1.0f : 1.0f);
        const float r = ce / m_params.camberPeakDeg;
        const float shape = r >= 0.0f ? 2.0f * r - r * r : (r > -1.0f ? 2.0f * r + r * r : -1.0f);
        camberLat = (std::max)(1.0f + m_params.camberLatGain * shape, 0.5f);
    }
    const float sinCamber = sinf(input.camber);
    const float camberLong = 1.0f - m_params.camberLongLoss * sinCamber * sinCamber;

    const float grip = fz * SpeedGrip(input.speed);
    const float dx = m_params.longitudinal.mu * TireLoadSensitivity(m_params.longitudinal.loadSens, fz) *
                     grip * camberLong;
    const float dy = m_params.lateral.mu * TireLoadSensitivity(m_params.lateral.loadSens, fz) *
                     grip * camberLat;

    // Similarity method: both slips as fractions of their peaks, curves at the combined magnitude.
    const Curve& longCurve = input.slipRatio >= 0.0f ? m_traction : m_braking;
    out.longitudinal = (nx / rho) * dx * Lookup(longCurve, rho * spx, spx);
    out.lateral      = -(ny / rho) * dy * Lookup(m_lateral, rho * spy, spy);
    return out;
}

float TireModel::PeakSlipRatio(float load, float speed) const
{
    return PeakSlip(m_params.longitudinal, (std::max)(load, 0.0f), speed);
}

float TireModel::PeakLateralForce(float load, float speed) const
{
    if (load <= 0.0f)
        return 0.0f;
    return m_params.lateral.mu * TireLoadSensitivity(m_params.lateral.loadSens, load) * load *
           SpeedGrip(speed);
}

float TireModel::CorneringStiffness(float load, float speed) const
{
    // Slope of the first curve interval; the spline's own tangent at zero is halved.
    const float sp = PeakSlip(m_params.lateral, (std::max)(load, 0.0f), speed);
    const float curveSlope = (m_lateral.data[1] - m_lateral.data[0]) / m_lateral.step;
    return PeakLateralForce(load, speed) * curveSlope * m_lateral.peakX / sp;
}

float TireModel::RollingResistanceTorque(float load) const
{
    return load > 0.0f ? m_params.rollingResistance * load / m_params.verticalStiffness : 0.0f;
}

} // namespace SE
