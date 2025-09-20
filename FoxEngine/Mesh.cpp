#include "Mesh.h"
#include "FileUtils.h"
#include "Renderer.h"

Mesh::Mesh(Graphics& gfx, std::vector<Vertex> vertices, std::vector<unsigned int> indices, std::shared_ptr<Material> material)
{
	pVertexBuffer = std::make_unique<VertexBuffer>(gfx, vertices);
	pIndexBuffer = std::make_unique<IndexBuffer>(gfx, indices);
	pMaterial = std::move(material);
	pTopology = std::make_unique<Topology>(gfx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pTransformCB = std::make_unique<TransformConstantBuffer>(gfx);
}
