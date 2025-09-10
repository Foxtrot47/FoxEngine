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
    lightMatrices.lightViewProj = DirectX::XMMatrixTranspose(CalculateLightMatrix(lightIndex));
    lightBuffer.lights[lightIndex] = lightData;
	isDirty = true;
}

DirectX::XMMATRIX LightManager::CalculateLightMatrix(const int lightIndex)
{
    if (lightIndex < 0 || lightIndex >= lightBuffer.activeLightCount)
        return DirectX::XMMatrixIdentity();

    const Light& light = lightBuffer.lights[lightIndex];
    DirectX::XMVECTOR lightPos = DirectX::XMLoadFloat3(&light.position);
    DirectX::XMVECTOR center = DirectX::XMVectorSet(
        light.position.x + 70.0f,  // Look 70 units forward in X
        light.position.y - 30.0f,  // Look 30 units down
        light.position.z + 50.0f,  // Look 50 units forward in Z
        0.0f
    );
    DirectX::XMVECTOR upVector = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH(
        lightPos,
        center,
        upVector
    );

    // Use perspective projection for point lights
    DirectX::XMMATRIX projMatrix = DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XM_PI / 2.0f,
        1.0f,
        1.0f,
        light.range
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
        lightMatrices.lightViewProj = DirectX::XMMatrixTranspose(CalculateLightMatrix(0));
        lightMatrixCBuff->Update(gfx, lightMatrices);
        lightCBuff->Update(gfx, lightBuffer);
        isDirty = false;
    }
}
