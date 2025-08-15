#pragma once
#include "SceneNode.h"
#include "Mesh.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>

class MeshNode : public SceneNode
{
public:
    MeshNode(Graphics& gfx, SceneNode* parent, std::optional<std::string> name);
    MeshNode(Graphics& gfx, SceneNode* parent, std::wstring modelPath, std::wstring texturePath, std::optional<std::string> name);
    MeshNode(Graphics& gfx,
             SceneNode* parent,
             const std::vector<Mesh::Vertex>& vertices,
             const std::vector<unsigned short>& indices,
             std::wstring texturePath,
             std::optional<std::string>);
    void AddMesh(std::unique_ptr<Mesh> mesh);
    void Draw(Graphics& gfx);
    void DrawUI(SceneNode*& pSelectedNode) override;
private:
    void LoadAssimpNode(Graphics& gfx, const aiNode* node, const aiScene* scene, const std::wstring& texturePath);
    std::vector<std::unique_ptr<Mesh>> meshes;
};
