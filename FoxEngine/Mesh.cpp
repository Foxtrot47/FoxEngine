#include "Mesh.h"
#include "BindableBase.h"
#include "FileUtils.h"

Mesh::Mesh(Graphics& gfx, std::vector<Vertex> vertices, std::vector<unsigned short> indices, std::shared_ptr<Material> material)
{
	AddBind(std::make_unique<VertexBuffer>(gfx, vertices));

	AddIndexBuffer(std::make_unique<IndexBuffer>(gfx, indices));

	AddBind(material);

	AddBind(std::make_unique<Topology>(gfx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST));

	AddBind(std::make_unique<TransformConstantBuffer>(gfx));
}

void Mesh::Update(float deltaTime)
{
}