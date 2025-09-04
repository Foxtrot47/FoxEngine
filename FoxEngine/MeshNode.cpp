#include "MeshNode.h"
#include "Mesh.h"
#include "Material.h"
#include "FileUtils.h"
#include <assimp/postprocess.h>

MeshNode::MeshNode(Graphics& gfx, SceneNode* parent, std::optional<std::string> name)
	:
	SceneNode(parent, name)
{
}

MeshNode::MeshNode(Graphics& gfx,
	SceneNode* parent,
	std::wstring modelPath,
	std::shared_ptr<Material> material,
	std::optional<std::string> name,
	const DirectX::XMFLOAT3& initialPosition,
	const DirectX::XMFLOAT4& initialRotationQuat,
	const DirectX::XMFLOAT3& initialScale
)
	:
	SceneNode(parent, name, initialPosition, initialRotationQuat, initialScale)
{
	std::string modelPathStr(modelPath.begin(), modelPath.end());
	Assimp::Importer importer;
	const auto pScene = importer.ReadFile(modelPathStr,
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_FlipUVs |
		aiProcess_GenSmoothNormals |
		aiProcess_CalcTangentSpace
	);

	assert(pScene && pScene->mRootNode && "Failed to load model file");

	if (material) {
		LoadAssimpNode(gfx, pScene->mRootNode, pScene, material);
	}
	else {
		// create materials from model data
		LoadAssimpNode(gfx, pScene->mRootNode, pScene, nullptr);
	}
}

MeshNode::MeshNode(Graphics& gfx, SceneNode* parent,
	const std::vector<Mesh::Vertex>& vertices,
	const std::vector<unsigned short>& indices,
	std::shared_ptr<Material> material,
	std::optional<std::string>)
	:
	SceneNode(parent, name)
{
	if (material) {
		meshes.push_back(std::make_unique<Mesh>(gfx, vertices, indices, material));
	}
	else {
		// Create a default material if none provided
		Material::MaterialInstanceData mData = {};
		mData.vsPath = GetShaderPath(L"PhongVS.cso");
		mData.psPath = GetShaderPath(L"PhongPS.cso");
		mData.texturePaths = std::unordered_map<int, std::wstring> {};
		auto pMaterial = std::make_unique<Material>(gfx, mData);
		meshes.push_back(std::make_unique<Mesh>(gfx, vertices, indices, std::move(pMaterial)));
	}
}

void MeshNode::LoadAssimpNode(Graphics& gfx, const aiNode* node, const aiScene* scene, std::shared_ptr<Material> material)
{
	// don't use assimps root node naming
	if (std::strcmp(node->mName.C_Str(), "RootNode"))
	{
		name = node->mName.C_Str();
	}

	bool isRootNode = (std::strcmp(node->mName.C_Str(), "RootNode"));

	if (isRootNode)
	{
		const auto& aiTransform = node->mTransformation;
		auto localTransform = DirectX::XMMatrixTranspose(DirectX::XMMATRIX(
			aiTransform.a1, aiTransform.a2, aiTransform.a3, aiTransform.a4,
			aiTransform.b1, aiTransform.b2, aiTransform.b3, aiTransform.b4,
			aiTransform.c1, aiTransform.c2, aiTransform.c3, aiTransform.c4,
			aiTransform.d1, aiTransform.d2, aiTransform.d3, aiTransform.d4
		));

		DirectX::XMVECTOR scaleVec;
		DirectX::XMVECTOR rotQuat;
		DirectX::XMVECTOR posVec;

		if (DirectX::XMMatrixDecompose(&scaleVec, &rotQuat, &posVec, localTransform))
		{
			DirectX::XMStoreFloat3(&scale, scaleVec);
			DirectX::XMStoreFloat3(&position, posVec);
			DirectX::XMStoreFloat4(&rotationQuat, rotQuat);
		}
	}

	for (unsigned int i = 0; i < node->mNumMeshes; ++i)
	{
		const aiMesh* pMesh = scene->mMeshes[node->mMeshes[i]];
		std::vector<Mesh::Vertex> vertices;
		vertices.reserve(pMesh->mNumVertices);
		for (unsigned int i = 0; i < pMesh->mNumVertices; i++)
		{
			Mesh::Vertex vertex = {};
			vertex.position = { pMesh->mVertices[i].x, pMesh->mVertices[i].y, pMesh->mVertices[i].z };
			if (pMesh->HasNormals())
			{
				vertex.normal = *reinterpret_cast<DirectX::XMFLOAT3*>(&pMesh->mNormals[i]);

			}
			else
			{
				vertex.normal = { 0.0f, 0.0f, 0.0f };
			}
			if (pMesh->mTextureCoords[0])
			{
				vertex.texCoord = { pMesh->mTextureCoords[0][i].x, pMesh->mTextureCoords[0][i].y };
			}
			else
			{
				vertex.texCoord = { 0.0f, 0.0f };
			}
			if (pMesh->HasTangentsAndBitangents())
			{
				vertex.tangent = *reinterpret_cast<DirectX::XMFLOAT3*>(&pMesh->mTangents[i]);
				vertex.bitangent = *reinterpret_cast<DirectX::XMFLOAT3*>(&pMesh->mBitangents[i]);
			}
			vertices.push_back(vertex);
		}

		std::vector<unsigned int> indices;
		indices.resize(pMesh->mNumFaces * 3);

		for (size_t i = 0; i < pMesh->mNumFaces; ++i)
		{
			const auto& face = pMesh->mFaces[i];
			assert(face.mNumIndices == 3);

			size_t idx = i * 3;
			indices[idx] = face.mIndices[0];
			indices[idx + 1] = face.mIndices[1];
			indices[idx + 2] = face.mIndices[2];
		}

		aiMaterial* aiMat = scene->mMaterials[pMesh->mMaterialIndex];
		Material::MaterialInstanceData mData = {};
		mData.vsPath = GetShaderPath(L"PhongVS.cso");;
		mData.psPath = GetShaderPath(L"PhongPS.cso");
		mData.texturePaths = std::unordered_map<int, std::wstring>{};

		if (material) {
			meshes.push_back(std::make_unique<Mesh>(gfx, std::move(vertices), std::move(indices), material));
		}
		else {

			for (unsigned int texIndex = 0; texIndex < aiMat->GetTextureCount(aiTextureType_DIFFUSE); texIndex++)
			{
				aiString texPath;
				if (aiMat->GetTexture(aiTextureType_DIFFUSE, texIndex, &texPath) == AI_SUCCESS)
				{
					std::filesystem::path p(texPath.C_Str());
					mData.texturePaths[0] = GetExecutableDirectory() + L"\\Textures\\" + p.filename().wstring();
				}
			}

			for (unsigned int texIndex = 0; texIndex < aiMat->GetTextureCount(aiTextureType_SPECULAR); texIndex++)
			{
				aiString texPath;
				if (aiMat->GetTexture(aiTextureType_SPECULAR, texIndex, &texPath) == AI_SUCCESS)
				{
					std::filesystem::path p(texPath.C_Str());
					mData.texturePaths[1] = GetExecutableDirectory() + L"\\Textures\\" + p.filename().wstring();
				}
			}

			for (unsigned int texIndex = 0; texIndex < aiMat->GetTextureCount(aiTextureType_NORMALS); texIndex++)
			{
				aiString texPath;
				if (aiMat->GetTexture(aiTextureType_NORMALS, texIndex, &texPath) == AI_SUCCESS)
				{
					std::filesystem::path p(texPath.C_Str());
					mData.texturePaths[2] = GetExecutableDirectory() + L"\\Textures\\" + p.filename().wstring();
				}
			}

			aiColor3D specColor;
			float specularIntensity;
			if (aiMat->Get(AI_MATKEY_SHININESS_STRENGTH, specularIntensity) == AI_SUCCESS)
			{
			}
			else if (aiMat->Get(AI_MATKEY_COLOR_SPECULAR, specColor) == AI_SUCCESS)
			{
				mData.specularIntensity = (specColor.r + specColor.g + specColor.b) / 3.0f;
			}
			else mData.specularIntensity = 1.0f;

			float shininess;
			if (aiMat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS)
			{
				// scale Assimp’s 0–1000 to 1–128
				mData.specularPower = shininess / 7.8125f;
			}
			auto pMaterial = std::make_unique<Material>(gfx, mData);
			meshes.push_back(std::make_unique<Mesh>(gfx, std::move(vertices), std::move(indices), std::move(pMaterial)));
		}
	}

	// handle child nodes
	for (unsigned int i = 0; i < node->mNumChildren; ++i)
	{
		auto childNode = std::make_unique<MeshNode>(gfx, this, std::nullopt);
		childNode->LoadAssimpNode(gfx, node->mChildren[i], scene, material);
		AddChild(std::move(childNode));
	}
	isTransformDirty = true;
}

void MeshNode::AddMesh(std::unique_ptr<Mesh> mesh)
{
	meshes.push_back(std::move(mesh));
}

void MeshNode::Draw(Graphics& gfx)
{
	const auto transform = GetWorldTransform();
	for (const auto& mesh : meshes)
	{
		mesh->Draw(gfx, transform);
	}
	for (auto& child : children)
	{
		child->Draw(gfx);
	}
}

void MeshNode::DrawSceneNode(SceneNode*& pSelectedNode)
{
	if (meshes.empty() && children.size() == 1)
	{
		children[0]->DrawSceneNode(pSelectedNode);
		return;
	}
	else {
		SceneNode::DrawSceneNode(pSelectedNode);
	}
}