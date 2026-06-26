#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/ConstantBuffer.h"
#include "Engine/Renderer/FullscreenQuad.h"

using Microsoft::WRL::ComPtr;

namespace SE {

class FXAA
{
public:
    bool Init(ID3D11Device* device, ShaderLibrary& shaders);

    // Apply FXAA to the given LDR SRV, outputting to the currently-bound RTV.
    void Apply(ID3D11DeviceContext* ctx,
               ID3D11ShaderResourceView* ldrSRV,
               uint32_t width, uint32_t height);

    bool  enabled         = true;
    float subpixQuality   = 0.75f;
    float edgeThreshold   = 0.166f;
    float edgeThresholdMin = 0.0833f;

private:
    struct CBData
    {
        float rcpFrameX;
        float rcpFrameY;
        float subpixQuality;
        float edgeThreshold;
        float edgeThresholdMin;
        float _pad[3];
    };

    const ShaderPermutation*  m_perm = nullptr;
    ConstantBuffer<CBData>    m_cb;
    FullscreenQuad            m_quad;
};

} // namespace SE
