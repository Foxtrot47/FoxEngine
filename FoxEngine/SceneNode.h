#pragma once
#include "Graphics.h"
#include <optional>

class SceneNode
{
public:
	SceneNode(SceneNode* parent, std::optional<std::string> name);
	void AddChild(std::unique_ptr<SceneNode> child);
	DirectX::XMMATRIX GetLocalTransform() const;
	DirectX::XMMATRIX GetWorldTransform() const;
	virtual void Draw(Graphics& gfx);
	virtual ~SceneNode() = default;
	virtual void DrawUI(SceneNode*& pSelectedNode);
	const std::string& GetName() const;
protected:
	void MarkDirty() const;
	DirectX::XMFLOAT3 position;
	DirectX::XMFLOAT4 rotationQuat;
	DirectX::XMFLOAT3 scale;

	SceneNode* parent;
	std::vector<std::unique_ptr<SceneNode>> children;

	std::string name;
	static int count;

	mutable DirectX::XMMATRIX worldTransform = DirectX::XMMatrixIdentity();
	mutable bool isTransformDirty = true;
};