#include "Box.h"
#include "BindableBase.h"
#include "FileUtils.h"

Box::Box(Graphics& gfx,
	std::mt19937& rng,
	std::uniform_real_distribution<float>& angularDistribution,
	std::uniform_real_distribution<float>& deltaDistribution,
	std::uniform_real_distribution<float>& orbitalDistribution,
	std::uniform_real_distribution<float>& radiusDistribution)
	:
	radius(radiusDistribution(rng)),
	deltaRoll(deltaDistribution(rng)),
	deltaPitch(deltaDistribution(rng)),
	deltaYaw(deltaDistribution(rng)),
	deltaPhi(orbitalDistribution(rng)),
	deltaTheta(orbitalDistribution(rng)),
	deltaChi(orbitalDistribution(rng)),
	chi(angularDistribution(rng)),
	theta(angularDistribution(rng)),
	phi(angularDistribution(rng))
{

	if (!IsStaticInitialized())
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

		AddStaticBindable(std::make_unique<VertexBuffer>(gfx, vertices));

		auto pVertexShader = std::make_unique<VertexShader>(gfx, GetExecutableDirectory() + L"\\VertexShader.cso");
		auto pVertexShaderByteCode = pVertexShader->GetByteCode();

		AddStaticBindable(std::move(pVertexShader));

		AddStaticBindable(std::make_unique<PixelShader>(gfx, GetExecutableDirectory() + L"\\PixelShader.cso"));

		const std::vector<unsigned short> indices =
		{
			0,2,1, 2,3,1,
			1,3,5, 3,7,5,
			2,6,3, 3,6,7,
			4,5,7, 4,7,6,
			0,4,2, 2,4,6,
			0,1,4, 1,5,4
		};

		AddStaticIndexBuffer(std::make_unique<IndexBuffer>(gfx, indices));

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
		AddStaticBindable(std::make_unique<PixelConstantBuffer<ColorConstantBuffer>>(gfx, colorConstantBuffer));

		const std::vector<D3D11_INPUT_ELEMENT_DESC> ied =
		{
			{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };

		AddStaticBindable(std::make_unique<InputLayout>(gfx, ied, pVertexShaderByteCode));

		AddStaticBindable(std::make_unique<Topology>(gfx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST));
	}

	AddBind(std::make_unique<TransformConstantBuffer>(gfx, *this));
}

void Box::Update(float deltaTime)
{
	roll += deltaRoll * deltaTime;
	pitch += deltaPitch * deltaTime;
	yaw += deltaYaw * deltaTime;
	theta += deltaTheta * deltaTime;
	phi += deltaPhi * deltaTime;
	chi += deltaChi * deltaTime;
}

DirectX::XMMATRIX Box::GetTransformXM() const
{
	return DirectX::XMMatrixRotationRollPitchYaw(pitch, yaw, roll) *
		DirectX::XMMatrixTranslation(radius, 0.0f, 0.0f) *
		DirectX::XMMatrixRotationRollPitchYaw(theta, phi, chi) *
		DirectX::XMMatrixTranslation(0.0f, 0.0f, 20.0f);
}
