#pragma once
#include "BindableBase.h"

class Material;
class Renderer;

class Mesh
{
public:
	struct Vertex
	{
		DirectX::XMFLOAT3 position;
		DirectX::XMFLOAT3 normal;
		DirectX::XMFLOAT2 texCoord;
		DirectX::XMFLOAT3 tangent;
		DirectX::XMFLOAT3 bitangent;
	};

	Mesh(Graphics& gfx, std::vector<Vertex> vertices, std::vector<unsigned int> indices, std::shared_ptr<Material> material);
	Material* GetMaterial() const { return pMaterial.get(); }
	VertexBuffer* GetVertexBuffer() const { return pVertexBuffer.get(); }
	IndexBuffer* GetIndexBuffer() const { return pIndexBuffer.get(); }
	Topology* GetTopology() const { return pTopology.get(); }
	TransformConstantBuffer* GetTransformCB() const { return pTransformCB.get(); }
private:
	std::unique_ptr<IndexBuffer> pIndexBuffer = nullptr;
	std::unique_ptr<VertexBuffer> pVertexBuffer = nullptr;
	std::shared_ptr<Material> pMaterial = nullptr;
	std::unique_ptr<Topology> pTopology = nullptr;
	std::unique_ptr<TransformConstantBuffer> pTransformCB = nullptr;
};

