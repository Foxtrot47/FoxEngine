#include "Engine/Renderer/MotionBlur.h"
#include "Engine/Core/Logger.h"
#include <algorithm>

namespace SE {

bool MotionBlur::Init(ID3D11Device* device, ShaderLibrary& shaders,
                      uint32_t width, uint32_t height)
{
    m_tileMaxPerm     = shaders.Get(L"Engine/Shaders/MotionBlur.hlsl", {{ "MB_PASS", "0" }});
    m_neighborMaxPerm = shaders.Get(L"Engine/Shaders/MotionBlur.hlsl", {{ "MB_PASS", "1" }});
    m_gatherPerm      = shaders.Get(L"Engine/Shaders/MotionBlur.hlsl", {{ "MB_PASS", "2" }});

    if (!m_tileMaxPerm || !m_neighborMaxPerm || !m_gatherPerm)
    {
        SE_LOG_ERROR("MotionBlur: failed to compile one or more shader permutations");
        return false;
    }

    if (!m_cb.Create(device))          return false;
    if (!m_quad.Init(device, shaders)) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    HRESULT hr = device->CreateSamplerState(&sd, m_linearSampler.GetAddressOf());
    if (FAILED(hr))
    {
        SE_LOG_ERROR("MotionBlur: CreateSamplerState failed (0x%08X)", hr);
        return false;
    }

    initRTs(device, width, height);
    SE_LOG_INFO("MotionBlur initialised — %ux%u (%ux%u tiles)",
                width, height, m_tileMaxRT.GetWidth(), m_tileMaxRT.GetHeight());
    return true;
}

void MotionBlur::Resize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    shutdownRTs();
    initRTs(device, width, height);
}

void MotionBlur::Shutdown()
{
    shutdownRTs();
    m_quad.Shutdown();
    m_linearSampler.Reset();
    m_tileMaxPerm = m_neighborMaxPerm = m_gatherPerm = nullptr;
}

void MotionBlur::Apply(ID3D11DeviceContext* ctx, RenderTarget& sceneHDR,
                       ID3D11ShaderResourceView* velocity,
                       ID3D11ShaderResourceView* depth,
                       const MotionBlurParams& params,
                       DirectX::XMMATRIX invViewProj,
                       DirectX::XMMATRIX prevViewProj,
                       float nearZ, float farZ)
{
    if (!params.enabled || !velocity || !depth)
        return;

    const uint32_t w = sceneHDR.GetWidth();
    const uint32_t h = sceneHDR.GetHeight();
    if (w == 0 || h == 0)
        return;

    if (m_blurRT.GetWidth() != w || m_blurRT.GetHeight() != h)
    {
        ComPtr<ID3D11Device> device;
        ctx->GetDevice(device.GetAddressOf());
        if (!device)
            return;
        Resize(device.Get(), w, h);
    }

    MotionBlurCB cb = {};
    DirectX::XMStoreFloat4x4(&cb.invViewProj,  invViewProj);
    DirectX::XMStoreFloat4x4(&cb.prevViewProj, prevViewProj);
    cb.targetSizeX    = static_cast<float>(w);
    cb.targetSizeY    = static_cast<float>(h);
    cb.rcpTargetSizeX = 1.0f / cb.targetSizeX;
    cb.rcpTargetSizeY = 1.0f / cb.targetSizeY;
    cb.tileSizeX      = static_cast<float>(m_tileMaxRT.GetWidth());
    cb.tileSizeY      = static_cast<float>(m_tileMaxRT.GetHeight());
    cb.shutterScale   = (std::clamp)(params.shutterAngle, 1.0f, 360.0f) / 360.0f;
    cb.maxBlurPixels  = (std::clamp)(params.maxBlurPixels, 1.0f, 128.0f);
    cb.nearZ          = (std::max)(nearZ, 1e-4f);
    cb.farZ           = (std::max)(farZ, cb.nearZ + 1e-3f);
    cb.sampleCount    = (std::clamp)(params.samples, 4, 32);
    m_cb.Update(ctx, cb);
    m_cb.BindPS(ctx, 0);

    ctx->PSSetSamplers(1, 1, m_linearSampler.GetAddressOf());

    auto unbindSRVs = [&]()
    {
        ID3D11ShaderResourceView* nullSRVs[4] = {};
        ctx->PSSetShaderResources(0, 4, nullSRVs);
    };

    // Pass 0: Tile max velocity (16x16 tiles)
    {
        bindDest(ctx, m_tileMaxRT);
        ID3D11ShaderResourceView* srvs[2] = { velocity, depth };
        ctx->PSSetShaderResources(0, 2, srvs);
        m_quad.Draw(ctx, m_tileMaxPerm);
        unbindSRVs();
    }

    // Pass 1: 3x3 neighbour max velocity
    {
        bindDest(ctx, m_neighborMaxRT);
        ID3D11ShaderResourceView* srv = m_tileMaxRT.GetSRV();
        ctx->PSSetShaderResources(0, 1, &srv);
        m_quad.Draw(ctx, m_neighborMaxPerm);
        unbindSRVs();
    }

    // Pass 2: Depth-aware reconstruction gather
    {
        bindDest(ctx, m_blurRT);
        ID3D11ShaderResourceView* srvs[4] = {
            sceneHDR.GetSRV(),
            velocity,
            depth,
            m_neighborMaxRT.GetSRV()
        };
        ctx->PSSetShaderResources(0, 4, srvs);
        m_quad.Draw(ctx, m_gatherPerm);
        unbindSRVs();
    }

    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

    ctx->CopyResource(sceneHDR.GetTexture(), m_blurRT.GetTexture());
}

void MotionBlur::initRTs(ID3D11Device* device, uint32_t w, uint32_t h)
{
    const uint32_t safeW = (std::max)(1u, w);
    const uint32_t safeH = (std::max)(1u, h);
    const uint32_t tileW = (std::max)(1u, (safeW + k_tileSize - 1u) / k_tileSize);
    const uint32_t tileH = (std::max)(1u, (safeH + k_tileSize - 1u) / k_tileSize);

    m_tileMaxRT.Init(device, tileW, tileH, DXGI_FORMAT_R16G16_FLOAT);
    m_neighborMaxRT.Init(device, tileW, tileH, DXGI_FORMAT_R16G16_FLOAT);
    m_blurRT.Init(device, safeW, safeH, DXGI_FORMAT_R16G16B16A16_FLOAT);
}

void MotionBlur::shutdownRTs()
{
    m_tileMaxRT.Shutdown();
    m_neighborMaxRT.Shutdown();
    m_blurRT.Shutdown();
}

void MotionBlur::bindDest(ID3D11DeviceContext* ctx, RenderTarget& rt)
{
    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(rt.GetWidth());
    vp.Height   = static_cast<float>(rt.GetHeight());
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ID3D11RenderTargetView* rtv = rt.GetRTV();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
}

} // namespace SE
