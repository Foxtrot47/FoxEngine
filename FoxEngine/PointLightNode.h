#pragma once
#include "SceneNode.h"
#include "ConstantBuffer.h"
#include <memory>
#include "SolidSphere.h"

class SolidSphere;

class PointLightNode : public SceneNode
{
public:
	PointLightNode(
		Graphics& gfx,
		SceneNode* parent,
		std::shared_ptr<LightManager>  lightManager,
		std::optional<std::string> name,
		const DirectX::XMFLOAT3& initialPosition = { 0.0f, 0.0f, 0.0f },
		const DirectX::XMFLOAT3& color = { 1.0f, 1.0f, 1.0f },
		float intensity = 1.0f,
		float range = 100.0f
	);
	void DrawInspectorWindow() override;

	void Draw(Graphics& gfx) override;
private:
	void UpdateLightManager();
	std::shared_ptr<LightManager>  lightManager;

	int lightIndex;
	DirectX::XMFLOAT3 lightColor;
	float lightIntensity;
	float lightRange;
	std::unique_ptr<class SolidSphere> sphere;
	bool valuesChanged;
};

