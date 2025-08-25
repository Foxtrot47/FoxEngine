#pragma once
#include "FPVCamera.h"
#include "Graphics.h"
#include "TransformNode.h"

class SceneManager
{
public:
	SceneManager(Graphics& gfx, const FPVCamera& cam);
	void Draw(Graphics& gfx) const;
	void DrawSceneGraph(Graphics& gfx);
private:
	std::unique_ptr<TransformNode> rootNode;

	SceneNode* pSelectedNode = nullptr;
};

