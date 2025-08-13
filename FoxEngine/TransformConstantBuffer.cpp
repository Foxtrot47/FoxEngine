#include "TransformConstantBuffer.h"

TransformConstantBuffer::TransformConstantBuffer(Graphics& gfx, UINT slot)
{
	if (!vertexConstantBuffer)
	{
		vertexConstantBuffer = std::make_unique<VertexConstantBuffer<Transforms>>(gfx, slot);
	}
}

void TransformConstantBuffer::Bind(Graphics& gfx, DirectX::XMMATRIX transform)
{
	const Transforms transforms = {
		DirectX::XMMatrixTranspose(transform),
		DirectX::XMMatrixTranspose(
			transform *
			gfx.GetCamera() *
			gfx.GetProjection()
		)
	};
	vertexConstantBuffer->Update(gfx, transforms);
    vertexConstantBuffer->Bind(gfx);
}

std::unique_ptr<VertexConstantBuffer<TransformConstantBuffer::Transforms>> TransformConstantBuffer::vertexConstantBuffer;