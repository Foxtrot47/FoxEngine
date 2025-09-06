#include "LightManager.h"

LightManager::LightManager(Graphics& gfx): isDirty(true)
{
    lightBuffer = {};
    lightBuffer.activeLightCount = 0;
    lightBuffer.ambientLight = {0.01f, 0.01f, 0.01f};
    lightBuffer.globalSpecularIntensity = 1.0f;

    lightCBuff = std::make_unique<PixelConstantBuffer<LightBuffer>>(gfx, 0u);
}

int LightManager::AddPointLight(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& color, float intensity,
                                float range)
{
    if (lightBuffer.activeLightCount >= MAX_LIGHTS)
        return -1;

    int lightIndex = lightBuffer.activeLightCount;
    Light& light = lightBuffer.lights[lightIndex];
    
    light.position = position;
    light.type = static_cast<int>(LightType::POINT);
    light.direction = { 0.0f, 0.0f, 0.0f };
    light.range = range;
    light.color = color;
    light.intensity = intensity;

    lightBuffer.activeLightCount++;
    isDirty = true;
    return lightIndex;
}

int LightManager::AddDirectionalLight(const DirectX::XMFLOAT3& direction, const DirectX::XMFLOAT3& color,
    float intensity)
{
    if (lightBuffer.activeLightCount >= MAX_LIGHTS)
        return -1;

    int lightIndex = lightBuffer.activeLightCount;
    Light& light = lightBuffer.lights[lightIndex];
    
    light.position = { 0.0f, 0.0f, 0.0f };
    light.type = static_cast<int>(LightType::DIRECTIONAL);
    light.direction = direction;
    light.range = 0.0f;
    light.color = color;
    light.intensity = intensity;

    lightBuffer.activeLightCount++;
    isDirty = true;
    return lightIndex;
}

void LightManager::UpdateLight(int lightIndex, Light lightData)
{
    if (lightIndex < 0 || lightIndex >= lightBuffer.activeLightCount)
        return;
    lightBuffer.lights[lightIndex] = lightData;
	isDirty = true;
}

void LightManager::Bind(Graphics& gfx) const
{
    lightCBuff->Bind(gfx);
}

void LightManager::Update(Graphics& gfx)
{
    if (isDirty)
    {
        lightCBuff->Update(gfx, lightBuffer);
        isDirty = false;
    } 
}
