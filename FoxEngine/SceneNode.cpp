#include "SceneNode.h"
#include "imgui.h"
#include "Math.h"

SceneNode::SceneNode(SceneNode* parent, std::optional<std::string> _name)
	:
	position(0.0f, 0.0f, 0.0f),
	rotationQuat({ 0.0f, 0.0f, 0.0f, 1.0f }),
	scale(1.0f, 1.0f, 1.0f),
	parent(parent)
{
	isTransformDirty = true;
	count++;
	!_name.has_value() ? name = "Unnamed_Node_" + SceneNode::count : name = _name.value();
}

void SceneNode::AddChild(std::unique_ptr<SceneNode> child)
{
	child->parent = this;
	child->isTransformDirty = true;
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
	if (isTransformDirty)
	{
		worldTransform = GetLocalTransform() *
			(parent != nullptr ? parent->GetWorldTransform() : DirectX::XMMatrixIdentity());
		isTransformDirty = false;
	}
	return worldTransform;
}

void SceneNode::Draw(Graphics& gfx)
{
	for (auto& child : children)
	{
		child->Draw(gfx);
	}
}

const std::string& SceneNode::GetName() const
{
	return name;
}

void SceneNode::MarkDirty() const
{
	isTransformDirty = true;
	for (auto& child : children)
	{
		child->MarkDirty();
	}
}

void SceneNode::DrawSceneNode(SceneNode*& pSelectedNode)
{
	// don't render for root node
	if (name == "Root_Node")
	{
		for (auto& child : children)
		{
			child->DrawSceneNode(pSelectedNode);
		}
		return;
	}

	const auto nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow
		| ((pSelectedNode == this) ? ImGuiTreeNodeFlags_Selected : 0)
		| (children.empty() ? ImGuiTreeNodeFlags_Leaf : 0);

	const bool isNodeOpen = ImGui::TreeNodeEx(
		(void*)this,
		nodeFlags,
		name.c_str()
	);

	if (ImGui::IsItemClicked())
	{
		pSelectedNode = this;
	}

	if (isNodeOpen)
	{
		for (auto& child : children)
		{
			child->DrawSceneNode(pSelectedNode);
		}
		ImGui::TreePop();
	}

}

void SceneNode::DrawInspectorWindow()
{
	ImGui::Text("Transform");
	if (ImGui::DragFloat3("Position", &position.x, 0.1f))
	{
		MarkDirty();
	}
	if (ImGui::DragFloat3("Scale", &scale.x, 0.1f))
	{
		MarkDirty();
	}
	// Store last Euler angles to stabilize UI updates
	static DirectX::XMFLOAT3 lastEuler = { 0.0f, 0.0f, 0.0f };

	// Get current Euler angles
	auto euler = ConvertQuaternionToEuler(rotationQuat);
	if (lastEuler.x == 0.0f && lastEuler.y == 0.0f && lastEuler.z == 0.0f)
	{
		lastEuler = euler;
	}

	if (ImGui::DragFloat3("Rotation", &lastEuler.x, 0.5f))
	{
		const auto newQuat = DirectX::XMQuaternionRotationRollPitchYaw(
			lastEuler.x * DirectX::XM_PI / 180.0f,
			lastEuler.y * DirectX::XM_PI / 180.0f,
			lastEuler.z * DirectX::XM_PI / 180.0f
		);
		DirectX::XMStoreFloat4(&rotationQuat, DirectX::XMQuaternionNormalize(newQuat));
		MarkDirty();
	}
}

int SceneNode::count;
