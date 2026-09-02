#include "Engine/Renderer/TAA.h"
#include "Engine/Core/Logger.h"

namespace SE {

static float Halton(uint32_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    while (index > 0)
    {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

bool TAA::Init(ID3D11Device* device, ShaderLibrary& shaders, uint32_t width, uint32_t height)
{
    m_perm = shaders.Get(L"Engine/Shaders/TAA.hlsl");
    if (!m_perm) { SE_LOG_ERROR("TAA: failed to load shader"); return false; }

    if (!m_cb.Create(device))          return false;
    if (!m_quad.Init(device, shaders)) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &m_linearSampler))) return false;

    Resize(device, width, height);
    return true;
}

void TAA::Resize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_resolveRT.Shutdown();
    m_historyRT.Shutdown();
    m_resolveRT.Init(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_historyRT.Init(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_historyValid = false;
}

DirectX::XMFLOAT2 TAA::NextJitter(uint32_t width, uint32_t height)
{
    m_jitterIndex = (m_jitterIndex % 8) + 1; // Halton indices 1..8
    const float jx = Halton(m_jitterIndex, 2) - 0.5f;
    const float jy = Halton(m_jitterIndex, 3) - 0.5f;
    return { jx * 2.0f / static_cast<float>(width),
             jy * 2.0f / static_cast<float>(height) };
}

void TAA::Apply(ID3D11DeviceContext* ctx, RenderTarget& sceneRT,
                ID3D11ShaderResourceView* depthSRV,
                ID3D11ShaderResourceView* velocitySRV,
                DirectX::XMMATRIX invViewProj,
                DirectX::XMMATRIX prevViewProj)
{
    const uint32_t w = sceneRT.GetWidth(), h = sceneRT.GetHeight();

    CBData cb = {};
    DirectX::XMStoreFloat4x4(&cb.invViewProj,  invViewProj);
    DirectX::XMStoreFloat4x4(&cb.prevViewProj, prevViewProj);
    cb.rcpFrameX   = 1.0f / static_cast<float>(w);
    cb.rcpFrameY   = 1.0f / static_cast<float>(h);
    cb.blendFactor = blendFactor;
    cb.reset       = m_historyValid ? 0.0f : 1.0f;
    cb.clampGamma  = clampGamma;
    m_cb.Update(ctx, cb);
    m_cb.BindPS(ctx, 0);

    ID3D11RenderTargetView* rtv = m_resolveRT.GetRTV();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(w);
    vp.Height = static_cast<float>(h);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    ID3D11ShaderResourceView* srvs[4] = {
        sceneRT.GetSRV(), m_historyRT.GetSRV(), velocitySRV, depthSRV
    };
    ctx->PSSetShaderResources(0, 4, srvs);
    ctx->PSSetSamplers(1, 1, m_linearSampler.GetAddressOf());

    m_quad.Draw(ctx, m_perm);

    ID3D11ShaderResourceView* nullSRVs[4] = {};
    ctx->PSSetShaderResources(0, 4, nullSRVs);
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

    // Resolved frame becomes both the new history and the scene image.
    ctx->CopySubresourceRegion(m_historyRT.GetTexture(), 0, 0, 0, 0,
                               m_resolveRT.GetTexture(), 0, nullptr);
    ctx->CopySubresourceRegion(sceneRT.GetTexture(), 0, 0, 0, 0,
                               m_resolveRT.GetTexture(), 0, nullptr);
    m_historyValid = true;
}

} // namespace SE
