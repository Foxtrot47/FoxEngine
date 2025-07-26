#pragma once
#include "framework.h"
#include <filesystem>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl.h>

class Graphics
{
public:
	Graphics(HWND hWnd);
	Graphics() = delete;
	Graphics(const Graphics&) = delete;
	Graphics& operator=(const Graphics&) = delete;
	~Graphics() = default;
	void EndFrame();
	void ClearBuffer(float red, float green, float blue);
	void DrawCube(float angle, float x, float y);
private:
	Microsoft::WRL::ComPtr<ID3D11Device> pDevice = nullptr;
	Microsoft::WRL::ComPtr<IDXGISwapChain> pSwapChain = nullptr;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> pContext = nullptr;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> pTarget = nullptr;
	static std::wstring GetExecutableDirectory();
};

