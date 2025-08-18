#include "FPVCamera.h"
#include "imgui.h"

FPVCamera::FPVCamera(HWND hWnd, Graphics& gfx)
	: hWnd(hWnd)
{
	if (!cameraCBuff)
	{
		cameraCBuff = std::make_unique<PixelConstantBuffer<CamerCbuff>>(gfx, 11u);
	}
	UpdateViewMatrix();
}

DirectX::XMMATRIX FPVCamera::GetViewMatrix() const {return viewMatrix; }

DirectX::XMMATRIX FPVCamera::GetProjectionMatrix() const
{
	return DirectX::XMMatrixPerspectiveFovLH(
		fov * DirectX::XM_PI / 180.0f,
		16.0f / 9.0f,
		nearPlane,
		farPlane
	);
}

void FPVCamera::Update(float dt)
{
	if (!isCursorLocked) return;

	POINT currentMousePos;
	GetCursorPos(&currentMousePos);
	ScreenToClient(hWnd, &currentMousePos);

	RECT clientRect;
	GetClientRect(hWnd, &clientRect);
	POINT center = { clientRect.right / 2, clientRect.bottom / 2 };
	ClientToScreen(hWnd, &center);
	SetCursorPos(center.x, center.y);

	float deltaX = static_cast<float>(currentMousePos.x - clientRect.right / 2);
	float deltaY = static_cast<float>(currentMousePos.y - clientRect.bottom / 2);

	yaw -= deltaX * dt;
	pitch -= deltaY * dt;

	pitch = std::clamp(pitch, -DirectX::XM_PI / 2.0f + 0.01f, DirectX::XM_PI / 2.0f - 0.01f);

	forward = { cos(pitch) * cos(yaw), sin(pitch), cos(pitch) * sin(yaw) };
	const auto forwardVector = DirectX::XMVector3Normalize(XMLoadFloat3(&forward));
	XMStoreFloat3(&forward, forwardVector);

	const auto newRightVector = DirectX::XMVector3Cross(
		DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		forwardVector
	);
	XMStoreFloat3(&right, newRightVector);

	const auto newUpVector = DirectX::XMVector3Cross(
		forwardVector,
		newRightVector
	);
	XMStoreFloat3(&up, newUpVector);

	UpdateViewMatrix();
}

void FPVCamera::HandleInput()
{
	// Toggle cursor lock on right-click
	if (GetForegroundWindow() == hWnd && ImGui::GetIO().WantCaptureMouse == false && GetAsyncKeyState(VK_RBUTTON) & 0x8000)
	{
		if (!isCursorLocked)
		{
			isCursorLocked = true;
			ShowCursor(false);
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
		}
	}

	// Release cursor on Escape and on focus loss
	if (GetAsyncKeyState(VK_ESCAPE) & 0x8000 || GetForegroundWindow() != hWnd)
	{
		if (isCursorLocked)
		{
			isCursorLocked = false;
			ShowCursor(true);
			ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
		}
	}
}

void FPVCamera::Bind(Graphics& gfx)
{
	const CamerCbuff buffer = { position, 0.0f };
	cameraCBuff->Update(gfx, buffer);
	cameraCBuff->Bind(gfx);
}

void FPVCamera::UpdateViewMatrix()
{
	const auto posVector = XMLoadFloat3(&position);
	viewMatrix = DirectX::XMMatrixLookAtLH(
		XMLoadFloat3(&position),
		DirectX::XMVectorAdd(posVector, XMLoadFloat3(&forward)),
		XMLoadFloat3(&up)
	);
}

FPVCamera::~FPVCamera()
{
	ShowCursor(true);
}