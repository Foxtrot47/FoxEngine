#include "Engine/Renderer/LightEnvironment.h"
#include <cmath>

namespace SE {

bool LightEnvironment::Init(ID3D11Device* device)
{
    if (!m_lightCB.Create(device))      return false;
    if (!m_pointLightCB.Create(device)) return false;
    return true;
}

void LightEnvironment::BindPS(ID3D11DeviceContext* ctx, DirectX::XMFLOAT3 cameraPos,
                              DirectX::XMMATRIX lightViewProj,
                              const PointLight* extraLights, int extraCount)
{
    using namespace DirectX;

    float er = XMConvertToRadians(elevDeg);
    float ar = XMConvertToRadians(azimDeg);

    LightCBData lc;
    lc.lightDir      = { cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar) };
    lc.iblIntensity  = iblIntensity;
    const float sunScale = sunEnabled ? lightIntensity : 0.0f;
    lc.lightColor    = { lightColor[0] * sunScale,
                         lightColor[1] * sunScale,
                         lightColor[2] * sunScale };
    lc.diffuseIBLSaturation = diffuseIBLSaturation;
    lc.cameraPos     = cameraPos;
    lc.debugLightMode = debugLightMode;
    XMStoreFloat4x4(&lc.lightViewProj, lightViewProj);
    m_lightCB.Update(ctx, lc);
    m_lightCB.BindPS(ctx, 1);

    PointLightCBData pl = {};
    auto add = [&pl](const PointLight& light) {
        if (pl.num >= 8) return;
        PointEntryData& entry = pl.lights[pl.num++];
        entry.pos    = light.position;
        entry.radius = light.radius;
        entry.color  = light.enabled ? light.color : DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f };
        entry._pad   = 0.0f;
    };
    for (int i = 0; i < numLights; ++i)
        add(lights[i]);
    for (int i = 0; i < extraCount; ++i)
        add(extraLights[i]);
    m_pointLightCB.Update(ctx, pl);
    m_pointLightCB.BindPS(ctx, 2);
}

} // namespace SE
