#pragma once
#include "framework.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectxMath.h>
#include <memory>
#include <vector>
#include <wrl.h>
#include <random>

class ShadowManager;
class LightManager;

class Graphics
{
	friend class Bindable;
	friend class ShadowManager;
	friend class LightManager;
public:
	Graphics(HWND hWnd, int windowWidth, int windowHeight);
	Graphics() = delete;
	Graphics(const Graphics&) = delete;
	Graphics& operator=(const Graphics&) = delete;
	~Graphics();
	void EndFrame();
	void BeginFrame(float red, float green, float blue);
	void BeginShadowPass();
	void BeginRenderPass();
	void DrawIndexed(UINT count);
	void SetProjection(DirectX::FXMMATRIX proj);
	DirectX::XMMATRIX GetProjection() const;
	void EnableImGui();
	void DisableImGui();
	bool IsImGuiEnabled() const;
	void SetCamera(DirectX::FXMMATRIX);
	DirectX::XMMATRIX GetCamera() const;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> GetRenderTargetView() const;
	std::shared_ptr<ShadowManager> GetShadowManager() const { return pShadowManager; }
	std::shared_ptr<LightManager> GetLightManager() const { return pLightManager; }
private:
	DirectX::XMMATRIX projection;
	D3D11_VIEWPORT renderViewPort = {};
	Microsoft::WRL::ComPtr<ID3D11Device> pDevice = nullptr;
	Microsoft::WRL::ComPtr<IDXGISwapChain> pSwapChain = nullptr;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> pContext = nullptr;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> pTarget = nullptr;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> pDepthStencilView = nullptr;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> pDepthStencilTexture = nullptr;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> pRasterizerState = nullptr;
	std::shared_ptr<ShadowManager> pShadowManager = nullptr;
	std::shared_ptr<LightManager> pLightManager = nullptr;
	bool imGuiEnabled = true;
	DirectX::XMMATRIX camera;
};

