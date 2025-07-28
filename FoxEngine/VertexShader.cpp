#include "VertexShader.h"
#include <filesystem>

VertexShader::VertexShader(Graphics& gfx, const std::wstring& path)
{
	if (!std::filesystem::exists(path)) {
		throw std::runtime_error("Shader file not found: " + std::string(path.begin(), path.end()));
	}

	D3DReadFileToBlob(path.c_str(), &pByteCodeBlob);
	GetDevice(gfx)->CreateVertexShader(
		pByteCodeBlob->GetBufferPointer(),
		pByteCodeBlob->GetBufferSize(),
		nullptr,
		&pVertexShader
	);
}

void VertexShader::Bind(Graphics& gfx)
{
	GetContext(gfx)->VSSetShader(pVertexShader.Get(), nullptr, 0u);		// Set the pixel shader
}

ID3DBlob* VertexShader::GetByteCode() const
{
	return pByteCodeBlob.Get();
}
