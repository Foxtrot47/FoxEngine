#include "Material.h"
#include "DirectXTex.h"
#include "FileUtils.h"
#include <filesystem>
#include <regex>

Material::Material(
	Graphics& gfx,
	const MaterialDesc& desc
)
	: specularIntensity(desc.specularIntensity),
	specularPower(desc.specularPower)
{
	InitializeTextures(gfx, desc);
	InitializeBindings(gfx, desc);
}

void Material::InitializeBindings(Graphics& gfx, const MaterialDesc& desc)
{
	materialCBuff = std::make_unique<PixelConstantBuffer<MaterialCbuff>>(gfx, 1u);

	std::wstring vsPath = GetExecutableDirectory() + (textures.contains(TextureType::Normal) ? L"\\PhongNormalVS.cso" : L"\\PhongVS.cso");
	std::wstring psPath = GetExecutableDirectory() + (textures.contains(TextureType::Normal) ? L"\\PhongNormalPS.cso" : L"\\PhongPS.cso");

	if (!std::filesystem::exists(vsPath)) {
		throw std::runtime_error("Shader file not found: " + std::string(vsPath.begin(), vsPath.end()));
	}

	HRESULT hr = E_FAIL;
	hr = D3DReadFileToBlob(vsPath.c_str(), &pVSByteCodeBlob);

	if (FAILED(hr)) {
		throw std::runtime_error("Failed to read vertex shader file: " + std::string(vsPath.begin(), vsPath.end()));
	}

	hr = GetDevice(gfx)->CreateVertexShader(
		pVSByteCodeBlob->GetBufferPointer(),
		pVSByteCodeBlob->GetBufferSize(),
		nullptr,
		&pVertexShader
	);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to create vertex shader: " + std::string(vsPath.begin(), vsPath.end()));
	}

	if (!std::filesystem::exists(psPath)) {
		throw std::runtime_error("Shader file not found: " + std::string(psPath.begin(), psPath.end()));
	}

	hr = D3DReadFileToBlob(psPath.c_str(), &pPSByteCodeBlob);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to read pixel shader file: " + std::string(psPath.begin(), psPath.end()));
	}

	hr = GetDevice(gfx)->CreatePixelShader(
		pPSByteCodeBlob->GetBufferPointer(),
		pPSByteCodeBlob->GetBufferSize(),
		nullptr,
		&pPixelShader
	);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to create pixel shader: " + std::string(psPath.begin(), psPath.end()));
	}

	topologyDesc =
	{
		{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "Normal", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TexCoord", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "Tangent", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "BiTangent", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};

	hr = GetDevice(gfx)->CreateInputLayout(
		topologyDesc.data(),
		(UINT)topologyDesc.size(),
		pVSByteCodeBlob->GetBufferPointer(),
		pVSByteCodeBlob->GetBufferSize(),
		&pInputLayout
	);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to create input layout:");
	}
}

void Material::InitializeTextures(Graphics& gfx, const MaterialDesc& desc)
{
	if (desc.diffusePath)
	{
		TextureData diffuseTexture;
		if (LoadTexture(gfx, *desc.diffusePath, diffuseTexture))
		{
			textures[TextureType::Diffuse] = std::move(diffuseTexture);
		}
	}
	if (desc.specularPath)
	{
		TextureData specTexture;
		if (LoadTexture(gfx, *desc.specularPath, specTexture))
		{
			textures[TextureType::Specular] = std::move(specTexture);
		}
	}
	if (desc.normalPath)
	{
		TextureData normalTex;
		if (LoadTexture(gfx, *desc.normalPath, normalTex))
		{
			textures[TextureType::Normal] = std::move(normalTex);
		}
	}
}

void Material::SetSpecularIntensity(float _specularIntensity)
{
	specularIntensity = _specularIntensity;
}

void Material::SetSpecularPower(float _specularPower)
{
	specularPower = _specularPower;
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
		OutputDebugStringA(("Texture missing: " + stringPath).c_str());
		return false;
	}

	// Create texture and shader resource view
	hr = DirectX::CreateShaderResourceView(
		GetDevice(gfx),
		scratchImage.GetImages(),
		scratchImage.GetImageCount(),
		metadata,
		&outTexture.textureView
	);

	if (FAILED(hr))
	{
		OutputDebugStringA(("Failed to create texture view: " + stringPath).c_str());
		return false;
	}

	D3D11_SAMPLER_DESC samplerDesc = {
		.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
	};

	hr = GetDevice(gfx)->CreateSamplerState(&samplerDesc, &outTexture.samplerState);
	if (FAILED(hr))
	{
		OutputDebugStringA(("Failed to create sampler state: " + stringPath).c_str());
		return false;
	}
	return true;
}

void Material::Bind(Graphics& gfx)
{
	GetContext(gfx)->VSSetShader(pVertexShader.Get(), nullptr, 0u);

	if (textures.contains(TextureType::Diffuse))
	{
		GetContext(gfx)->PSSetShaderResources(0u, 1u, textures[TextureType::Diffuse].textureView.GetAddressOf());
		GetContext(gfx)->PSSetSamplers(0u, 1u, textures[TextureType::Diffuse].samplerState.GetAddressOf());
	}

	if (textures.contains(TextureType::Specular))
	{
		GetContext(gfx)->PSSetShaderResources(1u, 1u, textures[TextureType::Specular].textureView.GetAddressOf());
		GetContext(gfx)->PSSetSamplers(1u, 1u, textures[TextureType::Specular].samplerState.GetAddressOf());
	}

	if (textures.contains(TextureType::Normal))
	{
		GetContext(gfx)->PSSetShaderResources(2u, 1u, textures[TextureType::Normal].textureView.GetAddressOf());
		GetContext(gfx)->PSSetSamplers(2u, 1u, textures[TextureType::Normal].samplerState.GetAddressOf());
	}

	GetContext(gfx)->PSSetShader(pPixelShader.Get(), nullptr, 0u);
	GetContext(gfx)->IASetInputLayout(pInputLayout.Get());

	const MaterialCbuff buff = {
		specularIntensity,
		specularPower,
		textures.contains(TextureType::Specular) ? 1.0f : 0.0f,
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