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
		float albedoColor[3];
		float specularIntensity;

		float specularPower;
		float hasSpecularMap;
		float hasNormalMap;
		float padding;
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

	// Methods for renderer to access shader paths
	const std::wstring& GetVertexShaderPath() const { return instanceData.vsPath; }
	const std::wstring& GetPixelShaderPath() const { return instanceData.psPath; }
	bool HasShaderOverride() const { return !instanceData.vsPath.empty() || !instanceData.psPath.empty(); }
private:
	struct TextureData
	{
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> textureView;
	};
	MaterialInstanceData instanceData;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> defaultSamplerState;

	std::unordered_map<int, TextureData> loadedTextures;
	std::unique_ptr<PixelConstantBuffer<MaterialCbuff>> materialCBuff;

	void InitializeTextures(Graphics& gfx);
	bool LoadTexture(Graphics& gfx, const std::wstring& path, TextureData& outTexture);
	std::vector<std::wstring> ExpandUDIM(const std::string& udimPath);
};