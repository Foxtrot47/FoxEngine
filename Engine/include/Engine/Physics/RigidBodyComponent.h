#pragma once
#include <DirectXMath.h>
#include "Engine/Scene/Component.h"

namespace SE {

// Authoring + mirror state for a simulated body. PhysicsWorld writes the
// simulated velocity back here every step and pushes edits of these fields
// into the simulation, so tools can treat them as plain data.
struct RigidBodyComponent : Component
{
    float mass       = 1.0f;  // kg
    bool  useGravity = true;

    float restitution = 0.6f; // bounciness [0=dead, 1=perfectly elastic]
    float friction    = 0.4f; // surface friction coefficient

    DirectX::XMFLOAT3 velocity = { 0.0f, 0.0f, 0.0f }; // linear, m/s

    // Instantaneous velocity change, independent of mass.
    void AddImpulse(DirectX::XMFLOAT3 deltaVelocity)
    {
        velocity.x += deltaVelocity.x;
        velocity.y += deltaVelocity.y;
        velocity.z += deltaVelocity.z;
    }
};

} // namespace SE
