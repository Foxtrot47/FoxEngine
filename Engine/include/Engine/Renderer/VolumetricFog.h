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

// Single-scattering volumetric fog with directional (sun) light shafts / god rays.
// Ray-marches from the camera to the reconstructed scene depth, sampling the
// cascaded shadow map at each step to accumulate in-scattered sunlight, then
// composites the result into the HDR buffer (before bloom so shafts glow).
class VolumetricFog
{
public:
    bool Init(ID3D11Device* device, ShaderLibrary& shaders,
              uint32_t width, uint32_t height);
    void Resize(ID3D11Device* device, uint32_t width, uint32_t height);
    void Shutdown();

    // Ray-marches fog and composites it into hdrRT.
    //   depthSRV      – readable scene depth (R32_FLOAT view of the HDR depth)
    //   csmSRV        – cascade shadow-map array SRV
    //   csmSampler    – comparison sampler for the cascade array
    //   skySRV        – sky panorama used as low-frequency IBL for fog ambient
    //   cascadeVP[3]  – per-cascade light view-projection matrices
    //   splitFar[2]   – far view-space Z of cascade 0 and 1
    void Apply(ID3D11DeviceContext* ctx,
               RenderTarget& hdrRT,
               ID3D11ShaderResourceView* depthSRV,
               ID3D11ShaderResourceView* csmSRV,
               ID3D11SamplerState* csmSampler,
               ID3D11ShaderResourceView* skySRV,
               DirectX::XMMATRIX view,
               DirectX::XMMATRIX proj,
               DirectX::XMFLOAT3 cameraPos,
               DirectX::XMFLOAT3 lightDir,
               DirectX::XMFLOAT3 lightColor,
               const DirectX::XMMATRIX cascadeVP[3],
               const float splitFar[2]);

    bool  enabled          = true;
    float density          = 0.03f;   // extinction / scattering coefficient
    float anisotropy       = 0.76f;   // Henyey-Greenstein g (forward scattering)
    float scatterIntensity = 1.0f;    // sun in-scatter strength
    int   steps            = 48;      // ray-march samples
    float maxDistance      = 60.0f;   // furthest march distance (world units)
    float ambientColor[3]  = { 1.0f, 1.0f, 1.0f }; // IBL fog tint
    float ambientIntensity = 0.10f;   // IBL fog strength
    float heightFalloff    = 0.0f;    // exponential density falloff with height
    float heightBase       = 0.0f;    // world Y where full density begins
    float noiseStrength    = 0.35f;   // density variation amount
    float noiseScale       = 0.035f;  // world-space noise frequency
    float noiseSpeed       = 0.0f;    // animated drift speed
    bool  blur             = true;    // smooth shafts with a separable Gaussian

private:
    struct alignas(16) FogCBData
    {
        DirectX::XMFLOAT4X4 invViewProj;     // 64
        DirectX::XMFLOAT4X4 view;            // 64
        DirectX::XMFLOAT4X4 cascadeVP[3];    // 192
        DirectX::XMFLOAT4   cascadeSplits;   // 16 (x,y used)
        DirectX::XMFLOAT3   cameraPos;   float density;          // 16
        DirectX::XMFLOAT3   lightDir;    float anisotropy;       // 16
        DirectX::XMFLOAT3   lightColor;  float stepCount;        // 16
        DirectX::XMFLOAT3   fogAmbient;  float ambientIntensity; // 16
        DirectX::XMFLOAT2   screenSize;  float maxDistance; float scatterIntensity; // 16
        float heightFalloff; float heightBase; float noiseStrength; float noiseScale; // 16
        float noiseTime; float _pad0; float _pad1; float _pad2;                      // 16
    };

    RenderTarget               m_fogRT;   // RGB=in-scatter, A=transmittance
    RenderTarget               m_blurRT;  // separable blur intermediate
    const ShaderPermutation*   m_fogPerm  = nullptr;
    const ShaderPermutation*   m_blurPerm = nullptr;
    ConstantBuffer<FogCBData>  m_cb;
    struct alignas(16) BlurCBData { float texelX, texelY, dirX, dirY; };
    ConstantBuffer<BlurCBData> m_blurCB;
    FullscreenQuad             m_quad;
    ComPtr<ID3D11SamplerState> m_pointSampler;
    ComPtr<ID3D11SamplerState> m_skySampler;
    ComPtr<ID3D11BlendState>   m_compositeBlend;
    uint32_t                   m_width  = 0;
    uint32_t                   m_height = 0;
    float                      m_noiseTime = 0.0f;
};

} // namespace SE
