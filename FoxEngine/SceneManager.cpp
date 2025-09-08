#include "SceneManager.h"
#include <imgui.h>
#include "FileUtils.h"
#include "MeshNode.h"
#include "PointLightNode.h"
#include "DirectionalLightNode.h"
#include "SkyboxNode.h"
#include <fstream>

SceneManager::SceneManager(Graphics& gfx, const FPVCamera& cam)
{
	const DirectX::XMFLOAT3 defaultPos = { 0.0f, 0.0f, 0.0f };
	const DirectX::XMFLOAT4 defaultRot = { 0.0f, 0.0f, 0.0f, 1.0f };
	const DirectX::XMFLOAT3 defaultScaling = { 1.0f, 1.0f, 1.0f };

	rootNode = std::make_unique<TransformNode>(nullptr, "Root_Node", defaultPos, defaultRot, defaultScaling);

	auto sun = std::make_unique<DirectionalLightNode>(
		gfx,
		rootNode.get(),
		gfx.GetLightManager(),
		"Sun",
		DirectX::XMFLOAT3(-71.0f, 40.0f, 0.0f)
	);
	rootNode->AddChild(std::move(sun));


	LoadScene(gfx, "F:\\Software\\Projects\\FoxEngine\\FoxEngine\\scene_downtown.json");
	auto node = std::make_unique<MeshNode>(
		gfx,
		rootNode.get(),
		GetModelPath(Utility::convertToUTF16("scene_01.glb")),
		materials,
		std::make_optional("Scene_Root"),
		defaultPos,
		defaultRot,
		defaultScaling);
	rootNode->AddChild(std::move(node));

	auto cumeMap = GetExecutableDirectory() + L"\\Textures\\citrus_orchard_road_puresky_2k.hdr";
	auto skybox = std::make_unique<SkyboxNode>(gfx,
		rootNode.get(),
		cumeMap,
		&cam
	);
	rootNode->AddChild(std::move(skybox));
}

void SceneManager::Draw(Graphics& gfx) const
{
	rootNode->Draw(gfx);
}

void SceneManager::DrawShadows(Graphics& gfx) const
{
	rootNode->DrawShadows(gfx);
}

void SceneManager::DrawSceneGraph(Graphics& gfx)
{
	if (ImGui::Begin("Scene Graph"))
	{
		rootNode->DrawSceneNode(pSelectedNode);
	}
	ImGui::End();

	if (ImGui::Begin("Inspector"))
	{
		if (pSelectedNode != nullptr)
		{
			pSelectedNode->DrawInspectorWindow();
		}
	}
	ImGui::End();
}

std::shared_ptr<Material> SceneManager::GetMaterial(const std::string& name) const
{
	auto it = materials.find(name);
	if (it != materials.end())
	{
		return it->second;
	}
	return nullptr;
}

void SceneManager::LoadScene(Graphics& gfx, const std::string& jsonPath)
{
	std::ifstream file(jsonPath);
	if (!file.is_open())
	{
		throw std::runtime_error("Failed to open scene file: " + jsonPath);
	}

	nlohmann::json sceneJson;
	file >> sceneJson;

	if (sceneJson.contains("materials") && sceneJson["materials"].is_object())
	{
		for (const auto& [materialName, materialJson] : sceneJson["materials"].items())
		{
			Material::MaterialInstanceData material = ParseBaseMaterial(materialJson);
			material.name = materialName;
			materials[materialName] = std::make_shared<Material>(gfx, material);
		}
	}
}

Material::MaterialInstanceData SceneManager::ParseBaseMaterial(const nlohmann::json& materialJson)
{
	Material::MaterialInstanceData mData = {};
	if (materialJson.contains("properties")) {
		const auto& props = materialJson["properties"];

		if (props.contains("diffuseColor") && props["diffuseColor"].is_array() && props["diffuseColor"].size() == 3)
		{
			mData.diffuseColor = {
				props["diffuseColor"][0].get<float>(),
				props["diffuseColor"][1].get<float>(),
				props["diffuseColor"][2].get<float>()
			};
		}
		mData.specularIntensity = props.value("specularIntensity", 1.0f);
		mData.specularPower = props.value("specularPower", 32.0f);
	}
	if (materialJson.contains("textures") && materialJson["textures"].is_object())
	{
		for (const auto& [textureSlot, texturePath] : materialJson["textures"].items())
		{
			if (!texturePath.is_string())
			{
				continue;
			}
			mData.texturePaths[std::stoi(textureSlot)] = GetTexturePath(std::wstring(Utility::convertToUTF16(texturePath.get<std::string>())));
		}
	}
	mData.vsPath = GetShaderPath(L"PhongNormalVS.cso");
	mData.psPath = GetShaderPath(L"PhongMultiLightsPS.cso");
	return mData;
}
