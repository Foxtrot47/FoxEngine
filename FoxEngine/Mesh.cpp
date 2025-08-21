#include "Mesh.h"
#include "BindableBase.h"
#include "FileUtils.h"

Mesh::Mesh(Graphics& gfx, std::vector<Vertex> vertices, std::vector<unsigned short> indices, std::unique_ptr<Material> material)
{
	//if (!IsStaticInitialized())
	//{
	//	AddStaticBindable(std::make_unique<VertexBuffer>(gfx, vertices));

	//	AddStaticBindable(std::make_unique<Texture>(gfx, texturePath));

	//	auto pVertexShader = std::make_unique<VertexShader>(gfx, GetExecutableDirectory() + L"\\PhongVS.cso");
	//	auto pVertexShaderByteCode = pVertexShader->GetByteCode();

	//	AddStaticBindable(std::move(pVertexShader));

	//	AddStaticBindable(std::make_unique<PixelShader>(gfx, GetExecutableDirectory() + L"\\PhongPS.cso"));

	//	AddStaticIndexBuffer(std::make_unique<IndexBuffer>(gfx, indices));

	//	const std::vector<D3D11_INPUT_ELEMENT_DESC> ied =
	//	{
	//		{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	//		{ "Normal", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	//		{ "TexCoord", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 }
	//	};

	//	AddStaticBindable(std::make_unique<InputLayout>(gfx, ied, pVertexShaderByteCode));

	//	AddStaticBindable(std::make_unique<Topology>(gfx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST));
	//}
	//else {
	//	SetIndexBufferFromStatic();
	//}

	AddBind(std::make_unique<VertexBuffer>(gfx, vertices));

	AddIndexBuffer(std::make_unique<IndexBuffer>(gfx, indices));

	AddBind(std::move(material));

	AddBind(std::make_unique<Topology>(gfx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST));

	AddBind(std::make_unique<TransformConstantBuffer>(gfx));
}

void Mesh::Update(float deltaTime)
{
}