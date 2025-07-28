#include "PixelShader.h"
#include <filesystem>

PixelShader::PixelShader(Graphics& gfx, const std::wstring& path)
{
	if (!std::filesystem::exists(path)) {
		throw std::runtime_error("Shader file not found: " + std::string(path.begin(), path.end()));
	}

	D3DReadFileToBlob(path.c_str(), &pByteCodeBlob);
	GetDevice(gfx)->CreatePixelShader(
		pByteCodeBlob->GetBufferPointer(),
		pByteCodeBlob->GetBufferSize(),
		nullptr,
		&pPixelShader
	);
}

void PixelShader::Bind(Graphics & gfx)
{
	GetContext(gfx)->PSSetShader(pPixelShader.Get(), nullptr, 0u);		// Set the pixel shader
}

ID3DBlob* PixelShader::GetByteCode() const
{
	return pByteCodeBlob.Get();
}
