#pragma once
#include "DrawableBase.h"
class Mesh : public DrawableBase<Mesh>
{
public:
	struct Vertex
	{
		DirectX::XMFLOAT3 position;
		DirectX::XMFLOAT3 normal;
		DirectX::XMFLOAT2 texCoord;
	};

	Mesh(Graphics& gfx, std::vector<Vertex> vertices, std::vector<unsigned short> indices, std::wstring texturePath);
	void Update(float deltaTime) override;
};

