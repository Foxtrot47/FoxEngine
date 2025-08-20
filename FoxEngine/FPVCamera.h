#pragma once
#include "Graphics.h"
#include "ConstantBuffer.h"
#include "Keeyboard.h"
#include "Mouse.h"

using namespace DirectX;
class FPVCamera
{
public:
	FPVCamera(HWND hWnd, Graphics& gfx, Keyboard& kbd, Mouse& mouse);
	XMMATRIX GetViewMatrix() const;
	XMMATRIX GetProjectionMatrix() const;
	void HandleInput();
	void Bind(Graphics& gfx) const;
	void Update(float dt);
	FPVCamera() = delete;
	~FPVCamera();
private:
	XMMATRIX viewMatrix;
	XMFLOAT3 position;
	XMFLOAT3 forward;
	XMFLOAT3 right;
	XMFLOAT3 up;
	float pitch;
	float yaw;
	float fov;
	float nearPlane;
	float farPlane;
	float cameraSpeed;
	float isCursorLocked;
	HWND hWnd;
	Keyboard& kbd;
	Mouse& mouse;

	struct CamerCbuff
	{
		XMFLOAT3 pos;
		float padding = 0.0f;
	};
	std::unique_ptr<PixelConstantBuffer<CamerCbuff>> cameraCBuff;
	void UpdateViewMatrix();
};

