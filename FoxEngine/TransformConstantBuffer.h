#pragma once
#include "Bindable.h"
#include "Drawable.h"
#include "BindableBase.h"
#include <DirectXMath.h>

class TransformConstantBuffer : public Bindable
{
public:
	struct Transforms
	{
		DirectX::XMMATRIX modelViewProjection;
		DirectX::XMMATRIX modelMatrix;
	};
	TransformConstantBuffer(Graphics& gfx, const Drawable& parent);
	void Bind(Graphics& gfx) override;
private:
	static std::unique_ptr<VertexConstantBuffer<Transforms>> vertexConstantBuffer;
	const Drawable& parent;
};