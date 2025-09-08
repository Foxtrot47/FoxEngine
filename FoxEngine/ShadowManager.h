#pragma once
#include "Graphics.h"

class ShadowManager
{
public:
	ShadowManager(Graphics& gfx);
	ShadowManager() = delete;
	void BeginShadowPass(Graphics& gfx) const;
	DirectX::XMMATRIX GetLightViewProj(Graphics& gfx);
	void Bind(Graphics& gfx) const;
	void Update(Graphics& gfx);
private:
	std::shared_ptr<LightManager> lightManager;
	D3D11_VIEWPORT shadowViewPort = {};
	Microsoft::WRL::ComPtr<ID3D11Texture2D> pShadowMapTexture = nullptr;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> pShadowDepthView = nullptr;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pShadowMapSRV = nullptr;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> pShadowRasterizerState = nullptr;
	Microsoft::WRL::ComPtr<ID3DBlob> pShadowVSByteCode;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> pInputLayout;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> pShadowVS;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> pComparisonSampler;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> pDepthStencilState;
	DirectX::XMMATRIX viewProjectionMatrix;
	std::vector<D3D11_INPUT_ELEMENT_DESC> topologyDesc = {};
};

