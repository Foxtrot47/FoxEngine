#pragma once
#include "SceneNode.h"
#include "Mesh.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>

class MeshNode : public SceneNode
{
public:
    MeshNode(Graphics& gfx, SceneNode* parent);
    MeshNode(Graphics& gfx, SceneNode* parent, std::wstring modelPath, std::wstring texturePath);
    MeshNode(Graphics& gfx,
             SceneNode* parent,
             const std::vector<Mesh::Vertex>& vertices,
             const std::vector<unsigned short>& indices,
             std::wstring texturePath);
    void AddMesh(std::unique_ptr<Mesh> mesh);
    void Draw(Graphics& gfx);
private:
    void LoadAssimpNode(Graphics& gfx, const aiNode* node, const aiScene* scene, const std::wstring& texturePath);
    std::vector<std::unique_ptr<Mesh>> meshes;
};
