#include "LightManager.h"

LightManager::LightManager(Graphics& gfx) : isDirty(true)
{
	lightBuffer = {};
	lightBuffer.activePointLightCount = 0;
	lightBuffer.ambientLight = { 0.01f, 0.01f, 0.01f };
	lightBuffer.globalSpecularIntensity = 1.0f;
	lightBuffer.hasDirectionalLight = 0;

	lightMatrices = {};

	lightCBuff = std::make_unique<PixelConstantBuffer<LightBuffer>>(gfx, 0u);
	lightMatrixCBuff = std::make_unique<PixelConstantBuffer<LightShadowMatrices>>(gfx, 1u);
}

int LightManager::AddPointLight(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& color, float intensity,
	float range)
{
	if (lightBuffer.activePointLightCount >= MAX_LIGHTS)
		return -1;

	int lightIndex = lightBuffer.activePointLightCount;
	PointLight& light = lightBuffer.pointLights[lightIndex];

	light.position = position;
	light.range = range;
	light.color = color;
	light.intensity = intensity;

	lightBuffer.activePointLightCount++;
	isDirty = true;
	return lightIndex;
}

void LightManager::SetDirectionalLight(const DirectX::XMFLOAT3& direction, const DirectX::XMFLOAT3& color,
	float intensity)
{
	lightBuffer.directionalLight.direction = direction;
	lightBuffer.directionalLight.color = color;
	lightBuffer.directionalLight.intensity = intensity;
	lightBuffer.hasDirectionalLight = 1;
	isDirty = true;
}

void LightManager::UpdatePointLight(int lightIndex, PointLight& lightData)
{
	if (lightIndex < 0 || lightIndex >= lightBuffer.activePointLightCount)
		return;
	lightMatrices.lightViewProj[lightIndex] = DirectX::XMMatrixTranspose(CalculatePointLightMatrix(lightIndex));
	lightBuffer.pointLights[lightIndex] = lightData;
	isDirty = true;
}

void LightManager::UpdateDirectionLight(DirectionalLight& lightData)
{
	lightBuffer.directionalLight = lightData;
	lightMatrices.directionalLightViewProj = DirectX::XMMatrixTranspose(CalculateDirectionalLightMatrix());
	isDirty = true;
}

DirectX::XMMATRIX LightManager::CalculatePointLightMatrix(const int lightIndex)
{
	if (lightIndex < 0 || lightIndex >= lightBuffer.activePointLightCount)
		return DirectX::XMMatrixIdentity();

	const PointLight& light = lightBuffer.pointLights[lightIndex];
	DirectX::XMVECTOR worldUp = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

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

DirectX::XMMATRIX LightManager::CalculateDirectionalLightMatrix()
{
	if (!lightBuffer.hasDirectionalLight)
		return DirectX::XMMatrixIdentity();

	DirectX::XMVECTOR worldUp = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	DirectX::XMVECTOR sceneCenter = DirectX::XMVectorSet(0.1f, 50.0f, 0.1f, 0.0f);
	DirectX::XMVECTOR lightDirection = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&lightBuffer.directionalLight.direction));
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

int LightManager::GetActiveLights() const
{
	return lightBuffer.activePointLightCount;
}

bool LightManager::HasDirectionalLight() const
{
	return lightBuffer.hasDirectionalLight != 0;
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
		for (int i = 0; i < lightBuffer.activePointLightCount; i++)
		{
			lightMatrices.lightViewProj[i] = DirectX::XMMatrixTranspose(CalculatePointLightMatrix(i));
		}

		if (lightBuffer.hasDirectionalLight)
		{
			lightMatrices.directionalLightViewProj = DirectX::XMMatrixTranspose(CalculateDirectionalLightMatrix());
		}

		lightMatrixCBuff->Update(gfx, lightMatrices);
		lightCBuff->Update(gfx, lightBuffer);
		isDirty = false;
	}
}
