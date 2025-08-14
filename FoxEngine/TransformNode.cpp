#include "TransformNode.h"

TransformNode::TransformNode(SceneNode* parent, std::optional<std::string> name)
    :
    SceneNode(parent, name)
{
}

void TransformNode::Draw(Graphics& gfx)
{
    for (auto& child : children)
    {
        child->Draw(gfx);
    }
}
