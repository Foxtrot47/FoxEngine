#pragma once
#include "Graphics.h"
#include "ConstantBuffer.h"
#include <memory>
#include "SolidSphere.h"

class SolidSphere;

class PointLight
{
public:
	PointLight(Graphics& gfx);
	void Bind(Graphics& gfx);
	void SpawnControlWindow();

	void DrawSphere(Graphics& gfx) const;
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

