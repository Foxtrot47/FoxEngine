#pragma once
#include "DrawableBase.h"
class Mesh : public DrawableBase<Mesh>
{
public:
	Mesh(Graphics& gfx, std::wstring& modelPath, std::wstring& texturePath);
	void Update(float deltaTime) override;
	DirectX::XMMATRIX GetTransformXM() const override;
};

