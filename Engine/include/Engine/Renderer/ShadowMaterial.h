#pragma once
#include <d3d11.h>

namespace SE {

// Per-submesh inputs for the alpha-tested shadow passes.
struct ShadowSubMesh
{
    ID3D11ShaderResourceView* albedo = nullptr;
    float alphaCutoff = 0.0f; // 0 = no alpha test
    bool cast = true;         // false for transparent materials
    bool billboard = false;   // cylindrical billboard: the card turns to face the light
};

// ShadowDepth.hlsl / PointShadowDepth.hlsl ShadowMaterialCB (b1, VS and PS).
struct ShadowMaterialCBData
{
    float alphaCutoff;
    float billboard;  // 1: expand the billboard card toward facing
    float _pad[2];
    float facing[4];  // w = 0: xyz world direction toward the light; w = 1: xyz light world position
};

} // namespace SE
