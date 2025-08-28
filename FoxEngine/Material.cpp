#include "Material.h"
#include "DirectXTex.h"
#include <filesystem>
#include <regex>

Material::Material(Graphics& gfx, const MaterialInstanceData& data)
	: instanceData(data)
{
	InitializeTextures(gfx);
	InitializeBindings(gfx);
}

void Material::InitializeBindings(Graphics& gfx)
{
	materialCBuff = std::make_unique<PixelConstantBuffer<MaterialCbuff>>(gfx, 1u);

	if (!std::filesystem::exists(instanceData.vsPath)) {
		throw std::runtime_error("Shader file not found: " + std::string(instanceData.vsPath.begin(), instanceData.vsPath.end()));
	}

	HRESULT hr = E_FAIL;
	hr = D3DReadFileToBlob(instanceData.vsPath.c_str(), &pVSByteCodeBlob);

	if (FAILED(hr)) {
		throw std::runtime_error("Failed to read vertex shader file: " + std::string(instanceData.vsPath.begin(), instanceData.vsPath.end()));
	}

	hr = GetDevice(gfx)->CreateVertexShader(
		pVSByteCodeBlob->GetBufferPointer(),
		pVSByteCodeBlob->GetBufferSize(),
		nullptr,
		&pVertexShader
	);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to create vertex shader: " + std::string(instanceData.vsPath.begin(), instanceData.vsPath.end()));
	}

	if (!std::filesystem::exists(instanceData.psPath)) {
		throw std::runtime_error("Shader file not found: " + std::string(instanceData.psPath.begin(), instanceData.psPath.end()));
	}

	hr = D3DReadFileToBlob(instanceData.psPath.c_str(), &pPSByteCodeBlob);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to read pixel shader file: " + std::string(instanceData.psPath.begin(), instanceData.psPath.end()));
	}

	hr = GetDevice(gfx)->CreatePixelShader(
		pPSByteCodeBlob->GetBufferPointer(),
		pPSByteCodeBlob->GetBufferSize(),
		nullptr,
		&pPixelShader
	);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to create pixel shader: " + std::string(instanceData.psPath.begin(), instanceData.psPath.end()));
	}

	if (instanceData.hasDepthState)
	{
		topologyDesc =
		{
			{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TexCoord", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
	}
	else {
		topologyDesc =
		{
			{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "Normal", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TexCoord", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "Tangent", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "BiTangent", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
	}

		hr = GetDevice(gfx)->CreateInputLayout(
			topologyDesc.data(),
			(UINT)topologyDesc.size(),
			pVSByteCodeBlob->GetBufferPointer(),
			pVSByteCodeBlob->GetBufferSize(),
			&pInputLayout
		);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create input layout");
	}
	
	D3D11_DEPTH_STENCIL_DESC dsDesc = {};
	dsDesc.DepthEnable = TRUE;
	dsDesc.StencilEnable = FALSE;
	if (instanceData.hasDepthState)
	{
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	}
	else
	{
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
}

	hr = GetDevice(gfx)->CreateDepthStencilState(&dsDesc, &pDepthState);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to create depth stencil state");
	}
}

void Material::InitializeTextures(Graphics& gfx)
{
	D3D11_SAMPLER_DESC samplerDesc = {
		.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
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
	return true;
}

void Material::Bind(Graphics& gfx)
{
	GetContext(gfx)->VSSetShader(pVertexShader.Get(), nullptr, 0u);
	GetContext(gfx)->OMSetDepthStencilState(pDepthState.Get(), 0);

	GetContext(gfx)->PSSetSamplers(0, 1u, defaultSamplerState.GetAddressOf());
	
	for (const auto& [slotName, textureData] : loadedTextures) {
		GetContext(gfx)->PSSetShaderResources(slotName, 1u, textureData.textureView.GetAddressOf());
	}

	GetContext(gfx)->PSSetShader(pPixelShader.Get(), nullptr, 0u);
	GetContext(gfx)->IASetInputLayout(pInputLayout.Get());

	const MaterialCbuff buff = {
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