#pragma once
#include "Graphics.h"
#include <optional>

class SceneNode
{
public:
	SceneNode(SceneNode* parent, std::optional<std::string> name);
	SceneNode(
		SceneNode* parent,
		std::optional<std::string> name,
		const DirectX::XMFLOAT3& initialPosition,
		const DirectX::XMFLOAT4& initialRotationQuat,
		const DirectX::XMFLOAT3& initialScale
	);
	void AddChild(std::unique_ptr<SceneNode> child);
	virtual DirectX::XMMATRIX GetLocalTransform() const;
	DirectX::XMMATRIX GetWorldTransform() const;
	virtual void Draw(Graphics& gfx);
	virtual ~SceneNode() = default;
	virtual void DrawSceneNode(SceneNode*& pSelectedNode);
	virtual void DrawInspectorWindow();
	const std::string& GetName() const;
	SceneNode() = delete;
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