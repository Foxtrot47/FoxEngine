#pragma once
#include "SceneNode.h"
#include "Mesh.h"
#include "Material.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <memory>
#include <unordered_map>

class MeshNode : public SceneNode
{
public:
    MeshNode(Graphics& gfx, SceneNode* parent, std::optional<std::string> name);
    MeshNode(
        Graphics& gfx,
        SceneNode* parent,
        std::wstring modelPath,
        std::shared_ptr<Material> material,
        std::optional<std::string> name,
        const DirectX::XMFLOAT3& initialPosition,
        const DirectX::XMFLOAT4& initialRotationQuat,
        const DirectX::XMFLOAT3& initialScale
    );
    MeshNode(
        Graphics& gfx,
        SceneNode* parent,
        std::wstring modelPath,
        const std::unordered_map<std::string, std::shared_ptr<Material>>& materials,
        std::optional<std::string> name,
        const DirectX::XMFLOAT3& initialPosition,
        const DirectX::XMFLOAT4& initialRotationQuat,
        const DirectX::XMFLOAT3& initialScale
    );
    MeshNode(Graphics& gfx,
             SceneNode* parent,
             const std::vector<Mesh::Vertex>& vertices,
             const std::vector<unsigned int>& indices,
             std::shared_ptr<Material> material,
             std::optional<std::string>);
    void AddMesh(std::unique_ptr<Mesh> mesh);
    void DrawSceneNode(SceneNode*& pSelectedNode) override;
    const std::vector<std::unique_ptr<Mesh>>& GetMeshes() const { return meshes; }
private:
    void LoadAssimpNode(Graphics& gfx, const aiNode* node, const aiScene* scene, const std::unordered_map<std::string, std::shared_ptr<Material>>& materials);
    std::vector<std::unique_ptr<Mesh>> meshes;
};
