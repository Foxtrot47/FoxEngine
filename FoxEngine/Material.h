#pragma once
#include "Graphics.h"
#include "BindableBase.h"
#include <unordered_map>

enum class TextureType
{
	Diffuse,
	Specular,
	Normal
};

class Material : public Bindable
{
public:
	struct MaterialCbuff
	{
		float specularIntensity;
		float specularPower;
		float hasSpecularMap;
		float hasNormalMap;
	};
	struct MaterialInstanceData {
		std::string name;
		std::wstring vsPath;
		std::wstring psPath;

		DirectX::XMFLOAT3 diffuseColor = { 1.0f, 1.0f, 1.0f };
		float specularIntensity = 1.0f;
		float specularPower = 32.0f;

		bool hasSpecularMap = false;
		bool hasDepthState = false;

		std::unordered_map<int, std::wstring> texturePaths;
	};
	Material(Graphics& gfx, const MaterialInstanceData& data);
	void SetSpecularIntensity(float specularIntensity);
	void SetSpecularPower(float specularPower);
	void Bind(Graphics& gfx) override;
private:
	struct TextureData
	{
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> textureView;
	};
	MaterialInstanceData instanceData;
	Microsoft::WRL::ComPtr<ID3DBlob> pVSByteCodeBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> pPSByteCodeBlob;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> pVertexShader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> pPixelShader;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> pInputLayout;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> pDepthState;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> pRasterizerState;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> defaultSamplerState;
	std::vector<D3D11_INPUT_ELEMENT_DESC> topologyDesc = {};

	std::unordered_map<int, TextureData> loadedTextures;
	std::unique_ptr<PixelConstantBuffer<MaterialCbuff>> materialCBuff;

	void InitializeBindings(Graphics& gfx);
	void InitializeTextures(Graphics& gfx);
	bool LoadTexture(Graphics& gfx, const std::wstring& path, TextureData& outTexture);
	std::vector<std::wstring> ExpandUDIM(const std::string& udimPath);
};