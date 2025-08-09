#include "Mesh.h"
#include "BindableBase.h"
#include "FileUtils.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

Mesh::Mesh(Graphics& gfx, std::wstring& modelPath, std::wstring& texturePath)
{
	if (!IsStaticInitialized())
	{
		std::string modelPathStr(modelPath.begin(), modelPath.end());
		Assimp::Importer importer;
		const auto pScene = importer.ReadFile(modelPathStr,
			aiProcess_Triangulate |
			aiProcess_FlipUVs |
			aiProcess_GenSmoothNormals);

		const auto pMesh = pScene->mMeshes[0];

		struct Vertex
		{
			DirectX::XMFLOAT3 position;
			DirectX::XMFLOAT3 normal;
			DirectX::XMFLOAT2 texCoord;
		};

		std::vector<Vertex> vertices;
		vertices.reserve(pMesh->mNumVertices);
		for (unsigned int i = 0; i < pMesh->mNumVertices; i++)
		{
			Vertex vertex = {};
			vertex.position = { pMesh->mVertices[i].x,pMesh->mVertices[i].y, pMesh->mVertices[i].z };
			if (pMesh->HasNormals())
			{
				vertex.normal = *reinterpret_cast<DirectX::XMFLOAT3*>(&pMesh->mNormals[i]);
			}
			else {
				vertex.normal = { 0.0f, 0.0f, 0.0f };
			}
			if (pMesh->mTextureCoords[0])
			{
				vertex.texCoord = { pMesh->mTextureCoords[0][i].x, pMesh->mTextureCoords[0][i].y };
			}
			else {
				vertex.texCoord = { 0.0f, 0.0f };
			}
			vertices.push_back(vertex);
		}

		AddStaticBindable(std::make_unique<VertexBuffer>(gfx, vertices));

		AddStaticBindable(std::make_unique<Texture>(gfx, texturePath));

		auto pVertexShader = std::make_unique<VertexShader>(gfx, GetExecutableDirectory() + L"\\PhongVS.cso");
		auto pVertexShaderByteCode = pVertexShader->GetByteCode();

		AddStaticBindable(std::move(pVertexShader));

		AddStaticBindable(std::make_unique<PixelShader>(gfx, GetExecutableDirectory() + L"\\PhongPS.cso"));

		std::vector<unsigned short> indices;
		indices.reserve(pMesh->mNumFaces * 3);
		for (unsigned short i = 0; i < pMesh->mNumFaces; i++)
		{
			const auto& face = pMesh->mFaces[i];
			assert(face.mNumIndices == 3);
			indices.push_back(face.mIndices[0]);
			indices.push_back(face.mIndices[1]);
			indices.push_back(face.mIndices[2]);
		}

		AddStaticIndexBuffer(std::make_unique<IndexBuffer>(gfx, indices));

		const std::vector<D3D11_INPUT_ELEMENT_DESC> ied =
		{
			{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "Normal", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
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

void Mesh::Update(float deltaTime)
{}

DirectX::XMMATRIX Mesh::GetTransformXM() const
{
	return DirectX::XMMatrixTranslation(0.0f, 0.0f, 0.0f);
}
