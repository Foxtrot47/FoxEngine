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
	lightMatrices.lightViewProj[lightIndex] = DirectX::XMMatrixTranspose(CalculateLightMatrix(lightIndex));
	lightBuffer.lights[lightIndex] = lightData;
	isDirty = true;
}

DirectX::XMMATRIX LightManager::CalculateLightMatrix(const int lightIndex)
{
	if (lightIndex < 0 || lightIndex >= lightBuffer.activeLightCount)
		return DirectX::XMMatrixIdentity();

	const Light& light = lightBuffer.lights[lightIndex];
	DirectX::XMVECTOR worldUp = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	if (light.type == static_cast<int>(LightType::POINT))
	{
		DirectX::XMVECTOR sceneCenter = DirectX::XMVectorSet(0.1f, 0.1f, 0.1f, 0.0f);
		DirectX::XMVECTOR lightPos = DirectX::XMLoadFloat3(&light.position);
		DirectX::XMVECTOR lightToTarget = DirectX::XMVector3Normalize(lightPos);

		DirectX::XMVECTOR baseUp = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
		// Calculate proper orthogonal up vector
		DirectX::XMVECTOR rightVector = DirectX::XMVector3Cross(lightToTarget, baseUp);
		DirectX::XMVECTOR upVector = DirectX::XMVector3Cross(rightVector, lightToTarget);
		upVector = DirectX::XMVector3Normalize(upVector);

		DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH(
			lightPos,
			sceneCenter,
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
	else if (light.type == static_cast<int>(LightType::DIRECTIONAL))
	{
		DirectX::XMVECTOR sceneCenter = DirectX::XMVectorSet(0.1f, 50.0f, 0.1f, 0.0f);
		DirectX::XMVECTOR lightDirection = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&light.direction));
		DirectX::XMVECTOR lightPos = DirectX::XMVectorSubtract(sceneCenter, DirectX::XMVectorScale(lightDirection, 850.0f));

		DirectX::XMVECTOR rightVector = DirectX::XMVector3Cross(lightDirection, worldUp);
		float rightLength = DirectX::XMVector3Length(rightVector).m128_f32[0];

		// Handle edge case when light direction is parallel to world up
		if (rightLength < 0.001f) {
			// Use alternative up vector
			DirectX::XMVECTOR altUp = DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
			rightVector = DirectX::XMVector3Cross(lightDirection, altUp);
			rightLength = DirectX::XMVector3Length(rightVector).m128_f32[0];

			// If still parallel, try another up vector
			if (rightLength < 0.001f) {
				altUp = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
				rightVector = DirectX::XMVector3Cross(lightDirection, altUp);
			}
		}
		rightVector = DirectX::XMVector3Normalize(rightVector);
		DirectX::XMVECTOR lightUp = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(rightVector, lightDirection));

		DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH(
			lightPos,
			sceneCenter,
			lightUp
		);

		DirectX::XMMATRIX projMatrix = DirectX::XMMatrixOrthographicLH(
			500.0f,     // Ortho width
			500.0f,     // Ortho height
			0.1f,       // Near plane at minimum
			1000.0f     // Far plane
		);

		return viewMatrix * projMatrix;
	}
}

int LightManager::GetActiveLights() const
{
	return lightBuffer.activeLightCount;
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
		for (int i = 0; i < lightBuffer.activeLightCount; i++)
		{
			lightMatrices.lightViewProj[i] = DirectX::XMMatrixTranspose(CalculateLightMatrix(i));
		}
		lightMatrixCBuff->Update(gfx, lightMatrices);
		lightCBuff->Update(gfx, lightBuffer);
		isDirty = false;
	}
}
