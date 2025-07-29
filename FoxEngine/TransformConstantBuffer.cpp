#include "TransformConstantBuffer.h"

TransformConstantBuffer::TransformConstantBuffer(Graphics& gfx, const Drawable& parent)
	:
	vertexConstantBuffer(gfx),
	parent(parent)
{}

void TransformConstantBuffer::Bind(Graphics& gfx)
{
	vertexConstantBuffer.Update(gfx,
		DirectX::XMMatrixTranspose(
			parent.GetTransformXM() * gfx.GetProjection()
		)
	);
	vertexConstantBuffer.Bind(gfx);
}