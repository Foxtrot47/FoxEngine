#pragma once
#include "Graphics.h"
#include "BindableBase.h"
#include <optional>
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
	struct MaterialDesc
	{
		std::optional<std::wstring> diffusePath;
		std::optional<std::wstring> specularPath;
		std::optional<std::wstring> normalPath;
		float specularIntensity = 1.0f;
		float specularPower = 32.0f;
	};
	Material(
		Graphics& gfx,
		const MaterialDesc& desc
	);
	void SetSpecularIntensity(float specularIntensity);
	void SetSpecularPower(float specularPower);
	void Bind(Graphics& gfx) override;
private:
	struct TextureData
	{
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> textureView;
		Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState;
	};
	Microsoft::WRL::ComPtr<ID3DBlob> pVSByteCodeBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> pPSByteCodeBlob;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> pVertexShader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> pPixelShader;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> pInputLayout;
	std::vector<D3D11_INPUT_ELEMENT_DESC> topologyDesc = {};

	std::unordered_map<TextureType, TextureData> textures;
	
	float specularIntensity = 1.0f;
	float specularPower = 32.0f;

	std::unique_ptr<PixelConstantBuffer<MaterialCbuff>> materialCBuff;

	void InitializeBindings(Graphics& gfx, const MaterialDesc& desc);
	void InitializeTextures(Graphics& gfx, const MaterialDesc& desc);
	bool LoadTexture(Graphics& gfx, const std::wstring& path, TextureData& outTexture);
	std::vector<std::wstring> ExpandUDIM(const std::string& udimPath);
};
