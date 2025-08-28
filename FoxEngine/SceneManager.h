#pragma once
#include "FPVCamera.h"
#include "Graphics.h"
#include "TransformNode.h"
#include "Utility.h"
#include "Material.h"
#include "MeshNode.h"
#include <nlohmann/json.hpp>

class SceneManager
{
public:
	SceneManager(Graphics& gfx, const FPVCamera& cam);
	void Draw(Graphics& gfx) const;
	void DrawSceneGraph(Graphics& gfx);
	void LoadScene(Graphics& gfx, const std::string& jsonPath);

    struct Transform {
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 rotationEuler = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 scale = { 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT4 GetRotationQuaternion() const;
    };

private:
	std::unique_ptr<TransformNode> rootNode;
	std::unordered_map<std::string, std::shared_ptr<Material>> materials;

	SceneNode* pSelectedNode = nullptr;
	Material::MaterialInstanceData ParseBaseMaterial(const nlohmann::json& materialJson);
	std::unique_ptr<MeshNode> ParseMesh(Graphics& gfx, const nlohmann::json& objectJson);
    Transform ParseTransform(const nlohmann::json& transformJson);
};

