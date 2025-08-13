#include "TransformNode.h"

TransformNode::TransformNode(SceneNode* parent)
    :
    SceneNode(parent)
{
}

void TransformNode::Draw(Graphics& gfx)
{
    for (auto& child : children)
    {
        child->Draw(gfx);
    }
}
