#include "Texture.h"
#include "DirectXTex.h"

#pragma comment(lib, "DirectXTex.lib")

Texture::Texture(Graphics& gfx, const std::string& path)
{
	std::wstring wPath = std::wstring(path.begin(), path.end());

	DirectX::TexMetadata metadata;
	DirectX::ScratchImage scratchImage;

	HRESULT hr = DirectX::LoadFromWICFile(
		wPath.c_str(),
		DirectX::WIC_FLAGS_NONE,
		&metadata,
		scratchImage
	);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to load texture from file: " + path);
	}

	hr = DirectX::CreateShaderResourceView(
		GetDevice(gfx),
		scratchImage.GetImages(),
		scratchImage.GetImageCount(),
		metadata,
		&pTextureView
	);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create texture view");
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
