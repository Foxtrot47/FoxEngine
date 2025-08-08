#include "TransformConstantBuffer.h"

TransformConstantBuffer::TransformConstantBuffer(Graphics& gfx, const Drawable& parent)
	:
	parent(parent)
{
	if (!vertexConstantBuffer)
	{
		vertexConstantBuffer = std::make_unique<VertexConstantBuffer<Transforms>>(gfx);
	}
}

void TransformConstantBuffer::Bind(Graphics& gfx)
{
	const auto modelMatrix = parent.GetTransformXM();
	const Transforms transforms = {
		DirectX::XMMatrixTranspose(modelMatrix),
		DirectX::XMMatrixTranspose(
			modelMatrix *
			gfx.GetCamera() *
			gfx.GetProjection()
		)
	};
	vertexConstantBuffer->Update(gfx, transforms);
    vertexConstantBuffer->Bind(gfx);
}

std::unique_ptr<VertexConstantBuffer<TransformConstantBuffer::Transforms>> TransformConstantBuffer::vertexConstantBuffer;