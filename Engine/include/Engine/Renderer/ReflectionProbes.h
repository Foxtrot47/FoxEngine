#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include "Engine/Renderer/ConstantBuffer.h"
#include "Engine/Renderer/FullscreenQuad.h"
#include "Engine/Renderer/RenderTarget.h"
#include "Engine/Renderer/ShaderLibrary.h"

namespace SE {

class EnvironmentLighting;

inline constexpr uint32_t k_maxReflectionProbes = 8;

// Box-projected reflection probes. Each probe captures the scene into a 128^2 HDR
// cube, which is resampled to a 256x128 equirect and GGX-prefiltered into one slice
// of a Texture2DArray (8 slices, 8 mips). Basic.hlsl blends the slices over the
// global specular IBL by probe weight (t28, ReflectionProbeCB b9).
class ReflectionProbes
{
public:
    struct Probe
    {
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 extents  = { 1.0f, 1.0f, 1.0f }; // half extents of the influence box
        float             blendDistance = 5.0f;
    };

    static constexpr uint32_t k_faceSize = 128;
    static constexpr uint32_t k_width    = 256;
    static constexpr uint32_t k_height   = 128;
    static constexpr uint32_t k_mipCount = 8;

    bool Init(ID3D11Device* device, ShaderLibrary& shaders);

    // Replaces the probe set; only the first k_maxReflectionProbes are kept (warns).
    void SetProbes(const std::vector<Probe>& probes);
    uint32_t     GetCount() const { return static_cast<uint32_t>(m_probes.size()); }
    const Probe& GetProbe(uint32_t index) const { return m_probes[index]; }

    // Binds cube face (0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z, PointShadowMap::FaceView
    // order) with the shared depth buffer, both cleared, and a k_faceSize viewport.
    void BeginFace(ID3D11DeviceContext* ctx, int face);
    // Converts the captured cube to equirect and prefilters it into slice `probe`.
    // Leaves no render target bound.
    void ResolveProbe(ID3D11DeviceContext* ctx, uint32_t probe, EnvironmentLighting& environment);

    // ReflectionProbeCB (b9) + probe array (t28). withProbes = false binds a zero
    // probe count, so capture passes sample only the global environment.
    void Bind(ID3D11DeviceContext* ctx, bool withProbes);

private:
    // Mirrors Basic.hlsl ReflectionProbeCB (b9).
    struct ProbeCBData
    {
        DirectX::XMFLOAT4 center[k_maxReflectionProbes];
        DirectX::XMFLOAT4 extents[k_maxReflectionProbes]; // xyz half extents, w blendDistance
        DirectX::XMFLOAT4 info;                           // x count, y mip count
    };
    static_assert(sizeof(ProbeCBData) == 272, "ProbeCBData must match Basic.hlsl ReflectionProbeCB");

    std::vector<Probe> m_probes;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_cubeTex;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_cubeRTV[6];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_cubeSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_depthTex; // shared D32_FLOAT, cleared per face
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>   m_dsv;

    RenderTarget m_equirectRT; // k_width x k_height, full mip chain (filter source)

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_arrayTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_arraySRV;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_arrayRTVs[k_maxReflectionProbes][k_mipCount];

    const ShaderPermutation*                   m_toEquirectPerm = nullptr;
    FullscreenQuad                             m_quad;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linearClamp;
    ConstantBuffer<ProbeCBData>                m_cb;
};

} // namespace SE
