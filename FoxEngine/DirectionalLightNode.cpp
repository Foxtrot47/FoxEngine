#include "DirectionalLightNode.h"
#include "imgui.h"
#include "Math.h"

DirectionalLightNode::DirectionalLightNode(
	Graphics& gfx,
	SceneNode* parent,
	std::shared_ptr<LightManager> lightManager,
	std::optional<std::string> name,
	const DirectX::XMFLOAT3& direction,
	const DirectX::XMFLOAT3& color,
	float intensity
)
	:
	SceneNode(parent, name),
	valuesChanged(true),
	lightManager(lightManager),
	lightDirection(direction),
	lightColor(color),
	lightIntensity(intensity)
{
	lightManager->SetDirectionalLight(direction, color, intensity);
}

void DirectionalLightNode::DrawInspectorWindow()
{
	ImGui::Text("Directional Light Properties");
	static DirectX::XMFLOAT3 eulerAngles = { 0.0f, 0.0f, 0.0f };

	if (ImGui::DragFloat2("Light Direction", &eulerAngles.x, 0.5f, -180.0f, 180.0f))
	{
		float pitch = DirectX::XMConvertToRadians(eulerAngles.x);
		float yaw = DirectX::XMConvertToRadians(eulerAngles.y);

		DirectX::XMFLOAT3 dir;
		dir.x = cosf(pitch) * sinf(yaw);
		dir.y = -sinf(pitch);
		dir.z = cosf(pitch) * cosf(yaw);

		DirectX::XMVECTOR v = DirectX::XMLoadFloat3(&dir);
		v = DirectX::XMVector3Normalize(v);
		DirectX::XMStoreFloat3(&lightDirection, v);

		valuesChanged = true;
	}

	if (ImGui::ColorEdit3("Light Color", reinterpret_cast<float*>(&lightColor)))
		valuesChanged = true;

	if (ImGui::SliderFloat("Light Intensity", &lightIntensity, 0.0f, 10.0f))
		valuesChanged = true;

	if (ImGui::Button("Reset"))
	{
		lightDirection = { -0.3f, -0.8f, 0.5f };
		lightColor = { 1.0f, 1.0f, 1.0f };
		lightIntensity = 1.0f;
		valuesChanged = true;
	}

	if (valuesChanged)
	{
		UpdateLightManager();
		valuesChanged = false;
	}
}

void DirectionalLightNode::Draw(Graphics& gfx)
{
	if (isTransformDirty)
	{
		UpdateLightManager();
	}
	for (auto& child : children)
	{
		child->Draw(gfx);
	}
}

void DirectionalLightNode::UpdateLightManager()
{
	LightManager::DirectionalLight light = {};
	light.direction = lightDirection;
	light.color = lightColor;
	light.intensity = lightIntensity;

	lightManager->UpdateDirectionLight(light);
}
