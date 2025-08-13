#include "SceneManager.h"
#include "FileUtils.h"
#include "MeshNode.h"
#include "TransformNode.h"

SceneManager::SceneManager(Graphics& gfx)
{
	rootNode = std::make_unique<TransformNode>(nullptr);

	auto modelPath = GetExecutableDirectory() + L"\\Models\\SM_Urb_Roa_Equipment_TrafficCone_Plastic_Red_01.FBX";
	auto texturePath = GetExecutableDirectory() + L"\\Textures\\T_Urb_Roa_Equipment_TrafficCone_Plastic_Red_01_D.EXR";
	auto cone = std::make_unique<MeshNode>(gfx,
		rootNode.get(),
		modelPath,
		texturePath);
	rootNode->AddChild(std::move(cone));
}

void SceneManager::Draw(Graphics& gfx) const
{
	rootNode->Draw(gfx);
}

