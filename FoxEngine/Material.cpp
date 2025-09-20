#include "Material.h"
#include "DirectXTex.h"
#include <filesystem>
#include <regex>

Material::Material(Graphics& gfx, const MaterialInstanceData& data)
	: instanceData(data)
{
	InitializeTextures(gfx);
	materialCBuff = std::make_unique<PixelConstantBuffer<MaterialCbuff>>(gfx, 2u);
}


void Material::InitializeTextures(Graphics& gfx)
{
	D3D11_SAMPLER_DESC samplerDesc = {
		.Filter = D3D11_FILTER_ANISOTROPIC,
		.AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
		.MaxAnisotropy = 16,
		.ComparisonFunc = D3D11_COMPARISON_NEVER,
		.MinLOD = 0,
		.MaxLOD = D3D11_FLOAT32_MAX,
	};

	HRESULT hr = E_FAIL;
	hr = GetDevice(gfx)->CreateSamplerState(&samplerDesc, &defaultSamplerState);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create default sampler state");
	}
	for (const auto& [slotName, texturePath] : instanceData.texturePaths) {
		TextureData textureData;
		if (LoadTexture(gfx, texturePath, textureData)) {
			loadedTextures[slotName] = std::move(textureData);
		}
	}
}

void Material::SetSpecularIntensity(float _specularIntensity)
{
	instanceData.specularIntensity = _specularIntensity;
}

void Material::SetSpecularPower(float _specularPower)
{
	instanceData.specularPower = _specularPower;
}

bool Material::LoadTexture(Graphics& gfx, const std::wstring& path, TextureData& outTexture)
{
	// Try to load DDS version first
	std::filesystem::path originalPath(path);
	std::filesystem::path ddsPath = originalPath;
	ddsPath.replace_extension(L".dds");

	// Debug path for logging
	std::string stringPath(originalPath.string());

	DirectX::TexMetadata metadata;
	DirectX::ScratchImage scratchImage;
	HRESULT hr = E_FAIL;

	// check if this is UDIM texture
	if (stringPath.find("<UDIM>") != std::string::npos)
	{
		auto udimTextures = ExpandUDIM(stringPath);
		// load first texture only for now
		if (!udimTextures.empty())
		{
			std::filesystem::path udimTexturePath = udimTextures.front();
			udimTexturePath.replace_extension(L".dds");

			std::wstring wDdsPath = udimTexturePath.wstring();
			hr = DirectX::LoadFromDDSFile(
				wDdsPath.c_str(),
				DirectX::DDS_FLAGS_NONE,
				&metadata,
				scratchImage
			);
		}
	}

	// Try DDS first (faster loading, compressed)
	if (std::filesystem::exists(ddsPath))
	{
		std::wstring wDdsPath = ddsPath.wstring();
		hr = DirectX::LoadFromDDSFile(
			wDdsPath.c_str(),
			DirectX::DDS_FLAGS_NONE,
			&metadata,
			scratchImage
		);
	}

	// Fallback to original format if DDS doesn't exist or failed
	if (FAILED(hr))
	{
		hr = DirectX::LoadFromWICFile(
			path.c_str(),
			DirectX::WIC_FLAGS_NONE,
			&metadata,
			scratchImage
		);

		if (SUCCEEDED(hr))
		{
			OutputDebugStringA(("Loaded WIC: " + stringPath + " (Consider converting to DDS)\n").c_str());
		}
	}

	// Handle missing texture
	if (FAILED(hr))
	{
		OutputDebugStringA(("Texture missing: " + stringPath + "\n").c_str());
		return false;
	}

	// Generate mipmaps if not present
	DirectX::ScratchImage mipMappedImage;
	if (metadata.mipLevels == 1)
	{
		hr = DirectX::GenerateMipMaps(
			scratchImage.GetImages(),
			scratchImage.GetImageCount(),
			metadata,
			DirectX::TEX_FILTER_DEFAULT,
			0,
			mipMappedImage
		);
		if (SUCCEEDED(hr))
		{
			hr = DirectX::CreateShaderResourceView(
				GetDevice(gfx),
				mipMappedImage.GetImages(),
				mipMappedImage.GetImageCount(),
				mipMappedImage.GetMetadata(),
				&outTexture.textureView
			);
			OutputDebugStringA(("Generated mipmaps for: " + stringPath + "\n").c_str());
		}
		else
		{
			// Mipmap generation failed, use original texture
			OutputDebugStringA(("Mipmap generation failed for: " + stringPath + " (using original)\n").c_str());
			hr = DirectX::CreateShaderResourceView(
				GetDevice(gfx),
				scratchImage.GetImages(),
				scratchImage.GetImageCount(),
				metadata,
				&outTexture.textureView
			);
		}
	}
	else
	{
		// Texture already has mipmaps, use as-is
		hr = DirectX::CreateShaderResourceView(
			GetDevice(gfx),
			scratchImage.GetImages(),
			scratchImage.GetImageCount(),
			metadata,
			&outTexture.textureView
		);
	}
	return true;
}

void Material::Bind(Graphics& gfx)
{
	GetContext(gfx)->PSSetSamplers(0, 1u, defaultSamplerState.GetAddressOf());

	for (const auto& [slotName, textureData] : loadedTextures) {
		GetContext(gfx)->PSSetShaderResources(slotName, 1u, textureData.textureView.GetAddressOf());
	}

	const MaterialCbuff buff = {
		{ instanceData.diffuseColor.x, instanceData.diffuseColor.y , instanceData.diffuseColor.z },
		instanceData.specularIntensity,
		instanceData.specularPower,
		0.0f,
		 0.0f
	};
	materialCBuff->Update(gfx, buff);
	materialCBuff->Bind(gfx);
}

std::vector<std::wstring> Material::ExpandUDIM(const std::string& udimPath)
{
	std::filesystem::path fullPath(udimPath);
	auto dir = fullPath.parent_path();

	// Base pattern: replace <UDIM> with 4 digits
	std::string baseName = fullPath.filename().string(); // e.g. Body_BaseColor.<UDIM>.png
	std::string regexPattern = std::regex_replace(baseName, std::regex("<UDIM>"), "([0-9]{4})");

	// ⚡ Relax extension checking: allow any extension
	// Strip extension from regexPattern
	size_t dotPos = regexPattern.find_last_of('.');
	if (dotPos != std::string::npos)
		regexPattern = regexPattern.substr(0, dotPos);

	regexPattern += "\\..*"; // allow any extension after a dot

	std::regex matcher(regexPattern, std::regex_constants::icase);

	std::vector<std::wstring> udimFiles;

	for (auto& entry : std::filesystem::directory_iterator(dir))
	{
		if (!entry.is_regular_file())
			continue;

		std::string filename = entry.path().filename().string();
		if (std::regex_match(filename, matcher))
		{
			udimFiles.push_back(entry.path().wstring());
		}
	}

	return udimFiles;
}