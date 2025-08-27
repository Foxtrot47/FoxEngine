#pragma once
#include <DirectXMath.h>
#include "Graphics.h"

class Bindable;

class Drawable
{
	template<class T>
	friend class DrawableBase;
public:
	Drawable() = default;
	Drawable(const Drawable&) = delete;

	void Draw(Graphics& gfx, DirectX::XMMATRIX transform) const;
	void AddBind(std::shared_ptr<Bindable> bindable);
	void AddIndexBuffer(std::shared_ptr<class IndexBuffer> indexBuffer);

	virtual void Update(float deltaTime) = 0;
	virtual ~Drawable() = default;
private:
	const class IndexBuffer* pIndexBuffer = nullptr;
	std::vector<std::shared_ptr<Bindable>> bindables;
};

