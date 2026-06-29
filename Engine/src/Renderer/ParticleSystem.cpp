#include "Engine/Renderer/ParticleSystem.h"
#include "Engine/Core/Logger.h"

using namespace DirectX;

namespace SE {

bool ParticleSystem::Init(ID3D11Device* device, ShaderLibrary& shaders, int maxParts)
{
    if (maxParts > 0) config.maxParticles = maxParts;
    int maxParticles = config.maxParticles;

    // Compile compute shaders
    m_csEmit = shaders.GetCS(L"Shaders/ParticleCompute.hlsl", "CS_Emit");
    m_csUpdate = shaders.GetCS(L"Shaders/ParticleCompute.hlsl", "CS_Update");
    m_csResetArgs = shaders.GetCS(L"Shaders/ParticleCompute.hlsl", "CS_ResetArgs");

    if (!m_csEmit || !m_csUpdate || !m_csResetArgs)
    {
        SE_LOG_ERROR("ParticleSystem: failed to compile compute shaders");
        return false;
    }

    // Render shader
    m_renderPerm = shaders.Get(L"Shaders/Particle.hlsl");
    if (!m_renderPerm) return false;

    // --- Create GPU buffers ---

    // Particle pool (structured buffer)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = maxParticles * 64; // Particle struct: pos(12) + vel(12) + color(16) + colorEnd(16) + life(4) + maxLife(4) = 64
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 64;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &m_particleBuffer))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = maxParticles;
        if (FAILED(device->CreateUnorderedAccessView(m_particleBuffer.Get(), &uavDesc, &m_particleUAV))) return false;
    }

    // Dead list (structured buffer of uint)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = maxParticles * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;

        // Initialize dead list with all indices
        std::vector<uint32_t> deadInit(maxParticles);
        for (int i = 0; i < maxParticles; ++i)
            deadInit[i] = static_cast<uint32_t>(i);
        D3D11_SUBRESOURCE_DATA initData = { deadInit.data() };
        if (FAILED(device->CreateBuffer(&bd, &initData, &m_deadListBuffer))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = maxParticles;
        if (FAILED(device->CreateUnorderedAccessView(m_deadListBuffer.Get(), &uavDesc, &m_deadListUAV))) return false;
    }

    // Counters (RWByteAddressBuffer, 8 bytes: deadCount + renderCount)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 8; // deadCount + aliveCount
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

        // Initialize: deadCount = maxParticles, aliveCount = 0
        uint32_t initData[2] = { static_cast<uint32_t>(maxParticles), 0 };
        D3D11_SUBRESOURCE_DATA init = { initData };
        if (FAILED(device->CreateBuffer(&bd, &init, &m_countersBuffer))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        uavDesc.Buffer.NumElements = 2;
        if (FAILED(device->CreateUnorderedAccessView(m_countersBuffer.Get(), &uavDesc, &m_countersUAV))) return false;
    }

    // Instance buffer for rendering — StructuredBuffer read by VS via SRV
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = maxParticles * 48;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 48;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &m_instanceBuffer))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = maxParticles;
        if (FAILED(device->CreateUnorderedAccessView(m_instanceBuffer.Get(), &uavDesc, &m_instanceUAV))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = maxParticles;
        if (FAILED(device->CreateShaderResourceView(m_instanceBuffer.Get(), &srvDesc, &m_instanceSRV))) return false;
    }

    // Draw args buffer (indirect arguments)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 20; // 5 * uint32_t
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

        uint32_t initArgs[5] = { 6, 0, 0, 0, 0 }; // 6 indices, 0 instances
        D3D11_SUBRESOURCE_DATA init = { initArgs };
        if (FAILED(device->CreateBuffer(&bd, &init, &m_drawArgsBuffer))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        uavDesc.Buffer.NumElements = 5;
        if (FAILED(device->CreateUnorderedAccessView(m_drawArgsBuffer.Get(), &uavDesc, &m_drawArgsUAV))) return false;
    }

    // --- Render resources ---

    // Quad vertices
    struct QuadVert { float x, y, u, v; };
    QuadVert quadVerts[4] = {
        { -0.5f,  0.5f, 0.0f, 0.0f },
        {  0.5f,  0.5f, 1.0f, 0.0f },
        {  0.5f, -0.5f, 1.0f, 1.0f },
        { -0.5f, -0.5f, 0.0f, 1.0f },
    };
    D3D11_BUFFER_DESC vbd = {};
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.ByteWidth = sizeof(quadVerts);
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vInit = { quadVerts };
    if (FAILED(device->CreateBuffer(&vbd, &vInit, &m_quadVB))) return false;

    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    D3D11_BUFFER_DESC ibd = {};
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.ByteWidth = sizeof(indices);
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iInit = { indices };
    if (FAILED(device->CreateBuffer(&ibd, &iInit, &m_quadIB))) return false;

    // Input layout — only per-vertex data (instances come from SRV, not VB)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device->CreateInputLayout(layout, 2, m_renderPerm->vsBlob->GetBufferPointer(),
            m_renderPerm->vsBlob->GetBufferSize(), &m_inputLayout))) return false;

    // Blend state: additive alpha
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blendDesc, &m_blendState))) return false;

    // Depth: read only
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&dsDesc, &m_depthState))) return false;

    // Raster: no cull
    D3D11_RASTERIZER_DESC rasDesc = {};
    rasDesc.FillMode = D3D11_FILL_SOLID;
    rasDesc.CullMode = D3D11_CULL_NONE;
    if (FAILED(device->CreateRasterizerState(&rasDesc, &m_rasterState))) return false;

    // Sampler
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&sampDesc, &m_sampler))) return false;

    // Constant buffers
    if (!m_cameraCB.Create(device)) return false;
    if (!m_emitCB.Create(device)) return false;
    if (!m_updateCB.Create(device)) return false;

    // Default white texture
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

    return true;
}

void ParticleSystem::Update(ID3D11DeviceContext* ctx, float dt)
{
    if (!enabled) return;

    m_time += dt;

    // 1) Reset draw args (instanceCount = 0)
    ctx->CSSetShader(m_csResetArgs, nullptr, 0);
    // CS_ResetArgs uses u3 = drawArgs
    ID3D11UnorderedAccessView* resetUAVs[5] = { nullptr, nullptr, nullptr, m_drawArgsUAV.Get(), nullptr };
    ctx->CSSetUnorderedAccessViews(0, 5, resetUAVs, nullptr);
    ctx->Dispatch(1, 1, 1);

    // 2) Emit new particles
    m_emitAccum += config.emitRate * dt;
    int toEmit = static_cast<int>(m_emitAccum);
    if (toEmit > 0)
    {
        m_emitAccum -= static_cast<float>(toEmit);

        EmitCB ecb = {};
        ecb.emitterPos = m_worldPos;
        ecb.spawnRadius = config.spawnRadius;
        ecb.velocityMin = config.velocityMin;
        ecb.lifetimeMin = config.lifetimeMin;
        ecb.velocityMax = config.velocityMax;
        ecb.lifetimeMax = config.lifetimeMax;
        ecb.colorStart = config.colorStart;
        ecb.colorEnd = config.colorEnd;
        ecb.sizeStart = config.sizeStart;
        ecb.sizeEnd = config.sizeEnd;
        ecb.emitCount = static_cast<uint32_t>(toEmit);
        ecb.randomSeed = m_time * 1000.0f;
        m_emitCB.Update(ctx, ecb);
        m_emitCB.BindCS(ctx, 0);

        ctx->CSSetShader(m_csEmit, nullptr, 0);
        // CS_Emit uses u0=particles, u1=counters, u4=deadList
        ID3D11UnorderedAccessView* emitUAVs[5] = {
            m_particleUAV.Get(), m_countersUAV.Get(), nullptr, nullptr, m_deadListUAV.Get()
        };
        ctx->CSSetUnorderedAccessViews(0, 5, emitUAVs, nullptr);
        ctx->Dispatch((toEmit + 63) / 64, 1, 1);
    }

    // 3) Update all particles (alive ones write to instance buffer)
    {
        UpdateCB ucb = {};
        ucb.deltaTime = dt;
        ucb.gravity = config.gravity;
        ucb.maxParticles = static_cast<uint32_t>(config.maxParticles);
        m_updateCB.Update(ctx, ucb);
        m_updateCB.BindCS(ctx, 0);

        ctx->CSSetShader(m_csUpdate, nullptr, 0);
        // CS_Update uses u0=particles, u1=counters, u2=instances, u3=drawArgs, u4=deadList
        ID3D11UnorderedAccessView* updateUAVs[5] = {
            m_particleUAV.Get(), m_countersUAV.Get(), m_instanceUAV.Get(),
            m_drawArgsUAV.Get(), m_deadListUAV.Get()
        };
        ctx->CSSetUnorderedAccessViews(0, 5, updateUAVs, nullptr);
        ctx->Dispatch((config.maxParticles + 255) / 256, 1, 1);
    }

    // Unbind UAVs
    ID3D11UnorderedAccessView* nullUAVs[5] = {};
    ctx->CSSetUnorderedAccessViews(0, 5, nullUAVs, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
}

void ParticleSystem::Render(ID3D11DeviceContext* ctx,
                               const XMMATRIX& view,
                               const XMMATRIX& proj,
                               uint32_t screenWidth, uint32_t screenHeight,
                               ID3D11ShaderResourceView* depthSRV,
                               float nearZ, float farZ)
{
    if (!enabled) return;

    // Camera CB
    XMFLOAT4X4 viewF;
    XMStoreFloat4x4(&viewF, view);
    CameraCB cb = {};
    cb.viewProj = view * proj;
    cb.camRight = { viewF._11, viewF._21, viewF._31 };
    cb.camUp = { viewF._12, viewF._22, viewF._32 };
    cb.atlasColumns = static_cast<float>(config.atlasColumns);
    cb.atlasRows = static_cast<float>(config.atlasRows);
    cb.atlasFrameCount = static_cast<float>(config.atlasFrameCount > 0
                            ? config.atlasFrameCount
                            : config.atlasColumns * config.atlasRows);
    cb.atlasSpeed = config.atlasSpeed;
    cb.nearZ = nearZ;
    cb.farZ = farZ;
    cb.softDistance = config.softDistance;
    cb._pad2 = 0.0f;
    m_cameraCB.Update(ctx, cb);
    m_cameraCB.BindVS(ctx, 0);
    m_cameraCB.BindPS(ctx, 0);

    // Viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(screenWidth);
    vp.Height = static_cast<float>(screenHeight);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Shaders
    ctx->VSSetShader(m_renderPerm->vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_renderPerm->ps.Get(), nullptr, 0);
    ctx->IASetInputLayout(m_inputLayout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Vertex buffers: slot 0 = quad only (instances via SRV)
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    ID3D11Buffer* vb = m_quadVB.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_quadIB.Get(), DXGI_FORMAT_R16_UINT, 0);

    // Bind instance data as VS SRV (t2)
    ctx->VSSetShaderResources(2, 1, m_instanceSRV.GetAddressOf());

    // Texture
    if (m_texture && m_texture->IsValid())
        m_texture->BindPS(ctx, 0);
    else
        ctx->PSSetShaderResources(0, 1, m_defaultSRV.GetAddressOf());
    ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    if (depthSRV)
        ctx->PSSetShaderResources(1, 1, &depthSRV);

    // Render states
    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(m_depthState.Get(), 0);
    ctx->RSSetState(m_rasterState.Get());

    // Indirect draw
    ctx->DrawIndexedInstancedIndirect(m_drawArgsBuffer.Get(), 0);

    // Restore
    ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
    ctx->PSSetShaderResources(1, 1, &nullSRV);
    ctx->VSSetShaderResources(2, 1, &nullSRV);
}

} // namespace SE

