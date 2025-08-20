#include "TransformNode.h"

TransformNode::TransformNode(
	SceneNode* parent,
	std::optional<std::string> name,
	const DirectX::XMFLOAT3& initialPosition,
	const DirectX::XMFLOAT4& initialRotationQuat,
	const DirectX::XMFLOAT3& initialScale
)
	:
	SceneNode(parent, name, initialPosition, initialRotationQuat, initialScale)
{
}

void TransformNode::Draw(Graphics& gfx)
{
	for (auto& child : children)
	{
		child->Draw(gfx);
	}
}
