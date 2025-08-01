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
			struct
			{
				float u;
				float v;
			} text;
		};

		const std::vector<Vertex> vertices =
		{
			// Front face
			{ {-1.0f, -1.0f,  1.0f}, {0.0f, 1.0f} },  // bottom-left
			{ { 1.0f, -1.0f,  1.0f}, {1.0f, 1.0f} },  // bottom-right
			{ {-1.0f,  1.0f,  1.0f}, {0.0f, 0.0f} },  // top-left
			{ { 1.0f,  1.0f,  1.0f}, {1.0f, 0.0f} },  // top-right

			// Back face
			{ { 1.0f, -1.0f, -1.0f}, {0.0f, 1.0f} },
			{ {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f} },
			{ { 1.0f,  1.0f, -1.0f}, {0.0f, 0.0f} },
			{ {-1.0f,  1.0f, -1.0f}, {1.0f, 0.0f} },

			// Left face
			{ {-1.0f, -1.0f, -1.0f}, {0.0f, 1.0f} },
			{ {-1.0f, -1.0f,  1.0f}, {1.0f, 1.0f} },
			{ {-1.0f,  1.0f, -1.0f}, {0.0f, 0.0f} },
			{ {-1.0f,  1.0f,  1.0f}, {1.0f, 0.0f} },

			// Right face
			{ { 1.0f, -1.0f,  1.0f}, {0.0f, 1.0f} },
			{ { 1.0f, -1.0f, -1.0f}, {1.0f, 1.0f} },
			{ { 1.0f,  1.0f,  1.0f}, {0.0f, 0.0f} },
			{ { 1.0f,  1.0f, -1.0f}, {1.0f, 0.0f} },

			// Top face
			{ {-1.0f,  1.0f,  1.0f}, {0.0f, 1.0f} },
			{ { 1.0f,  1.0f,  1.0f}, {1.0f, 1.0f} },
			{ {-1.0f,  1.0f, -1.0f}, {0.0f, 0.0f} },
			{ { 1.0f,  1.0f, -1.0f}, {1.0f, 0.0f} },

			// Bottom face
			{ {-1.0f, -1.0f, -1.0f}, {0.0f, 1.0f} },
			{ { 1.0f, -1.0f, -1.0f}, {1.0f, 1.0f} },
			{ {-1.0f, -1.0f,  1.0f}, {0.0f, 0.0f} },
			{ { 1.0f, -1.0f,  1.0f}, {1.0f, 0.0f} },
		};

		AddStaticBindable(std::make_unique<VertexBuffer>(gfx, vertices));

		AddStaticBindable(std::make_unique<Texture>(gfx, GetExecutableDirectory() + L"\\Textures\\cube.png"));

		auto pVertexShader = std::make_unique<VertexShader>(gfx, GetExecutableDirectory() + L"\\TexturedVS.cso");
		auto pVertexShaderByteCode = pVertexShader->GetByteCode();

		AddStaticBindable(std::move(pVertexShader));

		AddStaticBindable(std::make_unique<PixelShader>(gfx, GetExecutableDirectory() + L"\\TexturedPS.cso"));

		const std::vector<unsigned short> indices =
		{
			// Front face
			0,  1,  2,    2,  1,  3,
			// Back face  
			4,  5,  6,    6,  5,  7,
			// Left face
			8,  9,  10,   10, 9,  11,
			// Right face
			12, 13, 14,   14, 13, 15,
			// Top face
			16, 17, 18,   18, 17, 19,
			// Bottom face
			20, 21, 22,   22, 21, 23
		};

		AddStaticIndexBuffer(std::make_unique<IndexBuffer>(gfx, indices));



		const std::vector<D3D11_INPUT_ELEMENT_DESC> ied =
		{
			{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TexCoord", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};

		AddStaticBindable(std::make_unique<InputLayout>(gfx, ied, pVertexShaderByteCode));

		AddStaticBindable(std::make_unique<Topology>(gfx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST));
	}
	else {
		SetIndexBufferFromStatic();
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
		DirectX::XMMatrixRotationRollPitchYaw(theta, phi, chi);
}
