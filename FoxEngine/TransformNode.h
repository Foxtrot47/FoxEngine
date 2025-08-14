#pragma once
#include "SceneNode.h"

class TransformNode : public SceneNode
{
public:
    TransformNode(SceneNode* parent, std::optional<std::string> name);
    void Draw(Graphics& gfx) override;
};
