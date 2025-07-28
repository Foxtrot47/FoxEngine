#include "VertexBuffer.h"

void VertexBuffer::Bind(Graphics& gfx)
{
	const UINT offset = 0u;				// No offset
	GetContext(gfx)->IASetVertexBuffers(
		0u,								// Start at slot 0
		1u,								// One buffer
		pVertexBuffer.GetAddressOf(),	// Pointer to the vertex buffer
		&stride,						// Size of each vertex
		&offset							// No offset
	);
}
