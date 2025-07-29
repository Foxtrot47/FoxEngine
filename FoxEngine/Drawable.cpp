#include "Drawable.h"
#include <cassert>
#include "IndexBuffer.h"

void Drawable::Draw(Graphics& gfx) const
{
	for (auto& bindable : bindables)
	{
		bindable->Bind(gfx);
	}
	for (const auto& staticBindable : GetStaticBindables())
	{
		staticBindable->Bind(gfx);
	}
	gfx.DrawIndexed(pIndexBuffer->GetCount());
}

void Drawable::AddBind(std::unique_ptr<Bindable> bindable)
{
	assert("*Must* use AddIndexBuffer to bind index bufffer" && typeid(*bindable) != typeid(IndexBuffer));
	bindables.push_back(std::move(bindable));
}

void Drawable::AddIndexBuffer(std::unique_ptr<IndexBuffer> indexBuffer)
{
	assert("Attempting to add index buffer a second time" && pIndexBuffer == nullptr);
	pIndexBuffer = indexBuffer.get();
	bindables.push_back(std::move(indexBuffer));
}