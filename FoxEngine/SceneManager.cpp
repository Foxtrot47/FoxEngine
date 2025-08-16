#include "SceneManager.h"
#include <imgui.h>
#include "FileUtils.h"
#include "MeshNode.h"
#include "PointLightNode.h"
#include "TransformNode.h"

SceneManager::SceneManager(Graphics& gfx)
{
	rootNode = std::make_unique<TransformNode>(nullptr, "Root_Node");

	auto pointLight = std::make_unique<PointLightNode>(gfx, rootNode.get(), "Point_Light");
	rootNode->AddChild(std::move(pointLight));

	auto modelPath = GetExecutableDirectory() + L"\\Models\\SM_Urb_Roa_Equipment_TrafficCone_Plastic_Red_01.FBX";
	auto texturePath = GetExecutableDirectory() + L"\\Textures\\T_Urb_Roa_Equipment_TrafficCone_Plastic_Red_01_D.EXR";
	auto cone = std::make_unique<MeshNode>(gfx,
		rootNode.get(),
		modelPath,
		texturePath,
		"Traffic_Cone");
	rootNode->AddChild(std::move(cone));

	auto m1 = GetExecutableDirectory() + L"\\Models\\SM_Urb_Roa_Asphalt_01.FBX";
	auto m2 = GetExecutableDirectory() + L"\\Textures\\T_Ground_Asphalt_Coarse_01_D.EXR";
	auto road = std::make_unique<MeshNode>(gfx,
		rootNode.get(),
		m1,
		m2,
		"Alphalt_Road_01");
	rootNode->AddChild(std::move(road));
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

