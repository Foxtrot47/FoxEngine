#include "Texture.h"
#include "DirectXTex.h"
#include <filesystem>

#pragma comment(lib, "DirectXTex.lib")

Texture::Texture(Graphics& gfx, const std::wstring& path)
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

		if (SUCCEEDED(hr))
		{
			// Log that we're using DDS
			OutputDebugStringA(("Loaded DDS: " + ddsPath.string() + "\n").c_str());
		}
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

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to load texture: " + stringPath);
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
		throw std::runtime_error("Failed to create texture view for: " + stringPath);
	}

	D3D11_SAMPLER_DESC samplerDesc = {
		.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
		.AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
	};

	GetDevice(gfx)->CreateSamplerState(&samplerDesc, &pSamplerState);
}

void Texture::Bind(Graphics& gfx)
{
	GetContext(gfx)->PSSetShaderResources(0u, 1u, pTextureView.GetAddressOf());
	GetContext(gfx)->PSSetSamplers(0u, 1u, pSamplerState.GetAddressOf());
}
