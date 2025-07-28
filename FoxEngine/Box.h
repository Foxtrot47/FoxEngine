#pragma once
#include "Drawable.h"
class Box : public Drawable
{
public:
	Box(Graphics& gfx);
	DirectX::XMMATRIX GetTransformXM() const override;
};

