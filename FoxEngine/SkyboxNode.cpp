#include "SkyboxNode.h"

SkyboxNode::SkyboxNode(Graphics& gfx, SceneNode* parent, std::wstring& cubemapPath, const FPVCamera* camera)
:SceneNode(nullptr, "Skybox"),
camera(camera)
{
    skyboxDrawable = std::make_unique<Skybox>(gfx, 10000.0f, cubemapPath);
    isTransformDirty = true;
}

void SkyboxNode::Draw(Graphics& gfx)
{
    const auto transform = GetWorldTransform();
    skyboxDrawable->Draw(gfx, transform);
}

DirectX::XMMATRIX SkyboxNode::GetLocalTransform() const
{
    DirectX::XMFLOAT3 camPos = camera->GetPosition();
    return DirectX::XMMatrixTranslation(camPos.x, camPos.y, camPos.z);
}
