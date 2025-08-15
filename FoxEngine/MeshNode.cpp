#include "MeshNode.h"
#include "Mesh.h"

#include <assimp/postprocess.h>

MeshNode::MeshNode(Graphics& gfx, SceneNode* parent, std::optional<std::string> name)
    :
    SceneNode(parent, name)
{
}

MeshNode::MeshNode(Graphics& gfx,
                   SceneNode* parent,
                   std::wstring modelPath,
                   std::wstring texturePath,
                   std::optional<std::string> name)
    :
    SceneNode(parent, name)
{
    std::string modelPathStr(modelPath.begin(), modelPath.end());
    Assimp::Importer importer;
    const auto pScene = importer.ReadFile(modelPathStr,
                                          aiProcess_Triangulate |
                                          aiProcess_JoinIdenticalVertices |
                                          aiProcess_FlipUVs
    );

    assert(pScene && pScene->mRootNode && "Failed to load model file");

    LoadAssimpNode(gfx, pScene->mRootNode, pScene, texturePath);
}

MeshNode::MeshNode(Graphics& gfx, SceneNode* parent,
                   const std::vector<Mesh::Vertex>& vertices,
                   const std::vector<unsigned short>& indices,
                   std::wstring texturePath,
                   std::optional<std::string>)
    :
    SceneNode(parent, name)
{
    meshes.push_back(std::make_unique<Mesh>(gfx, vertices, indices, texturePath));
}

void MeshNode::LoadAssimpNode(Graphics& gfx, const aiNode* node, const aiScene* scene, const std::wstring& texturePath)
{
    // don't use assimps root node naming
    if (std::strcmp(node->mName.C_Str(),  "RootNode"))
    {
        name = node->mName.C_Str();
    }

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

    for (unsigned int i = 0; i < node->mNumMeshes; ++i)
    {
        const aiMesh* pMesh = scene->mMeshes[node->mMeshes[i]];
        std::vector<Mesh::Vertex> vertices;
        vertices.reserve(pMesh->mNumVertices);
        for (unsigned int i = 0; i < pMesh->mNumVertices; i++)
        {
            Mesh::Vertex vertex = {};
            vertex.position = {pMesh->mVertices[i].x, pMesh->mVertices[i].y, pMesh->mVertices[i].z};
            if (pMesh->HasNormals())
            {
                vertex.normal = *reinterpret_cast<DirectX::XMFLOAT3*>(&pMesh->mNormals[i]);
            }
            else
            {
                vertex.normal = {0.0f, 0.0f, 0.0f};
            }
            if (pMesh->mTextureCoords[0])
            {
                vertex.texCoord = {pMesh->mTextureCoords[0][i].x, pMesh->mTextureCoords[0][i].y};
            }
            else
            {
                vertex.texCoord = {0.0f, 0.0f};
            }
            vertices.push_back(vertex);
        }

        std::vector<unsigned short> indices;
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
        meshes.push_back(std::make_unique<Mesh>(gfx, std::move(vertices), std::move(indices), texturePath));
    }

    // handle child nodes
    for (unsigned int i = 0; i < node->mNumChildren; ++i)
    {
        auto childNode = std::make_unique<MeshNode>(gfx, parent, std::nullopt);
        childNode->LoadAssimpNode(gfx, node->mChildren[i], scene, texturePath);
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