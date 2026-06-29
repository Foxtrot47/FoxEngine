#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/ConstantBuffer.h"
#include "Engine/Renderer/Texture2D.h"
#include "Engine/Assets/AssetManager.h"

using Microsoft::WRL::ComPtr;

namespace SE {

struct ParticleEmitterConfig
{
    float emitRate        = 50.0f;
    int   maxParticles    = 1000;

    float lifetimeMin     = 1.0f;
    float lifetimeMax     = 3.0f;

    DirectX::XMFLOAT3 velocityMin = { -1.0f, 1.0f, -1.0f };
    DirectX::XMFLOAT3 velocityMax = {  1.0f, 5.0f,  1.0f };

    float sizeStart       = 0.2f;
    float sizeEnd         = 0.05f;

    DirectX::XMFLOAT4 colorStart = { 1.0f, 0.8f, 0.2f, 1.0f };
    DirectX::XMFLOAT4 colorEnd   = { 1.0f, 0.2f, 0.0f, 0.0f };

    DirectX::XMFLOAT3 gravity = { 0.0f, -2.0f, 0.0f };

    DirectX::XMFLOAT3 spawnOffset = { 0.0f, 0.0f, 0.0f };
    float spawnRadius     = 0.0f;

    int   atlasColumns    = 1;
    int   atlasRows       = 1;
    int   atlasFrameCount = 0;
    float atlasSpeed      = 1.0f;

    float softDistance    = 0.5f;
};

class ParticleSystem
{
public:
    bool Init(ID3D11Device* device, ShaderLibrary& shaders, int maxParticles = 0);

    void SetPosition(const DirectX::XMFLOAT3& pos) { m_worldPos = pos; }
    void SetTexture(AssetHandle<Texture2D> tex) { m_texture = tex; }

    // GPU compute update — dispatches emit + simulate on the GPU
    void Update(ID3D11DeviceContext* ctx, float dt);

    // Indirect draw of all alive particles
    void Render(ID3D11DeviceContext* ctx,
                const DirectX::XMMATRIX& view,
                const DirectX::XMMATRIX& proj,
                uint32_t screenWidth, uint32_t screenHeight,
                ID3D11ShaderResourceView* depthSRV = nullptr,
                float nearZ = 0.1f, float farZ = 500.0f);

    ParticleEmitterConfig config;
    bool enabled = true;

private:
    struct EmitCB
    {
        DirectX::XMFLOAT3 emitterPos;
        float spawnRadius;
        DirectX::XMFLOAT3 velocityMin;
        float lifetimeMin;
        DirectX::XMFLOAT3 velocityMax;
        float lifetimeMax;
        DirectX::XMFLOAT4 colorStart;
        DirectX::XMFLOAT4 colorEnd;
        float sizeStart;
        float sizeEnd;
        uint32_t emitCount;
        float randomSeed;
    };

    struct UpdateCB
    {
        float deltaTime;
        DirectX::XMFLOAT3 gravity;
        uint32_t maxParticles;
        DirectX::XMFLOAT3 _pad;
    };

    struct CameraCB
    {
        DirectX::XMMATRIX viewProj;
        DirectX::XMFLOAT3 camRight;
        float _pad0;
        DirectX::XMFLOAT3 camUp;
        float _pad1;
        float atlasColumns;
        float atlasRows;
        float atlasFrameCount;
        float atlasSpeed;
        float nearZ;
        float farZ;
        float softDistance;
        float _pad2;
    };

    DirectX::XMFLOAT3 m_worldPos = { 0, 0, 0 };
    float m_emitAccum = 0.0f;
    float m_time      = 0.0f;

    // Compute shaders
    ID3D11ComputeShader* m_csEmit      = nullptr;
    ID3D11ComputeShader* m_csUpdate    = nullptr;
    ID3D11ComputeShader* m_csResetArgs = nullptr;

    // GPU buffers
    ComPtr<ID3D11Buffer> m_particleBuffer;
    ComPtr<ID3D11Buffer> m_deadListBuffer;
    ComPtr<ID3D11Buffer> m_countersBuffer;
    ComPtr<ID3D11Buffer> m_instanceBuffer;
    ComPtr<ID3D11Buffer> m_drawArgsBuffer;

    // UAVs
    ComPtr<ID3D11UnorderedAccessView> m_particleUAV;
    ComPtr<ID3D11UnorderedAccessView> m_deadListUAV;
    ComPtr<ID3D11UnorderedAccessView> m_countersUAV;
    ComPtr<ID3D11UnorderedAccessView> m_instanceUAV;
    ComPtr<ID3D11UnorderedAccessView> m_drawArgsUAV;

    // SRV for instance buffer (VS reads)
    ComPtr<ID3D11ShaderResourceView> m_instanceSRV;

    // Render resources
    ComPtr<ID3D11Buffer>             m_quadVB;
    ComPtr<ID3D11Buffer>             m_quadIB;
    ComPtr<ID3D11InputLayout>        m_inputLayout;
    ComPtr<ID3D11BlendState>         m_blendState;
    ComPtr<ID3D11DepthStencilState>  m_depthState;
    ComPtr<ID3D11RasterizerState>    m_rasterState;
    ComPtr<ID3D11SamplerState>       m_sampler;
    const ShaderPermutation*         m_renderPerm = nullptr;
    ConstantBuffer<CameraCB>         m_cameraCB;
    ConstantBuffer<EmitCB>           m_emitCB;
    ConstantBuffer<UpdateCB>         m_updateCB;
    ComPtr<ID3D11ShaderResourceView> m_defaultSRV;
    AssetHandle<Texture2D>           m_texture;
};

} // namespace SE
