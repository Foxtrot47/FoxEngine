#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/ConstantBuffer.h"
#include "Engine/Renderer/FullscreenQuad.h"
#include "Engine/Renderer/RenderTarget.h"

using Microsoft::WRL::ComPtr;

namespace SE {

// Temporal Anti-Aliasing. Consumes the HDR scene, depth, and the velocity
// buffer written by the forward pass; accumulates a history target and writes
// the resolved image back into the scene RT.
class TAA
{
public:
    bool Init(ID3D11Device* device, ShaderLibrary& shaders, uint32_t width, uint32_t height);
    void Resize(ID3D11Device* device, uint32_t width, uint32_t height);

    // Halton (2,3) sub-pixel jitter offset in NDC units for the current frame.
    // Call once per frame before building the projection matrix.
    DirectX::XMFLOAT2 NextJitter(uint32_t width, uint32_t height);

    void Apply(ID3D11DeviceContext* ctx, RenderTarget& sceneRT,
               ID3D11ShaderResourceView* depthSRV,
               ID3D11ShaderResourceView* velocitySRV,
               DirectX::XMMATRIX invViewProj,
               DirectX::XMMATRIX prevViewProj);

    // Discard accumulated history (scene load, teleport, toggle).
    void ResetHistory() { m_historyValid = false; }

    bool  enabled     = true;
    float blendFactor = 0.93f; // history weight
    float clampGamma  = 1.25f; // variance clip width (stddevs)

private:
    struct CBData
    {
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT4X4 prevViewProj;
        float rcpFrameX;
        float rcpFrameY;
        float blendFactor;
        float reset;
        float clampGamma;
        float _pad[3];
    };

    const ShaderPermutation*   m_perm = nullptr;
    ConstantBuffer<CBData>     m_cb;
    FullscreenQuad             m_quad;
    RenderTarget               m_resolveRT;
    RenderTarget               m_historyRT;
    ComPtr<ID3D11SamplerState> m_linearSampler;
    uint32_t                   m_jitterIndex = 0;
    bool                       m_historyValid = false;
};

} // namespace SE
