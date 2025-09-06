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
		DirectX::XMMATRIX modelMatrix;
		DirectX::XMMATRIX modelViewProjection;
	};
	TransformConstantBuffer(Graphics& gfx, UINT slot = 0u);
	static void Bind(Graphics& gfx, DirectX::XMMATRIX transform);
	void Bind(Graphics& gfx) override
	{
		assert("Don't call Bind on TransformConstantBuffer without transform");
	}
private:
	static std::unique_ptr<VertexConstantBuffer<Transforms>> vertexConstantBuffer;
};