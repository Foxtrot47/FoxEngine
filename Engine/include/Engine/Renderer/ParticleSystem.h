#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>
#include <random>
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/ConstantBuffer.h"
#include "Engine/Renderer/Texture2D.h"
#include "Engine/Assets/AssetManager.h"

using Microsoft::WRL::ComPtr;

namespace SE {

struct Particle
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 velocity;
    DirectX::XMFLOAT4 color;       // current color (interpolated)
    float size;
    float life;                     // remaining life [0..maxLife]
    float maxLife;
};

struct ParticleEmitterConfig
{
    // Spawn
    float emitRate        = 50.0f;   // particles per second
    int   maxParticles    = 1000;

    // Lifetime
    float lifetimeMin     = 1.0f;
    float lifetimeMax     = 3.0f;

    // Initial velocity (random within range)
    DirectX::XMFLOAT3 velocityMin = { -1.0f, 1.0f, -1.0f };
    DirectX::XMFLOAT3 velocityMax = {  1.0f, 5.0f,  1.0f };

    // Size over lifetime
    float sizeStart       = 0.2f;
    float sizeEnd         = 0.05f;

    // Color over lifetime
    DirectX::XMFLOAT4 colorStart = { 1.0f, 0.8f, 0.2f, 1.0f };
    DirectX::XMFLOAT4 colorEnd   = { 1.0f, 0.2f, 0.0f, 0.0f };

    // World-space gravity applied to velocity
    DirectX::XMFLOAT3 gravity = { 0.0f, -2.0f, 0.0f };

    // Emitter shape: point (all spawn at origin)
    DirectX::XMFLOAT3 spawnOffset = { 0.0f, 0.0f, 0.0f };
    float spawnRadius     = 0.0f;   // random sphere radius around offset
};

class ParticleSystem
{
public:
    bool Init(ID3D11Device* device, ShaderLibrary& shaders);

    void SetPosition(const DirectX::XMFLOAT3& pos) { m_worldPos = pos; }
    void SetTexture(AssetHandle<Texture2D> tex) { m_texture = tex; }

    // Update particle simulation (call once per frame)
    void Update(float dt);

    // Render all living particles as camera-facing billboards
    void Render(ID3D11DeviceContext* ctx,
                const DirectX::XMMATRIX& view,
                const DirectX::XMMATRIX& proj,
                uint32_t screenWidth, uint32_t screenHeight);

    ParticleEmitterConfig config;
    bool enabled = true;

    uint32_t GetAliveCount() const { return m_aliveCount; }

private:
    void Emit(int count);
    void UpdateInstanceBuffer(ID3D11DeviceContext* ctx, const DirectX::XMMATRIX& view);

    struct InstanceData
    {
        DirectX::XMFLOAT4 posAndSize;  // xyz = world pos, w = size
        DirectX::XMFLOAT4 color;
    };

    struct CameraCB
    {
        DirectX::XMMATRIX viewProj;
        DirectX::XMFLOAT3 camRight;
        float _pad0;
        DirectX::XMFLOAT3 camUp;
        float _pad1;
    };

    std::vector<Particle>     m_particles;
    std::vector<InstanceData> m_instances;
    uint32_t                  m_aliveCount = 0;
    float                     m_emitAccum  = 0.0f;
    DirectX::XMFLOAT3        m_worldPos   = { 0, 0, 0 };

    // GPU resources
    ComPtr<ID3D11Buffer>       m_quadVB;
    ComPtr<ID3D11Buffer>       m_quadIB;
    ComPtr<ID3D11Buffer>       m_instanceVB;
    ComPtr<ID3D11InputLayout>  m_inputLayout;
    ComPtr<ID3D11BlendState>   m_blendState;
    ComPtr<ID3D11DepthStencilState> m_depthState;
    ComPtr<ID3D11RasterizerState>   m_rasterState;
    ComPtr<ID3D11SamplerState>      m_sampler;
    const ShaderPermutation*   m_perm = nullptr;
    ConstantBuffer<CameraCB>   m_cameraCB;
    ComPtr<ID3D11ShaderResourceView> m_defaultSRV;
    AssetHandle<Texture2D>     m_texture;

    std::mt19937               m_rng;
};

} // namespace SE
