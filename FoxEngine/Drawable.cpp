#include "Drawable.h"
#include <cassert>
#include "IndexBuffer.h"
#include "TransformConstantBuffer.h"
#include "ShadowConstantBuffer.h"

void Drawable::Draw(Graphics& gfx, DirectX::XMMATRIX transform) const
{
	for (auto& bindable : bindables)
	{
		if (auto* transformCB = dynamic_cast<TransformConstantBuffer*>(bindable.get()))
		{
			transformCB->Bind(gfx, transform);
		}
		else if (auto* shadowCB = dynamic_cast<ShadowConstantBuffer*>(bindable.get()))
		{
		}
		else
		{
			bindable->Bind(gfx);
		}
	}
	gfx.DrawIndexed(pIndexBuffer->GetCount());
}

void Drawable::DrawShadows(Graphics& gfx, DirectX::XMMATRIX modelMatrix) const
{
	for (auto& bindable : bindables)
	{
		if (auto* shadowCB = dynamic_cast<ShadowConstantBuffer*>(bindable.get()))
		{
			shadowCB->Bind(gfx, modelMatrix);
		}
		else if (auto* material = dynamic_cast<Material*>(bindable.get()))
		{
		}
		else if (auto* tb = dynamic_cast<TransformConstantBuffer*>(bindable.get()))
		{
		}
		else
		{
			bindable->Bind(gfx);
		}
	}
	gfx.DrawIndexed(pIndexBuffer->GetCount());
}

void Drawable::AddBind(std::shared_ptr<Bindable> bindable)
{
	assert("*Must* use AddIndexBuffer to bind index bufffer" && typeid(*bindable) != typeid(IndexBuffer));
	bindables.push_back(std::move(bindable));
}

void Drawable::AddIndexBuffer(std::shared_ptr<IndexBuffer> indexBuffer)
{
	assert("Attempting to add index buffer a second time" && pIndexBuffer == nullptr);
	pIndexBuffer = indexBuffer.get();
	bindables.push_back(std::move(indexBuffer));
}