#include "SolidSphere.h"
#include "BindableBase.h"
#include "FileUtils.h"

struct Vertex
{
	struct {
		float x;
		float y;
		float z;
	} pos;
};

// from https://danielsieger.com/blog/2021/03/27/generating-spheres.html
static void MakeUvSphere(float radius, int segU, int segV,
	std::vector<Vertex>& vertices,
	std::vector<unsigned int>& indices)
{
	vertices.clear();
	indices.clear();
	vertices.reserve((segU + 1) * (segV + 1));

	for (int v = 0; v <= segV; ++v) {
		float phi = float(v) / segV * DirectX::XM_PI;             // 0..PI
		float y = std::cos(phi);
		float r = std::sin(phi);
		for (int u = 0; u <= segU; ++u) {
			float theta = float(u) / segU * DirectX::XM_2PI;      // 0..2PI
			float x = r * std::cos(theta);
			float z = r * std::sin(theta);

			Vertex vert{};
			vert.pos = { x * radius, y * radius, z * radius };
			vertices.push_back(vert);
		}
	}

	// indices
	for (int v = 0; v < segV; ++v) {
		for (int u = 0; u < segU; ++u) {
			unsigned int i0 = (unsigned int)(v * (segU + 1) + u);
			unsigned int i1 = (unsigned int)(i0 + 1);
			unsigned int i2 = (unsigned int)(i0 + (segU + 1));
			unsigned int i3 = (unsigned int)(i2 + 1);

			// two triangles per quad
			indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
			indices.push_back(i2); indices.push_back(i1); indices.push_back(i3);
		}
	}
}

SolidSphere::SolidSphere(Graphics& gfx, float radius, int segU, int segV)
{
	std::vector<Vertex> vertices;
	std::vector<unsigned int> indices;

	MakeUvSphere(radius, segU, segV, vertices, indices);

	AddBind(std::make_unique<VertexBuffer>(gfx, vertices));

	AddIndexBuffer(std::make_unique<IndexBuffer>(gfx, indices));

	Material::MaterialInstanceData sphereMat = {};
	sphereMat.name = "M_Sphere";
	sphereMat.vsPath = GetShaderPath(L"PhongVS.cso");
	sphereMat.psPath = GetShaderPath(L"AlbedoPS.cso");
	AddBind(std::make_unique<Material>(gfx, sphereMat));

	AddBind(std::make_unique<Topology>(gfx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST));

	AddBind(std::make_unique<TransformConstantBuffer>(gfx));
}

void SolidSphere::Update(float deltaTime)
{
}

void SolidSphere::SetPosition(const DirectX::XMFLOAT3& _position) { position = _position; }
