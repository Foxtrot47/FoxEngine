#pragma once
#include "Graphics.h"

class SceneNode
{
public:
	SceneNode(SceneNode* parent);
	void AddChild(std::unique_ptr<SceneNode> child);
	DirectX::XMMATRIX GetLocalTransform() const;
	DirectX::XMMATRIX GetWorldTransform() const;
	virtual void Draw(Graphics& gfx);
	virtual ~SceneNode() = default;
protected:
	DirectX::XMFLOAT3 position;
	DirectX::XMFLOAT4 rotationQuat;
	DirectX::XMFLOAT3 scale;

	SceneNode* parent;
	std::vector<std::unique_ptr<SceneNode>> children;
};

