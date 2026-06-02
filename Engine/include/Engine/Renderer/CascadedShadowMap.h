#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>
#include "Engine/Renderer/ConstantBuffer.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/Mesh.h"
#include "Engine/Renderer/VertexBuffer.h"
#include "Engine/Renderer/IndexBuffer.h"

namespace SE {

static constexpr int CSM_NUM_CASCADES = 3;

class CascadedShadowMap
{
public:
    bool Init(ID3D11Device* device, ShaderLibrary& shaders, uint32_t resolution = 2048);

    // Compute cascade split distances and per-cascade light view-projection matrices.
    void Update(DirectX::XMFLOAT3 lightDir,
                DirectX::XMMATRIX cameraView,
                DirectX::XMMATRIX cameraProj,
                float cameraNear, float cameraFar);

    // Render shadow depth for a single cascade.
    void BeginCascade(ID3D11DeviceContext* ctx, int cascade);
    void DrawMesh(ID3D11DeviceContext* ctx, const Mesh& mesh, DirectX::XMMATRIX model);
    void DrawSphere(ID3D11DeviceContext* ctx, DirectX::XMFLOAT3 position, float radius);
    void EndCascade(ID3D11DeviceContext* ctx);

    // Bind cascade array SRV (t3), sampler (s1), and CSM cbuffer (b6) for lit pass.
    void BindForLitPass(ID3D11DeviceContext* ctx);
    void Unbind(ID3D11DeviceContext* ctx);

    // For backwards compat with LightCB (uses cascade 0 as primary)
    DirectX::XMMATRIX GetLightViewProj() const { return m_cascadeVP[0]; }

    // Public tuning parameters
    float splitLambda = 0.75f;  // PSSM lambda (0=linear, 1=logarithmic)

private:
    struct ShadowCBData
    {
        DirectX::XMFLOAT4X4 model;
        DirectX::XMFLOAT4X4 viewProj;
    };

    struct CSMCBData
    {
        DirectX::XMFLOAT4X4 cascadeVP[CSM_NUM_CASCADES];
        float splitDistances[4];  // view-space Z splits (padded to float4)
    };

    uint32_t m_resolution = 2048;
    int      m_currentCascade = 0;

    DirectX::XMMATRIX m_cascadeVP[CSM_NUM_CASCADES] = {};
    float             m_splits[CSM_NUM_CASCADES + 1] = {};

    Microsoft::WRL::ComPtr<ID3D11Texture2D>           m_depthTex;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>    m_dsv[CSM_NUM_CASCADES];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  m_srv;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>        m_shadowSampler;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>         m_layout;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>     m_shadowRS;

    VertexBuffer  m_sphereVB;
    IndexBuffer   m_sphereIB;

    const ShaderPermutation* m_perm = nullptr;
    ConstantBuffer<ShadowCBData> m_cb;
    ConstantBuffer<CSMCBData>    m_csmCB;

    D3D11_VIEWPORT          m_savedVP  = {};
    ID3D11RenderTargetView* m_savedRTV = nullptr;
    ID3D11DepthStencilView* m_savedDSV = nullptr;
    ID3D11RasterizerState*  m_savedRS  = nullptr;
};

} // namespace SE
