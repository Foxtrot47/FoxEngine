#include "PointLightNode.h"
#include "imgui.h"
#include "Math.h"

PointLightNode::PointLightNode(
	Graphics& gfx,
	SceneNode* parent,
	LightManager& lightManager,
	std::optional<std::string> name,
	const DirectX::XMFLOAT3& initialPosition,
	const DirectX::XMFLOAT3& color,
	float intensity,
	float range
)
	:
	SceneNode(parent, name, initialPosition, { 0.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }),
	valuesChanged(true),
	lightManager(lightManager),
	lightColor(color),
	lightIntensity(intensity),
	lightRange(range)
{
	lightIndex = lightManager.AddPointLight(initialPosition, color, intensity, range);
	sphere = std::make_unique<SolidSphere>(gfx, 0.3f, 20, 20);
}

void PointLightNode::DrawInspectorWindow()
{
	ImGui::Text("Point Light Properties"); 
	ImGui::Text("Transform");
	if (ImGui::DragFloat3("Position", &position.x, 0.1f))
	{
		valuesChanged = true;
		MarkDirty();	
	}
	// no rotation and scale cuz it makes no sense

	if (ImGui::ColorEdit3("Light Color", reinterpret_cast<float*>(&lightColor)))
		valuesChanged = true;

	if (ImGui::SliderFloat("Light Intensity", &lightIntensity, 0.0f, 100.0f))
		valuesChanged = true;

	if (ImGui::SliderFloat("Range", &lightRange, 1.0f, 500.0f))
		valuesChanged = true;

	if (ImGui::Button("Reset"))
	{
		lightColor = { 1.0f, 1.0f, 1.0f };
		lightIntensity = 1.0f;
		lightRange = 100.0f;
		valuesChanged = true;
	}

	if (valuesChanged)
	{
		UpdateLightManager();
		valuesChanged = false;
	}
}

void PointLightNode::Draw(Graphics& gfx)
{
	if (isTransformDirty)
	{
		UpdateLightManager();
	}
	if (sphere)
	{
		sphere->SetPosition(position);
		sphere->Draw(gfx, DirectX::XMMatrixTranslation(
			position.x,
			position.y,
			position.z));
	}

	for (auto& child : children)
	{
		child->Draw(gfx);
	}
}

void PointLightNode::UpdateLightManager()
{
	if (lightIndex >= 0)
	{
		LightManager::Light light = {};
		light.position = position;
		light.type = static_cast<int>(LightManager::LightType::POINT);
		light.direction = { 0.0f, 0.0f, 0.0f };
		light.range = lightRange;
		light.color = lightColor;
		light.intensity = lightIntensity;

		lightManager.UpdateLight(lightIndex, light);
	}
}
