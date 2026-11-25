#pragma once
#include <array>
#include <vector>
#include <DirectXMath.h>

namespace SE {

// Aero map of one car (aero.json, generated from the rFactor 2 .hdv). All
// coefficients are force areas (m^2, multiplied by the dynamic pressure);
// lift < 0 is downforce. Positions are vehicle space.
struct AeroParams
{
    // c1 * t + c2 * t^2 with t = min(|rake deg|, maxDeg); rake is positive
    // nose down (rear ride height above front).
    struct RakeTerm { float c1 = 0.0f, c2 = 0.0f, maxDeg = 0.0f; };
    struct Rake { RakeTerm noseDown, noseUp; };

    // Lift multiplier against yaw: linear to peakMult at peakDeg, then falls
    // by sideways * sin(yaw past the peak). The defaults leave lift unchanged.
    struct Yaw { float sideways = 0.0f, peakDeg = 0.0f, peakMult = 1.0f; };

    // Force coefficients for air approaching from each direction (the
    // component of the element's velocity along it), vehicle space.
    struct Directional { DirectX::XMFLOAT3 left = {}, right = {}, up = {}, down = {}, back = {}; };

    struct Element
    {
        DirectX::XMFLOAT3 center = {};
        float drag = 0.0f;
        float lift = 0.0f;
        Rake rakeDrag, rakeLift;
        Yaw yaw;
        Directional directional;
    };

    struct FrontWing : Element
    {
        float heightRange[2] = { 0.0f, 0.1f }; // m of ride height under the wing
        std::vector<float> heightLut;          // lift multiplier over heightRange
    };

    struct Body : Element
    {
        float dragHeightAvg  = 0.0f; // drag per m of mean ride height
        float dragHeightDiff = 0.0f; // drag per m of rake
        float maxHeight      = 0.0f; // clamp for both
    };

    struct Diffuser
    {
        DirectX::XMFLOAT3 center = {};
        float lift = 0.0f;
        Yaw   yaw;
        float rearHeight[3]  = {}; // sqrt, linear, square terms of the rear ride height
        float frontHeight[4] = {}; // sqrt, linear, square terms of the front ride height; max height
        float rake[3]        = {}; // rake offset, linear, square terms
        float limits[3]      = {}; // stall height, max rear height, max rake deviation
        float stall[2]       = {}; // min-to-mean undertray height blend, lift lost at full stall
        float undertrayHalfWidth = 0.0f;
    };

    float airDensity = 1.225f;            // kg/m^3
    float staticRideHeight[2] = {};       // front, rear reference plane height at rest, m
    float frontAxleZ = 1.0f, rearAxleZ = -1.0f;
    FrontWing frontWing;
    Element   rearWing;
    Body      body;
    Diffuser  diffuser;
};

enum AeroElementIndex { AeroFrontWing = 0, AeroRearWing = 1, AeroBody = 2, AeroDiffuser = 3, AeroElementCount = 4 };

struct AeroInput
{
    float frontRideHeight = 0.0f; // m, reference plane above ground at the front axle
    float rearRideHeight  = 0.0f;
    float rollSlope       = 0.0f; // d(ride height)/dX, m per m
    // Element velocity through still air, vehicle space.
    std::array<DirectX::XMFLOAT3, AeroElementCount> elementVelocity = {};
};

struct AeroElementForce
{
    DirectX::XMFLOAT3 point = {}; // application point, vehicle space
    DirectX::XMFLOAT3 force = {}; // vehicle space, N
};

struct AeroForces
{
    std::array<AeroElementForce, AeroElementCount> elements;
    float drag = 0.0f;           // N against +Z
    float downforceFront = 0.0f; // -Y force split to the axles by lever rule, N
    float downforceRear  = 0.0f;
};

DirectX::XMFLOAT3 AeroElementCenter(const AeroParams& params, int element);

// Pure function of the state; no world access.
AeroForces EvaluateAero(const AeroParams& params, const AeroInput& input);

} // namespace SE
