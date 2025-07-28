#include "Box.h"
#include "BindableBase.h"
#include "FileUtils.h"

Box::Box(Graphics& gfx)
{

	struct Vertex
	{
		struct
		{
			float x;
			float y;
			float z;
		} pos;
	};

	const std::vector<Vertex> vertices =
	{
		{ -1.0f,-1.0f,-1.0f	 },
		{ 1.0f,-1.0f,-1.0f	 },
		{ -1.0f,1.0f,-1.0f	 },
		{ 1.0f,1.0f,-1.0f	  },
		{ -1.0f,-1.0f,1.0f	 },
		{ 1.0f,-1.0f,1.0f	  },
		{ -1.0f,1.0f,1.0f	 },
		{ 1.0f,1.0f,1.0f	 },
	};

	AddBind(std::make_unique<VertexBuffer>(gfx, vertices));

	auto pVertexShader = std::make_unique<VertexShader>(gfx, GetExecutableDirectory() + L"\\VertexShader.cso");
	auto pVertexShaderByteCode = pVertexShader->GetByteCode();
	
	AddBind(std::move(pVertexShader));

	AddBind(std::make_unique<PixelShader>(gfx, GetExecutableDirectory() + L"\\PixelShader.cso"));

	const std::vector<unsigned short> indices =
	{
		0,2,1, 2,3,1,
		1,3,5, 3,7,5,
		2,6,3, 3,6,7,
		4,5,7, 4,7,6,
		0,4,2, 2,4,6,
		0,1,4, 1,5,4 
	};

	AddIndexBuffer(std::make_unique<IndexBuffer>(gfx, indices));

	struct ConstantBuffer {
		DirectX::XMMATRIX transform; // Transformation matrix
	};

	const ConstantBuffer cb =
	{
		{
			DirectX::XMMatrixTranspose(
				DirectX::XMMatrixTranslation(1.0f, 1.0f, 10) * // Translate the triangle
				DirectX::XMMatrixPerspectiveFovLH(1.0f, 16.0f / 9.0f, 0.5, 10.0f) // Perspective projection
			)
		}
	}; 
	AddBind(std::make_unique<VertexConstantBuffer<ConstantBuffer>>(gfx, cb));

	struct ColorConstantBuffer
	{
		struct
		{
			float r;
			float g;
			float b;
			float a;
		} face_colors[6];
	};
	const ColorConstantBuffer colorConstantBuffer =
	{
		{
			{ 1.0f, 0.0f, 0.0f, 1.0f }, // Red
			{ 0.0f, 1.0f, 0.0f, 1.0f }, // Green
			{ 0.0f, 0.0f, 1.0f, 1.0f }, // Blue
			{ 1.0f, 1.0f, 0.0f, 1.0f }, // Yellow
			{ 1.0f, 0.5f, 0.5f, 1.0f }, // Light Red
			{ 0.5f, 1.0f, 1.0f, 1.0f }  // Cyan
		}
	};
	AddBind(std::make_unique<PixelConstantBuffer<ColorConstantBuffer>>(gfx, colorConstantBuffer));

	const std::vector<D3D11_INPUT_ELEMENT_DESC> ied =
	{
		{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };

	AddBind(std::make_unique<InputLayout>(gfx, ied, pVertexShaderByteCode));

	AddBind(std::make_unique<Topology>(gfx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST));
}

DirectX::XMMATRIX Box::GetTransformXM() const
{
	return DirectX::XMMatrixTranslation(1.0f, 1.0f, 6.0f);
}
