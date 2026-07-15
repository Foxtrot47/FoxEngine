#pragma once

#include <utility>
#include "Engine/Scene/Component.h"
#include "Engine/Scene/LevelAsset.h"

namespace SE {

struct LevelIdentityComponent : Component
{
    explicit LevelIdentityComponent(LevelEntityID sourceID) : sourceID(sourceID) {}
    LevelEntityID sourceID = 0;
};

struct MeshRendererComponent : Component
{
    explicit MeshRendererComponent(LevelMeshRendererDefinition definition)
        : definition(std::move(definition)) {}
    LevelMeshRendererDefinition definition;
};

struct DirectionalLightComponent : Component
{
    explicit DirectionalLightComponent(LevelDirectionalLightDefinition definition)
        : definition(std::move(definition)) {}
    LevelDirectionalLightDefinition definition;
};

struct PointLightComponent : Component
{
    explicit PointLightComponent(LevelPointLightDefinition definition)
        : definition(std::move(definition)) {}
    LevelPointLightDefinition definition;
};

struct PrimitiveRendererComponent : Component
{
    explicit PrimitiveRendererComponent(LevelPrimitiveRendererDefinition definition)
        : definition(std::move(definition)) {}
    LevelPrimitiveRendererDefinition definition;
};

struct ParticleEmitterComponent : Component
{
    explicit ParticleEmitterComponent(LevelParticleEmitterDefinition definition)
        : definition(std::move(definition)) {}
    LevelParticleEmitterDefinition definition;
};

} // namespace SE
