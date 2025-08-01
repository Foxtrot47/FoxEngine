#pragma once
#include "Graphics.h"

class Camera
{
public:
	DirectX::XMMATRIX GetViewMatrix() const;
	void Reset();
private:
	float orbitalRadius = 20.0f;
	float pitch = 0.0f;
	float roll = 0.0f;
	float yaw = 0.0f;
	float theta = 0.0f;
	float phi = 0.0f;
};

