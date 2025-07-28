#include "IndexBuffer.h"

IndexBuffer::IndexBuffer(Graphics& gfx, const std::vector<unsigned short>& indices)
	:
	count((UINT)indices.size())
{
	D3D11_BUFFER_DESC descIndexBuffer = {
		.ByteWidth = UINT(count * sizeof(unsigned short)),
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_INDEX_BUFFER,
		.CPUAccessFlags = 0u,
		.MiscFlags = 0u,
		.StructureByteStride = sizeof(unsigned short)
	};

	D3D11_SUBRESOURCE_DATA isd = {};
	isd.pSysMem = indices.data();
	GetDevice(gfx)->CreateBuffer(&descIndexBuffer, &isd, &pIndexBuffer);
}

void IndexBuffer::Bind(Graphics& gfx)
{
	GetContext(gfx)->IASetIndexBuffer(pIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0u);
}

UINT IndexBuffer::GetCount() const
{
	return count;
}