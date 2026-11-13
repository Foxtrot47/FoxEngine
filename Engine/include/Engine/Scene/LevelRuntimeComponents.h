#pragma once

#include <utility>
#include <vector>
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

// Marks an actor's root entity. SceneRuntime adds it when it builds the actor
// (level placement or fox.world.spawn); the ScriptSystem instantiates the
// class with these property overrides.
struct ActorComponent : Component
{
    ActorComponent(std::string actorClass, nlohmann::json properties)
        : actorClass(std::move(actorClass)), properties(std::move(properties)) {}
    std::string    actorClass;
    nlohmann::json properties;
};

// Extra script components placed on an actor, instantiated after the actor's
// own class instance.
struct ScriptComponent : Component
{
    explicit ScriptComponent(std::vector<LevelScriptDefinition> scripts)
        : scripts(std::move(scripts)) {}
    std::vector<LevelScriptDefinition> scripts;
};

} // namespace SE
