#pragma once
#include <DirectXMath.h>
#include "Graphics.h"

class Bindable;

class Drawable
{
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
	const IndexBuffer* pIndexBuffer = nullptr;
	std::vector<std::unique_ptr<Bindable>> bindables;
};

