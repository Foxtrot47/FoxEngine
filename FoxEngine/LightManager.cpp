#include "LightManager.h"

LightManager::LightManager(Graphics& gfx): isDirty(true)
{
    lightBuffer = {};
    lightBuffer.activeLightCount = 0;
    lightBuffer.ambientLight = {0.01f, 0.01f, 0.01f};
    lightBuffer.globalSpecularIntensity = 1.0f;

    lightMatrices = {};

    lightCBuff = std::make_unique<PixelConstantBuffer<LightBuffer>>(gfx, 0u);
    lightMatrixCBuff = std::make_unique<PixelConstantBuffer<LightShadowMatrices>>(gfx, 1u);
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
    lightMatrices.lightViewProj = CalculateLightMatrix(lightIndex);
    lightBuffer.lights[lightIndex] = lightData;
	isDirty = true;
}

DirectX::XMMATRIX LightManager::CalculateLightMatrix(const int lightIndex)
{
    if (lightIndex < 0 || lightIndex >= lightBuffer.activeLightCount)
        return DirectX::XMMatrixIdentity();

    auto center = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
	DirectX::XMVECTOR lightDirection = DirectX::XMLoadFloat3(&lightBuffer.lights[lightIndex].direction);
    lightDirection = DirectX::XMVector3Normalize(lightDirection);

    DirectX::XMVECTOR lightPosition = DirectX::XMVectorSubtract(
        DirectX::XMLoadFloat3(&center),
        DirectX::XMVectorScale(lightDirection, 400.0f)
    );

    DirectX::XMVECTOR upVector = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH(
        lightPosition,
        DirectX::XMLoadFloat3(&center),
        upVector
    );

    float shadowDistance = 500.0f;  // Very large
    float orthoSize = 5000.0f;       // Very large
    auto projMatrix = DirectX::XMMatrixOrthographicLH(
        orthoSize,
        orthoSize,
        0.1f,
        shadowDistance
    );

    return viewMatrix * projMatrix;
}

void LightManager::Bind(Graphics& gfx) const
{
    lightCBuff->Bind(gfx);
    lightMatrixCBuff->Bind(gfx);
}

void LightManager::Update(Graphics& gfx)
{
    if (isDirty)
    {
        lightCBuff->Update(gfx, lightBuffer);
        lightMatrixCBuff->Update(gfx, lightMatrices);
        isDirty = false;
    } 
}
