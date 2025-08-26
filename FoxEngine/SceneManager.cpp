#include "SceneManager.h"
#include <imgui.h>
#include "FileUtils.h"
#include "MeshNode.h"
#include "PointLightNode.h"
#include "TransformNode.h"
#include "SkyboxNode.h"
#include "SkyboxNode.h"

SceneManager::SceneManager(Graphics& gfx, const FPVCamera& cam)
{
	const DirectX::XMFLOAT3 defaultPos = { 0.0f, 0.0f, 0.0f };
	const DirectX::XMFLOAT4 defaultRot = { 0.0f, 0.0f, 0.0f, 1.0f };
	const DirectX::XMFLOAT3 defaultScaling = { 1.0f, 1.0f, 1.0f };

	rootNode = std::make_unique<TransformNode>(nullptr, "Root_Node", defaultPos, defaultRot, defaultScaling);

	 auto pointLight = std::make_unique<PointLightNode>(gfx, rootNode.get(), "Point_Light", DirectX::XMFLOAT3(-5.0f, 40.0f, 0.0f));
	 rootNode->AddChild(std::move(pointLight));
	
	 auto m1 = GetExecutableDirectory() + L"\\Models\\SM_Urb_Roa_Asphalt_01.FBX";
	 auto m2 = GetExecutableDirectory() + L"\\Textures\\T_Ground_Asphalt_Coarse_01_D.EXR";
	 auto road = std::make_unique<MeshNode>(gfx,
	 	rootNode.get(),
	 	m1,
	 	&m2,
	 	"Alphalt_Road_01",
	 	defaultPos,
	 	defaultRot,
	 	defaultScaling
	 );
	 rootNode->AddChild(std::move(road));
	
	 auto m3 = GetExecutableDirectory() + L"\\Models\\Business girl.fbx";
	  auto car = std::make_unique<MeshNode>(gfx,
	  	rootNode.get(),
	  	m3,
	  	nullptr,
	  	"MP-4",
	  	defaultPos,
	  	defaultRot,
	  	DirectX::XMFLOAT3({ 0.1f, 0.1f, 0.1f })
	  );
	  rootNode->AddChild(std::move(car));

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

