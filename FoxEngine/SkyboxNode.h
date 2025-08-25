#pragma once
#include <string>

#include "FPVCamera.h"
#include "Graphics.h"
#include "SceneNode.h"
#include "Skybox.h"

class SkyboxNode : public SceneNode
{
public:
    SkyboxNode(Graphics& gfx, SceneNode* parent, std::wstring& cubemapPath, const FPVCamera* camera);
    void Draw(Graphics& gfx);
    DirectX::XMMATRIX GetLocalTransform() const override;
private:
    std::unique_ptr<Skybox> skyboxDrawable;
    const FPVCamera* camera;
};
