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
	std::vector<unsigned short>& indices)
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
			unsigned short i0 = (unsigned short)(v * (segU + 1) + u);
			unsigned short i1 = (unsigned short)(i0 + 1);
			unsigned short i2 = (unsigned short)(i0 + (segU + 1));
			unsigned short i3 = (unsigned short)(i2 + 1);

			// two triangles per quad
			indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
			indices.push_back(i2); indices.push_back(i1); indices.push_back(i3);
		}
	}
}

SolidSphere::SolidSphere(Graphics& gfx, float radius, int segU, int segV)
{
	if (!IsStaticInitialized())
	{
		std::vector<Vertex> vertices;
		std::vector<unsigned short> indices;

		MakeUvSphere(radius, segU, segV, vertices, indices);

		AddStaticBindable(std::make_unique<VertexBuffer>(gfx, vertices));

		auto pVertexShader = std::make_unique<VertexShader>(gfx, GetExecutableDirectory() + L"\\SolidSphereVS.cso");
		auto pVertexShaderByteCode = pVertexShader->GetByteCode();

		AddStaticBindable(std::move(pVertexShader));

		AddStaticBindable(std::make_unique<PixelShader>(gfx, GetExecutableDirectory() + L"\\SolidSpherePS.cso"));

		AddStaticIndexBuffer(std::make_unique<IndexBuffer>(gfx, indices));

		const std::vector<D3D11_INPUT_ELEMENT_DESC> ied =
		{
			{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};

		AddStaticBindable(std::make_unique<InputLayout>(gfx, ied, pVertexShaderByteCode));

		AddStaticBindable(std::make_unique<Topology>(gfx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST));
	}
	else {
		SetIndexBufferFromStatic();
	}

	AddBind(std::make_unique<TransformConstantBuffer>(gfx, *this));
}

DirectX::XMMATRIX SolidSphere::GetTransformXM() const
{
	return
		DirectX::XMMatrixScaling(scale, scale, scale) * DirectX::XMMatrixTranslation(position.x, position.y, position.z);
}

void SolidSphere::Update(float deltaTime)
{
}

void SolidSphere::SetPosition(const DirectX::XMFLOAT3& _position) { position = _position; }
