#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>
#include "Engine/Renderer/ConstantBuffer.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/Mesh.h"

namespace SE {

// Single spot light with a perspective shadow map.
class SpotLight
{
public:
    bool Init(ID3D11Device* device, ShaderLibrary& shaders, uint32_t resolution = 1024);

    // Update light transform matrices from current parameters.
    void Update();

    // Shadow pass: render scene depth from spot light's perspective.
    void BeginShadowPass(ID3D11DeviceContext* ctx);
    void DrawMesh(ID3D11DeviceContext* ctx, const Mesh& mesh, DirectX::XMMATRIX model);
    void EndShadowPass(ID3D11DeviceContext* ctx);

    // Bind spot light cbuffer (b5) and shadow map SRV (t8) for lit pass.
    void BindForLitPass(ID3D11DeviceContext* ctx);
    void Unbind(ID3D11DeviceContext* ctx);

    // Public parameters (tweak via ImGui)
    bool  enabled     = false;
    DirectX::XMFLOAT3 position  = { 0.0f, 10.0f, 0.0f };
    DirectX::XMFLOAT3 direction = { 0.0f, -1.0f, 0.0f }; // normalized
    DirectX::XMFLOAT3 color     = { 1.0f, 1.0f, 1.0f };
    float range       = 50.0f;
    float innerAngle  = 20.0f;  // degrees — full-intensity cone
    float outerAngle  = 35.0f;  // degrees — falloff to zero
    float intensity   = 5.0f;
    float shadowBias  = 0.0005f;

    DirectX::XMMATRIX GetViewProj() const { return m_viewProj; }
    ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }

private:
    struct SpotLightCBData
    {
        DirectX::XMFLOAT3 position;   float range;
        DirectX::XMFLOAT3 direction;  float outerCos;
        DirectX::XMFLOAT3 color;      float innerCos;
        DirectX::XMFLOAT4X4 viewProj;
        float shadowBias; int spotEnabled; float _pad[2];
    };

    struct ShadowCBData
    {
        DirectX::XMFLOAT4X4 model;
        DirectX::XMFLOAT4X4 viewProj;
    };

    uint32_t m_resolution = 1024;
    DirectX::XMMATRIX m_viewProj = {};

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_depthTex;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>   m_dsv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>    m_shadowRS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>       m_inputLayout;

    const ShaderPermutation* m_shadowPerm = nullptr;
    ConstantBuffer<ShadowCBData>     m_shadowCB;
    ConstantBuffer<SpotLightCBData>  m_lightCB;

    D3D11_VIEWPORT          m_savedVP  = {};
    ID3D11RenderTargetView* m_savedRTV = nullptr;
    ID3D11DepthStencilView* m_savedDSV = nullptr;
    ID3D11RasterizerState*  m_savedRS  = nullptr;
};

} // namespace SE
