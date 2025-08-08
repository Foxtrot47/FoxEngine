#pragma once
#include "Graphics.h"
#include "ConstantBuffer.h"

class PointLight
{
public:
	PointLight(Graphics& gfx);
	void Bind(Graphics& gfx);
	void SpawnControlWindow();
private:
	struct LightCBuff
	{
		DirectX::XMFLOAT3 pos;
		float padding;
		DirectX::XMFLOAT3 lightColor;
		float padding2;
	};
	DirectX::XMFLOAT3 pos = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 color = { 1.0f, 1.0f, 1.0f };
	std::unique_ptr<PixelConstantBuffer<LightCBuff>> lightPSCbuff;
};

