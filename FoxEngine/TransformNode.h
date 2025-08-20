#pragma once
#include "SceneNode.h"

class TransformNode : public SceneNode
{
public:
    TransformNode(
        SceneNode* parent,
        std::optional<std::string> name,
        const DirectX::XMFLOAT3& initialPosition,
        const DirectX::XMFLOAT4& initialRotationQuat,
        const DirectX::XMFLOAT3& initialScale
    );
    void Draw(Graphics& gfx) override;
};
