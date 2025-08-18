#pragma once
#include "Graphics.h"
#include "ConstantBuffer.h"

using namespace DirectX;
class FPVCamera
{
public:
	FPVCamera(HWND hWnd, Graphics& gfx);
	DirectX::XMMATRIX GetViewMatrix() const;
	DirectX::XMMATRIX GetProjectionMatrix() const;
	void HandleInput();
	void Bind(Graphics& gfx);
	void Update(float dt);
	~FPVCamera();
private:
	DirectX::XMMATRIX viewMatrix;
	DirectX::XMFLOAT3 position = { 0.0f, 0.0f, -5.0f };
	DirectX::XMFLOAT3 forward = { 0.0f, 0.0f, 1.0f };
	DirectX::XMFLOAT3 right = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 up = { 0.0f, 1.0f, 0.0f };
	float pitch = 0.0f;
	float yaw = 0.0f;

	float fov = 80.0f;
	float nearPlane = 0.5f;
	float farPlane = 1000.0f;

	float isCursorLocked = false;
	HWND hWnd;

	struct CamerCbuff
	{
		DirectX::XMFLOAT3 pos;
		float padding = 0.0f;
	};
	std::unique_ptr<PixelConstantBuffer<CamerCbuff>> cameraCBuff;
	void UpdateViewMatrix();
};

