#include "Engine/Renderer/VolumetricFog.h"
#include "Engine/Core/Logger.h"

using namespace DirectX;

namespace SE {

bool VolumetricFog::Init(ID3D11Device* device, ShaderLibrary& shaders,
                         uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;
    m_fogWidth  = width  > 1 ? width  / 2 : 1;
    m_fogHeight = height > 1 ? height / 2 : 1;

    // Half-res RGBA16F: RGB = accumulated in-scattered light (HDR), A = transmittance.
    // The march is ALU-bound, so quarter the pixel count and reconstruct with a
    // depth-aware upsample at composite time.
    if (!m_fogRT.Init(device, m_fogWidth, m_fogHeight, DXGI_FORMAT_R16G16B16A16_FLOAT))
        return false;

    m_fogPerm = shaders.Get(L"Engine/Shaders/VolumetricFog.hlsl");
    if (!m_fogPerm) { SE_LOG_ERROR("VolumetricFog: failed to compile VolumetricFog.hlsl"); return false; }

    m_upsamplePerm = shaders.Get(L"Engine/Shaders/VolumetricFogUpsample.hlsl");
    if (!m_upsamplePerm) { SE_LOG_ERROR("VolumetricFog: failed to compile VolumetricFogUpsample.hlsl"); return false; }

    if (!m_cb.Create(device)) return false;
    if (!m_upsampleCB.Create(device)) return false;
    if (!m_quad.Init(device, shaders)) return false;

    // Point-clamp sampler for exact depth, shadow, and blur taps (s0).
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    HRESULT hr = device->CreateSamplerState(&sd, m_pointSampler.GetAddressOf());
    if (FAILED(hr)) { SE_LOG_ERROR("VolumetricFog: sampler failed"); return false; }

    D3D11_SAMPLER_DESC skySd = {};
    skySd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    skySd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    skySd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    skySd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    skySd.MaxLOD   = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&skySd, m_skySampler.GetAddressOf());
    if (FAILED(hr)) { SE_LOG_ERROR("VolumetricFog: sky sampler failed"); return false; }

    // Composite blend: Final = Src.rgb + Dest * Src.a
    //   (Src = fog: in-scatter in rgb, transmittance in alpha)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bd, m_compositeBlend.GetAddressOf());
    if (FAILED(hr)) { SE_LOG_ERROR("VolumetricFog: blend state failed"); return false; }

    SE_LOG_INFO("VolumetricFog initialised — march %ux%u, output %ux%u",
                m_fogWidth, m_fogHeight, m_width, m_height);
    return true;
}

void VolumetricFog::Resize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;
    m_fogWidth  = width  > 1 ? width  / 2 : 1;
    m_fogHeight = height > 1 ? height / 2 : 1;
    m_fogRT.Shutdown();
    m_fogRT.Init(device, m_fogWidth, m_fogHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
}

void VolumetricFog::Shutdown()
{
    m_fogRT.Shutdown();
    m_pointSampler.Reset();
    m_skySampler.Reset();
    m_compositeBlend.Reset();
}

void VolumetricFog::Apply(ID3D11DeviceContext* ctx,
                          RenderTarget& hdrRT,
                          ID3D11ShaderResourceView* depthSRV,
                          ID3D11ShaderResourceView* csmSRV,
                          ID3D11SamplerState* csmSampler,
                          ID3D11ShaderResourceView* skySRV,
                          ID3D11ShaderResourceView* spotShadowSRV,
                          XMMATRIX view,
                          XMMATRIX proj,
                          XMFLOAT3 cameraPos,
                          XMFLOAT3 lightDir,
                          XMFLOAT3 lightColor,
                          XMFLOAT3 spotPosition,
                          XMFLOAT3 spotDirection,
                          XMFLOAT3 spotColor,
                          float spotRange,
                          float spotInnerCos,
                          float spotOuterCos,
                          float spotShadowBias,
                          float spotVolumetricIntensity,
                          int spotEnabled,
                          XMMATRIX spotViewProj,
                          const XMMATRIX cascadeVP[3],
                          const float splitFar[2])
{
    if (!enabled) return;

    XMMATRIX viewProj    = XMMatrixMultiply(view, proj);
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

    FogCBData cb = {};
    XMStoreFloat4x4(&cb.invViewProj, invViewProj);
    XMStoreFloat4x4(&cb.view, view);
    for (int i = 0; i < 3; ++i)
        XMStoreFloat4x4(&cb.cascadeVP[i], cascadeVP[i]);
    cb.cascadeSplits    = { splitFar[0], splitFar[1], 0.0f, 0.0f };
    cb.cameraPos        = cameraPos;
    cb.density          = density;
    cb.lightDir         = lightDir;
    cb.anisotropy       = anisotropy;
    cb.lightColor       = lightColor;
    cb.stepCount        = (float)(steps < 1 ? 1 : steps);
    cb.fogAmbient       = { ambientColor[0], ambientColor[1], ambientColor[2] };
    cb.ambientIntensity = ambientIntensity;
    cb.screenSize       = { (float)m_fogWidth, (float)m_fogHeight };
    cb.maxDistance      = maxDistance;
    cb.scatterIntensity = scatterIntensity;
    cb.heightFalloff    = heightFalloff;
    cb.heightBase       = heightBase;
    cb.noiseStrength    = noiseStrength;
    cb.noiseScale       = noiseScale;
    cb.noiseTime        = m_noiseTime;
    cb.nearClearDistance = nearClearDistance;
    cb.distanceDensity   = distanceDensity;
    XMStoreFloat4x4(&cb.spotViewProj, spotViewProj);
    cb.spotPosition    = spotPosition;
    cb.spotRange       = spotRange;
    cb.spotDirection   = spotDirection;
    cb.spotOuterCos    = spotOuterCos;
    cb.spotColor       = spotColor;
    cb.spotInnerCos    = spotInnerCos;
    cb.spotShadowBias  = spotShadowBias;
    cb.spotVolumetricIntensity = spotVolumetricIntensity;
    cb.spotEnabled     = spotEnabled;
    m_noiseTime += noiseSpeed * (1.0f / 60.0f);
    m_cb.Update(ctx, cb);

    // --- Pass 1: ray-march fog into m_fogRT ---
    {
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // transmittance starts at 1
        m_fogRT.Begin(ctx, clear);

        ctx->VSSetShader(m_fogPerm->vs.Get(), nullptr, 0);
        ctx->PSSetShader(m_fogPerm->ps.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[4] = { depthSRV, csmSRV, skySRV, spotShadowSRV };
        ctx->PSSetShaderResources(0, 4, srvs);
        ID3D11SamplerState* samplers[3] = { m_pointSampler.Get(), csmSampler, m_skySampler.Get() };
        ctx->PSSetSamplers(0, 3, samplers);

        m_cb.BindPS(ctx, 0);
        m_quad.DrawGeometryOnly(ctx);

        ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
        ctx->PSSetShaderResources(0, 4, nullSRVs);
        m_fogRT.End(ctx);
    }

    // --- Pass 2: depth-aware upsample + composite into HDR buffer ---
    {
        UpsampleCBData up = {};
        up.lowResSize    = { (float)m_fogWidth, (float)m_fogHeight };
        up.invLowResSize = { 1.0f / (float)m_fogWidth, 1.0f / (float)m_fogHeight };
        m_upsampleCB.Update(ctx, up);

        ID3D11RenderTargetView* rtv = hdrRT.GetRTV();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {};
        vp.Width    = (float)hdrRT.GetWidth();
        vp.Height   = (float)hdrRT.GetHeight();
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);

        float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ctx->OMSetBlendState(m_compositeBlend.Get(), blendFactor, 0xFFFFFFFF);

        ID3D11ShaderResourceView* srvs[2] = { m_fogRT.GetSRV(), depthSRV };
        ctx->PSSetShaderResources(0, 2, srvs);
        m_upsampleCB.BindPS(ctx, 0);
        m_quad.Draw(ctx, m_upsamplePerm);

        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        ctx->PSSetShaderResources(0, 2, nullSRVs);
        ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    }
}

} // namespace SE
