#pragma once
#include "Graphics.h"
#include "TransformNode.h"

class SceneManager
{
public:
	SceneManager(Graphics& gfx);
	void Draw(Graphics& gfx) const;
private:
	std::unique_ptr<TransformNode> rootNode;
};

