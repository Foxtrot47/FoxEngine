#include "SceneManager.h"
#include <imgui.h>
#include "FileUtils.h"
#include "MeshNode.h"
#include "PointLightNode.h"
#include "SkyboxNode.h"
#include <fstream>

SceneManager::SceneManager(Graphics& gfx, const FPVCamera& cam)
{
	const DirectX::XMFLOAT3 defaultPos = { 0.0f, 0.0f, 0.0f };
	const DirectX::XMFLOAT4 defaultRot = { 0.0f, 0.0f, 0.0f, 1.0f };
	const DirectX::XMFLOAT3 defaultScaling = { 1.0f, 1.0f, 1.0f };

	rootNode = std::make_unique<TransformNode>(nullptr, "Root_Node", defaultPos, defaultRot, defaultScaling);

	 auto pointLight = std::make_unique<PointLightNode>(gfx, rootNode.get(), "Point_Light", DirectX::XMFLOAT3(-5.0f, 40.0f, 0.0f));
	 rootNode->AddChild(std::move(pointLight));

	 LoadScene(gfx, "F:\\Software\\Projects\\FoxEngine\\FoxEngine\\scene_nyc.json");

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

void SceneManager::LoadScene(Graphics& gfx, const std::string& jsonPath)
{
	std::ifstream file(jsonPath);
	if (!file.is_open())
	{
		throw std::runtime_error("Failed to open scene file: " + jsonPath);
	}

	nlohmann::json sceneJson;
	file >> sceneJson;

	std::vector<Material::MaterialInstanceData> baseMaterials;

	if (sceneJson.contains("materials") && sceneJson["materials"].is_object())
	{
		for (const auto& [materialName, materialJson] : sceneJson["materials"].items())
		{
			Material::MaterialInstanceData material = ParseBaseMaterial(materialJson);
			material.name = materialName;
			materials[materialName] = std::make_shared<Material>(gfx, material);
		}
	}

	if (sceneJson.contains("objects") && sceneJson["objects"].is_array())
	{
		for (const auto& objectJson : sceneJson["objects"])
		{
			auto mesh = ParseMesh(gfx, objectJson);
			rootNode->AddChild(std::move(mesh));
		}
	}
}

Material::MaterialInstanceData SceneManager::ParseBaseMaterial(const nlohmann::json& materialJson)
{
	Material::MaterialInstanceData mData = {};
	mData.vsPath = GetShaderPath(std::wstring(Utility::convertToUTF16(materialJson.value("vertexShader", "AlbedoVS.cso"))));
	mData.psPath = GetShaderPath(std::wstring(Utility::convertToUTF16(materialJson.value("pixelShader", "AlbedoPS.cso"))));

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
	return mData;
}

std::unique_ptr<MeshNode> SceneManager::ParseMesh(Graphics& gfx, const nlohmann::json& objectJson)
{
	const auto& name = objectJson.value("name", "");
	const auto& modelPath = objectJson.value("modelPath", "");
	const auto& materialName = objectJson.value("materialName", "");

	if (modelPath.empty()) {
		throw std::runtime_error("Object " + name + " has no mesh path");
	}
	if (materialName.empty()) {
		throw std::runtime_error("Object " + name + " has no material reference");
	}


	Transform transform;
	if (objectJson.contains("transform"))
	{
		transform = ParseTransform(objectJson["transform"]);
	}
	try {
		const auto& pMaterial = materials.at(materialName);
		const auto quat = transform.GetRotationQuaternion();
		auto node = std::make_unique<MeshNode>(
			gfx,
			rootNode.get(),
			GetModelPath(Utility::convertToUTF16(modelPath)),
			pMaterial,
			std::make_optional(name),
			transform.position,
			quat,
			transform.scale);
		return node;
	}
	catch (const std::exception& e)
	{
		throw std::runtime_error("Failed to load resources for " + name + ": " + e.what());
	}
}

SceneManager::Transform SceneManager::ParseTransform(const nlohmann::json& transformJson) {
	Transform transform;

	// Parse position
	if (transformJson.contains("position") && transformJson["position"].is_array() && transformJson["position"].size() == 3) {
		transform.position = {
			transformJson["position"][0].get<float>(),
			transformJson["position"][1].get<float>(),
			transformJson["position"][2].get<float>()
		};
	}

	// Parse rotation (Euler angles)
	if (transformJson.contains("rotation") && transformJson["rotation"].is_array() && transformJson["rotation"].size() == 3) {
		transform.rotationEuler = {
			transformJson["rotation"][0].get<float>(), // pitch
			transformJson["rotation"][1].get<float>(), // yaw
			transformJson["rotation"][2].get<float>()  // roll
		};
	}

	// Parse scale
	if (transformJson.contains("scale")) {
		if (transformJson["scale"].is_array() && transformJson["scale"].size() == 3) {
			transform.scale = {
				transformJson["scale"][0].get<float>(),
				transformJson["scale"][1].get<float>(),
				transformJson["scale"][2].get<float>()
			};
		}
		else if (transformJson["scale"].is_number()) {
			// Uniform scale
			float uniformScale = transformJson["scale"].get<float>();
			transform.scale = { uniformScale, uniformScale, uniformScale };
		}
	}

	return transform;
}

DirectX::XMFLOAT4 SceneManager::Transform::GetRotationQuaternion() const
{
	const auto quat = DirectX::XMQuaternionRotationRollPitchYaw(
		rotationEuler.x * DirectX::XM_PI / 180.0f,
		rotationEuler.y * DirectX::XM_PI / 180.0f,
		rotationEuler.z * DirectX::XM_PI / 180.0f
	);
	DirectX::XMFLOAT4 result;
	DirectX::XMStoreFloat4(&result, DirectX::XMQuaternionNormalize(quat));
	return result;
}