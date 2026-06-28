#include "Engine/Renderer/ParticleSystem.h"
#include "Engine/Core/Logger.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace SE {

static float RandRange(std::mt19937& rng, float lo, float hi)
{
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(rng);
}

bool ParticleSystem::Init(ID3D11Device* device, ShaderLibrary& shaders)
{
    m_rng.seed(std::random_device{}());

    // Compile particle shader
    m_perm = shaders.Get(L"Shaders/Particle.hlsl");
    if (!m_perm) { SE_LOG_ERROR("ParticleSystem: shader load failed"); return false; }

    // Quad vertices: 4 corners [-0.5, 0.5] with UVs
    struct QuadVert { float x, y, u, v; };
    QuadVert quadVerts[4] = {
        { -0.5f,  0.5f, 0.0f, 0.0f },  // top-left
        {  0.5f,  0.5f, 1.0f, 0.0f },  // top-right
        {  0.5f, -0.5f, 1.0f, 1.0f },  // bottom-right
        { -0.5f, -0.5f, 0.0f, 1.0f },  // bottom-left
    };

    D3D11_BUFFER_DESC vbd = {};
    vbd.Usage     = D3D11_USAGE_IMMUTABLE;
    vbd.ByteWidth = sizeof(quadVerts);
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vInit = { quadVerts };
    if (FAILED(device->CreateBuffer(&vbd, &vInit, &m_quadVB)))
        return false;

    // Index buffer
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    D3D11_BUFFER_DESC ibd = {};
    ibd.Usage     = D3D11_USAGE_IMMUTABLE;
    ibd.ByteWidth = sizeof(indices);
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iInit = { indices };
    if (FAILED(device->CreateBuffer(&ibd, &iInit, &m_quadIB)))
        return false;

    // Instance buffer (dynamic, max particles)
    D3D11_BUFFER_DESC instBD = {};
    instBD.Usage          = D3D11_USAGE_DYNAMIC;
    instBD.ByteWidth      = sizeof(InstanceData) * config.maxParticles;
    instBD.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    instBD.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&instBD, nullptr, &m_instanceVB)))
        return false;

    // Input layout: per-vertex (slot 0) + per-instance (slot 1)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA,   0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D11_INPUT_PER_VERTEX_DATA,   0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,  D3D11_INPUT_PER_INSTANCE_DATA,  1 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA,  1 },
    };
    if (FAILED(device->CreateInputLayout(layout, 4, m_perm->vsBlob->GetBufferPointer(),
            m_perm->vsBlob->GetBufferSize(), &m_inputLayout)))
        return false;

    // Blend state: additive alpha
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable    = TRUE;
    blendDesc.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend      = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blendDesc, &m_blendState)))
        return false;

    // Depth state: read but no write (particles don't occlude each other)
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable    = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&dsDesc, &m_depthState)))
        return false;

    // Raster state: no culling (billboards face camera but quads could be wound either way)
    D3D11_RASTERIZER_DESC rasDesc = {};
    rasDesc.FillMode = D3D11_FILL_SOLID;
    rasDesc.CullMode = D3D11_CULL_NONE;
    if (FAILED(device->CreateRasterizerState(&rasDesc, &m_rasterState)))
        return false;

    // Sampler
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&sampDesc, &m_sampler)))
        return false;

    // Constant buffer
    if (!m_cameraCB.Create(device)) return false;

    // Create default 1x1 white texture as fallback
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        uint32_t white = 0xFFFFFFFF;
        D3D11_SUBRESOURCE_DATA init = { &white, 4, 0 };
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(device->CreateTexture2D(&td, &init, &tex)))
            device->CreateShaderResourceView(tex.Get(), nullptr, &m_defaultSRV);
    }

    // Reserve particle pool
    m_particles.reserve(config.maxParticles);
    m_instances.reserve(config.maxParticles);

    return true;
}

void ParticleSystem::Emit(int count)
{
    for (int i = 0; i < count && (int)m_particles.size() < config.maxParticles; ++i)
    {
        Particle p = {};
        // Position: emitter world pos + random offset within spawn radius
        float rx = RandRange(m_rng, -1.0f, 1.0f);
        float ry = RandRange(m_rng, -1.0f, 1.0f);
        float rz = RandRange(m_rng, -1.0f, 1.0f);
        float len = std::sqrt(rx*rx + ry*ry + rz*rz);
        if (len > 0.001f) { rx /= len; ry /= len; rz /= len; }
        float r = RandRange(m_rng, 0.0f, config.spawnRadius);
        p.position.x = m_worldPos.x + config.spawnOffset.x + rx * r;
        p.position.y = m_worldPos.y + config.spawnOffset.y + ry * r;
        p.position.z = m_worldPos.z + config.spawnOffset.z + rz * r;

        // Velocity
        p.velocity.x = RandRange(m_rng, config.velocityMin.x, config.velocityMax.x);
        p.velocity.y = RandRange(m_rng, config.velocityMin.y, config.velocityMax.y);
        p.velocity.z = RandRange(m_rng, config.velocityMin.z, config.velocityMax.z);

        // Lifetime
        p.maxLife = RandRange(m_rng, config.lifetimeMin, config.lifetimeMax);
        p.life    = p.maxLife;

        // Initial state
        p.color = config.colorStart;
        p.size  = config.sizeStart;

        m_particles.push_back(p);
    }
}

void ParticleSystem::Update(float dt)
{
    if (!enabled) return;

    // Emit new particles
    m_emitAccum += config.emitRate * dt;
    int toEmit = static_cast<int>(m_emitAccum);
    if (toEmit > 0)
    {
        Emit(toEmit);
        m_emitAccum -= static_cast<float>(toEmit);
    }

    // Update existing particles
    for (auto& p : m_particles)
    {
        p.life -= dt;
        if (p.life <= 0.0f) continue;

        // Apply gravity
        p.velocity.x += config.gravity.x * dt;
        p.velocity.y += config.gravity.y * dt;
        p.velocity.z += config.gravity.z * dt;

        // Integrate position
        p.position.x += p.velocity.x * dt;
        p.position.y += p.velocity.y * dt;
        p.position.z += p.velocity.z * dt;

        // Interpolate size and color over lifetime
        float t = 1.0f - (p.life / p.maxLife); // 0 at birth, 1 at death
        p.size = config.sizeStart + (config.sizeEnd - config.sizeStart) * t;
        p.color.x = config.colorStart.x + (config.colorEnd.x - config.colorStart.x) * t;
        p.color.y = config.colorStart.y + (config.colorEnd.y - config.colorStart.y) * t;
        p.color.z = config.colorStart.z + (config.colorEnd.z - config.colorStart.z) * t;
        p.color.w = config.colorStart.w + (config.colorEnd.w - config.colorStart.w) * t;
    }

    // Remove dead particles (swap-and-pop)
    m_particles.erase(
        std::remove_if(m_particles.begin(), m_particles.end(),
            [](const Particle& p) { return p.life <= 0.0f; }),
        m_particles.end());

    m_aliveCount = static_cast<uint32_t>(m_particles.size());
}

void ParticleSystem::UpdateInstanceBuffer(ID3D11DeviceContext* ctx, const XMMATRIX& view)
{
    m_instances.clear();
    m_instances.reserve(m_particles.size());

    for (const auto& p : m_particles)
    {
        InstanceData inst;
        inst.posAndSize = { p.position.x, p.position.y, p.position.z, p.size };
        inst.color      = p.color;
        m_instances.push_back(inst);
    }

    // Sort back-to-front for correct alpha blending
    // Extract camera position from inverse view (row 3 of inverse = camera world pos)
    XMVECTOR det;
    XMMATRIX invView = XMMatrixInverse(&det, view);
    XMFLOAT3 camPos;
    XMStoreFloat3(&camPos, invView.r[3]);

    std::sort(m_instances.begin(), m_instances.end(),
        [&camPos](const InstanceData& a, const InstanceData& b)
        {
            float da = (a.posAndSize.x - camPos.x) * (a.posAndSize.x - camPos.x)
                     + (a.posAndSize.y - camPos.y) * (a.posAndSize.y - camPos.y)
                     + (a.posAndSize.z - camPos.z) * (a.posAndSize.z - camPos.z);
            float db = (b.posAndSize.x - camPos.x) * (b.posAndSize.x - camPos.x)
                     + (b.posAndSize.y - camPos.y) * (b.posAndSize.y - camPos.y)
                     + (b.posAndSize.z - camPos.z) * (b.posAndSize.z - camPos.z);
            return da > db; // far first
        });

    // Upload
    if (!m_instances.empty())
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        ctx->Map(m_instanceVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, m_instances.data(), m_instances.size() * sizeof(InstanceData));
        ctx->Unmap(m_instanceVB.Get(), 0);
    }
}

void ParticleSystem::Render(ID3D11DeviceContext* ctx,
                            const XMMATRIX& view,
                            const XMMATRIX& proj,
                            uint32_t screenWidth, uint32_t screenHeight)
{
    if (!enabled || m_particles.empty()) return;

    UpdateInstanceBuffer(ctx, view);

    // Extract camera right/up from view matrix (rows of transposed view = columns of view)
    XMFLOAT4X4 viewF;
    XMStoreFloat4x4(&viewF, view);
    CameraCB cb = {};
    cb.viewProj = view * proj;
    cb.camRight = { viewF._11, viewF._21, viewF._31 };
    cb.camUp    = { viewF._12, viewF._22, viewF._32 };
    m_cameraCB.Update(ctx, cb);
    m_cameraCB.BindVS(ctx, 0);

    // Set viewport
    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(screenWidth);
    vp.Height   = static_cast<float>(screenHeight);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Bind shaders
    ctx->VSSetShader(m_perm->vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_perm->ps.Get(), nullptr, 0);
    ctx->IASetInputLayout(m_inputLayout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind vertex buffers (slot 0 = quad, slot 1 = instances)
    UINT strides[2] = { sizeof(float) * 4, sizeof(InstanceData) };
    UINT offsets[2] = { 0, 0 };
    ID3D11Buffer* vbs[2] = { m_quadVB.Get(), m_instanceVB.Get() };
    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    ctx->IASetIndexBuffer(m_quadIB.Get(), DXGI_FORMAT_R16_UINT, 0);

    // Bind texture (or default white)
    if (m_texture && m_texture->IsValid())
        m_texture->BindPS(ctx, 0);
    else
        ctx->PSSetShaderResources(0, 1, m_defaultSRV.GetAddressOf());
    ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    // Set render states
    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(m_depthState.Get(), 0);
    ctx->RSSetState(m_rasterState.Get());

    // Draw instanced
    ctx->DrawIndexedInstanced(6, m_aliveCount, 0, 0, 0);

    // Restore defaults
    ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
}

} // namespace SE
