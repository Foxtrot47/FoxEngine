#pragma once
#include "Drawable.h"
#include <DirectXMath.h>

class Skybox : public Drawable
{
public:
	struct Vertex
	{
		DirectX::XMFLOAT3 position;
		DirectX::XMFLOAT2 uv;
	};
	Skybox(Graphics& gfx, float size, std::wstring cubemapPath);
	void Update(float deltaTime) override;
	static void MakeUvSphere(float radius, int segU, int segV,
		std::vector<Skybox::Vertex>& vertices,
		std::vector<unsigned short>& indices);
private:
	std::vector<Vertex> vertices;
	std::vector<unsigned short> indices;
	float size;
};

