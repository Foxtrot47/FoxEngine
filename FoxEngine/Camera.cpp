#include "Camera.h"
#include "imgui.h"

Camera::Camera(Graphics& gfx)
{
	if (!cameraCBuff)
	{
		cameraCBuff = std::make_unique<PixelConstantBuffer<CamerCbuff>>(gfx, 11u);
	}
}

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

void Camera::CreateControlWindow()
{
	if (ImGui::Begin("Camera Controls"))
	{
		ImGui::Text("Position");
		ImGui::SliderFloat("Orbital Radiu", &orbitalRadius, 0.1f, 80.0f, "%.1f");
		ImGui::SliderAngle("Theta", &theta, -180.0f, 180.0f);
		ImGui::SliderAngle("Phi", &phi, -89.0f, 89.0f);

		ImGui::Text("Rotation"); 
		ImGui::SliderAngle("Roll", &roll, -180.0f, 180.0f);
		ImGui::SliderAngle("Pitch", &pitch, -180.0f, 180.0f);
		ImGui::SliderAngle("Yaw", &yaw, -180.0f, 180.0f);

		if (ImGui::Button("Reset"))
		{
			Reset();
		}
	}
	ImGui::End();
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

void Camera::Bind(Graphics& gfx)
{
	const CamerCbuff buffer = { GetPositionWS(), 0.0f};
	cameraCBuff->Update(gfx, buffer);
	cameraCBuff->Bind(gfx);
}

DirectX::XMFLOAT3 Camera::GetPositionWS() const
{
	using namespace DirectX;
	XMVECTOR posV = XMVector3Transform(
		XMVectorSet(0.0f, 0.0f, -orbitalRadius, 0.0f),
		XMMatrixRotationRollPitchYaw(phi, -theta, 0.0f)
	);
	XMFLOAT3 pos;
	XMStoreFloat3(&pos, posV);
	return pos;
}