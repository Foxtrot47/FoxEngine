#pragma once
#include <d3d11.h>

namespace SE {

// Per-submesh inputs for the alpha-tested shadow passes.
struct ShadowSubMesh
{
    ID3D11ShaderResourceView* albedo = nullptr;
    float alphaCutoff = 0.0f; // 0 = no alpha test
    bool cast = true;         // false for transparent materials
};

struct ShadowMaterialCBData
{
    float alphaCutoff;
    float _pad[3];
};

} // namespace SE
