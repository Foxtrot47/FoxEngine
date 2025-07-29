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

	void Draw(Graphics& gfx) const;
	void AddBind(std::unique_ptr<Bindable> bindable);
	void AddIndexBuffer(std::unique_ptr<class IndexBuffer> indexBuffer);

	virtual void Update(float deltaTime) = 0;
	virtual DirectX::XMMATRIX GetTransformXM() const = 0;
	virtual ~Drawable() = default;
private:
	virtual const std::vector<std::unique_ptr<Bindable>>& GetStaticBindables() const = 0;
private:
	const class IndexBuffer* pIndexBuffer = nullptr;
	std::vector<std::unique_ptr<Bindable>> bindables;
};

