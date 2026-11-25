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

struct MotionBlurParams
{
    bool  enabled       = false;
    float shutterAngle  = 180.0f;
    float maxBlurPixels = 32.0f;
    int   samples       = 12;
};

// Tile-based velocity motion blur (McGuire 2012 reconstruction filter):
//   Pass 0: tile max velocity (16x16 tiles, R16G16_FLOAT)
//   Pass 1: 3x3 neighbour max velocity
//   Pass 2: depth-aware gather along neighbour-max velocity, copied back into sceneHDR.
class MotionBlur
{
public:
    static constexpr uint32_t k_tileSize = 16;

    bool Init(ID3D11Device* device, ShaderLibrary& shaders,
              uint32_t width, uint32_t height);
    void Resize(ID3D11Device* device, uint32_t width, uint32_t height);
    void Shutdown();

    // Reads from sceneHDR, velocity (render-res UV delta), and depth (render-res),
    // writes the motion-blurred HDR image back into sceneHDR.
    void Apply(ID3D11DeviceContext* ctx, RenderTarget& sceneHDR,
               ID3D11ShaderResourceView* velocity,
               ID3D11ShaderResourceView* depth,
               const MotionBlurParams& params,
               DirectX::XMMATRIX invViewProj,
               DirectX::XMMATRIX prevViewProj,
               float nearZ, float farZ);

private:
    struct MotionBlurCB
    {
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT4X4 prevViewProj;
        float targetSizeX;
        float targetSizeY;
        float rcpTargetSizeX;
        float rcpTargetSizeY;
        float tileSizeX;
        float tileSizeY;
        float shutterScale;
        float maxBlurPixels;
        float nearZ;
        float farZ;
        int   sampleCount;
        float _pad;
    };
    static_assert(sizeof(MotionBlurCB) == 176, "MotionBlurCB must match HLSL layout");

    void initRTs(ID3D11Device* device, uint32_t w, uint32_t h);
    void shutdownRTs();
    void bindDest(ID3D11DeviceContext* ctx, RenderTarget& rt);

    RenderTarget m_tileMaxRT;
    RenderTarget m_neighborMaxRT;
    RenderTarget m_blurRT;

    const ShaderPermutation* m_tileMaxPerm     = nullptr;
    const ShaderPermutation* m_neighborMaxPerm = nullptr;
    const ShaderPermutation* m_gatherPerm      = nullptr;

    ConstantBuffer<MotionBlurCB> m_cb;
    FullscreenQuad               m_quad;
    ComPtr<ID3D11SamplerState>   m_linearSampler;
};

} // namespace SE
