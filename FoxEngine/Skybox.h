#pragma once
#include "BindableBase.h"
#include <DirectXMath.h>

class Skybox
{
public:
	struct Vertex
	{
		DirectX::XMFLOAT3 position;
		DirectX::XMFLOAT2 uv;
	};
	Skybox(Graphics& gfx, float size, std::wstring cubemapPath);
	static void MakeUvSphere(float radius, int segU, int segV,
		std::vector<Skybox::Vertex>& vertices,
		std::vector<unsigned int>& indices);
	void Draw(Graphics& gfx, DirectX::XMMATRIX transform) const;
private:
	std::vector<Vertex> vertices;
	std::vector<unsigned int> indices;
	float size;

	std::unique_ptr<IndexBuffer> pIndexBuffer = nullptr;
	std::unique_ptr<VertexBuffer> pVertexBuffer = nullptr;
	std::unique_ptr<Material> pMaterial = nullptr;
	std::unique_ptr<Topology> pTopology = nullptr;
	std::unique_ptr<TransformConstantBuffer> pTransformCB = nullptr;
};

