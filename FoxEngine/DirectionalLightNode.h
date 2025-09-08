#pragma once
#include "SceneNode.h"


class DirectionalLightNode : public SceneNode
{
public:
	DirectionalLightNode(
		Graphics& gfx,
		SceneNode* parent,
		std::shared_ptr<LightManager> lightManager,
		std::optional<std::string> name,
		const DirectX::XMFLOAT3& direction = { -0.3f, -0.8f, 0.5f },
		const DirectX::XMFLOAT3& color = { 1.0f, 1.0f, 1.0f },
		float intensity = 1.0f
	);
	void Bind(Graphics& gfx);
	void DrawInspectorWindow() override;

	void Draw(Graphics& gfx) override;
private:
	void UpdateLightManager();
	std::shared_ptr<LightManager> lightManager;

	int lightIndex;
	DirectX::XMFLOAT3 lightColor;
	DirectX::XMFLOAT3 lightDirection;
	float lightIntensity;
	bool valuesChanged;
};
