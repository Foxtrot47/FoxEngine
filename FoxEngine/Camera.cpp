#include "Camera.h"

DirectX::XMMATRIX Camera::GetViewMatrix() const
{
	// Calculate the camera position based on spherical coordinates
	const auto position = DirectX::XMVector3Transform(
		DirectX::XMVectorSet(0.0f, 0.0f, -orbitalRadius, 0.0f),
		DirectX::XMMatrixRotationRollPitchYaw(phi, -theta, 0.0f)
	);

	return DirectX::XMMatrixLookAtLH(
		position,
		DirectX::XMVectorZero(),									// Look at the origin
		DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)				// Up vector
	) * DirectX::XMMatrixRotationRollPitchYaw(pitch, -yaw, roll);
}

void Camera::Reset()
{
	orbitalRadius = 20.0f;
	pitch = 0.0f;
	roll = 0.0f;
	yaw = 0.0f;
	theta = 0.0f;
	phi = 0.0f;
}
