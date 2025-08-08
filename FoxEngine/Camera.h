#pragma once
#include "Graphics.h"
#include "ConstantBuffer.h"

class Camera
{
public:
	Camera(Graphics& gfx);
	DirectX::XMMATRIX GetViewMatrix() const;
	void CreateControlWindow();
	void Reset();
	void Bind(Graphics& gfx);
private:
	float orbitalRadius = 20.0f;
	float pitch = 0.0f;
	float roll = 0.0f;
	float yaw = 0.0f;
	float theta = 0.0f;
	float phi = 0.0f;

	struct CamerCbuff
	{
		DirectX::XMFLOAT3 pos;
		float padding = 0.0f;
	};
	std::unique_ptr<PixelConstantBuffer<CamerCbuff>> cameraCBuff;
	DirectX::XMFLOAT3 GetPositionWS() const;
};

