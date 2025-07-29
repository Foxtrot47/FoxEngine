#pragma once
#include "Drawable.h"
#include "IndexBuffer.h"

// template class to share static bindables across all instances of a drawable type
template<class T>
class DrawableBase : public Drawable
{
protected:
	// assuming that we will have at least one bindable
	static bool IsStaticInitialized()
	{
		return !staticBindables.empty();
	}
	static void AddStaticBindable(std::unique_ptr<Bindable> bindable)
	{
		assert("*Must* use AddStaticIndexBuffer to bind index buffer" && typeid(*bindable) != typeid(IndexBuffer));
		staticBindables.push_back(std::move(bindable));
	}
	void AddStaticIndexBuffer(std::unique_ptr<IndexBuffer> indexBuffer)
	{
		assert("Attempting to add an index buffer a second time" && pIndexBuffer == nullptr);
		pIndexBuffer = indexBuffer.get();
		staticBindables.push_back(std::move(indexBuffer));
	}
private:
	std::vector<std::unique_ptr<Bindable>>& GetStaticBindables() const override
	{
		return staticBindables;
	}
	// define a static vector to hold static bindables
	static std::vector<std::unique_ptr<Bindable>> staticBindables;
};

// define the static member variable
template<class T>
std::vector<std::unique_ptr<Bindable>> DrawableBase<T>::staticBindables;

