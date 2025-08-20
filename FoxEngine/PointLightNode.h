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
		std::optional<std::string> name,
		const DirectX::XMFLOAT3& initialPosition
	);
	void Bind(Graphics& gfx);
	void DrawInspectorWindow() override;

	void Draw(Graphics& gfx) override;
	void Reset();
private:
	struct LightCBuff
	{
		DirectX::XMFLOAT3 lightPos;
		float padding;
		DirectX::XMFLOAT3 lightColor;
		float ambientStrength;
		DirectX::XMFLOAT3 ambientLight;
		float specularIntensity;
		float specularPower;
		float padding2[3];
	};
	LightCBuff lightCbuff;
	std::unique_ptr<PixelConstantBuffer<LightCBuff>> plightPSCbuff;
	std::unique_ptr<class SolidSphere> sphere;
	bool valuesChanged;
};

