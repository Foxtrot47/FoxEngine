#include "Engine/Physics/Vehicle/AeroModel.h"

#include <algorithm>
#include <cmath>

namespace SE {

using namespace DirectX;

namespace {

constexpr float k_degPerRad = 57.29577951f;

struct State
{
    const AeroParams& params;
    const AeroInput&  input;
    float rake;    // m, rear minus front ride height
    float rakeDeg; // > 0 = nose down

    // Ride height of the reference plane at a vehicle-space (x, z).
    float Height(float x, float z) const
    {
        const float t = (z - params.rearAxleZ) / (params.frontAxleZ - params.rearAxleZ);
        return input.rearRideHeight + (input.frontRideHeight - input.rearRideHeight) * t +
               input.rollSlope * x;
    }

    float RakeValue(const AeroParams::Rake& terms) const
    {
        const AeroParams::RakeTerm& term = rakeDeg >= 0.0f ? terms.noseDown : terms.noseUp;
        const float t = (std::min)(fabsf(rakeDeg), term.maxDeg);
        return term.c1 * t + term.c2 * t * t;
    }
};

float YawFactor(const AeroParams::Yaw& yaw, const XMFLOAT3& v)
{
    const float beta = atan2f(fabsf(v.x), v.z) * k_degPerRad;
    if (beta <= yaw.peakDeg)
        return yaw.peakDeg > 0.0f ? 1.0f + (yaw.peakMult - 1.0f) * beta / yaw.peakDeg : 1.0f;
    const float past = (std::min)(beta - yaw.peakDeg, 90.0f) / k_degPerRad;
    return yaw.peakMult * (1.0f - yaw.sideways * sinf(past));
}

float SampleLut(const AeroParams::FrontWing& wing, float height)
{
    const auto& lut = wing.heightLut;
    const float last = static_cast<float>(lut.size() - 1);
    const float f = std::clamp((height - wing.heightRange[0]) / (wing.heightRange[1] - wing.heightRange[0]) * last,
                               0.0f, last);
    const size_t i = (std::min)(static_cast<size_t>(f), lut.size() - 2);
    const float t = f - static_cast<float>(i);
    return lut[i] + (lut[i + 1] - lut[i]) * t;
}

float DiffuserLift(const State& s)
{
    const AeroParams::Diffuser& d = s.params.diffuser;
    const float hR = std::clamp(s.input.rearRideHeight, 0.0f, d.limits[1]);
    const float hF = std::clamp(s.input.frontRideHeight, 0.0f, d.frontHeight[3]);
    const float dr = std::clamp(s.rake - d.rake[0], -d.limits[2], d.limits[2]);
    float lift = d.lift +
                 d.rearHeight[0] * sqrtf(hR) + d.rearHeight[1] * hR + d.rearHeight[2] * hR * hR +
                 d.frontHeight[0] * sqrtf(hF) + d.frontHeight[1] * hF + d.frontHeight[2] * hF * hF +
                 d.rake[1] * dr + d.rake[2] * dr * dr;

    // Stall when the undertray (corners at the axles) nears the ground.
    const float w = d.undertrayHalfWidth;
    const float heights[4] = { s.Height(-w, s.params.frontAxleZ), s.Height(w, s.params.frontAxleZ),
                               s.Height(-w, s.params.rearAxleZ),  s.Height(w, s.params.rearAxleZ) };
    const float minHeight = (std::min)((std::min)(heights[0], heights[1]), (std::min)(heights[2], heights[3]));
    const float mean = 0.25f * (heights[0] + heights[1] + heights[2] + heights[3]);
    const float hs = minHeight + (mean - minHeight) * d.stall[0];
    if (d.limits[0] > 0.0f && hs < d.limits[0])
        lift *= 1.0f - d.stall[1] * (1.0f - (std::max)(hs, 0.0f) / d.limits[0]);
    return lift;
}

XMFLOAT3 Directional(const AeroParams::Directional& dir, const XMFLOAT3& v, float rho)
{
    // Each face sees the velocity component along its direction.
    XMVECTOR force = XMVectorZero();
    auto add = [&](float component, const XMFLOAT3& coefficient) {
        const float c = (std::max)(component, 0.0f);
        force = XMVectorAdd(force, XMVectorScale(XMLoadFloat3(&coefficient), 0.5f * rho * c * c));
    };
    add(-v.x, dir.left);
    add(v.x, dir.right);
    add(v.y, dir.up);
    add(-v.y, dir.down);
    add(-v.z, dir.back);
    XMFLOAT3 out;
    XMStoreFloat3(&out, force);
    return out;
}

} // namespace

XMFLOAT3 AeroElementCenter(const AeroParams& params, int element)
{
    switch (element)
    {
    case AeroFrontWing: return params.frontWing.center;
    case AeroRearWing:  return params.rearWing.center;
    case AeroBody:      return params.body.center;
    default:            return params.diffuser.center;
    }
}

AeroForces EvaluateAero(const AeroParams& params, const AeroInput& input)
{
    const float wheelbase = params.frontAxleZ - params.rearAxleZ;
    const float rake = input.rearRideHeight - input.frontRideHeight;
    const State s{ params, input, rake, atanf(rake / wheelbase) * k_degPerRad };

    float lift[AeroElementCount], drag[AeroElementCount];
    const auto& fw = params.frontWing;
    lift[AeroFrontWing] = fw.lift * SampleLut(fw, s.Height(0.0f, fw.center.z)) + s.RakeValue(fw.rakeLift);
    drag[AeroFrontWing] = fw.drag + s.RakeValue(fw.rakeDrag);

    const auto& rw = params.rearWing;
    lift[AeroRearWing] = rw.lift + s.RakeValue(rw.rakeLift);
    drag[AeroRearWing] = rw.drag + s.RakeValue(rw.rakeDrag);

    const auto& body = params.body;
    const float meanHeight = 0.5f * (input.frontRideHeight + input.rearRideHeight);
    lift[AeroBody] = body.lift + s.RakeValue(body.rakeLift);
    drag[AeroBody] = body.drag +
                     body.dragHeightAvg * std::clamp(meanHeight, 0.0f, body.maxHeight) +
                     body.dragHeightDiff * std::clamp(rake, -body.maxHeight, body.maxHeight) +
                     s.RakeValue(body.rakeDrag);

    lift[AeroDiffuser] = DiffuserLift(s);
    drag[AeroDiffuser] = 0.0f;

    const AeroParams::Yaw* yaw[AeroElementCount] = { &fw.yaw, &rw.yaw, &body.yaw, &params.diffuser.yaw };
    const AeroParams::Directional none;
    const AeroParams::Directional* directional[AeroElementCount] = {
        &fw.directional, &rw.directional, &body.directional, &none };

    AeroForces out;
    const float rho = params.airDensity;
    for (int e = 0; e < AeroElementCount; ++e)
    {
        const XMFLOAT3& v = input.elementVelocity[e];
        XMFLOAT3 force = Directional(*directional[e], v, rho);
        if (v.z > 0.0f)
        {
            const float q = 0.5f * rho * v.z * v.z;
            force.y += q * lift[e] * YawFactor(*yaw[e], v);
            force.z -= q * drag[e];
        }
        AeroElementForce& element = out.elements[e];
        element.point = AeroElementCenter(params, e);
        element.force = force;
        out.drag -= force.z;
        const float frontShare = (element.point.z - params.rearAxleZ) / wheelbase;
        out.downforceFront -= force.y * frontShare;
        out.downforceRear  -= force.y * (1.0f - frontShare);
    }
    return out;
}

} // namespace SE
