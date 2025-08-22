#include "Material.h"
#include "DirectXTex.h"
#include "FileUtils.h"
#include <filesystem>

Material::Material(
	Graphics& gfx,
	const std::wstring* texturePath
)
	:useDiffuse(true)
{
	InitializeBindings(gfx, texturePath);
}

Material::Material(Graphics& gfx)
	:useDiffuse(false)
{
	InitializeBindings(gfx, nullptr);
}

void Material::InitializeBindings(Graphics& gfx, const std::wstring* texturePath)
{
	materialCBuff = std::make_unique<PixelConstantBuffer<MaterialCbuff>>(gfx, 1u);

	std::wstring vsPath;
	if (useDiffuse)
	{
		if (texturePath == nullptr)
		{
			OutputDebugStringA("Bruh");
		}
		LoadTexture(gfx, *texturePath);
		vsPath = GetExecutableDirectory() + L"\\PhongVS.cso";
	}
	else
	{
		vsPath = GetExecutableDirectory() + L"\\SolidSphereVS.cso";
	}
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

	std::wstring psPath;
	if (useDiffuse)
	{
		psPath = GetExecutableDirectory() + L"\\PhongPS.cso";
	}
	else
	{
		psPath = GetExecutableDirectory() + L"\\SolidSpherePS.cso";
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
		{ "TexCoord", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 }
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

void Material::LoadTexture(Graphics& gfx, const std::wstring& path)
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
		useDiffuse = false;
		OutputDebugStringA(("Texture missing: " + stringPath + ", using diffuseColor\n").c_str());
		pTextureView = nullptr;
		pSamplerState = nullptr;
		return;
	}

	// Create texture and shader resource view
	hr = DirectX::CreateShaderResourceView(
		GetDevice(gfx),
		scratchImage.GetImages(),
		scratchImage.GetImageCount(),
		metadata,
		&pTextureView
	);

	if (FAILED(hr))
	{
		useDiffuse = false;
		OutputDebugStringA(("Failed to create texture view: " + stringPath + ", using diffuseColor\n").c_str());
		pTextureView = nullptr;
		pSamplerState = nullptr;
		return;
	}

	D3D11_SAMPLER_DESC samplerDesc = {
		.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
	};

	hr = GetDevice(gfx)->CreateSamplerState(&samplerDesc, &pSamplerState);
	if (FAILED(hr))
	{
		useDiffuse = false;
		OutputDebugStringA(("Failed to create sampler state: " + stringPath + ", using diffuseColor\n").c_str());
		pTextureView = nullptr;
		pSamplerState = nullptr;
		return;
	}
	useDiffuse = true;
}

void Material::Bind(Graphics& gfx)
{
	GetContext(gfx)->VSSetShader(pVertexShader.Get(), nullptr, 0u);

	if (useDiffuse)
	{
		GetContext(gfx)->PSSetShaderResources(0u, 1u, pTextureView.GetAddressOf());
		GetContext(gfx)->PSSetSamplers(0u, 1u, pSamplerState.GetAddressOf());
	}

	GetContext(gfx)->PSSetShader(pPixelShader.Get(), nullptr, 0u);
	GetContext(gfx)->IASetInputLayout(pInputLayout.Get());

	const MaterialCbuff buff = {
		specularIntensity,
		specularPower,
		{ 0.0f, 0.0f }
	};
	materialCBuff->Update(gfx, buff);
	materialCBuff->Bind(gfx);
}


