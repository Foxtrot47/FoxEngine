#pragma once
#include "framework.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectxMath.h>
#include <memory>
#include <vector>
#include <wrl.h>
#include <random>

class Graphics
{
	friend class Bindable;
public:
	Graphics(HWND hWnd);
	Graphics() = delete;
	Graphics(const Graphics&) = delete;
	Graphics& operator=(const Graphics&) = delete;
	~Graphics();
	void EndFrame();
	void BeginFrame(float red, float green, float blue);
	void DrawIndexed(UINT count);
	void SetProjection(DirectX::FXMMATRIX proj);
	DirectX::XMMATRIX GetProjection() const;
	void EnableImGui();
	void DisableImGui();
	bool IsImGuiEnabled() const;
	void SetCamera(DirectX::FXMMATRIX);
	DirectX::XMMATRIX GetCamera() const;
private:
	DirectX::XMMATRIX projection;
	Microsoft::WRL::ComPtr<ID3D11Device> pDevice = nullptr;
	Microsoft::WRL::ComPtr<IDXGISwapChain> pSwapChain = nullptr;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> pContext = nullptr;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> pTarget = nullptr;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> pDepthStencilView = nullptr;
	bool imGuiEnabled = true;
	DirectX::XMMATRIX camera;
};

