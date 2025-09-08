#include "ShadowConstantBuffer.h"
#include "ShadowManager.h"

ShadowConstantBuffer::ShadowConstantBuffer(Graphics& gfx, UINT slot)
{
	if (!vertexConstantBuffer)
	{
		vertexConstantBuffer = std::make_unique<VertexConstantBuffer<ShadowTransforms>>(gfx, slot);
	}
}

void ShadowConstantBuffer::Bind(Graphics& gfx, DirectX::XMMATRIX modelMatrix)
{
	auto shadowMgr = gfx.GetShadowManager();
	
	auto lightViewProj = gfx.GetShadowManager()->GetLightViewProj(gfx);

	const ShadowTransforms transforms = {
		DirectX::XMMatrixTranspose(modelMatrix * lightViewProj),
	};

	vertexConstantBuffer->Update(gfx, transforms);
	vertexConstantBuffer->Bind(gfx);
}

std::unique_ptr<VertexConstantBuffer<ShadowConstantBuffer::ShadowTransforms>> ShadowConstantBuffer::vertexConstantBuffer;