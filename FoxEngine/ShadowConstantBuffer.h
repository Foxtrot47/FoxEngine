#pragma once
#include "Bindable.h"
#include "Drawable.h"
#include "BindableBase.h"
#include <DirectXMath.h>

class ShadowConstantBuffer : public Bindable
{
	public:
	struct ShadowTransforms
	{
		DirectX::XMMATRIX modelLightViewProjection;
	};
	ShadowConstantBuffer(Graphics& gfx, UINT slot = 0u);
	static void Bind(Graphics& gfx, DirectX::XMMATRIX modelMatrix);
	void Bind(Graphics& gfx) override
	{
		assert("Don't call Bind on ShadowConstantBuffer without transform");
	}
private:
	static std::unique_ptr<VertexConstantBuffer<ShadowTransforms>> vertexConstantBuffer;
};

