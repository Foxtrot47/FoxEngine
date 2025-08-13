#pragma once
#include "SceneNode.h"

class TransformNode : public SceneNode
{
public:
    TransformNode(SceneNode* parent);
    void Draw(Graphics& gfx) override;
};
