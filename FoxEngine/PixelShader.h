#pragma once
#include "Bindable.h"
#include <string>

class PixelShader : public Bindable
{
public:
	PixelShader(Graphics& gfx, const std::wstring& path);
	void Bind(Graphics& gfx) override;
	ID3DBlob* GetByteCode() const;
protected:
	Microsoft::WRL::ComPtr<ID3DBlob> pByteCodeBlob;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> pPixelShader;
};

