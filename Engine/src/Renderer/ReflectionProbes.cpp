#include "Engine/Renderer/ReflectionProbes.h"
#include "Engine/Core/Logger.h"
#include "Engine/Renderer/EnvironmentLighting.h"

namespace SE {

bool ReflectionProbes::Init(ID3D11Device* device, ShaderLibrary& shaders)
{
    m_toEquirectPerm = shaders.Get(L"Engine/Shaders/CubeToEquirect.hlsl");
    if (!m_toEquirectPerm)
    {
        SE_LOG_ERROR("ReflectionProbes: CubeToEquirect.hlsl failed");
        return false;
    }
    if (!m_quad.Init(device, shaders) || !m_cb.Create(device))
        return false;

    // Capture cube: 6-slice HDR array with the TextureCube flag, one RTV per face.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = k_faceSize;
    td.Height           = k_faceSize;
    td.MipLevels        = 1;
    td.ArraySize        = 6;
    td.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags        = D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &m_cubeTex)))
        return false;
    for (int f = 0; f < 6; ++f)
    {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format                         = td.Format;
        rd.ViewDimension                  = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.FirstArraySlice = static_cast<UINT>(f);
        rd.Texture2DArray.ArraySize       = 1;
        if (FAILED(device->CreateRenderTargetView(m_cubeTex.Get(), &rd, &m_cubeRTV[f])))
            return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC cubeSrv = {};
    cubeSrv.Format                = td.Format;
    cubeSrv.ViewDimension         = D3D11_SRV_DIMENSION_TEXTURECUBE;
    cubeSrv.TextureCube.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(m_cubeTex.Get(), &cubeSrv, &m_cubeSRV)))
        return false;

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width            = k_faceSize;
    dd.Height           = k_faceSize;
    dd.MipLevels        = 1;
    dd.ArraySize        = 1;
    dd.Format           = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.Usage            = D3D11_USAGE_DEFAULT;
    dd.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&dd, nullptr, &m_depthTex)) ||
        FAILED(device->CreateDepthStencilView(m_depthTex.Get(), nullptr, &m_dsv)))
        return false;

    if (!m_equirectRT.Init(device, k_width, k_height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                           /*withDepth=*/false, /*depthReadable=*/false, /*generateMips=*/true))
        return false;

    // Prefiltered probe array: one slice per probe, RTV per slice and mip.
    D3D11_TEXTURE2D_DESC ad = {};
    ad.Width            = k_width;
    ad.Height           = k_height;
    ad.MipLevels        = k_mipCount;
    ad.ArraySize        = k_maxReflectionProbes;
    ad.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ad.SampleDesc.Count = 1;
    ad.Usage            = D3D11_USAGE_DEFAULT;
    ad.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&ad, nullptr, &m_arrayTex)) ||
        FAILED(device->CreateShaderResourceView(m_arrayTex.Get(), nullptr, &m_arraySRV)))
        return false;
    for (uint32_t slice = 0; slice < k_maxReflectionProbes; ++slice)
        for (uint32_t mip = 0; mip < k_mipCount; ++mip)
        {
            D3D11_RENDER_TARGET_VIEW_DESC rd = {};
            rd.Format                         = ad.Format;
            rd.ViewDimension                  = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.MipSlice        = mip;
            rd.Texture2DArray.FirstArraySlice = slice;
            rd.Texture2DArray.ArraySize       = 1;
            if (FAILED(device->CreateRenderTargetView(m_arrayTex.Get(), &rd,
                                                      &m_arrayRTVs[slice][mip])))
                return false;
        }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device->CreateSamplerState(&sd, &m_linearClamp));
}

void ReflectionProbes::SetProbes(const std::vector<Probe>& probes)
{
    m_probes = probes;
    if (m_probes.size() > k_maxReflectionProbes)
    {
        SE_LOG_WARN("only the first 8 reflection probes are used");
        m_probes.resize(k_maxReflectionProbes);
    }
}

void ReflectionProbes::BeginFace(ID3D11DeviceContext* ctx, int face)
{
    // The probe array may still be bound as t28 from the previous frame.
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(28, 1, &nullSRV);

    ID3D11RenderTargetView* rtv = m_cubeRTV[face].Get();
    ctx->OMSetRenderTargets(1, &rtv, m_dsv.Get());
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ctx->ClearRenderTargetView(rtv, black);
    ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(k_faceSize);
    vp.Height   = static_cast<float>(k_faceSize);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
}

void ReflectionProbes::ResolveProbe(ID3D11DeviceContext* ctx, uint32_t probe,
                                    EnvironmentLighting& environment)
{
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(28, 1, &nullSRV);

    m_equirectRT.Begin(ctx);
    ctx->PSSetShaderResources(0, 1, m_cubeSRV.GetAddressOf());
    ctx->PSSetSamplers(1, 1, m_linearClamp.GetAddressOf());
    m_quad.Draw(ctx, m_toEquirectPerm);
    ctx->PSSetShaderResources(0, 1, &nullSRV);
    m_equirectRT.End(ctx);
    m_equirectRT.GenerateMips(ctx);

    environment.FilterSpecular(ctx, m_equirectRT.GetSRV(), m_arrayRTVs[probe],
                               k_width, k_height, k_mipCount);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

void ReflectionProbes::Bind(ID3D11DeviceContext* ctx, bool withProbes)
{
    ProbeCBData data = {};
    const uint32_t count = withProbes ? GetCount() : 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const Probe& p = m_probes[i];
        data.center[i]  = { p.position.x, p.position.y, p.position.z, 0.0f };
        data.extents[i] = { p.extents.x, p.extents.y, p.extents.z, p.blendDistance };
    }
    data.info = { static_cast<float>(count), static_cast<float>(k_mipCount), 0.0f, 0.0f };
    m_cb.Update(ctx, data);
    m_cb.BindPS(ctx, 9);
    ID3D11ShaderResourceView* srv = count > 0 ? m_arraySRV.Get() : nullptr;
    ctx->PSSetShaderResources(28, 1, &srv);
}

} // namespace SE
