#include "Engine/Renderer/EnvironmentLighting.h"
#include "Engine/Core/Logger.h"
#include <algorithm>
#include <cmath>

namespace SE {

bool EnvironmentLighting::Init(ID3D11Device* device, ShaderLibrary& shaders)
{
    m_filterPerm = shaders.Get(L"Engine/Shaders/EnvironmentFilter.hlsl");
    if (!m_filterPerm || !m_quad.Init(device, shaders) || !m_cb.Create(device))
        return false;

    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&desc, &m_sampler)))
        return false;

    return CreateBRDFLut(device, nullptr);
}

bool EnvironmentLighting::CreateFilteredTarget(
    ID3D11Device* device, uint32_t width, uint32_t height, uint32_t mipLevels,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv,
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>>& rtvs)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = mipLevels;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &texture)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = mipLevels;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &sd, &srv)))
        return false;

    rtvs.resize(mipLevels);
    for (uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = td.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = mip;
        if (FAILED(device->CreateRenderTargetView(texture.Get(), &rd, &rtvs[mip])))
            return false;
    }
    return true;
}

bool EnvironmentLighting::CreateBRDFLut(ID3D11Device* device, ID3D11DeviceContext*)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 128;
    td.Height = 128;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &m_brdfLutTexture)) ||
        FAILED(device->CreateRenderTargetView(m_brdfLutTexture.Get(), nullptr, &m_brdfLutRTV)) ||
        FAILED(device->CreateShaderResourceView(m_brdfLutTexture.Get(), nullptr, &m_brdfLutSRV)))
        return false;
    return true;
}

void EnvironmentLighting::DrawFilter(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                                     uint32_t width, uint32_t height,
                                     ID3D11ShaderResourceView* source, const FilterCB& data)
{
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    m_cb.Update(ctx, data);
    m_cb.BindPS(ctx, 0);
    ctx->PSSetShaderResources(0, 1, &source);
    ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_quad.Draw(ctx, m_filterPerm);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
}

bool EnvironmentLighting::Build(ID3D11Device* device, ID3D11DeviceContext* ctx,
                                ID3D11ShaderResourceView* panorama)
{
    Clear();
    if (!device || !ctx || !panorama)
        return false;

    constexpr uint32_t specWidth = 512;
    constexpr uint32_t specHeight = 256;
    constexpr uint32_t specMips = 9;
    if (!CreateFilteredTarget(device, 64, 32, 1, m_irradianceTexture,
                              m_irradianceSRV, m_irradianceRTVs) ||
        !CreateFilteredTarget(device, specWidth, specHeight, specMips, m_specularTexture,
                              m_specularSRV, m_specularRTVs))
    {
        Clear();
        return false;
    }

    ID3D11RenderTargetView* savedRTV = nullptr;
    ID3D11DepthStencilView* savedDSV = nullptr;
    D3D11_VIEWPORT savedVP = {};
    UINT viewportCount = 1;
    ctx->OMGetRenderTargets(1, &savedRTV, &savedDSV);
    ctx->RSGetViewports(&viewportCount, &savedVP);

    D3D11_SHADER_RESOURCE_VIEW_DESC sourceDesc = {};
    panorama->GetDesc(&sourceDesc);
    const float sourceMips = static_cast<float>((std::max)(1u, sourceDesc.Texture2D.MipLevels));
    DrawFilter(ctx, m_irradianceRTVs[0].Get(), 64, 32, panorama,
               { 1.0f, 1, 128, sourceMips });
    for (uint32_t mip = 0; mip < specMips; ++mip)
    {
        const float roughness = static_cast<float>(mip) / static_cast<float>(specMips - 1);
        DrawFilter(ctx, m_specularRTVs[mip].Get(),
                   (std::max)(1u, specWidth >> mip), (std::max)(1u, specHeight >> mip),
                   panorama, { roughness, 0, mip == 0 ? 1 : 96, sourceMips });
    }
    DrawFilter(ctx, m_brdfLutRTV.Get(), 128, 128, nullptr, { 0.0f, 2, 128, 1.0f });

    ctx->OMSetRenderTargets(1, &savedRTV, savedDSV);
    if (viewportCount > 0) ctx->RSSetViewports(1, &savedVP);
    if (savedRTV) savedRTV->Release();
    if (savedDSV) savedDSV->Release();
    SE_LOG_INFO("EnvironmentLighting: generated irradiance, GGX specular, and BRDF LUT");
    return true;
}

void EnvironmentLighting::Clear()
{
    m_irradianceRTVs.clear();
    m_irradianceSRV.Reset();
    m_irradianceTexture.Reset();
    m_specularRTVs.clear();
    m_specularSRV.Reset();
    m_specularTexture.Reset();
}

} // namespace SE
