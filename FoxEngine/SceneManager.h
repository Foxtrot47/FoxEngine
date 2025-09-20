#pragma once
#include "FPVCamera.h"
#include "Graphics.h"
#include "TransformNode.h"
#include "Utility.h"
#include "Material.h"
#include "MeshNode.h"
#include "LightManager.h"
#include <nlohmann/json.hpp>

class SceneManager
{
public:
	SceneManager(Graphics& gfx, const FPVCamera& cam);
	void DrawSceneGraph(Graphics& gfx);
	void LoadScene(Graphics& gfx, const std::string& jsonPath);

	std::shared_ptr<Material> GetMaterial(const std::string& name) const;
	SceneNode& GetRootNode() { return *rootNode;  }
private:
	std::unique_ptr<SceneNode> rootNode;
	std::unordered_map<std::string, std::shared_ptr<Material>> materials;

	SceneNode* pSelectedNode = nullptr;
	Material::MaterialInstanceData ParseBaseMaterial(const nlohmann::json& materialJson);
};

