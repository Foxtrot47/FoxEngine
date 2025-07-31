#pragma once
#include "Bindable.h"
#include <string>

class Texture : public Bindable
{
public:
	Texture(Graphics& gfx, const std::string& path);
	void Bind(Graphics& gfx) override;
protected:
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pTextureView;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> pSamplerState;
};

