#pragma once
#include "Drawable.h"
#include "IndexBuffer.h"

class Material;

class Mesh : public Drawable
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

	Mesh(Graphics& gfx, std::vector<Vertex> vertices, std::vector<unsigned short> indices, std::unique_ptr<Material> material);
	void Update(float deltaTime) override;
};

