#pragma once
#include <cstdint>
#include <DirectXMath.h>

namespace SE::Scripting {

// fox.Vec3: doubles so Lua numbers round-trip exactly; converted to float at
// engine boundaries.
struct ScriptVec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline DirectX::XMFLOAT3 ToFloat3(const ScriptVec3& v)
{
    return { static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) };
}

inline ScriptVec3 FromFloat3(const DirectX::XMFLOAT3& v)
{
    return { v.x, v.y, v.z };
}

// fox.Entity: a scene entity id plus the scene generation it was taken from,
// so a handle kept across a scene change fails loudly instead of reaching
// whichever entity reuses the id in the next scene.
struct ScriptEntity
{
    uint32_t id = 0;
    uint32_t generation = 0;
};

// fox.Transform: the TransformComponent of an entity, resolved on every access.
struct ScriptTransform
{
    ScriptEntity entity;
};

// fox.ActorComponent: one component (part) of an actor, addressed by the
// part's child entity.
struct ScriptActorPart
{
    ScriptEntity entity;
};

} // namespace SE::Scripting
