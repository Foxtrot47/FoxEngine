#pragma once
#include <DirectXMath.h>
#include "Engine/Scene/Component.h"

namespace SE {

struct CameraComponent : Component
{
    DirectX::XMFLOAT3 eye    = {  0.0f, 2.0f, -10.0f };
    DirectX::XMFLOAT3 target = {  0.0f, 2.0f,   0.0f };
    DirectX::XMFLOAT3 up     = {  0.0f, 1.0f,   0.0f };

    float fovDeg = 60.0f;
    float nearZ  = 0.1f;
    float farZ   = 1000.0f;

    // Orthographic projection (editor axis/isometric views). orthoHeight is the
    // vertical world-space extent of the view volume.
    bool  orthographic = false;
    float orthoHeight  = 20.0f;

    DirectX::XMMATRIX GetViewMatrix() const
    {
        using namespace DirectX;
        return XMMatrixLookAtLH(
            XMLoadFloat3(&eye),
            XMLoadFloat3(&target),
            XMLoadFloat3(&up));
    }

    DirectX::XMMATRIX GetProjectionMatrix(float aspect) const
    {
        if (orthographic)
        {
            // Symmetric near/far so geometry "behind" the eye still renders —
            // axis views act like an infinite ortho camera (Blender-style).
            return DirectX::XMMatrixOrthographicLH(
                orthoHeight * aspect, orthoHeight, -farZ, farZ);
        }
        return DirectX::XMMatrixPerspectiveFovLH(
            DirectX::XMConvertToRadians(fovDeg), aspect, nearZ, farZ);
    }
};

} // namespace SE
