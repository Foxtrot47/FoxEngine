#include "FPVCamera.h"
#include "imgui.h"

FPVCamera::FPVCamera(HWND hWnd, Graphics& gfx, Keyboard& kbd, Mouse& mouse)
	: position({-10.0f, 10.0f, -50.0f}),
	  forward({0.0f, 0.0f, 1.0f}),
	  right({0.0f, 0.0f, 0.0f}),
	  up({0.0f, 1.0f, 0.0f}),
	  pitch(0.0f),
	  yaw(0.0f),
	  fov(80.0f),
	  nearPlane(0.01f),
	  farPlane(10000.0f),
	  cameraSpeed(10.0f),
	  isCursorLocked(false),
	  hWnd(hWnd),
	  kbd(kbd),
	  mouse(mouse)
{
	if (!cameraCBuff)
	{
		cameraCBuff = std::make_unique<PixelConstantBuffer<CamerCbuff>>(gfx, 11u);
	}
	UpdateViewMatrix();
}

XMMATRIX FPVCamera::GetViewMatrix() const { return viewMatrix; }

XMMATRIX FPVCamera::GetProjectionMatrix() const
{
	return XMMatrixPerspectiveFovLH(
		fov * XM_PI / 180.0f,
		16.0f / 9.0f,
		nearPlane,
		farPlane
	);
}

XMFLOAT3 FPVCamera::GetPosition() const
{
	return position;
}


void FPVCamera::Update(float dt)
{
	if (!isCursorLocked) return;
    
    const float rotationSpeed = 0.01f;
	auto delta = mouse.GetRawDelta();
	yaw -= delta.x * rotationSpeed;
	pitch -= delta.y * rotationSpeed;
	pitch = std::clamp(pitch, -XM_PI / 2.0f + 0.01f, XM_PI / 2.0f - 0.01f);
	
	forward = { cos(pitch) * cos(yaw), sin(pitch), cos(pitch) * sin(yaw) };
	const auto forwardVector = XMVector3Normalize(XMLoadFloat3(&forward));
	XMStoreFloat3(&forward, forwardVector);

	const auto newRightVector = XMVector3Cross(
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		forwardVector
	);
	XMStoreFloat3(&right, XMVector3Normalize(newRightVector));

	const auto newUpVector = XMVector3Cross(
		forwardVector,
		newRightVector
	);
	XMStoreFloat3(&up, XMVector3Normalize(newUpVector));

	float velocity = cameraSpeed * dt;
	XMFLOAT3 moveTransform = { 0.0f, 0.0f, 0.0f };
	if (kbd.KeyIsPressed('W'))
	{
		moveTransform.x += forward.x * velocity;
		moveTransform.y += forward.y * velocity;
		moveTransform.z += forward.z * velocity;
	}
	if (kbd.KeyIsPressed('S'))
	{
		moveTransform.x -= forward.x * velocity;
		moveTransform.y -= forward.y * velocity;
		moveTransform.z -= forward.z * velocity;
	}
	if (kbd.KeyIsPressed('D'))
	{
		moveTransform.x += right.x * velocity;
		moveTransform.y += right.y * velocity;
		moveTransform.z += right.z * velocity;
	}
	if (kbd.KeyIsPressed('A'))
	{
		moveTransform.x -= right.x * velocity;
		moveTransform.y -= right.y * velocity;
		moveTransform.z -= right.z * velocity;
	}
	if (kbd.KeyIsPressed(VK_SPACE))
	{
		moveTransform.x += up.x * velocity;
		moveTransform.y += up.y * velocity;
		moveTransform.z += up.z * velocity;
	}
	if (kbd.KeyIsPressed(VK_CONTROL))
	{
		moveTransform.x -= up.x * velocity;
		moveTransform.y -= up.y * velocity;
		moveTransform.z -= up.z * velocity;
	}
	position.x += moveTransform.x;
	position.y += moveTransform.y;
	position.z += moveTransform.z;

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
			RECT rect;
			GetClientRect(hWnd, &rect);
			ClientToScreen(hWnd, reinterpret_cast<POINT*>(&rect.left));
			ClientToScreen(hWnd, reinterpret_cast<POINT*>(&rect.right));
			ClipCursor(&rect);
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
		}
	}

	// Release cursor on Escape and on focus loss
	if (GetAsyncKeyState(VK_ESCAPE) & 0x8000 || GetForegroundWindow() != hWnd)
	{
		if (isCursorLocked)
		{
			isCursorLocked = false;
			ShowCursor(true);
			ClipCursor(nullptr);
			ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard);
		}
	}
}

void FPVCamera::Bind(Graphics& gfx) const
{
	const CamerCbuff buffer = { position, 0.0f };
	cameraCBuff->Update(gfx, buffer);
	cameraCBuff->Bind(gfx);
}

void FPVCamera::UpdateViewMatrix()
{
	const auto posVector = XMLoadFloat3(&position);
	viewMatrix = XMMatrixLookAtLH(
		XMLoadFloat3(&position),
		XMVectorAdd(posVector, XMLoadFloat3(&forward)),
		XMLoadFloat3(&up)
	);
}

FPVCamera::~FPVCamera()
{
	ShowCursor(true);
}