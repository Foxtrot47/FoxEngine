#include "PointLightNode.h"
#include "imgui.h"
#include "Math.h"

PointLightNode::PointLightNode(
	Graphics& gfx,
	SceneNode* parent,
	std::optional<std::string> name,
	const DirectX::XMFLOAT3& initialPosition
)
	:
	SceneNode(parent, name, initialPosition, { 0.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }),
	valuesChanged(true)
{
	if (!plightPSCbuff)
	{
		plightPSCbuff = std::make_unique<PixelConstantBuffer<LightCBuff>>(gfx);
	}
	Reset();
	sphere = std::make_unique<SolidSphere>(gfx, 0.3f, 20, 20);
}

void PointLightNode::DrawInspectorWindow()
{
	ImGui::Text("Transform");
	if (ImGui::DragFloat3("Position", &position.x, 0.1f))
	{
		valuesChanged = true;
		MarkDirty();
	}
	// no rotation and scale cuz it makes no sense

	if (ImGui::ColorEdit3("Light Color", reinterpret_cast<float*>(&lightCbuff.lightColor)))
		valuesChanged = true;

	if (ImGui::ColorEdit3("Ambient Light", reinterpret_cast<float*>(&lightCbuff.ambientLight)))
		valuesChanged = true;

	if (ImGui::SliderFloat("Ambient Strength", &lightCbuff.ambientStrength, 0.0f, 1.0f))
		valuesChanged = true;

	if (ImGui::SliderFloat("Global Specular Intensity", &lightCbuff.globalSpecularIntensity, 0.0f, 1.0f))
		valuesChanged = true;

	if (ImGui::Button("Reset"))
	{
		Reset();
	}
}

void PointLightNode::Draw(Graphics& gfx)
{
	Bind(gfx);
	if (sphere)
	{
		sphere->SetPosition(lightCbuff.lightPos);
		sphere->Draw(gfx, DirectX::XMMatrixTranslation(
			lightCbuff.lightPos.x,
			lightCbuff.lightPos.y,
			lightCbuff.lightPos.z));
	}

	for (auto& child : children)
	{
		child->Draw(gfx);
	}
}

void PointLightNode::Bind(Graphics& gfx)
{
	if (valuesChanged)
	{
		lightCbuff.lightPos = position;
		plightPSCbuff->Update(gfx, lightCbuff);
		plightPSCbuff->Bind(gfx);
		valuesChanged = false;
	}
}

void PointLightNode::Reset()
{
	const LightCBuff buff = {
		{0.0f, 0.0f, 0.0f},
		0.0f,
		{1.0f, 1.0f, 1.0f},
		0.1f,
		{1.0f, 1.0f, 1.0f},
		1.0f,
		{0.0f, 0.0f, 0.0f, 0.0f}
	};
	lightCbuff = buff;
	valuesChanged = true;
}
