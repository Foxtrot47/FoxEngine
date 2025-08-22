#pragma once
#include "Graphics.h"
#include "BindableBase.h"

class Material : public Bindable
{
public:
	struct MaterialCbuff
	{
		float specularIntensity;
		float specularPower;
		float padding[2];
	};
	Material(
		Graphics& gfx,
		const std::wstring* texturePath
	);
	Material(
		Graphics& gfx
	);
	void InitializeBindings(Graphics& gfx, const std::wstring* texturePath);
	void SetSpecularIntensity(float specularIntensity);
	void SetSpecularPower(float specularPower);
	void Bind(Graphics& gfx) override;
private:
	Microsoft::WRL::ComPtr<ID3DBlob> pVSByteCodeBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> pPSByteCodeBlob;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> pVertexShader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> pPixelShader;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pTextureView;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> pSamplerState;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> pInputLayout;
	std::vector<D3D11_INPUT_ELEMENT_DESC> topologyDesc = {};
	
	float specularIntensity = 1.0f;
	float specularPower = 32.0f;
	bool useDiffuse;

	std::unique_ptr<PixelConstantBuffer<MaterialCbuff>> materialCBuff;

	void LoadTexture(Graphics& gfx, const std::wstring& path);
};

