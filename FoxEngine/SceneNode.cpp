#include "SceneNode.h"

SceneNode::SceneNode(SceneNode* parent)
	:
	position(0.0f, 0.0f, 0.0f),
	rotationQuat({ 0.0f, 0.0f, 0.0f, 1.0f }),
	scale(1.0f, 1.0f, 1.0f),
	parent(parent)
{}

void SceneNode::AddChild(std::unique_ptr<SceneNode> child)
{
	child->parent = this;
	children.push_back(std::move(child));
}
DirectX::XMMATRIX SceneNode::GetLocalTransform() const
{
	return DirectX::XMMatrixScaling(scale.x, scale.y, scale.z) *
		DirectX::XMMatrixRotationQuaternion(XMLoadFloat4(&rotationQuat)) *
		DirectX::XMMatrixTranslation(position.x, position.y, position.z);
}
DirectX::XMMATRIX SceneNode::GetWorldTransform() const
{
	return GetLocalTransform() *
		(parent != nullptr ? parent->GetWorldTransform() : DirectX::XMMatrixIdentity());
}

void SceneNode::Draw(Graphics& gfx)
{
	for (auto& child : children)
	{
		child->Draw(gfx);
	}
}
